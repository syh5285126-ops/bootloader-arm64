/******************************************************************************
 * bm_emmc_core.c
 *
 * BM1684X eMMC 协议层：卡上电识别、总线/速度配置、按扇区读写、容量识别。
 * 命令时序遵循 JEDEC eMMC 规范 + SD Host Controller 标准，类比 rk3588 项目的
 * rk_emmc_core.c。
 *
 * 与 rk3588 的差异：
 *   - rk3588 的 SDHCI/PHY 已由 U-Boot 配好，驱动不碰；本项目引导不经过 eMMC，
 *     PHY/时钟初始化由 bm1684xSdhciInit() 内部完成（14 步 PHY 时序，见
 *     bm1684xSdhci.c 的 phyInit()），本文件调用一次即完成，无需重复实现。
 *     但 bm1684xSdhciInit() 只管控制器内部寄存器（0x50100000 这一段），不管
 *     TOP 域时钟使能/软复位（0x50010000+0x800/+0xC00）——这一步在"eMMC是引导
 *     介质"的场景下由 BootROM 替 TF-A/U-Boot 做了，TF-A/U-Boot 代码里也确实
 *     找不到显式调用；本项目场景不同（不从 eMMC 引导），所以本文件新增
 *     bmTopDomainInit() 主动补上这一步，位定义已与 TF-A bm_clock.h /
 *     platform_def.h / U-Boot reset-bitmain.c 三处交叉核对一致。
 *   - rk3588 走 PIO；本项目走 DMA（SDMA），因此读写卡前后要做 cache 维护：
 *     写卡前 flush 发送缓冲区，读卡完成后 invalidate 接收缓冲区。
 ******************************************************************************/
#include "bm_emmc_glue_cfg.h"
#ifdef BM1684X_EMMC

#include <string.h>
#include <stdio.h>
#include "bm_emmc.h"
#include "bm1684xSdhciOsal.h"
#include "bm1684xSdhciHw.h"
#include "bm_emmc_osal_os3.h"

/*--------------------------------------------------------------------------
 * 内存屏障（dsb）
 *
 * 背景：U-Boot 在 ARM64 上每次寄存器读写（writel/readl）都自带 dmb 屏障，而本
 * 项目复用的引擎层 bm1684xSdhci.c 用的是纯 volatile 访问（REG_RD/WR_*），没有
 * 任何屏障——它隐含假设 MMIO 区映射成 Device 内存（Device 访问之间 CPU 本就保序），
 * 这在原 VxWorks 平台上是成立的。
 *
 * 本文件在两类边界上仍然显式补 dsb，不依赖"区一定是 Device 映射"这个隐含前提：
 *   1) TOP 域时钟/复位这种"写完必须真正生效、再等固定时间"的时序点——若写还压在
 *      写缓冲里就开始计时，复位恢复窗口会被缩短；
 *   2) DMA 缓冲区(普通可缓存内存) 与 控制器寄存器(Device内存) 之间的跨域顺序——
 *      这是 DMA 最经典的出错点，必须保证"数据/cache 操作先落地，再启动 DMA"。
 *
 * 用编译器内联汇编实现，避免依赖天脉3 SDK 头文件；非 ARM 平台退化为编译屏障。
 *------------------------------------------------------------------------*/
#if defined(__aarch64__) || defined(__arm__)
#define BM_DSB()   __asm__ __volatile__ ("dsb sy" ::: "memory")
#else
#define BM_DSB()   __asm__ __volatile__ (""        ::: "memory")
#endif

/* 天脉3 平台提供的 DMA 缓冲区 cache 维护接口（已向用户核实真实签名为
 * ACoreOs_status_code ACoreOs_cache_flush/invalidate(ACoreOs_cache_types type,
 * void *pStartAddr, ULONG size)，声明在天脉3 SDK 的 cache.h 里，故此处改为
 * 包含该头文件，不再自行 extern 声明）：
 *   写卡前对发送缓冲区 flush —— 把 CPU 缓存里的最新数据刷到内存，DMA 才能读到正确内容；
 *   读卡完成后对接收缓冲区 invalidate —— 强制 CPU 重新从内存读取 DMA 刚写入的数据。
 * type 统一传 ACOREOS_CACHE_DAT（数据缓存，本驱动不涉及指令缓存）。*/
#include "cache.h"

/* 天脉3 平台提供的微秒级延时函数（同 bm_emmc_osal_os3.c 假设） */
extern void delay_us(unsigned int time_us);

