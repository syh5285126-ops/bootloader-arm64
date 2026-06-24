/*
 * bm1684xSdhci.c — BM1684X Synopsys DesignWare SDHCI driver
 *
 * Self-contained: only depends on bm1684xSdhciHw.h and bm1684xSdhciOsal.h.
 * No VxWorks SDK headers required.
 *
 * Two command-completion strategies are compiled in together and selected at
 * runtime based on whether the OSAL semaphore/IRQ callbacks are non-NULL:
 *
 *   Interrupt mode  – ISR clears status, signals semaphore; caller blocks.
 *   Polling mode    – caller spins on INT_STATUS register (no IRQ needed).
 *
 * Copyright (c) 2024 Bitmain / Sophgo.  SPDX-License-Identifier: BSD-3-Clause
 */

#include "bm1684xSdhciHw.h"
#include "bm1684xSdhciOsal.h"

/* -------------------------------------------------------------------------
 * Private constants
 * ------------------------------------------------------------------------- */

#define SDHCI_CMD_TIMEOUT_MS    1000U
#define SDHCI_XFER_TIMEOUT_MS   5000U
#define SDHCI_RESET_TIMEOUT_US  100000U
/* 发命令前等 CMD/DAT 忙位清除的超时上限。对照 u-boot drivers/mmc/sdhci.c
 * 的 sdhci_send_command()：它从 SDHCI_CMD_DEFAULT_TIMEOUT(100ms) 起步、超时
 * 翻倍重试直到 SDHCI_CMD_MAX_TIMEOUT(3200ms) 才放弃（本芯片参考驱动 bm_sd.c
 * 则干脆无限等）。带数据的命令在卡"编程忙"时等的就是这个 DAT 忙位，给足
 * 余量取 u-boot 放弃前的等待总量 3.2s，避免给太短误判超时；正常情况忙位
 * 很快清除，几乎不增加耗时。 */
#define SDHCI_INHIBIT_TIMEOUT_US 3200000U
#define SDHCI_CLK_STABLE_US     150U
#define SDHCI_PHY_RESET_US      20U
#define SDHCI_MAX_DIVIDER       256U

/* Error codes returned by internal helpers */
#define BM_OK                    0
#define BM_ERR_TIMEOUT          (-1)
#define BM_ERR_BADARG           (-2)
#define BM_ERR_HW               (-3)

/* -------------------------------------------------------------------------
 * Private device state
 * ------------------------------------------------------------------------- */

struct BM1684X_SDHCI_DEV {
    BM1684X_SDHCI_OSAL  osal;          /* copy of caller-supplied OSAL      */
    void               *base;          /* virtual base from osal.iomap()    */
    void               *topBase;       /* TOP register virtual base         */
    unsigned int        irqNum;
    unsigned int        devIndex;      /* 0=eMMC, 1=SD                      */
    unsigned int        is64Bit;
    unsigned int        clkInHz;       /* input clock from SoC              */
    unsigned int        useIrq;        /* 1 when interrupt mode is active   */
    void               *cmdSem;        /* semaphore for CMD_COMPLETE        */
    void               *xferSem;       /* semaphore for XFER_COMPLETE       */
    volatile unsigned int isrStatus;   /* raw INT_STATUS captured by ISR    */
    volatile unsigned int isrErrSts;   /* raw ERR_INT_STATUS captured       */
    /* static storage if osal.mem_alloc is NULL */
};

/* One statically allocated device for environments without a heap */
static struct BM1684X_SDHCI_DEV s_staticDev;

/* -------------------------------------------------------------------------
 * Internal helpers: interrupt / polling wait
 * ------------------------------------------------------------------------- */

static int useIntMode(const struct BM1684X_SDHCI_DEV *pDev)
{
    return (pDev->useIrq &&
            pDev->osal.sem_wait   != (void *)0 &&
            pDev->osal.sem_signal != (void *)0);
}

/*
 * Wait for INT_STATUS to have any bit from `mask` set.
 * Polling path: spins calling udelay(1) until the bit appears or timeout.
 * Clears the matched bits before returning.
 * Returns 0 and writes matched status into *pSts on success.
 */
static int pollWaitStatus(struct BM1684X_SDHCI_DEV *pDev,
                          unsigned int mask, unsigned int timeoutMs,
                          unsigned int *pSts)
{
    unsigned int elapsed = 0;
    unsigned int sts;
    unsigned int loops = timeoutMs * 1000U; /* 1 µs per loop */

    while (elapsed < loops) {
        sts = (unsigned int)REG_RD16(pDev->base, SDHCI_INT_STATUS) |
              ((unsigned int)REG_RD16(pDev->base, SDHCI_ERR_INT_STATUS) << 16);

        if (sts & SDHCI_INT_ERROR) {
            /* 【排障发现的诊断盲区，已修复】轮询模式下这里清完 INT_STATUS/
             * ERR_INT_STATUS 后直接返回 BM_ERR_HW，调用方 bm_sd_core.c 的
             * sdSendCmdDbg() 是在 bm1684xSdhciSendCmd() 返回之后才去读硬件
             * 寄存器打印诊断信息的——但那时寄存器已经被清空，且
             * bm1684xSdhciSendCmd() 在返回前还调用了 sdhciReset()，
             * PRESENT_STATE 也已经复位回空闲态。于是日志一直打印出
             * "int=0x0000 err=0x0000、state 是干净空闲态"，看起来跟 BM_ERR_HW
             * 矛盾，实际是清错在前、打印在后，把现场清掉了。这里仿照 ISR
             * 路径（isrStatus/isrErrSts 是 IRQ 模式才会填，轮询模式从未填过）
             * 把出错那一刻的原始值保存下来，调用方可以用新增的
             * bm1684xSdhciGetLastIntStatus()/GetLastErrStatus() 取到真实现场，
             * 不用再猜。 */
            pDev->isrStatus = sts;
            pDev->isrErrSts = (sts >> 16) & 0xFFFFU;

            REG_WR16(pDev->base, SDHCI_ERR_INT_STATUS,
                     REG_RD16(pDev->base, SDHCI_ERR_INT_STATUS));
            REG_WR16(pDev->base, SDHCI_INT_STATUS, SDHCI_INT_ERROR);
            if (pSts) *pSts = sts;
            return BM_ERR_HW;
        }
        if (sts & mask) {
            /* clear matched normal bits */
            REG_WR16(pDev->base, SDHCI_INT_STATUS,
                     (unsigned short)(sts & 0xFFFFU & mask));
            if (pSts) *pSts = sts;
            return BM_OK;
        }
        if (pDev->osal.udelay) pDev->osal.udelay(1);
        elapsed++;
    }
    return BM_ERR_TIMEOUT;
}

