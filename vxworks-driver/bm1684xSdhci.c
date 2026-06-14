/*
 * bm1684xSdhci.c — BM1684X Synopsys DesignWare SDHCI 驱动
 *
 * 完全自包含：仅依赖 bm1684xSdhciHw.h 和 bm1684xSdhciOsal.h，
 * 无需任何 VxWorks SDK 头文件。
 *
 * 命令完成等待策略在编译时同时编入，运行时根据 OSAL 信号量/中断
 * 回调是否为非 NULL 自动选择：
 *
 *   中断模式 — ISR 清除中断状态后通知信号量，调用方阻塞等待。
 *   轮询模式 — 调用方循环读取 INT_STATUS 寄存器，无需中断支持。
 *
 * 版权所有 (c) 2024 Bitmain / Sophgo.  SPDX-License-Identifier: BSD-3-Clause
 */

#include "bm1684xSdhciHw.h"
#include "bm1684xSdhciOsal.h"

/* -------------------------------------------------------------------------
 * 私有常量
 * ------------------------------------------------------------------------- */

#define SDHCI_CMD_TIMEOUT_MS    1000U    /* 命令完成超时（毫秒） */
#define SDHCI_XFER_TIMEOUT_MS   5000U   /* 数据传输超时（毫秒） */
#define SDHCI_RESET_TIMEOUT_US  100000U /* 软复位等待上限（微秒） */
#define SDHCI_CLK_STABLE_US     150U    /* 时钟稳定等待时间（微秒） */
#define SDHCI_PHY_RESET_US      20U     /* PHY 复位后稳定时间（微秒） */
#define SDHCI_MAX_DIVIDER       256U    /* 时钟分频器最大值 */

/* 内部错误码 */
#define BM_OK                    0
#define BM_ERR_TIMEOUT          (-1)  /* 操作超时 */
#define BM_ERR_BADARG           (-2)  /* 非法参数 */
#define BM_ERR_HW               (-3)  /* 硬件错误 */

/* -------------------------------------------------------------------------
 * 私有设备状态结构体
 * ------------------------------------------------------------------------- */

struct BM1684X_SDHCI_DEV {
    BM1684X_SDHCI_OSAL  osal;          /* 调用方传入的 OSAL 副本            */
    void               *base;          /* SDHCI 寄存器虚拟基地址            */
    void               *topBase;       /* TOP 寄存器虚拟基地址              */
    unsigned int        irqNum;        /* 中断号                            */
    unsigned int        devIndex;      /* 设备索引：0=eMMC，1=SD            */
    unsigned int        is64Bit;       /* 1=使用 64 位 DMA 地址             */
    unsigned int        clkInHz;       /* SoC 输入时钟频率（Hz）            */
    unsigned int        useIrq;        /* 1=中断模式已激活                  */
    void               *cmdSem;        /* CMD_COMPLETE 信号量               */
    void               *xferSem;       /* XFER_COMPLETE 信号量              */
    volatile unsigned int isrStatus;   /* ISR 捕获的 INT_STATUS 原始值      */
    volatile unsigned int isrErrSts;   /* ISR 捕获的 ERR_INT_STATUS 原始值  */
    /* 当 osal.mem_alloc 为 NULL 时使用静态存储 */
};

/* 无堆环境下使用的静态设备结构体（全局唯一） */
static struct BM1684X_SDHCI_DEV s_staticDev;

/* -------------------------------------------------------------------------
 * 内部辅助：判断当前是否使用中断模式
 * ------------------------------------------------------------------------- */

static int useIntMode(const struct BM1684X_SDHCI_DEV *pDev)
{
    /* useIrq 已在 init 阶段设置，此处再确认信号量回调仍有效 */
    return (pDev->useIrq &&
            pDev->osal.sem_wait   != (void *)0 &&
            pDev->osal.sem_signal != (void *)0);
}