/*--------------------------------------------------------------------------
 * eMMC 命令号
 *------------------------------------------------------------------------*/
#define MMC_GO_IDLE_STATE         0
#define MMC_SEND_OP_COND          1
#define MMC_ALL_SEND_CID          2
#define MMC_SET_RELATIVE_ADDR     3
#define MMC_SWITCH                6
#define MMC_SELECT_CARD           7
#define MMC_SEND_EXT_CSD          8
#define MMC_SEND_CSD              9
#define MMC_STOP_TRANSMISSION     12          /* 多块传输结束后停止 */
#define MMC_SEND_STATUS           13
#define MMC_SET_BLOCKLEN          16
#define MMC_READ_SINGLE_BLOCK     17
#define MMC_READ_MULTIPLE_BLOCK   18
#define MMC_WRITE_BLOCK           24
#define MMC_WRITE_MULTIPLE_BLOCK  25

/* CMD1 OCR 参数：扇区寻址(bit30) + 电压窗口 2.7~3.6V */
#define MMC_OCR_ARG               0x40FF8000U
#define MMC_OCR_BUSY              0x80000000U   /* 上电完成 */
#define MMC_OCR_SECTOR_MODE       0x40000000U   /* 扇区寻址 */

/* 卡状态位（CMD13 响应） */
#define MMC_STATUS_RDY_FOR_DATA   (1U << 8)
#define MMC_STATUS_CURR_STATE_SH  9
#define MMC_STATUS_CURR_STATE_MSK 0xFU
#define MMC_STATE_PRG             7             /* 编程中 */

/* EXT_CSD 字段索引 */
#define EXT_CSD_BUS_WIDTH         183
#define EXT_CSD_SEC_COUNT         212           /* 4 字节，小端，单位扇区 */

/* SWITCH(CMD6) 参数：访问模式 = write byte(3) */
#define MMC_SWITCH_WRITE_BYTE     3U
#define MMC_SWITCH_ARG(idx, val) \
        ((MMC_SWITCH_WRITE_BYTE << 24) | ((u32)(idx) << 16) | ((u32)(val) << 8))

/*--------------------------------------------------------------------------
 * 内部状态
 *------------------------------------------------------------------------*/
static BM1684X_SDHCI_DEV *g_dev          = NULL;
static int g_inited      = 0;
static u32 g_rca          = 1;       /* 主机给 eMMC 分配的相对地址 */
static int g_sectorMode  = 0;       /* 1=扇区寻址 0=字节寻址 */
static u32 g_blockCount  = 0;       /* 用户区总扇区数（512B/扇区） */

/* EXT_CSD 读缓冲（512B），静态避免占栈，且地址固定方便 cache 维护 */
static u8 g_extCsd[512];

#ifndef BM_EMMC_LOCK_TIMEOUT_MS
#define BM_EMMC_LOCK_TIMEOUT_MS 60000U
#endif

static void *g_ioMutex = NULL;
static int g_ioLockReady = 0;
static int g_ioLockUseMutex = 0;
static volatile int g_ioLockInitBusy = 0;
static volatile int g_ioSpinBusy = 0;

static int bmAtomicTryLock(volatile int *lock)
{
#if defined(__GNUC__)
    return __sync_lock_test_and_set(lock, 1) == 0;
#else
    if (*lock)
        return 0;
    *lock = 1;
    return 1;
#endif
}

static void bmAtomicUnlock(volatile int *lock)
{
#if defined(__GNUC__)
    __sync_lock_release(lock);
#else
    *lock = 0;
#endif
}

static int bmEmmcSpinLock(volatile int *lock, u32 timeoutMs)
{
    u32 left = timeoutMs * 100U; /* 10us per retry */

    while (!bmAtomicTryLock(lock))
    {
        if (left-- == 0U)
            return BM_EMMC_ETIMEOUT;
        delay_us(10);
    }
    return BM_EMMC_OK;
}

static int bmEmmcEnsureIoLock(void)
{
    if (g_ioLockReady)
        return BM_EMMC_OK;

    if (bmEmmcSpinLock(&g_ioLockInitBusy, BM_EMMC_LOCK_TIMEOUT_MS) != BM_EMMC_OK)
        return BM_EMMC_ETIMEOUT;

    if (!g_ioLockReady)
    {
        if (g_bm1684xOsalOs3.mutex_create != NULL &&
            g_bm1684xOsalOs3.mutex_lock   != NULL &&
            g_bm1684xOsalOs3.mutex_unlock != NULL)
        {
            g_ioMutex = g_bm1684xOsalOs3.mutex_create();
            if (g_ioMutex != NULL)
                g_ioLockUseMutex = 1;
        }

        if (!g_ioLockUseMutex)
        {
            /* ponytail: atomic fallback, replace with RTOS mutex if wait latency matters. */
            printf("[bm_emmc] OS mutex not hooked, use atomic fallback lock\r\n");
        }
        g_ioLockReady = 1;
    }

    bmAtomicUnlock(&g_ioLockInitBusy);
    return BM_EMMC_OK;
}