/*
 * Interrupt-mode wait: block on semaphore, then read isrStatus set by ISR.
 * Returns 0 on success, BM_ERR_TIMEOUT or BM_ERR_HW on failure.
 */
static int irqWaitStatus(struct BM1684X_SDHCI_DEV *pDev,
                         void *sem, unsigned int timeoutMs,
                         unsigned int *pSts)
{
    int rc = pDev->osal.sem_wait(sem, timeoutMs);
    if (rc != 0)
        return BM_ERR_TIMEOUT;

    if (pSts) *pSts = pDev->isrStatus;

    if (pDev->isrStatus & SDHCI_INT_ERROR)
        return BM_ERR_HW;

    return BM_OK;
}

/* -------------------------------------------------------------------------
 * Software reset helpers
 * ------------------------------------------------------------------------- */

static int sdhciReset(struct BM1684X_SDHCI_DEV *pDev, unsigned char mask)
{
    unsigned int i;

    REG_WR8(pDev->base, SDHCI_SOFTWARE_RESET, mask);
    for (i = 0; i < SDHCI_RESET_TIMEOUT_US; i++) {
        if ((REG_RD8(pDev->base, SDHCI_SOFTWARE_RESET) & mask) == 0)
            return BM_OK;
        if (pDev->osal.udelay) pDev->osal.udelay(1);
    }
    return BM_ERR_TIMEOUT;
}

/* -------------------------------------------------------------------------
 * PHY initialisation  (14-step sequence from bm_sd.c / sdhci-bitmain.c)
 * devIndex 0 = eMMC (INPSEL feedback path), 1 = SD (bypass path)
 * ------------------------------------------------------------------------- */