/*
 * pollWaitStatus — 轮询等待 INT_STATUS 中指定位置位。
 *
 * 每次循环调用 udelay(1) 微秒，直到目标位出现或超时。
 * 成功时清除已匹配的状态位，并通过 pSts 返回状态值。
 * 返回 0 表示成功，BM_ERR_HW 表示硬件错误，BM_ERR_TIMEOUT 表示超时。
 */
static int pollWaitStatus(struct BM1684X_SDHCI_DEV *pDev,
                          unsigned int mask, unsigned int timeoutMs,
                          unsigned int *pSts)
{
    unsigned int elapsed = 0;
    unsigned int sts;
    unsigned int loops = timeoutMs * 1000U; /* 每次循环约 1 µs */

    while (elapsed < loops) {
        /* 将普通中断状态（低 16 位）与错误状态（高 16 位）合并读取 */
        sts = (unsigned int)REG_RD16(pDev->base, SDHCI_INT_STATUS) |
              ((unsigned int)REG_RD16(pDev->base, SDHCI_ERR_INT_STATUS) << 16);

        if (sts & SDHCI_INT_ERROR) {
            /* 发现硬件错误：清除错误状态位后立即返回 */
            REG_WR16(pDev->base, SDHCI_ERR_INT_STATUS,
                     REG_RD16(pDev->base, SDHCI_ERR_INT_STATUS));
            REG_WR16(pDev->base, SDHCI_INT_STATUS, SDHCI_INT_ERROR);
            if (pSts) *pSts = sts;
            return BM_ERR_HW;
        }
        if (sts & mask) {
            /* 目标位已置位：写 1 清除已匹配的普通状态位 */
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
 * irqWaitStatus — 中断模式等待：阻塞信号量，然后读取 ISR 保存的状态。
 * 返回 0 表示成功，BM_ERR_TIMEOUT 或 BM_ERR_HW 表示失败。
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
 * 软件复位辅助
 * ------------------------------------------------------------------------- */

static int sdhciReset(struct BM1684X_SDHCI_DEV *pDev, unsigned char mask)
{
    unsigned int i;

    REG_WR8(pDev->base, SDHCI_SOFTWARE_RESET, mask);
    /* 轮询等待复位完成（对应位自动清零） */
    for (i = 0; i < SDHCI_RESET_TIMEOUT_US; i++) {
        if ((REG_RD8(pDev->base, SDHCI_SOFTWARE_RESET) & mask) == 0)
            return BM_OK;
        if (pDev->osal.udelay) pDev->osal.udelay(1);
    }
    return BM_ERR_TIMEOUT;
}

/* -------------------------------------------------------------------------
 * PHY 初始化（来自 bm_sd.c / sdhci-bitmain.c 的 14 步序列）
 *
 * devIndex 0 = eMMC：SMPLDL 使用内部反馈路径（INPSEL=0x2）
 * devIndex 1 = SD  ：SMPLDL 使用旁路路径（BYPASS_EN=1）
 * ------------------------------------------------------------------------- */

static void phyInit(struct BM1684X_SDHCI_DEV *pDev)
{
    unsigned int phyCnfg;

    /* 步骤 1：拉低 PHY_RSTN 使 PHY 进入复位状态 */
    phyCnfg = REG_RD32(pDev->base, SDHCI_P_PHY_CNFG);
    phyCnfg &= ~(1U << PHY_CNFG_PHY_RSTN);
    REG_WR32(pDev->base, SDHCI_P_PHY_CNFG, phyCnfg);

    /* 步骤 2：配置 PAD 驱动强度斜率：P 型=0x9，N 型=0x8 */
    phyCnfg = (0x9U << PHY_CNFG_PAD_SP) | (0x8U << PHY_CNFG_PAD_SN);
    REG_WR32(pDev->base, SDHCI_P_PHY_CNFG, phyCnfg);

    /* 步骤 3：CMD PAD：RXSEL=1，弱上拉使能，P 斜率=0xA，N 斜率=6 */
    REG_WR16(pDev->base, SDHCI_P_CMDPAD_CNFG,
             (unsigned short)(
                 (1U << PAD_CNFG_RXSEL) |
                 (1U << PAD_CNFG_WEAKPULL_EN) |
                 (0xAU << PAD_CNFG_TXSLEW_CTRL_P) |
                 (6U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 步骤 4：DAT PAD：配置与 CMD PAD 相同 */
    REG_WR16(pDev->base, SDHCI_P_DATPAD_CNFG,
             (unsigned short)(
                 (1U << PAD_CNFG_RXSEL) |
                 (1U << PAD_CNFG_WEAKPULL_EN) |
                 (0xAU << PAD_CNFG_TXSLEW_CTRL_P) |
                 (6U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 步骤 5：CLK PAD：无上拉/下拉，RXSEL=0，仅设置斜率 */
    REG_WR16(pDev->base, SDHCI_P_CLKPAD_CNFG,
             (unsigned short)(
                 (0xAU << PAD_CNFG_TXSLEW_CTRL_P) |
                 (6U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 步骤 6：STB PAD：RXSEL=1，弱下拉（WEAKPULL_EN=2），同斜率 */
    REG_WR16(pDev->base, SDHCI_P_STBPAD_CNFG,
             (unsigned short)(
                 (1U << PAD_CNFG_RXSEL) |
                 (2U << PAD_CNFG_WEAKPULL_EN) |
                 (0xAU << PAD_CNFG_TXSLEW_CTRL_P) |
                 (6U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 步骤 7：RST_N PAD：RXSEL=1，弱上拉，同斜率 */
    REG_WR16(pDev->base, SDHCI_P_RSTNPAD_CNFG,
             (unsigned short)(
                 (1U << PAD_CNFG_RXSEL) |
                 (1U << PAD_CNFG_WEAKPULL_EN) |
                 (0xAU << PAD_CNFG_TXSLEW_CTRL_P) |
                 (6U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 步骤 8：COMMDL：禁用旁路（使用延迟链） */
    REG_WR8(pDev->base, SDHCI_P_COMMDL_CNFG, 0);

    /* 步骤 9：SDCLKDL：启用旁路，延迟步数默认 0x0A */
    REG_WR8(pDev->base, SDHCI_P_SDCLKDL_CNFG,
            (unsigned char)(1U << SDCLKDL_BYPASS_EN));
    REG_WR8(pDev->base, SDHCI_P_SDCLKDL_DC, SDCLKDL_DC_DEFAULT);

    /* 步骤 10：SMPLDL 采样路径
     *   eMMC：使用内部反馈路径（INPSEL=0x2），提高采样稳定性
     *   SD  ：使用旁路路径（BYPASS_EN=1），适合低速信号 */
    if (pDev->devIndex == BM1684X_EMMC_INDEX)
        REG_WR8(pDev->base, SDHCI_P_SMPLDL_CNFG,
                (unsigned char)(0x2U << SMPLDL_INPSEL_CNFG));
    else
        REG_WR8(pDev->base, SDHCI_P_SMPLDL_CNFG,
                (unsigned char)(1U << SMPLDL_BYPASS_EN));

    /* 步骤 11：ATDL：使用旁路路径 */
    REG_WR8(pDev->base, SDHCI_P_ATDL_CNFG,
            (unsigned char)(1U << ATDL_BYPASS_EN));

    /* 步骤 12：释放 PHY 复位（PHY_RSTN 置 1） */
    phyCnfg = REG_RD32(pDev->base, SDHCI_P_PHY_CNFG);
    phyCnfg |= (1U << PHY_CNFG_PHY_RSTN);
    REG_WR32(pDev->base, SDHCI_P_PHY_CNFG, phyCnfg);

    /* 步骤 13：等待 PHY 内部电路稳定 */
    if (pDev->osal.udelay) pDev->osal.udelay(SDHCI_PHY_RESET_US);

    /* 步骤 14：轮询 PHY_PWRGOOD 位，确认 PHY 上电完成 */
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
 * 主机控制器硬件初始化
 * ------------------------------------------------------------------------- */

static int hwInit(struct BM1684X_SDHCI_DEV *pDev)
{
    unsigned short hc2;
    int rc;

    /* 全局软复位，清除所有内部状态 */
    rc = sdhciReset(pDev, SDHCI_RESET_ALL);
    if (rc != BM_OK) return rc;

    /* 启用版本 4 模式、可选 64 位地址、CMD23 预设块计数 */
    hc2 = (unsigned short)(SDHCI_HC2_VER4_ENABLE | SDHCI_HC2_CMD23_SUPPORT);
    if (pDev->is64Bit)
        hc2 |= (unsigned short)SDHCI_HC2_64BIT_ADDR;
    REG_WR16(pDev->base, SDHCI_HOST_CONTROL2, hc2);

    /* 上电：选择 3.3 V 电压 */
    REG_WR8(pDev->base, SDHCI_POWER_CONTROL,
            (unsigned char)(SDHCI_POWER_ON | SDHCI_POWER_330));

    /* 使能全部普通及错误中断状态位（仅状态可见，中断信号默认关闭） */
    REG_WR16(pDev->base, SDHCI_INT_STATUS_EN,
             (unsigned short)(SDHCI_INT_ALL_NORMAL));
    REG_WR16(pDev->base, SDHCI_ERR_INT_STATUS_EN,
             (unsigned short)(SDHCI_INT_ALL_ERROR));
    /* 传输发起前不产生中断信号，按需开启 */
    REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN, 0);

    /* DMA 模式：使用 SDMA（简单模式，无需描述符环） */
    {
        unsigned char hc1 = REG_RD8(pDev->base, SDHCI_HOST_CONTROL);
        hc1 = (unsigned char)((hc1 & ~(unsigned char)SDHCI_CTRL_DMA_MASK) |
                              (unsigned char)SDHCI_CTRL_SDMA);
        REG_WR8(pDev->base, SDHCI_HOST_CONTROL, hc1);
    }

    /* 执行 PHY 初始化序列 */
    phyInit(pDev);

    return BM_OK;
}

/* -------------------------------------------------------------------------
 * 时钟控制
 * ------------------------------------------------------------------------- */

int bm1684xSdhciSetClk(BM1684X_SDHCI_DEV *pDev, unsigned int clkHz)
{
    unsigned int div;
    unsigned short clkCtrl;
    unsigned int i;

    if (!pDev || clkHz == 0)
        return BM_ERR_BADARG;

    /* 先停止时钟输出 */
    REG_WR16(pDev->base, SDHCI_CLOCK_CONTROL, 0);

    /* 计算满足目标频率的最小分频比（偶数步进） */
    if (clkHz >= pDev->clkInHz) {
        div = 1U;
    } else {
        for (div = 2U; div <= SDHCI_MAX_DIVIDER * 2U; div += 2U) {
            if ((pDev->clkInHz / div) <= clkHz)
                break;
        }
    }

    /* SD 主机规范：分频字段 = div/2（0 表示直通，即 /1） */
    {
        unsigned int divField = (div > 1U) ? (div >> 1U) : 0U;
        clkCtrl = (unsigned short)(
            SDHCI_CLK_INT_EN |
            ((divField & SDHCI_CLK_DIV_MASK) << SDHCI_CLK_DIV_SHIFT));
    }
    REG_WR16(pDev->base, SDHCI_CLOCK_CONTROL, clkCtrl);

    /* 等待内部时钟稳定 */
    for (i = 0; i < 150U; i++) {
        if (REG_RD16(pDev->base, SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INT_STABLE)
            break;
        if (pDev->osal.udelay) pDev->osal.udelay(1);
    }

    /* 使能向卡输出时钟 */
    clkCtrl |= (unsigned short)SDHCI_CLK_CARD_EN;
    REG_WR16(pDev->base, SDHCI_CLOCK_CONTROL, clkCtrl);

    if (pDev->osal.udelay) pDev->osal.udelay(SDHCI_CLK_STABLE_US);
    return BM_OK;
}

/* -------------------------------------------------------------------------
 * 总线宽度设置
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
    /* width == 1：两个位均清零，即 1 位模式 */

    REG_WR8(pDev->base, SDHCI_HOST_CONTROL, hc1);
    return BM_OK;
}

/* -------------------------------------------------------------------------
 * 根据 BM1684X_MMC_CMD 描述符构建命令寄存器值
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
        /* 136 位长响应，带 CRC 校验 */
        flags = SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC;
        break;
    case BM1684X_RESP_R3:
    case BM1684X_RESP_R4:
        /* 无 CRC / 无索引检查的短响应（OCR、SDIO） */
        flags = SDHCI_CMD_RESP_SHORT;
        break;
    case BM1684X_RESP_R1B:
        /* 短响应 + 忙碌信号，需等待 DAT0 空闲 */
        flags = SDHCI_CMD_RESP_SHORT_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX_CHK;
        break;
    default:
        /* R1/R5/R6/R7：标准短响应，带 CRC 和索引检查 */
        flags = SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX_CHK;
        break;
    }

    if (pData)
        flags |= SDHCI_CMD_DATA;  /* 命令携带数据传输 */

    return SDHCI_MAKE_CMD(pCmd->cmdIdx, flags);
}

/* -------------------------------------------------------------------------
 * 构建 SDMA 数据传输模式寄存器值
 * ------------------------------------------------------------------------- */

static unsigned short buildXferMode(const BM1684X_MMC_DATA *pData)
{
    unsigned short mode = SDHCI_TRNS_DMA | SDHCI_TRNS_BLK_CNT_EN;

    if (pData->blkCount > 1U) {
        /* 多块传输：使能多块模式并自动发送 CMD12 停止命令 */
        mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
    }
    if (pData->flags & BM1684X_DATA_READ)
        mode |= SDHCI_TRNS_READ;  /* 读方向：数据从卡流向主机 */

    return mode;
}

/* 前向声明：使 IsrWrapper 能够引用公开 ISR 函数 */
void bm1684xSdhciIsr(BM1684X_SDHCI_DEV *pDev);

/* -------------------------------------------------------------------------
 * 中断服务例程 — 在 RTOS ISR 中断上下文中调用
 * ------------------------------------------------------------------------- */

/* 类型适配包装：OSAL irq_connect 要求 void (*)(void *)，此处做强制转换 */
static void bm1684xSdhciIsrWrapper(void *arg)
{
    bm1684xSdhciIsr((BM1684X_SDHCI_DEV *)arg);
}

void bm1684xSdhciIsr(BM1684X_SDHCI_DEV *pDev)
{
    unsigned int sts;
    unsigned int errSts;

    if (!pDev || !pDev->useIrq) return;

    /* 读取中断状态（需在清除前保存） */
    sts    = (unsigned int)REG_RD16(pDev->base, SDHCI_INT_STATUS);
    errSts = (unsigned int)REG_RD16(pDev->base, SDHCI_ERR_INT_STATUS);

    /* 写 1 清除已读到的状态位（write-1-to-clear） */
    REG_WR16(pDev->base, SDHCI_ERR_INT_STATUS, (unsigned short)errSts);
    REG_WR16(pDev->base, SDHCI_INT_STATUS, (unsigned short)sts);

    /* 关闭中断信号，防止处理期间重复进入 ISR */
    REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN, 0);

    /* 将状态保存供 irqWaitStatus 读取，若有错误则合并错误标志 */
    pDev->isrStatus = sts | (errSts ? (unsigned int)SDHCI_INT_ERROR : 0U);
    pDev->isrErrSts = errSts;

    /* SDMA 512 KB 边界中断：重新写入 DMA 地址寄存器以继续传输 */
    if (sts & SDHCI_INT_DMA_END) {
        unsigned int sa = REG_RD32(pDev->base, SDHCI_DMA_ADDRESS);
        REG_WR32(pDev->base, SDHCI_DMA_ADDRESS, sa);
    }

    /* 命令完成：唤醒等待 cmdSem 的线程 */
    if ((sts & SDHCI_INT_CMD_COMPLETE) && pDev->cmdSem &&
        pDev->osal.sem_signal)
        pDev->osal.sem_signal(pDev->cmdSem);

    /* 传输完成或传输错误：唤醒等待 xferSem 的线程 */
    if ((sts & (SDHCI_INT_XFER_COMPLETE | SDHCI_INT_ERROR)) &&
        pDev->xferSem && pDev->osal.sem_signal)
        pDev->osal.sem_signal(pDev->xferSem);
}

/* -------------------------------------------------------------------------
 * 发送命令核心实现
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

    /* 确定需要等待哪些 inhibit 位：有数据或 R1b 响应时还需等 DAT 通道空闲 */
    inhibitMask = SDHCI_STATE_CMD_INHIBIT;
    if (pData || pCmd->respType == BM1684X_RESP_R1B)
        inhibitMask |= SDHCI_STATE_DAT_INHIBIT;

    {
        unsigned int i;
        for (i = 0; i < 100000U; i++) {
            if ((REG_RD32(pDev->base, SDHCI_PRESENT_STATE) & inhibitMask) == 0)
                break;
            if (pDev->osal.udelay) pDev->osal.udelay(1);
        }
        if (REG_RD32(pDev->base, SDHCI_PRESENT_STATE) & inhibitMask)
            return BM_ERR_TIMEOUT;
    }

    /* 写入数据寄存器（必须在写命令前完成） */
    if (pData) {
        unsigned long dmaAddr = (unsigned long)(unsigned long long)(unsigned long)pData->buf;

        REG_WR16(pDev->base, SDHCI_BLOCK_SIZE,
                 (unsigned short)SDHCI_MAKE_BLKSZ(7, pData->blkSize));
        REG_WR16(pDev->base, SDHCI_BLOCK_COUNT,
                 (unsigned short)(pData->blkCount & 0xFFFFU));

        /* SDMA：写入 DMA 地址低 32 位；64 位模式下额外写高 32 位 */
        REG_WR32(pDev->base, SDHCI_DMA_ADDRESS, (unsigned int)(dmaAddr & 0xFFFFFFFFUL));
        if (pDev->is64Bit)
            REG_WR32(pDev->base, SDHCI_ADMA_SA_HIGH,
                     (unsigned int)((dmaAddr >> 32) & 0xFFFFFFFFUL));

        xferMode = buildXferMode(pData);
    }

    REG_WR32(pDev->base, SDHCI_ARGUMENT, pCmd->cmdArg);
    cmdReg = buildCmdFlags(pCmd, pData);

    /* =======================================================================
     * 中断模式：使能中断信号 → 写命令 → 阻塞等待信号量
     * ===================================================================== */
    if (useIntMode(pDev)) {
        pDev->isrStatus = 0;
        pDev->isrErrSts = 0;

        /* 使能核心中断信号（命令完成、传输完成、DMA 边界、错误） */
        REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN,
                 (unsigned short)SDHCI_INT_CORE_MASK);

        /* 写传输模式和命令寄存器，触发命令发送 */
        REG_WR16(pDev->base, SDHCI_TRANSFER_MODE, xferMode);
        REG_WR16(pDev->base, SDHCI_COMMAND, cmdReg);

        /* 等待 CMD_COMPLETE（超时 1000 ms） */
        rc = irqWaitStatus(pDev, pDev->cmdSem, SDHCI_CMD_TIMEOUT_MS, &sts);
        if (rc != BM_OK) {
            sdhciReset(pDev, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
            return rc;
        }

        /* 读取响应寄存器 */
        if (pCmd->respType == BM1684X_RESP_R2) {
            pCmd->resp[0] = REG_RD32(pDev->base, SDHCI_RESPONSE_0);
            pCmd->resp[1] = REG_RD32(pDev->base, SDHCI_RESPONSE_1);
            pCmd->resp[2] = REG_RD32(pDev->base, SDHCI_RESPONSE_2);
            pCmd->resp[3] = REG_RD32(pDev->base, SDHCI_RESPONSE_3);
        } else if (pCmd->respType != BM1684X_RESP_NONE) {
            pCmd->resp[0] = REG_RD32(pDev->base, SDHCI_RESPONSE_0);
        }

        if (!pData) {
            /* 纯命令传输：关闭中断信号后返回 */
            REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN, 0);
            return BM_OK;
        }

        /* 重新使能中断，等待 XFER_COMPLETE（超时 5000 ms） */
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

    /* =======================================================================
     * 轮询模式：关闭中断信号 → 写命令 → 轮询 INT_STATUS
     * ===================================================================== */
    REG_WR16(pDev->base, SDHCI_INT_SIGNAL_EN, 0);  /* 确保不产生中断信号 */
    REG_WR16(pDev->base, SDHCI_TRANSFER_MODE, xferMode);
    REG_WR16(pDev->base, SDHCI_COMMAND, cmdReg);

    /* 轮询等待命令完成 */
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

    /* 轮询等待数据传输完成（处理 SDMA 512 KB 边界重新加载） */
    for (;;) {
        rc = pollWaitStatus(pDev,
                            SDHCI_INT_XFER_COMPLETE | SDHCI_INT_DMA_END,
                            SDHCI_XFER_TIMEOUT_MS, &sts);
        if (rc != BM_OK) {
            sdhciReset(pDev, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
            return rc;
        }
        if (sts & SDHCI_INT_DMA_END) {
            /* SDMA 边界：重写地址寄存器使 DMA 继续向后推进 */
            unsigned int sa = REG_RD32(pDev->base, SDHCI_DMA_ADDRESS);
            REG_WR32(pDev->base, SDHCI_DMA_ADDRESS, sa);
        }
        if (sts & SDHCI_INT_XFER_COMPLETE)
            break;  /* 全部数据传输完成 */
    }

    return BM_OK;
}

/* -------------------------------------------------------------------------
 * 卡检测
 * ------------------------------------------------------------------------- */

int bm1684xSdhciCardPresent(BM1684X_SDHCI_DEV *pDev)
{
    if (!pDev) return 0;
    /* eMMC 焊接在板上，始终在位 */
    if (pDev->devIndex == BM1684X_EMMC_INDEX) return 1;
    /* SD：读取 PRESENT_STATE 的卡检测位 */
    return (REG_RD32(pDev->base, SDHCI_PRESENT_STATE) &
            SDHCI_STATE_CARD_PRESENT) ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * 从 TOP 寄存器读取 MODE_SEL 以确定输入时钟频率
 * ------------------------------------------------------------------------- */

static unsigned int getInputClk(struct BM1684X_SDHCI_DEV *pDev)
{
    unsigned int modeSel;

    if (!pDev->topBase) return BM1684X_EMMC_CLK_NORMAL_HZ;

    modeSel = REG_RD32(pDev->topBase, BM1684X_TOP_CONF_INFO) & 0x7U;

    switch (modeSel) {
    case BM1684X_MODE_BYPASS: return BM1684X_EMMC_CLK_BYPASS_HZ;  /* 旁路模式 25 MHz */
    default:                  return BM1684X_EMMC_CLK_NORMAL_HZ;   /* 其他模式 100 MHz */
    }
}

/* -------------------------------------------------------------------------
 * 公开初始化接口
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

    /* 分配设备结构体：优先使用 OSAL 堆，否则使用静态全局变量 */
    if (pOsal && pOsal->mem_alloc) {
        pDev = (struct BM1684X_SDHCI_DEV *)pOsal->mem_alloc(
                   (unsigned int)sizeof(*pDev));
        if (!pDev) return (BM1684X_SDHCI_DEV *)0;
        /* 手动清零（不依赖 memset） */
        {
            unsigned char *p = (unsigned char *)pDev;
            unsigned int   n = (unsigned int)sizeof(*pDev);
            while (n--) *p++ = 0;
        }
    } else {
        /* 使用静态存储，同样清零 */
        pDev = &s_staticDev;
        {
            unsigned char *p = (unsigned char *)pDev;
            unsigned int   n = (unsigned int)sizeof(*pDev);
            while (n--) *p++ = 0;
        }
    }

    /* 复制 OSAL 结构体 */
    if (pOsal) pDev->osal = *pOsal;

    pDev->irqNum   = irqNum;
    pDev->devIndex = devIndex;
    pDev->is64Bit  = is64BitAddr;

    /* 映射 SDHCI 寄存器：有 iomap 时调用，否则假定平坦映射直接使用物理地址 */
    if (pDev->osal.iomap) {
        pDev->base = pDev->osal.iomap(regPhysBase, 0x1000U);
    } else {
        pDev->base = (void *)(unsigned long)regPhysBase;
    }
    if (!pDev->base) goto fail;

    /* 映射 TOP 寄存器（可选，用于读取 MODE_SEL 判断时钟频率） */
    if (pDev->osal.iomap) {
        pDev->topBase = pDev->osal.iomap(BM1684X_TOP_PHYS_BASE, 0x1000U);
    } else {
        pDev->topBase = (void *)(unsigned long)BM1684X_TOP_PHYS_BASE;
    }

    pDev->clkInHz = getInputClk(pDev);

    /* 判断是否启用中断模式：需要信号量和 IRQ 回调全部有效 */
    useIrq = (pDev->osal.sem_create  != (void *)0 &&
               pDev->osal.sem_wait   != (void *)0 &&
               pDev->osal.sem_signal != (void *)0 &&
               pDev->osal.irq_connect!= (void *)0 &&
               pDev->osal.irq_enable != (void *)0);

    if (useIrq) {
        /* 创建命令完成和数据传输完成两个信号量 */
        pDev->cmdSem  = pDev->osal.sem_create();
        pDev->xferSem = pDev->osal.sem_create();
        if (!pDev->cmdSem || !pDev->xferSem) {
            useIrq = 0;  /* 创建失败，回退到轮询模式 */
        } else {
            /* 注册并使能中断 */
            if (pDev->osal.irq_connect(irqNum, bm1684xSdhciIsrWrapper, pDev) == 0 &&
                pDev->osal.irq_enable(irqNum) == 0) {
                pDev->useIrq = 1U;  /* 中断模式激活 */
            } else {
                useIrq = 0;  /* 注册失败，回退到轮询模式 */
            }
        }
    }
    (void)useIrq;  /* pDev->useIrq 已在上方设置，此处消除未使用警告 */

    /* 执行硬件初始化和 PHY 初始化 */
    if (hwInit(pDev) != BM_OK) goto fail;

    /* 以识别频率（200 kHz）启动时钟，等待卡就绪 */
    bm1684xSdhciSetClk(pDev, BM1684X_EMMC_CLK_INIT_HZ);

    return (BM1684X_SDHCI_DEV *)pDev;

fail:
    /* 初始化失败：若使用了动态内存则释放 */
    if (pDev != &s_staticDev && pDev->osal.mem_free)
        pDev->osal.mem_free(pDev);
    return (BM1684X_SDHCI_DEV *)0;
}