static int bmEmmcIoLock(void)
{
    int ret = bmEmmcEnsureIoLock();

    if (ret != BM_EMMC_OK)
        return ret;

    if (g_ioLockUseMutex)
        return (g_bm1684xOsalOs3.mutex_lock(g_ioMutex) == 0) ? BM_EMMC_OK : BM_EMMC_EIO;

    return bmEmmcSpinLock(&g_ioSpinBusy, BM_EMMC_LOCK_TIMEOUT_MS);
}

static void bmEmmcIoUnlock(void)
{
    if (g_ioLockUseMutex)
        g_bm1684xOsalOs3.mutex_unlock(g_ioMutex);
    else
        bmAtomicUnlock(&g_ioSpinBusy);
}

/*--------------------------------------------------------------------------
 * TOP 域时钟使能 + 软复位：与 TF-A bm_clock.h（GATE_CLK_EMMC_200M/AXI_EMMC/
 * 100K_EMMC）、platform_def.h（BIT_MASK_TOP_SOFT_RST0_EMMC=BIT(20)）、
 * U-Boot reset-bitmain.c（assert→deassert 复位手法）三处比对核实过的位定义
 * 一致（均已在 bm1684xSdhciHw.h 里定义为 BM1684X_CLK_xxx/BM1684X_RST_EMMC）。
 *
 * 为什么需要这一步：TF-A 的 bm_sd.c、U-Boot 的 sdhci-bitmain.c 都从来没有
 * 显式调用这两个寄存器——因为它们只在"eMMC 是引导介质"的场景下运行，BootROM
 * 选中 eMMC 引导时已经替它们把时钟开了、复位放了。本项目场景不同：天脉3不从
 * eMMC 引导，没有任何更早期的代码保证这两步已经做过，所以这里主动补一遍，
 * 否则控制器寄存器可能根本不在总线上、读出来全 0xFFFFFFFF 或直接卡死。
 * 【上板验证重点】：如果发现 bm1684xSdhciInit() 一进去就卡住或读寄存器全 F，
 * 先怀疑这一步没生效（比如 TOP_BASE 物理地址或位定义跟你实际芯片不符）。
 *------------------------------------------------------------------------*/
static void bmTopDomainInit(void)
{
    volatile u32 *pClkEn = (volatile u32 *)(BM1684X_TOP_PHYS_BASE + BM1684X_TOP_CLOCK_EN0);
    volatile u32 *pRst   = (volatile u32 *)(BM1684X_TOP_PHYS_BASE + BM1684X_TOP_SOFT_RST0);

    /* 时钟门控：AXI + 200M + 100K 三路 eMMC 时钟全部打开 */
    *pClkEn |= (BM1684X_CLK_AXI_EMMC | BM1684X_CLK_EMMC_200M | BM1684X_CLK_100K_EMMC);
    BM_DSB();          /* 确保时钟使能真正写到位，再开始等待/复位 */
    delay_us(10);

    /* 软复位：先拉低（置0=复位）再释放（置1=放开），手法照搬 u-boot
     * reset-bitmain.c 的 assert/deassert，每步之间留出和 u-boot 一致的延时 */
    *pRst &= ~BM1684X_RST_EMMC;
    BM_DSB();          /* 确保"进入复位"已生效，再开始计复位保持时间 */
    delay_us(1000);
    *pRst |= BM1684X_RST_EMMC;
    BM_DSB();          /* 确保"放开复位"已生效，再计恢复时间并交给引擎层 */
    delay_us(1000);
}

/*--------------------------------------------------------------------------
 * 【诊断打印】命令发送的调试包装，从 bm_sd_core.c 的 sdSendCmdDbg() 搬过来——
 * eMMC 之前格式化阶段报错时原代码只 return BM_EMMC_EIO，没有任何寄存器现场
 * 信息，分不清是"命令没人理"还是"数据阶段 DMA 没收到完成信号"，也不知道是
 * 格式化时哪一条命令、写到哪个 LBA、多大的块数失败。逻辑和 SD 版完全一致
 * （同一套 SDHCI 寄存器定义、同一个引擎层），仅把基址换成 eMMC 控制器的
 * BM1684X_EMMC_PHYS_BASE。确认根因后可以整段删除。
 *------------------------------------------------------------------------*/