static void phyInit(struct BM1684X_SDHCI_DEV *pDev)
{
    unsigned int phyCnfg;

    /* 1. Assert PHY reset (bit 0 = 0) */
    phyCnfg = REG_RD32(pDev->base, SDHCI_P_PHY_CNFG);
    phyCnfg &= ~(1U << PHY_CNFG_PHY_RSTN);
    REG_WR32(pDev->base, SDHCI_P_PHY_CNFG, phyCnfg);

    /* 2. PAD slew: SP=0x9, SN=0x8 */
    phyCnfg = (0x9U << PHY_CNFG_PAD_SP) | (0x8U << PHY_CNFG_PAD_SN);
    REG_WR32(pDev->base, SDHCI_P_PHY_CNFG, phyCnfg);

    /* 3. CMD pad: RXSEL=2, WEAKPULL_EN=1, TXSLEW_CTRL_P=0x3, TXSLEW_CTRL_N=2
     * （数值已对照本仓库 trusted-firmware-a/drivers/bitmain/bm_sd.c 的
     * bm_sd_phy_init()/bm_emmc_phy_init() 改正——之前这里的 RXSEL=1、
     * TXSLEW=0xA/6 跟 TF-A 两个真实实现都不一致，疑似当年写错的固定值，
     * TF-A 对 eMMC/SD 这几个 pad 电气参数用的是同一组数值） */
    REG_WR16(pDev->base, SDHCI_P_CMDPAD_CNFG,
             (unsigned short)(
                 (2U << PAD_CNFG_RXSEL) |
                 (1U << PAD_CNFG_WEAKPULL_EN) |
                 (0x3U << PAD_CNFG_TXSLEW_CTRL_P) |
                 (2U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 4. DAT pad: same as CMD */
    REG_WR16(pDev->base, SDHCI_P_DATPAD_CNFG,
             (unsigned short)(
                 (2U << PAD_CNFG_RXSEL) |
                 (1U << PAD_CNFG_WEAKPULL_EN) |
                 (0x3U << PAD_CNFG_TXSLEW_CTRL_P) |
                 (2U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 5. CLK pad: RXSEL=2, WEAKPULL_EN=0, TXSLEW_CTRL_P=0x3, TXSLEW_CTRL_N=2 */
    REG_WR16(pDev->base, SDHCI_P_CLKPAD_CNFG,
             (unsigned short)(
                 (2U << PAD_CNFG_RXSEL) |
                 (0x3U << PAD_CNFG_TXSLEW_CTRL_P) |
                 (2U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 6. STB pad: RXSEL=2, WEAKPULL_EN=2 (pull-down), same slew */
    REG_WR16(pDev->base, SDHCI_P_STBPAD_CNFG,
             (unsigned short)(
                 (2U << PAD_CNFG_RXSEL) |
                 (2U << PAD_CNFG_WEAKPULL_EN) |
                 (0x3U << PAD_CNFG_TXSLEW_CTRL_P) |
                 (2U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 7. RST_N pad: RXSEL=2, WEAKPULL_EN=1, same slew */
    REG_WR16(pDev->base, SDHCI_P_RSTNPAD_CNFG,
             (unsigned short)(
                 (2U << PAD_CNFG_RXSEL) |
                 (1U << PAD_CNFG_WEAKPULL_EN) |
                 (0x3U << PAD_CNFG_TXSLEW_CTRL_P) |
                 (2U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 8. COMMDL: bypass disabled（TF-A 没有动这个寄存器，保持原样不算冲突） */
    REG_WR8(pDev->base, SDHCI_P_COMMDL_CNFG, 0);

    /* 9. SDCLKDL：改成 TF-A 的"固定延迟"模式（EXTDLY_EN），不是旁路。
     * 之前这里用 BYPASS_EN，跟 TF-A eMMC/SD 两条路径都不一致。 */
    REG_WR8(pDev->base, SDHCI_P_SDCLKDL_CNFG,
            (unsigned char)(1U << SDCLKDL_EXTDLY_EN));
    REG_WR8(pDev->base, SDHCI_P_SDCLKDL_DC, SDCLKDL_DC_DEFAULT);

    /* 10. SMPLDL: eMMC 用 INPSEL=0x2，SD 用 BYPASS——这条原来就跟 TF-A 一致，未改 */
    if (pDev->devIndex == BM1684X_EMMC_INDEX)
        REG_WR8(pDev->base, SDHCI_P_SMPLDL_CNFG,
                (unsigned char)(0x2U << SMPLDL_INPSEL_CNFG));
    else
        REG_WR8(pDev->base, SDHCI_P_SMPLDL_CNFG,
                (unsigned char)(1U << SMPLDL_BYPASS_EN));

    /* 11. ATDL：改成 TF-A 的 INPSEL=2，不是旁路（eMMC/SD 在 TF-A 里这条也是
     * 同一个值，之前这里用 BYPASS_EN 跟两条路径都不一致）。 */
    REG_WR8(pDev->base, SDHCI_P_ATDL_CNFG,
            (unsigned char)(2U << ATDL_INPSEL_CNFG));

    /* 12. Release PHY reset */
    phyCnfg = REG_RD32(pDev->base, SDHCI_P_PHY_CNFG);
    phyCnfg |= (1U << PHY_CNFG_PHY_RSTN);
    REG_WR32(pDev->base, SDHCI_P_PHY_CNFG, phyCnfg);

    /* 13. Short delay for PHY to stabilise */
    if (pDev->osal.udelay) pDev->osal.udelay(SDHCI_PHY_RESET_US);

    /* 14. Wait for PHY power-good */
    {
        unsigned int i;
        for (i = 0; i < 1000U; i++) {
            if (REG_RD32(pDev->base, SDHCI_P_PHY_CNFG) &
                (1U << PHY_CNFG_PHY_PWRGOOD))
                break;
            if (pDev->osal.udelay) pDev->osal.udelay(1);
        }
    }
}

/* -------------------------------------------------------------------------
 * Host controller hardware initialisation
 * ------------------------------------------------------------------------- */

static int hwInit(struct BM1684X_SDHCI_DEV *pDev)
{
    unsigned short hc2;
    int rc;

    /* Full software reset */
    rc = sdhciReset(pDev, SDHCI_RESET_ALL);
    if (rc != BM_OK) return rc;

    /* 64位 DMA 寻址：照参考驱动 bm_sd_hw_init()——读控制器能力寄存器
     * CAPABILITIES1(0x40) 的 bit27（V4 模式 64 位系统地址支持），硬件支持就
     * 打开 HOST_CONTROL2 的 64BIT_ADDR(bit13)，并把 is64Bit 标志置上让发命令
     * 时一并写 ADMA_SA_HIGH。
     * 【这是上板写失败的真正根因】本板 DRAM 在 4GB 以上（日志里 Ramdisk、
     * selftest 缓冲区地址都是 0x3_xxxx_xxxx），DMA 缓冲区物理地址高 32 位非 0。
     * 原来写死 BM_SD_USE_64BIT_DMA=0、从不开 64 位寻址，控制器只认地址低 32
     * 位 → DMA 打到被截断的错误地址（0x3_08af_5200 → 0x08af_5200）→ 数据
     * 阶段永远等不到完成而超时；而 CMD24 命令本身只走命令线、不碰 DMA，所以
     * 命令能过、卡回正常 R1，唯独数据搬运卡死——正是上板现象。改成按硬件能力
     * 自动开启，不再依赖那个写死的配置开关（eMMC 路径同样受益）。 */
    if (REG_RD32(pDev->base, SDHCI_CAPABILITIES) & SDHCI_CAP2_SYS_ADDR_64)
        pDev->is64Bit = 1U;

    /* Set version-4 mode, optionally 64-bit addressing, CMD23 support */
    hc2 = (unsigned short)(SDHCI_HC2_VER4_ENABLE | SDHCI_HC2_CMD23_SUPPORT);
    if (pDev->is64Bit)
        hc2 |= (unsigned short)SDHCI_HC2_64BIT_ADDR;
    REG_WR16(pDev->base, SDHCI_HOST_CONTROL2, hc2);

    /* Power on: 3.3 V */
    REG_WR8(pDev->base, SDHCI_POWER_CONTROL,
            (unsigned char)(SDHCI_POWER_ON | SDHCI_POWER_330));

    /* 数据超时门限：取寄存器允许的最大值 0x0E（TMCLK 下最长的超时周期数），
     * 跟参考驱动 bm_sd.c（每次发数据命令前都写 0x0E）、以及本仓库现成的
     * vxbBm1684xSdhci.c（VxWorks 版，bm1684xSdhciHwInit() 里同样写 0x0E）
     * 三处一致。之前这里完全没碰这个寄存器，复位默认值未知，万一默认值偏小，
     * 数据阶段稍有延迟控制器就会先报"Data Timeout Error"——取最大值排除这个
     * 干扰，不会有副作用（只是把硬件自己的超时检测放宽，host 侧轮询仍有自己
     * 的超时保护）。 */
    REG_WR8(pDev->base, SDHCI_TIMEOUT_CONTROL, 0x0EU);

    /* Enable all normal + error status bits (masked from signalling) */
    REG_WR16(pDev->base, SDHCI_INT_STATUS_EN,
             (unsigned short)(SDHCI_INT_ALL_NORMAL));
    REG_WR16(pDev->base, SDHCI_ERR_INT_STATUS_EN,
             (unsigned short)(SDHCI_INT_ALL_ERROR));
    /* Signals disabled until a transfer is in flight */
    REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN, 0);

    /* DMA mode: SDMA (simple, no descriptor ring needed at this layer) */
    {
        unsigned char hc1 = REG_RD8(pDev->base, SDHCI_HOST_CONTROL);
        hc1 = (unsigned char)((hc1 & ~(unsigned char)SDHCI_CTRL_DMA_MASK) |
                              (unsigned char)SDHCI_CTRL_SDMA);
        REG_WR8(pDev->base, SDHCI_HOST_CONTROL, hc1);
    }

    /* PHY */
    phyInit(pDev);

    return BM_OK;
}

/* -------------------------------------------------------------------------
 * Clock control
 * ------------------------------------------------------------------------- */

int bm1684xSdhciSetClk(BM1684X_SDHCI_DEV *pDev, unsigned int clkHz)
{
    unsigned int div;
    unsigned short clkCtrl;
    unsigned int i;

    if (!pDev || clkHz == 0)
        return BM_ERR_BADARG;

    /* Stop clock */
    REG_WR16(pDev->base, SDHCI_CLOCK_CONTROL, 0);

    if (clkHz >= pDev->clkInHz) {
        div = 1U;
    } else {
        for (div = 2U; div <= SDHCI_MAX_DIVIDER * 2U; div += 2U) {
            if ((pDev->clkInHz / div) <= clkHz)
                break;
        }
    }

    /* SD Host spec: divider field = div/2 (0 means /1) */
    {
        unsigned int divField = (div > 1U) ? (div >> 1U) : 0U;
        clkCtrl = (unsigned short)(
            SDHCI_CLK_INT_EN |
            ((divField & SDHCI_CLK_DIV_MASK) << SDHCI_CLK_DIV_SHIFT));
    }
    REG_WR16(pDev->base, SDHCI_CLOCK_CONTROL, clkCtrl);

    /* Wait for internal clock stable */
    for (i = 0; i < 150U; i++) {
        if (REG_RD16(pDev->base, SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INT_STABLE)
            break;
        if (pDev->osal.udelay) pDev->osal.udelay(1);
    }

    /* Enable SD clock to card */
    clkCtrl |= (unsigned short)SDHCI_CLK_CARD_EN;
    REG_WR16(pDev->base, SDHCI_CLOCK_CONTROL, clkCtrl);

    if (pDev->osal.udelay) pDev->osal.udelay(SDHCI_CLK_STABLE_US);
    return BM_OK;
}

/* -------------------------------------------------------------------------
 * Bus width
 * ------------------------------------------------------------------------- */

int bm1684xSdhciSetBusWidth(BM1684X_SDHCI_DEV *pDev, unsigned int width)
{
    unsigned char hc1;

    if (!pDev) return BM_ERR_BADARG;

    hc1 = REG_RD8(pDev->base, SDHCI_HOST_CONTROL);
    hc1 &= (unsigned char)~(SDHCI_CTRL_4BITBUS | SDHCI_CTRL_8BITBUS);

    if (width == 4U)
        hc1 |= (unsigned char)SDHCI_CTRL_4BITBUS;
    else if (width == 8U)
        hc1 |= (unsigned char)SDHCI_CTRL_8BITBUS;
    /* width == 1: both bits clear */

    REG_WR8(pDev->base, SDHCI_HOST_CONTROL, hc1);
    return BM_OK;
}

/* -------------------------------------------------------------------------
 * Build command register word from BM1684X_MMC_CMD descriptor
 * ------------------------------------------------------------------------- */

static unsigned short buildCmdFlags(const BM1684X_MMC_CMD *pCmd,
                                    const BM1684X_MMC_DATA *pData)
{
    unsigned short flags = 0;

    switch (pCmd->respType) {
    case BM1684X_RESP_NONE:
        flags = SDHCI_CMD_RESP_NONE;
        break;
    case BM1684X_RESP_R2:
        flags = SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC;
        break;
    case BM1684X_RESP_R3:
    case BM1684X_RESP_R4:
        flags = SDHCI_CMD_RESP_SHORT;
        break;
    case BM1684X_RESP_R1B:
        flags = SDHCI_CMD_RESP_SHORT_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX_CHK;
        break;
    default:
        flags = SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX_CHK;
        break;
    }

    if (pData)
        flags |= SDHCI_CMD_DATA;

    return SDHCI_MAKE_CMD(pCmd->cmdIdx, flags);
}

/* -------------------------------------------------------------------------
 * Data transfer setup (SDMA)
 * ------------------------------------------------------------------------- */

static unsigned short buildXferMode(const BM1684X_MMC_DATA *pData)
{
    /* 传输模式口径对照【本芯片专用、已跑通】的参考驱动 bm_sd.c
     * （bm_sd_send_cmd_with_data 第 88~99 行）：它对读(CMD17/18)和写(CMD24/25)
     * 【一律带上 TRNS_MULTI】，连单块也带，靠 BLK_CNT_EN + 块数寄存器=1 来界定
     * 只传一块；并且【从不用 AUTO_CMD12】，多块传输由上层在传完后显式发 CMD12。
     * 注意：这一点与 u-boot 通用 sdhci.c 不一致（u-boot 只在块数>1 时才置
     * MULTI）。本项目其余细节遵循"以 u-boot 为准"，但这条是 BM1684X 这颗
     * Synopsys/比特大陆控制器的硬件特性：单块模式(MULTI=0)下写命令做完数据后，
     * 卡进入"编程忙"，控制器的 DAT 忙位疑似清不干净——表现为单块写之后 DAT
     * 一直占用，紧跟的命令(CMD13/CMD17)发不出去而超时。参考驱动用"一律 MULTI"
     * 绕开了这个单块模式的坑。这里照搬参考驱动，单块也置 MULTI。
     *
     * 【AUTO_CMD12 已去掉，这是多块写失败的根因】之前自作主张在多块时加了
     * AUTO_CMD12（想省一条显式 CMD12），结果单块写(CMD24，无 AUTO_CMD12)能过、
     * 多块写(CMD25，带 AUTO_CMD12)一上来就在命令阶段报错——上板日志实锤：
     * selftest 单块自检通过、FAT 格式化的 CMD25 多块写全程失败。本颗控制器在
     * V4 模式下 AUTO_CMD12 有问题，参考驱动从不用它。现严格照参考驱动：传输模式
     * 永不置 AUTO_CMD12，多块传输由 bm_sd_core.c 在传完后显式发 CMD12 收尾。 */
    unsigned short mode = SDHCI_TRNS_DMA | SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_MULTI;

    if (pData->flags & BM1684X_DATA_READ)
        mode |= SDHCI_TRNS_READ;

    return mode;
}

/* forward declaration so IsrWrapper can reference it */
void bm1684xSdhciIsr(BM1684X_SDHCI_DEV *pDev);

/* -------------------------------------------------------------------------
 * ISR — call from your RTOS ISR when the SDHCI interrupt fires
 * ------------------------------------------------------------------------- */

static void bm1684xSdhciIsrWrapper(void *arg)
{
    bm1684xSdhciIsr((BM1684X_SDHCI_DEV *)arg);
}

void bm1684xSdhciIsr(BM1684X_SDHCI_DEV *pDev)
{
    unsigned int sts;
    unsigned int errSts;

    if (!pDev || !pDev->useIrq) return;

    sts    = (unsigned int)REG_RD16(pDev->base, SDHCI_INT_STATUS);
    errSts = (unsigned int)REG_RD16(pDev->base, SDHCI_ERR_INT_STATUS);

    /* Clear what we read (write-1-to-clear) */
    REG_WR16(pDev->base, SDHCI_ERR_INT_STATUS, (unsigned short)errSts);
    REG_WR16(pDev->base, SDHCI_INT_STATUS, (unsigned short)sts);

    /* Mask signal enables so we don't re-enter while processing */
    REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN, 0);

    pDev->isrStatus = sts | (errSts ? (unsigned int)SDHCI_INT_ERROR : 0U);
    pDev->isrErrSts = errSts;

    /* SDMA boundary: reload DMA address to resume scatter。
     * V4模式下真正的地址寄存器是 ADMA_SA_LOW（见 bm1684xSdhciSendCmd() 里
     * 的同一处根因说明），SDHCI_DMA_ADDRESS 此时已被复用成块计数寄存器，
     * 不能再读它当地址重新写回去。 */
    if (sts & SDHCI_INT_DMA_END) {
        unsigned short hc2reg = REG_RD16(pDev->base, SDHCI_HOST_CONTROL2);
        if (hc2reg & SDHCI_HC2_VER4_ENABLE) {
            unsigned int sa = REG_RD32(pDev->base, SDHCI_ADMA_SA_LOW);
            REG_WR32(pDev->base, SDHCI_ADMA_SA_LOW, sa);
        } else {
            unsigned int sa = REG_RD32(pDev->base, SDHCI_DMA_ADDRESS);
            REG_WR32(pDev->base, SDHCI_DMA_ADDRESS, sa);
        }
    }

    /* Signal waiting thread */
    if ((sts & SDHCI_INT_CMD_COMPLETE) && pDev->cmdSem &&
        pDev->osal.sem_signal)
        pDev->osal.sem_signal(pDev->cmdSem);

    if ((sts & (SDHCI_INT_XFER_COMPLETE | SDHCI_INT_ERROR)) &&
        pDev->xferSem && pDev->osal.sem_signal)
        pDev->osal.sem_signal(pDev->xferSem);
}

/* -------------------------------------------------------------------------
 * Core send-command implementation
 * ------------------------------------------------------------------------- */

int bm1684xSdhciSendCmd(BM1684X_SDHCI_DEV *pDev,
                        BM1684X_MMC_CMD   *pCmd,
                        BM1684X_MMC_DATA  *pData)
{
    unsigned short cmdReg;
    unsigned short xferMode = 0;
    unsigned int   waitMask;
    unsigned int   sts = 0;
    int            rc;
    unsigned int   inhibitMask;

    if (!pDev || !pCmd) return BM_ERR_BADARG;

    /* 之前这里的注释判断是错的：上次只看了参考驱动 bm_sd_send_cmd_without_data()
     * 里第一段 "mask = SDHCI_CMD_INHIBIT" 的等待循环，漏看了同一个函数后面单独
     * 还有一段（第288~294行）：算完 cmd flags 之后，只要 flags != RESP_NONE
     * （也就是除 CMD0 外的所有命令，CMD13 也在内），还会【再等一次 DAT_INHIBIT
     * 清除】。也就是说参考驱动的真实规则是"除 CMD0 外所有命令都要等 DAT 忙位
     * 清除"，不是"只有带数据/R1B忙响应的命令才等"——之前那条"已改回"的结论是
     * 误读，这里改成跟参考驱动两段等待加起来的真实效果一致：除 CMD0 外都等
     * CMD_INHIBIT|DAT_INHIBIT。 */
    inhibitMask = SDHCI_STATE_CMD_INHIBIT;
    if (pCmd->respType != BM1684X_RESP_NONE)
        inhibitMask |= SDHCI_STATE_DAT_INHIBIT;

    /* 发命令前清掉上一条命令残留的中断状态位：参考驱动 bm_sd_send_cmd_with_data()/
     * bm_sd_send_cmd_without_data() 都是在函数最开头、等忙位之前就清
     * INT_STATUS/ERR_INT_STATUS=0xFFFF；之前我们把这一步放在等完忙位之后，
     * 顺序跟参考驱动不一致，这里挪到等待忙位循环之前，跟参考驱动顺序对齐。 */
    REG_WR16(pDev->base, SDHCI_INT_STATUS, 0xFFFFU);
    REG_WR16(pDev->base, SDHCI_ERR_INT_STATUS, 0xFFFFU);

    {
        unsigned int i;
        for (i = 0; i < SDHCI_INHIBIT_TIMEOUT_US; i++) {
            if ((REG_RD32(pDev->base, SDHCI_PRESENT_STATE) & inhibitMask) == 0)
                break;
            if (pDev->osal.udelay) pDev->osal.udelay(1);
        }
        if (REG_RD32(pDev->base, SDHCI_PRESENT_STATE) & inhibitMask)
            return BM_ERR_TIMEOUT;
    }

    /* 参考驱动每条命令都重写一次超时门限寄存器（不止初始化时写一次）：
     * sdhciReset() 在命令/数据阶段超时的错误路径里会被调用，复位范围
     * 可能影响这个寄存器，跟参考驱动一样每条命令都重写一次更保险。 */
    REG_WR8(pDev->base, SDHCI_TIMEOUT_CONTROL, 0x0EU);

    /* Program data registers before writing the command */
    if (pData) {
        unsigned long  dmaAddr = (unsigned long)(unsigned long long)(unsigned long)pData->buf;
        unsigned short hc2reg  = REG_RD16(pDev->base, SDHCI_HOST_CONTROL2);

        REG_WR16(pDev->base, SDHCI_BLOCK_SIZE,
                 (unsigned short)SDHCI_MAKE_BLKSZ(7, pData->blkSize));

        /* 根因排查结论（对照本芯片已跑通的参考驱动 bm_sd_prepare()）：
         * hwInit() 里始终无条件给 HOST_CONTROL2 置位 HC2_VER4_ENABLE。SDHCI
         * 规范规定一旦开了这个位，原来在偏移 0x00 的 legacy "SDMA 系统地址"
         * 寄存器（即 SDHCI_DMA_ADDRESS）就被复用成"32位块计数"寄存器，真正
         * 的 SDMA 地址要改写到 ADMA_SA_LOW/HIGH（偏移0x58/0x5C）——这正是
         * CMD24 写命令本身成功、但数据阶段卡死（DAT 忙位再也不清）的根因：
         * 之前一直按"未开V4"的旧布局写，把真实缓冲区地址写进了被复用成块
         * 计数的寄存器（解释成块数是个天文数字），而控制器真正用来做 DMA
         * 的 ADMA_SA_LOW 从未写过、是脏值。这里按 V4 位分支，跟参考驱动
         * bm_sd_prepare() 的写法保持一致。 */
        if (hc2reg & SDHCI_HC2_VER4_ENABLE) {
            REG_WR32(pDev->base, SDHCI_ADMA_SA_LOW,
                     (unsigned int)(dmaAddr & 0xFFFFFFFFUL));
            REG_WR32(pDev->base, SDHCI_ADMA_SA_HIGH,
                     (unsigned int)((dmaAddr >> 32) & 0xFFFFFFFFUL));
            REG_WR32(pDev->base, SDHCI_DMA_ADDRESS, pData->blkCount);
            REG_WR16(pDev->base, SDHCI_BLOCK_COUNT, 0);
        } else {
            REG_WR16(pDev->base, SDHCI_BLOCK_COUNT,
                     (unsigned short)(pData->blkCount & 0xFFFFU));
            REG_WR32(pDev->base, SDHCI_DMA_ADDRESS,
                     (unsigned int)(dmaAddr & 0xFFFFFFFFUL));
            if (pDev->is64Bit)
                REG_WR32(pDev->base, SDHCI_ADMA_SA_HIGH,
                         (unsigned int)((dmaAddr >> 32) & 0xFFFFFFFFUL));
        }

        /* 每次传输前重新选一次 SDMA 模式：对齐参考驱动 bm_sd_prepare()
         * 末尾的 HOST_CONTROL DMA 选择（它每次传输都做）。hwInit() 里虽然
         * 已经选过一次、SetBusWidth 也只改总线位宽位不动 DMA 选择位，稳态
         * 下本来就是 SDMA；但命令/数据阶段超时的错误路径里调过
         * sdhciReset(CMD|DATA)，保险起见每次传输前按参考驱动重选一遍，只读
         * 改写 DMA 选择位[4:3]，不动总线位宽位。 */
        {
            unsigned char hc1 = REG_RD8(pDev->base, SDHCI_HOST_CONTROL);
            hc1 = (unsigned char)((hc1 & ~(unsigned char)SDHCI_CTRL_DMA_MASK) |
                                  (unsigned char)SDHCI_CTRL_SDMA);
            REG_WR8(pDev->base, SDHCI_HOST_CONTROL, hc1);
        }

        xferMode = buildXferMode(pData);
    }

    REG_WR32(pDev->base, SDHCI_ARGUMENT, pCmd->cmdArg);
    cmdReg = buildCmdFlags(pCmd, pData);

    /* -----------------------------------------------------------------------
     * INTERRUPT MODE: enable signals, write command, wait on semaphore
     * --------------------------------------------------------------------- */
    if (useIntMode(pDev)) {
        pDev->isrStatus = 0;
        pDev->isrErrSts = 0;

        /* Enable CMD_COMPLETE interrupt signal */
        REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN,
                 (unsigned short)SDHCI_INT_CORE_MASK);

        /* Write transfer mode + command (32-bit combined write) */
        REG_WR16(pDev->base, SDHCI_TRANSFER_MODE, xferMode);
        REG_WR16(pDev->base, SDHCI_COMMAND, cmdReg);

        /* Wait for CMD_COMPLETE */
        rc = irqWaitStatus(pDev, pDev->cmdSem, SDHCI_CMD_TIMEOUT_MS, &sts);
        if (rc != BM_OK) {
            sdhciReset(pDev, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
            return rc;
        }

        /* Read back response */
        if (pCmd->respType == BM1684X_RESP_R2) {
            pCmd->resp[0] = REG_RD32(pDev->base, SDHCI_RESPONSE_0);
            pCmd->resp[1] = REG_RD32(pDev->base, SDHCI_RESPONSE_1);
            pCmd->resp[2] = REG_RD32(pDev->base, SDHCI_RESPONSE_2);
            pCmd->resp[3] = REG_RD32(pDev->base, SDHCI_RESPONSE_3);
        } else if (pCmd->respType != BM1684X_RESP_NONE) {
            pCmd->resp[0] = REG_RD32(pDev->base, SDHCI_RESPONSE_0);
        }

        if (!pData) {
            REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN, 0);
            return BM_OK;
        }

        /* Re-arm for XFER_COMPLETE */
        pDev->isrStatus = 0;
        pDev->isrErrSts = 0;
        REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN,
                 (unsigned short)SDHCI_INT_CORE_MASK);

        rc = irqWaitStatus(pDev, pDev->xferSem, SDHCI_XFER_TIMEOUT_MS, &sts);
        REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN, 0);
        if (rc != BM_OK) {
            sdhciReset(pDev, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
            return rc;
        }
        return BM_OK;
    }

    /* -----------------------------------------------------------------------
     * POLLING MODE: write command, spin on INT_STATUS
     * --------------------------------------------------------------------- */
    REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN, 0);   /* no interrupt signals */
    REG_WR16(pDev->base, SDHCI_TRANSFER_MODE, xferMode);
    REG_WR16(pDev->base, SDHCI_COMMAND, cmdReg);

    /* Wait CMD_COMPLETE */
    waitMask = SDHCI_INT_CMD_COMPLETE;
    rc = pollWaitStatus(pDev, waitMask, SDHCI_CMD_TIMEOUT_MS, &sts);
    if (rc != BM_OK) {
        sdhciReset(pDev, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
        return rc;
    }

    if (pCmd->respType == BM1684X_RESP_R2) {
        pCmd->resp[0] = REG_RD32(pDev->base, SDHCI_RESPONSE_0);
        pCmd->resp[1] = REG_RD32(pDev->base, SDHCI_RESPONSE_1);
        pCmd->resp[2] = REG_RD32(pDev->base, SDHCI_RESPONSE_2);
        pCmd->resp[3] = REG_RD32(pDev->base, SDHCI_RESPONSE_3);
    } else if (pCmd->respType != BM1684X_RESP_NONE) {
        pCmd->resp[0] = REG_RD32(pDev->base, SDHCI_RESPONSE_0);
    }

    if (!pData) return BM_OK;

    /* Poll XFER_COMPLETE (handling SDMA DMA_END boundary re-loads) */
    for (;;) {
        rc = pollWaitStatus(pDev,
                            SDHCI_INT_XFER_COMPLETE | SDHCI_INT_DMA_END,
                            SDHCI_XFER_TIMEOUT_MS, &sts);
        if (rc != BM_OK) {
            sdhciReset(pDev, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
            return rc;
        }
        if (sts & SDHCI_INT_DMA_END) {
            /* SDMA 512 KB boundary: reload address to continue。
             * V4模式下真正地址在 ADMA_SA_LOW，原因同上（bm1684xSdhciSendCmd()
             * 里的根因说明）。 */
            unsigned short hc2reg = REG_RD16(pDev->base, SDHCI_HOST_CONTROL2);
            if (hc2reg & SDHCI_HC2_VER4_ENABLE) {
                unsigned int sa = REG_RD32(pDev->base, SDHCI_ADMA_SA_LOW);
                REG_WR32(pDev->base, SDHCI_ADMA_SA_LOW, sa);
            } else {
                unsigned int sa = REG_RD32(pDev->base, SDHCI_DMA_ADDRESS);
                REG_WR32(pDev->base, SDHCI_DMA_ADDRESS, sa);
            }
        }
        if (sts & SDHCI_INT_XFER_COMPLETE)
            break;
    }

    return BM_OK;
}

/* -------------------------------------------------------------------------
 * Card detect
 * ------------------------------------------------------------------------- */

int bm1684xSdhciCardPresent(BM1684X_SDHCI_DEV *pDev)
{
    if (!pDev) return 0;
    /* eMMC is always present */
    if (pDev->devIndex == BM1684X_EMMC_INDEX) return 1;
    return (REG_RD32(pDev->base, SDHCI_PRESENT_STATE) &
            SDHCI_STATE_CARD_PRESENT) ? 1 : 0;
}

/*--------------------------------------------------------------------------
 * 取最近一次 bm1684xSdhciSendCmd() 失败时刻、清空前捕获到的原始
 * INT_STATUS|（ERR_INT_STATUS<<16）/ERR_INT_STATUS 值。轮询模式下由
 * pollWaitStatus() 在 SDHCI_INT_ERROR 出现的那一刻保存；中断模式下由
 * bm1684xSdhciIsr() 保存。调用方（bm_sd_core.c 的诊断打印）应改用这两个
 * 接口而不是事后再读硬件寄存器——事后寄存器已经被清空、控制器也可能已被
 * sdhciReset() 复位回空闲态，读到的是假的"无错误"现场。
 *------------------------------------------------------------------------*/
unsigned int bm1684xSdhciGetLastIntStatus(BM1684X_SDHCI_DEV *pDev)
{
    return pDev ? pDev->isrStatus : 0U;
}

unsigned int bm1684xSdhciGetLastErrStatus(BM1684X_SDHCI_DEV *pDev)
{
    return pDev ? pDev->isrErrSts : 0U;
}

/* -------------------------------------------------------------------------
 * Input clock query (reads MODE_SEL from TOP registers)
 * ------------------------------------------------------------------------- */

static unsigned int getInputClk(struct BM1684X_SDHCI_DEV *pDev)
{
    unsigned int modeSel;

    if (!pDev->topBase) return BM1684X_EMMC_CLK_NORMAL_HZ;

    modeSel = REG_RD32(pDev->topBase, BM1684X_TOP_CONF_INFO) & 0x7U;

    switch (modeSel) {
    case BM1684X_MODE_BYPASS: return BM1684X_EMMC_CLK_BYPASS_HZ;
    default:                  return BM1684X_EMMC_CLK_NORMAL_HZ;
    }
}

/* -------------------------------------------------------------------------
 * Public initialisation
 * ------------------------------------------------------------------------- */

BM1684X_SDHCI_DEV *bm1684xSdhciInit(
    const BM1684X_SDHCI_OSAL *pOsal,
    unsigned long              regPhysBase,
    unsigned int               irqNum,
    unsigned int               devIndex,
    unsigned int               is64BitAddr)
{
    struct BM1684X_SDHCI_DEV *pDev;
    int useIrq;

    /* Allocate device struct */
    if (pOsal && pOsal->mem_alloc) {
        pDev = (struct BM1684X_SDHCI_DEV *)pOsal->mem_alloc(
                   (unsigned int)sizeof(*pDev));
        if (!pDev) return (BM1684X_SDHCI_DEV *)0;
        /* zero-init */
        {
            unsigned char *p = (unsigned char *)pDev;
            unsigned int   n = (unsigned int)sizeof(*pDev);
            while (n--) *p++ = 0;
        }
    } else {
        /* use static storage, zero it */
        pDev = &s_staticDev;
        {
            unsigned char *p = (unsigned char *)pDev;
            unsigned int   n = (unsigned int)sizeof(*pDev);
            while (n--) *p++ = 0;
        }
    }

    /* Copy OSAL */
    if (pOsal) pDev->osal = *pOsal;

    pDev->irqNum   = irqNum;
    pDev->devIndex = devIndex;
    pDev->is64Bit  = is64BitAddr;

    /* Map SDHCI registers */
    if (pDev->osal.iomap) {
        pDev->base = pDev->osal.iomap(regPhysBase, 0x1000U);
    } else {
        /* flat-mapped: cast physical address directly */
        pDev->base = (void *)(unsigned long)regPhysBase;
    }
    if (!pDev->base) goto fail;

    /* Map TOP registers (optional: used for clock detection) */
    if (pDev->osal.iomap) {
        pDev->topBase = pDev->osal.iomap(BM1684X_TOP_PHYS_BASE, 0x1000U);
    } else {
        pDev->topBase = (void *)(unsigned long)BM1684X_TOP_PHYS_BASE;
    }

    pDev->clkInHz = getInputClk(pDev);

    /* Decide interrupt vs polling mode */
    useIrq = (pDev->osal.sem_create  != (void *)0 &&
               pDev->osal.sem_wait   != (void *)0 &&
               pDev->osal.sem_signal != (void *)0 &&
               pDev->osal.irq_connect!= (void *)0 &&
               pDev->osal.irq_enable != (void *)0);

    if (useIrq) {
        pDev->cmdSem  = pDev->osal.sem_create();
        pDev->xferSem = pDev->osal.sem_create();
        if (!pDev->cmdSem || !pDev->xferSem) {
            useIrq = 0;
        } else {
            if (pDev->osal.irq_connect(irqNum, bm1684xSdhciIsrWrapper, pDev) == 0 &&
                pDev->osal.irq_enable(irqNum) == 0) {
                pDev->useIrq = 1U;
            } else {
                useIrq = 0;
            }
        }
    }
    (void)useIrq; /* pDev->useIrq already set (or stays 0 = polling) */

    /* Hardware + PHY init */
    if (hwInit(pDev) != BM_OK) goto fail;

    /* Set identification clock */
    bm1684xSdhciSetClk(pDev, BM1684X_EMMC_CLK_INIT_HZ);

    return (BM1684X_SDHCI_DEV *)pDev;

fail:
    if (pDev != &s_staticDev && pDev->osal.mem_free)
        pDev->osal.mem_free(pDev);
    return (BM1684X_SDHCI_DEV *)0;
}