#define SDHCI_ERR_CMD_TIMEOUT   (1U << 0)   /* 命令超时：卡没回应这条命令 */
#define SDHCI_ERR_CMD_CRC       (1U << 1)
#define SDHCI_ERR_CMD_END_BIT   (1U << 2)
#define SDHCI_ERR_CMD_INDEX     (1U << 3)
#define SDHCI_ERR_DATA_TIMEOUT  (1U << 4)   /* 数据阶段超时：发完命令但数据没传完 */
#define SDHCI_ERR_DATA_CRC      (1U << 5)
#define SDHCI_ERR_DATA_END_BIT  (1U << 6)
#define SDHCI_ERR_CURRENT_LIMIT (1U << 7)
#define SDHCI_ERR_AUTO_CMD      (1U << 8)
#define SDHCI_ERR_ADMA          (1U << 9)   /* ADMA 描述符/地址错误 */

static const char *mmcDecodeErrSts(u16 errSt)
{
    if (errSt & SDHCI_ERR_ADMA)         return "ADMA_ERROR";
    if (errSt & SDHCI_ERR_AUTO_CMD)     return "AUTO_CMD_ERROR";
    if (errSt & SDHCI_ERR_DATA_CRC)     return "DATA_CRC_ERROR";
    if (errSt & SDHCI_ERR_DATA_END_BIT) return "DATA_END_BIT_ERROR";
    if (errSt & SDHCI_ERR_DATA_TIMEOUT) return "DATA_TIMEOUT_ERROR";
    if (errSt & SDHCI_ERR_CMD_INDEX)    return "CMD_INDEX_ERROR";
    if (errSt & SDHCI_ERR_CMD_END_BIT)  return "CMD_END_BIT_ERROR";
    if (errSt & SDHCI_ERR_CMD_CRC)      return "CMD_CRC_ERROR";
    if (errSt & SDHCI_ERR_CMD_TIMEOUT)  return "CMD_TIMEOUT_ERROR";
    return errSt ? "UNKNOWN" : "(none-latched)";
}

static int mmcSendCmdDbg(const char *name, BM1684X_MMC_CMD *pCmd, BM1684X_MMC_DATA *pData)
{
    int ret = bm1684xSdhciSendCmd(g_dev, pCmd, pData);

    if (ret != 0)
    {
        volatile u32 *pState = (volatile u32 *)(BM1684X_EMMC_PHYS_BASE + SDHCI_PRESENT_STATE);
        volatile u16 *pIntSt = (volatile u16 *)(BM1684X_EMMC_PHYS_BASE + SDHCI_INT_STATUS);
        volatile u16 *pErrSt = (volatile u16 *)(BM1684X_EMMC_PHYS_BASE + SDHCI_ERR_INT_STATUS);
        volatile u16 *pClkCt = (volatile u16 *)(BM1684X_EMMC_PHYS_BASE + SDHCI_CLOCK_CONTROL);
        u32 state = *pState;
        u16 intSt = *pIntSt;
        u16 errSt = *pErrSt;
        u16 clkCt = *pClkCt;
        /* 实时寄存器值在 ret=BM_ERR_HW 时已被引擎层清空/复位，真实出错现场要看
         * 下面这两个"快照"（引擎层在清空前保存下来的） */
        u32 latchedInt = bm1684xSdhciGetLastIntStatus(g_dev);
        u16 latchedErr = (u16)bm1684xSdhciGetLastErrStatus(g_dev);

        printf("[bm_emmc] FAIL %s(CMD%u) ret=%d state=0x%08x int=0x%04x err=0x%04x"
               " clk=0x%04x resp0=0x%08x arg=0x%08x blkCnt=%u buf=%p align512k=0x%05x"
               " data=%s inhibit=%s%s clk:%s%s%s"
               " latched_int=0x%08x latched_err=0x%04x(%s)\r\n",
               name, (unsigned int)pCmd->cmdIdx, ret,
               (unsigned int)state, (unsigned int)intSt, (unsigned int)errSt,
               (unsigned int)clkCt, (unsigned int)pCmd->resp[0],
               (unsigned int)pCmd->cmdArg,
               pData ? pData->blkCount : 0U,
               pData ? pData->buf : NULL,
               pData ? (unsigned int)(((unsigned long)pData->buf) & 0x7FFFFUL) : 0U,
               pData ? (pData->flags & BM1684X_DATA_READ ? "READ" : "WRITE") : "-",
               (state & SDHCI_STATE_CMD_INHIBIT) ? "CMD," : "",
               (state & SDHCI_STATE_DAT_INHIBIT) ? "DAT," : "",
               (clkCt & SDHCI_CLK_INT_EN)     ? "INT_EN," : "INT_EN(0)!,",
               (clkCt & SDHCI_CLK_INT_STABLE) ? "STABLE," : "STABLE(0)!,",
               (clkCt & SDHCI_CLK_CARD_EN)    ? "CARD_EN" : "CARD_EN(0)!",
               (unsigned int)latchedInt, (unsigned int)latchedErr,
               mmcDecodeErrSts(latchedErr));
    }
    return ret;
}

/*--------------------------------------------------------------------------
 * 等待卡回到可用状态（CMD13 轮询）
 *------------------------------------------------------------------------*/
static int mmcWaitReady(void)
{
    BM1684X_MMC_CMD cmd;
    int ret;
    u32 timeout = 1000000U;   /* 约 1s（每次 10us） */
    u32 state;

    for (;;)
    {
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmdIdx   = MMC_SEND_STATUS;
        cmd.cmdArg   = g_rca << 16;
        cmd.respType = BM1684X_RESP_R1;
        ret = mmcSendCmdDbg("STATUS", &cmd, NULL);
        if (ret != 0)
            return BM_EMMC_EIO;

        state = (cmd.resp[0] >> MMC_STATUS_CURR_STATE_SH) & MMC_STATUS_CURR_STATE_MSK;
        if ((cmd.resp[0] & MMC_STATUS_RDY_FOR_DATA) && (state != MMC_STATE_PRG))
            return BM_EMMC_OK;

        if (timeout-- == 0U)
            return BM_EMMC_ETIMEOUT;
        delay_us(10);
    }
}

/*--------------------------------------------------------------------------
 * 多块传输（CMD18/CMD25）结束后发 CMD12 停止传输。
 * 与 bm_sd_core.c 的 sdStopTransmission() 同一原因：共用引擎层
 * bm1684xSdhci.c 的 buildXferMode() 已【从不用 AUTO_CMD12】（本颗 Synopsys/
 * 比特大陆控制器在 V4 模式下用 AUTO_CMD12 会在命令阶段报错），改由上层在多块
 * 传完后显式发一条 CMD12 收尾。eMMC 之前漏了这一步——单块读写没事，但格式化
 * 时的大块多扇区连续写(CMD25)传完后控制器 DAT 线一直占着，紧跟的命令发不出去
 * 而失败，正是缺这条 CMD12。CMD12 是带忙信号的 R1b 响应。
 *------------------------------------------------------------------------*/
static int mmcStopTransmission(void)
{
    BM1684X_MMC_CMD cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx   = MMC_STOP_TRANSMISSION;
    cmd.cmdArg   = 0;
    cmd.respType = BM1684X_RESP_R1B;
    return mmcSendCmdDbg("CMD12", &cmd, NULL);
}

/*--------------------------------------------------------------------------
 * 修改 EXT_CSD 单字节（CMD6 SWITCH），并等待生效
 *------------------------------------------------------------------------*/
static int mmcSwitch(u8 index, u8 value)
{
    BM1684X_MMC_CMD cmd;
    int ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx   = MMC_SWITCH;
    cmd.cmdArg   = MMC_SWITCH_ARG(index, value);
    cmd.respType = BM1684X_RESP_R1B;
    ret = mmcSendCmdDbg("SWITCH", &cmd, NULL);
    if (ret != 0)
        return BM_EMMC_EIO;

    return mmcWaitReady();
}

/*--------------------------------------------------------------------------
 * LBA -> 命令参数（扇区/字节寻址自适应）
 *------------------------------------------------------------------------*/
static u32 lbaToArg(u32 lba)
{
    return g_sectorMode ? lba : (lba * BM_EMMC_BLOCK_SIZE);
}

/*--------------------------------------------------------------------------
 * 初始化：控制器（含 PHY/时钟）+ eMMC 卡识别
 *------------------------------------------------------------------------*/
static int bm_emmc_init_locked(void)
{
    BM1684X_MMC_CMD  cmd;
    BM1684X_MMC_DATA data;
    int ret;
    u32 timeout;

    if (g_inited)
        return BM_EMMC_OK;

    /* 先把 TOP 域 eMMC 时钟/复位打开，再让引擎层去碰控制器寄存器，
     * 否则控制器可能根本没上电/没出复位（详见上面 bmTopDomainInit() 的注释） */
    bmTopDomainInit();

    /* 引擎初始化：内部完成 14 步 PHY 时序 + 控制器复位上电 + 识别时钟（200kHz） */
    g_dev = bm1684xSdhciInit(&g_bm1684xOsalOs3, BM1684X_EMMC_PHYS_BASE,
                              0U, BM1684X_EMMC_INDEX, BM_EMMC_USE_64BIT_DMA);
    if (g_dev == NULL)
        return BM_EMMC_EIO;

    /* 置位 EMMC_CTRL_R 的 CARD_IS_EMMC（厂商区 +0x2C bit0），让控制器按 eMMC
     * 卡的专属行为处理（如 RST_N 复位脚）。参照 u-boot sdhci-bitmain.c 的
     * probe() 流程补上——本仓库现成的引擎层 bm1684xSdhci.c 没有设置这一位。 */
    {
        volatile u16 *pVendorPtr = (volatile u16 *)(BM1684X_EMMC_PHYS_BASE + SDHCI_VENDOR_SPECIFIC_AREA);
        u16 vendorOff = (u16)(*pVendorPtr & 0x0FFFU);   /* 厂商区基址偏移，标准 SDHCI 字段 */
        volatile u16 *pEmmcCtrl = (volatile u16 *)(BM1684X_EMMC_PHYS_BASE + vendorOff + SDHCI_EMMC_CTRL_R_OFF);
        *pEmmcCtrl |= 0x1U;
    }

    bm1684xSdhciSetBusWidth(g_dev, 1U);
    delay_us(2000);

    /* CMD0：复位到 idle */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = MMC_GO_IDLE_STATE; cmd.cmdArg = 0; cmd.respType = BM1684X_RESP_NONE;
    ret = mmcSendCmdDbg("IDLE", &cmd, NULL);
    if (ret != 0)
        return BM_EMMC_EIO;
    delay_us(2000);

    /* CMD1：反复发送直到上电完成 */
    timeout = 1000U;   /* 最多约 2s */
    for (;;)
    {
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmdIdx = MMC_SEND_OP_COND; cmd.cmdArg = MMC_OCR_ARG; cmd.respType = BM1684X_RESP_R3;
        ret = mmcSendCmdDbg("OP_COND", &cmd, NULL);
        if (ret != 0)
            return BM_EMMC_EIO;
        if (cmd.resp[0] & MMC_OCR_BUSY)
            break;
        if (timeout-- == 0U)
            return BM_EMMC_ETIMEOUT;
        delay_us(2000);
    }
    g_sectorMode = (cmd.resp[0] & MMC_OCR_SECTOR_MODE) ? 1 : 0;

    /* CMD2：读 CID（进入识别态） */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = MMC_ALL_SEND_CID; cmd.cmdArg = 0; cmd.respType = BM1684X_RESP_R2;
    ret = mmcSendCmdDbg("ALL_CID", &cmd, NULL);
    if (ret != 0)
        return BM_EMMC_EIO;

    /* CMD3：主机给卡分配 RCA */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = MMC_SET_RELATIVE_ADDR; cmd.cmdArg = g_rca << 16; cmd.respType = BM1684X_RESP_R1;
    ret = mmcSendCmdDbg("REL_ADDR", &cmd, NULL);
    if (ret != 0)
        return BM_EMMC_EIO;

    /* CMD9：读 CSD（容量以 EXT_CSD SEC_COUNT 为准，此处仅完成状态机流转） */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = MMC_SEND_CSD; cmd.cmdArg = g_rca << 16; cmd.respType = BM1684X_RESP_R2;
    ret = mmcSendCmdDbg("SEND_CSD", &cmd, NULL);
    if (ret != 0)
        return BM_EMMC_EIO;

    /* CMD7：选中卡，进入传输态 */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = MMC_SELECT_CARD; cmd.cmdArg = g_rca << 16; cmd.respType = BM1684X_RESP_R1B;
    ret = mmcSendCmdDbg("SELECT", &cmd, NULL);
    if (ret != 0)
        return BM_EMMC_EIO;

    /* 切换总线位宽（先卡侧 EXT_CSD，再控制器侧）。
     * 目标时钟 25MHz 低于 legacy 上限(26MHz)，不需要切高速档/调 DLL，
     * PHY 初始化阶段已是旁路状态，求稳优先。*/
#if (BM_EMMC_BUS_WIDTH == 4)
    if (mmcSwitch(EXT_CSD_BUS_WIDTH, 1U) == BM_EMMC_OK)
        bm1684xSdhciSetBusWidth(g_dev, 4U);
#else
    bm1684xSdhciSetBusWidth(g_dev, 1U);
#endif

    /* 切到工作时钟（默认 25MHz 安全档） */
    ret = bm1684xSdhciSetClk(g_dev, BM_EMMC_TRAN_CLK_HZ);
    if (ret != 0)
        return BM_EMMC_EIO;

    /* CMD8：读 EXT_CSD，取 SEC_COUNT 作为容量 */
    memset(&cmd, 0, sizeof(cmd));
    data.buf = g_extCsd; data.blkSize = BM_EMMC_BLOCK_SIZE; data.blkCount = 1U;
    data.flags = BM1684X_DATA_READ;
    cmd.cmdIdx = MMC_SEND_EXT_CSD; cmd.cmdArg = 0; cmd.respType = BM1684X_RESP_R1;
    ret = mmcSendCmdDbg("EXT_CSD", &cmd, &data);
    if (ret == 0)
    {
        /* DMA 写入完成，CPU 读取前先让缓存失效，避免读到旧值（同读扇区路径，
         * 先 dsb 定序 DMA 完成观测，再 invalidate） */
        BM_DSB();
        ACoreOs_cache_invalidate(ACOREOS_CACHE_DAT, g_extCsd, BM_EMMC_BLOCK_SIZE);

        g_blockCount = (u32)g_extCsd[EXT_CSD_SEC_COUNT]
                      | ((u32)g_extCsd[EXT_CSD_SEC_COUNT + 1] << 8)
                      | ((u32)g_extCsd[EXT_CSD_SEC_COUNT + 2] << 16)
                      | ((u32)g_extCsd[EXT_CSD_SEC_COUNT + 3] << 24);
    }

    /* CMD16：设块长 512（扇区寻址下无副作用，保持兼容） */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx = MMC_SET_BLOCKLEN; cmd.cmdArg = BM_EMMC_BLOCK_SIZE; cmd.respType = BM1684X_RESP_R1;
    (void)mmcSendCmdDbg("SET_BLOCKLEN", &cmd, NULL);

    g_inited = 1;
    return BM_EMMC_OK;
}

int bm_emmc_init(void)
{
    int ret = bmEmmcIoLock();

    if (ret != BM_EMMC_OK)
        return ret;

    ret = bm_emmc_init_locked();
    bmEmmcIoUnlock();
    return ret;
}

int bm_emmc_reinit(void)
{
    int ret = bmEmmcIoLock();

    if (ret != BM_EMMC_OK)
        return ret;

    g_inited     = 0;
    g_blockCount = 0;
    ret = bm_emmc_init_locked();
    bmEmmcIoUnlock();
    return ret;
}

/*--------------------------------------------------------------------------
 * 读扇区
 *------------------------------------------------------------------------*/
int bm_emmc_read_blocks(u32 lba, u32 count, void *buf)
{
    BM1684X_MMC_CMD  cmd;
    BM1684X_MMC_DATA data;
    int ret = BM_EMMC_OK;

    if ((count == 0U) || (buf == NULL))
        return BM_EMMC_EPARAM;

    ret = bmEmmcIoLock();
    if (ret != BM_EMMC_OK)
        return ret;

    if (!g_inited)
    {
        ret = bm_emmc_init_locked();
        if (ret != BM_EMMC_OK)
            goto out;
    }

    /* 读卡前也要先 invalidate 接收缓冲区（与 bm_sd_read_blocks 保持一致）：
     * 参考驱动 bm_sd_prepare() 对读写都无条件先做一次缓存维护，不是只在读完
     * 之后做。如果 buf 之前残留了缓存里的旧内容（哪怕是干净行），DMA 写入期间
     * 该缓存行仍可能有效，导致读完后 CPU 命中旧值——SD 那条线实测复现过"读命令
     * 成功、字节0却是旧值"，就是漏了读前这一次。补 dsb 保证"丢弃旧缓存行"排在
     * 后面引擎层启动 DMA 的寄存器写之前。*/
    ACoreOs_cache_invalidate(ACOREOS_CACHE_DAT, buf, count * BM_EMMC_BLOCK_SIZE);
    BM_DSB();

    memset(&data, 0, sizeof(data));
    data.buf = buf; data.blkSize = BM_EMMC_BLOCK_SIZE; data.blkCount = count;
    data.flags = BM1684X_DATA_READ;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx   = (count > 1U) ? MMC_READ_MULTIPLE_BLOCK : MMC_READ_SINGLE_BLOCK;
    cmd.cmdArg   = lbaToArg(lba);
    cmd.respType = BM1684X_RESP_R1;

    ret = mmcSendCmdDbg("READ", &cmd, &data);
    if (ret != 0)
    {
        ret = BM_EMMC_EIO;
        goto out;
    }

    /* 多块读(CMD18)是开放式命令，传完要显式发 CMD12 停止（引擎层不再用
     * AUTO_CMD12，与 bm_sd_read_blocks 一致）。单块读(CMD17)自带终止，
     * 不需要 CMD12。*/
    if (count > 1U)
    {
        ret = mmcStopTransmission();
        if (ret != 0)
        {
            ret = BM_EMMC_EIO;
            goto out;
        }
    }

    /* 先补一道 dsb，确保"引擎层已观察到 DMA 传输完成"（最后那次 Device 状态寄存器
     * 读）排在下面 invalidate 之前——避免在 DMA 尚未真正写完内存时就丢弃缓存行。
     * 然后 invalidate 接收缓冲区，强制 CPU 重新从内存读取 DMA 刚写入的数据。*/
    BM_DSB();
    ACoreOs_cache_invalidate(ACOREOS_CACHE_DAT, buf, count * BM_EMMC_BLOCK_SIZE);
    ret = BM_EMMC_OK;

out:
    bmEmmcIoUnlock();
    return ret;
}

/*--------------------------------------------------------------------------
 * 写扇区
 *------------------------------------------------------------------------*/
int bm_emmc_write_blocks(u32 lba, u32 count, const void *buf)
{
    BM1684X_MMC_CMD  cmd;
    BM1684X_MMC_DATA data;
    int ret = BM_EMMC_OK;

    if ((count == 0U) || (buf == NULL))
        return BM_EMMC_EPARAM;

    ret = bmEmmcIoLock();
    if (ret != BM_EMMC_OK)
        return ret;

    if (!g_inited)
    {
        ret = bm_emmc_init_locked();
        if (ret != BM_EMMC_OK)
            goto out;
    }

    /* 写卡前对发送缓冲区 flush，把 CPU 缓存里的最新数据刷到内存，DMA 才能读到正确内容。
     * flush 之后补一道 dsb：保证"数据落内存"这件事，排在后面引擎层启动 DMA 的寄存器
     * 写之前完成（普通内存→Device 内存的跨域定序，volatile 管不了）。正确实现的
     * cache_flush 内部本就带尾部 dsb，这里再补一道是冗余但零风险的保险。*/
    ACoreOs_cache_flush(ACOREOS_CACHE_DAT, (void *)buf, count * BM_EMMC_BLOCK_SIZE);
    BM_DSB();

    memset(&data, 0, sizeof(data));
    data.buf = (void *)buf; data.blkSize = BM_EMMC_BLOCK_SIZE; data.blkCount = count;
    data.flags = BM1684X_DATA_WRITE;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx   = (count > 1U) ? MMC_WRITE_MULTIPLE_BLOCK : MMC_WRITE_BLOCK;
    cmd.cmdArg   = lbaToArg(lba);
    cmd.respType = BM1684X_RESP_R1;

    ret = mmcSendCmdDbg("WRITE", &cmd, &data);
    if (ret != 0)
    {
        ret = BM_EMMC_EIO;
        goto out;
    }

    /* 多块写(CMD25)是开放式命令，传完要显式发 CMD12 停止（引擎层不再用
     * AUTO_CMD12，与 bm_sd_write_blocks 一致）。单块写(CMD24)自带终止，
     * 不需要 CMD12。eMMC 之前漏了这条，格式化大块连续写传完后 DAT 线没释放、
     * 紧跟命令发不出去——大概率就是格式化阶段报错的根因。*/
    if (count > 1U)
    {
        ret = mmcStopTransmission();
        if (ret != 0)
        {
            ret = BM_EMMC_EIO;
            goto out;
        }
    }

    /* 等卡内部编程完成，确保数据落盘 */
    ret = mmcWaitReady();

out:
    bmEmmcIoUnlock();
    return ret;
}

u32 bm_emmc_get_block_count(void)
{
    u32 n;

    if (bmEmmcIoLock() != BM_EMMC_OK)
        return 0;
    n = g_blockCount;
    bmEmmcIoUnlock();
    return n;
}

#endif /* BM1684X_EMMC */
