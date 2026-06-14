/* vxbBm1684xSdhci.c - BM1684X Synopsys DesignWare SDHCI VxBus 驱动 */

/*
 * 版权所有 (c) 2024 Bitmain / Sophgo
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BM1684X eMMC/SD 主机控制器的 VxWorks 7 VxBus FDT 驱动。
 *
 * 硬件：Synopsys DesignWare SDHCI v4，带集成 PHY
 *   - eMMC 控制器：0x50100000（index=0，有 PHY，64 位寻址）
 *   - SD  控制器：0x50101000（index=1，有 PHY，64 位寻址）
 *
 * FDT compatible 字符串：bitmain,synopsys-sdhc
 *
 * 编译依赖（VxWorks 7 SDK）：
 *   #include <vxWorks.h>
 *   #include <vxBus.h>
 *   #include <hwif/vxbus/vxbLib.h>
 *   #include <hwif/buslib/vxbFdtLib.h>
 *   #include <hwif/buslib/vxbSdhcLib.h>
 *   #include <semLib.h>
 *   #include <taskLib.h>
 *   #include <sysLib.h>
 */

#include <vxWorks.h>
#include <vxBus.h>
#include <hwif/vxbus/vxbLib.h>
#include <hwif/buslib/vxbFdtLib.h>
#include <hwif/buslib/vxbSdhcLib.h>
#include <semLib.h>
#include <sysLib.h>

#include "vxbBm1684xSdhci.h"

/* ============================================================
 * 内部函数前向声明
 * ============================================================ */

LOCAL STATUS bm1684xSdhciProbe    (VXB_DEV_ID pDev);
LOCAL STATUS bm1684xSdhciAttach   (VXB_DEV_ID pDev);

LOCAL STATUS bm1684xSdhciClkSet   (VXB_SDHCI_HOST * pHost, UINT32 clkHz);
LOCAL STATUS bm1684xSdhciBusSet   (VXB_SDHCI_HOST * pHost, UINT32 width);
LOCAL STATUS bm1684xSdhciVoltSet  (VXB_SDHCI_HOST * pHost, UINT32 vddMv);
LOCAL STATUS bm1684xSdhciCmdIssue (VXB_SDHCI_HOST * pHost,
                                   VXB_SDHCI_CMD  * pCmd,
                                   VXB_SDHCI_DATA * pData);
LOCAL BOOL   bm1684xSdhciCardDetect(VXB_SDHCI_HOST * pHost);
LOCAL void   bm1684xSdhciIntrHandler(VXB_DEV_ID pDev);

LOCAL void   bm1684xSdhciPhyInit  (BM1684X_SDHCI_DRV * pDrv);
LOCAL STATUS bm1684xSdhciHwInit   (BM1684X_SDHCI_DRV * pDrv);
LOCAL UINT32 bm1684xSdhciGetClkHz (BM1684X_SDHCI_DRV * pDrv);

/* ============================================================
 * VxBus 驱动注册
 * ============================================================ */

/* FDT 设备匹配表：列出本驱动支持的 compatible 字符串 */
LOCAL VXB_FDT_DEV_MATCH_ENTRY bm1684xSdhciMatchTbl[] = {
    { "bitmain,synopsys-sdhc", NULL },
    {}    /* 结束标志 */
};

/* VxBus 方法表：绑定 probe 和 attach 回调 */
LOCAL VXB_DRV_METHOD bm1684xSdhciMethods[] = {
    { VXB_DEVMETHOD_CALL(vxbDevProbe),  (FUNCPTR)bm1684xSdhciProbe  },
    { VXB_DEVMETHOD_CALL(vxbDevAttach), (FUNCPTR)bm1684xSdhciAttach },
    VXB_DEVMETHOD_END
};

/* VxBus 驱动描述符，由 VXB_DRV_DEF 宏注册到内核驱动链表 */
VXB_DRV vxbBm1684xSdhciDrv = {
    { NULL },
    "bm1684xSdhci",                                      /* 驱动名称 */
    "BM1684X Synopsys DesignWare eMMC/SD SDHCI Controller", /* 驱动描述 */
    VXB_BUSID_FDT,                                       /* 总线类型：FDT */
    0,
    0,
    bm1684xSdhciMethods,
    NULL
};

VXB_DRV_DEF(vxbBm1684xSdhciDrv)

/* VxWorks SDHCI 主机操作函数表（由 SD/eMMC 协议栈调用） */
LOCAL VXB_SDHCI_OPS bm1684xSdhciOps = {
    .setClk       = bm1684xSdhciClkSet,     /* 设置时钟频率   */
    .setBusWidth  = bm1684xSdhciBusSet,     /* 设置总线宽度   */
    .setVdd       = bm1684xSdhciVoltSet,    /* 设置信号电压   */
    .issueCmd     = bm1684xSdhciCmdIssue,  /* 发送命令       */
    .cardDetect   = bm1684xSdhciCardDetect, /* 检测卡是否在位 */
};

/* ============================================================
 * 辅助函数：从 SoC MODE_SEL GPIO 读取输入时钟频率
 *
 * BM1684X TOP 寄存器偏移 0x4 的 [2:0] 位编码 MODE_SEL：
 *   0x0 = Normal  → 100 MHz
 *   0x1 = Fast    → 100 MHz
 *   0x2 = Safe    → 100 MHz
 *   0x3 = Bypass  →  25 MHz（旁路模式，用于调试）
 * ============================================================ */

LOCAL UINT32 bm1684xSdhciGetClkHz(BM1684X_SDHCI_DRV * pDrv)
{
    UINT32 confInfo;
    UINT32 modeSel;

    confInfo = vxbRead32(BM1684X_TOP_BASE + 0x04u);
    modeSel  = confInfo & 0x7u;  /* 取低 3 位 */

    if (modeSel == 0x3u)             /* 旁路模式：25 MHz */
        return 25000000UL;

    return BM1684X_EMMC_CLK_MAX_HZ;  /* 正常/快速/安全模式：100 MHz */
}

/* ============================================================
 * PHY 初始化（14 步序列）
 *
 * Synopsys PHY 需要按特定顺序配置 PAD 和延迟链。
 * eMMC（index=0）与 SD（index=1）仅在 SMPLDL_CNFG 寄存器上有差异：
 *   eMMC：INPSEL_CNFG=0x2（内部反馈路径），提高高速采样稳定性。
 *   SD  ：BYPASS_EN=1（旁路延迟链），适合 SD 卡自带时序参考的场景。
 * ============================================================ */

LOCAL void bm1684xSdhciPhyInit(BM1684X_SDHCI_DRV * pDrv)
{
    UINT32 reg32;
    int    loop;

    /* 步骤 1：全局软复位，清除控制器所有内部状态 */
    SDHCI_WR8(pDrv, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL);
    for (loop = 100; loop > 0; loop--) {
        if (SDHCI_RD8(pDrv, SDHCI_SOFTWARE_RESET) == 0u)
            break;
        sysUsDelay(10000);  /* 每次等待 10 ms */
    }

    /* 步骤 2：等待 PHY 上电完成（PHY_PWRGOOD 置 1） */
    for (loop = 100; loop > 0; loop--) {
        if (SDHCI_RD32(pDrv, SDHCI_P_PHY_CNFG) & (1u << PHY_CNFG_PHY_PWRGOOD))
            break;
        sysUsDelay(10000);
    }

    /* 步骤 3：拉低 PHY_RSTN，使 PHY 进入复位状态 */
    SDHCI_CLR32(pDrv, SDHCI_P_PHY_CNFG, (1u << PHY_CNFG_PHY_RSTN));

    /* 步骤 4：配置 PAD 驱动强度斜率（PAD_SP=0x9，PAD_SN=0x8） */
    reg32 = (1u        << PHY_CNFG_PHY_PWRGOOD) |
            (0x9u      << PHY_CNFG_PAD_SP)       |
            (0x8u      << PHY_CNFG_PAD_SN);
    SDHCI_WR32(pDrv, SDHCI_P_PHY_CNFG, reg32);

    /* 步骤 5：配置 CMD PAD（RXSEL=2，弱上拉，P 斜率=3，N 斜率=2） */
    SDHCI_WR16(pDrv, SDHCI_P_CMDPAD_CNFG,
               (0x2u << PAD_CNFG_RXSEL)         |
               (0x1u << PAD_CNFG_WEAKPULL_EN)   |
               (0x3u << PAD_CNFG_TXSLEW_CTRL_P) |
               (0x2u << PAD_CNFG_TXSLEW_CTRL_N));

    /* 步骤 6：配置 DAT PAD（与 CMD PAD 相同） */
    SDHCI_WR16(pDrv, SDHCI_P_DATPAD_CNFG,
               (0x2u << PAD_CNFG_RXSEL)         |
               (0x1u << PAD_CNFG_WEAKPULL_EN)   |
               (0x3u << PAD_CNFG_TXSLEW_CTRL_P) |
               (0x2u << PAD_CNFG_TXSLEW_CTRL_N));

    /* 步骤 7：配置 CLK PAD（无上下拉，RXSEL=2） */
    SDHCI_WR16(pDrv, SDHCI_P_CLKPAD_CNFG,
               (0x2u << PAD_CNFG_RXSEL)         |
               (0x3u << PAD_CNFG_TXSLEW_CTRL_P) |
               (0x2u << PAD_CNFG_TXSLEW_CTRL_N));

    /* 步骤 8：配置 STB PAD（弱下拉，用于 HS400 数据选通） */
    SDHCI_WR16(pDrv, SDHCI_P_STBPAD_CNFG,
               (0x2u << PAD_CNFG_RXSEL)         |
               (0x2u << PAD_CNFG_WEAKPULL_EN)   |  /* 2=弱下拉 */
               (0x3u << PAD_CNFG_TXSLEW_CTRL_P) |
               (0x2u << PAD_CNFG_TXSLEW_CTRL_N));

    /* 步骤 9：配置 RST_N PAD（弱上拉） */
    SDHCI_WR16(pDrv, SDHCI_P_RSTNPAD_CNFG,
               (0x2u << PAD_CNFG_RXSEL)         |
               (0x1u << PAD_CNFG_WEAKPULL_EN)   |
               (0x3u << PAD_CNFG_TXSLEW_CTRL_P) |
               (0x2u << PAD_CNFG_TXSLEW_CTRL_N));

    /* 步骤 10：使能 SD 时钟 DLL 的扩展延迟 */
    SDHCI_WR8(pDrv, SDHCI_P_SDCLKDL_CNFG, (1u << SDCLKDL_EXTDLY_EN));

    /* 步骤 11：设置时钟延迟步数（默认 0.1 ns，约 10 步 × 10 ps/步） */
    SDHCI_WR8(pDrv, SDHCI_P_SDCLKDL_DC, SDCLKDL_DC_DEFAULT);

    /* 步骤 12：配置采样延迟路径（eMMC 与 SD 在此处分叉） */
    if (pDrv->devIndex == BM1684X_EMMC_INDEX) {
        /*
         * eMMC：使用内部反馈路径（INPSEL_CNFG=0x2）。
         * 该路径将 SD 时钟环回作为采样参考，在高速 eMMC 模式下
         * 可有效减小建立/保持时间裕量的不确定性。
         */
        SDHCI_WR8(pDrv, SDHCI_P_SMPLDL_CNFG,
                  (0x2u << SMPLDL_INPSEL_CNFG));
    } else {
        /*
         * SD 卡：旁路采样延迟链（BYPASS_EN=1）。
         * SD 卡自带输出时序参考，无需额外延迟补偿。
         */
        SDHCI_WR8(pDrv, SDHCI_P_SMPLDL_CNFG,
                  (1u << SMPLDL_BYPASS_EN));
    }

    /* 步骤 13：自动调谐延迟使用内部路径（初始化阶段） */
    SDHCI_WR8(pDrv, SDHCI_P_ATDL_CNFG, (0x2u << ATDL_INPSEL_CNFG));

    /* 步骤 14：释放 PHY 复位（PHY_RSTN 置 1），PHY 开始正常工作 */
    SDHCI_SET32(pDrv, SDHCI_P_PHY_CNFG, (1u << PHY_CNFG_PHY_RSTN));
}

/* ============================================================
 * 硬件初始化（PHY 初始化完成后调用一次）
 * ============================================================ */

LOCAL STATUS bm1684xSdhciHwInit(BM1684X_SDHCI_DRV * pDrv)
{
    UINT16 hc2;
    UINT16 vendorOffset;

    /* 复位命令通道和数据通道，清除残留状态 */
    SDHCI_WR8(pDrv, SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);

    /* 上电，选择 3.3 V 信号电压 */
    SDHCI_WR8(pDrv, SDHCI_POWER_CONTROL, SDHCI_POWER_330 | SDHCI_POWER_ON);

    /* 超时控制：设为最大值 0xE（约 50 kHz TMCLK × 2^27 周期） */
    SDHCI_WR8(pDrv, SDHCI_TIMEOUT_CONTROL, 0x0Eu);

    /* 主机控制寄存器 2：使能版本 4 模式和 CMD23 预设块计数 */
    hc2 = SDHCI_RD16(pDrv, SDHCI_HOST_CONTROL2);
    hc2 |= SDHCI_HC2_CMD23_SUPPORT;
    hc2 |= SDHCI_HC2_VER4_ENABLE;
    SDHCI_WR16(pDrv, SDHCI_HOST_CONTROL2, hc2);

    /* 若请求 64 位 DMA 且硬件能力寄存器支持，则使能 64 位地址 */
    if (pDrv->is64BitAddr) {
        UINT32 caps = SDHCI_RD32(pDrv, SDHCI_CAPABILITIES2);
        if (caps & (1u << 27)) {   /* 能力寄存器 bit27：sys_addr_64 支持位 */
            SDHCI_SET16(pDrv, SDHCI_HOST_CONTROL2, SDHCI_HC2_64BIT_ADDR);
        }
    }

    /* 若硬件支持异步中断，则使能（降低中断延迟） */
    if (SDHCI_RD32(pDrv, SDHCI_CAPABILITIES2) & (1u << 29))
        SDHCI_SET16(pDrv, SDHCI_HOST_CONTROL2, SDHCI_HC2_ASYNC_INT);

    /* 使能全部普通及错误中断状态位（对 INT_STATUS 可见） */
    SDHCI_WR16(pDrv, SDHCI_INT_STATUS_EN,     0xFFFFu);
    SDHCI_WR16(pDrv, SDHCI_ERR_INT_STATUS_EN, 0xFFFFu);

    /* 使能关键中断信号：命令完成、传输完成、DMA 边界、错误 */
    SDHCI_WR16(pDrv, SDHCI_INT_SIGNAL_EN,
               SDHCI_INT_CMD_COMPLETE  |
               SDHCI_INT_XFER_COMPLETE |
               SDHCI_INT_DMA_END       |
               SDHCI_INT_ERROR);

    /* 通过厂商扩展寄存器将设备标记为 eMMC 模式 */
    vendorOffset = SDHCI_RD16(pDrv, SDHCI_VENDOR_SPECIFIC_AREA) & 0x0FFFu;
    pDrv->vendorBase = vendorOffset;
    SDHCI_SET16(pDrv, vendorOffset + SDHCI_EMMC_CTRL_R_OFFSET, 0x1u);

    return OK;
}

/* ============================================================
 * 时钟管理
 * ============================================================ */

LOCAL STATUS bm1684xSdhciClkSet(VXB_SDHCI_HOST * pHost, UINT32 clkHz)
{
    BM1684X_SDHCI_DRV * pDrv;
    UINT32              div;
    UINT16              clkCtrl;
    int                 i;

    pDrv = (BM1684X_SDHCI_DRV *)pHost->pDrvCtrl;

    if (clkHz == 0u) {
        /* 停止 SD 时钟输出（总线空闲时调用） */
        SDHCI_CLR16(pDrv, SDHCI_CLOCK_CONTROL, SDHCI_CLK_CARD_EN);
        return OK;
    }

    /* 计算分频比：SDHCI v3 10 位分频时钟模式，输出频率 = clkFreq / (2 × div) */
    if (pDrv->clkFreq <= clkHz) {
        div = 0u;  /* div=0 表示直通（/1） */
    } else {
        for (div = 1u; div <= 0xFFu; div++) {
            if ((pDrv->clkFreq / (2u * div)) <= clkHz)
                break;
        }
    }

    /* 修改分频值前先停止时钟，防止产生毛刺 */
    clkCtrl  = SDHCI_RD16(pDrv, SDHCI_CLOCK_CONTROL);
    clkCtrl &= ~(SDHCI_CLK_CARD_EN | SDHCI_CLK_PLL_EN | SDHCI_CLK_INT_EN);
    clkCtrl &= ~(SDHCI_CLK_DIV_MASK << SDHCI_CLK_DIV_SHIFT);
    clkCtrl &= ~SDHCI_CLK_GEN_SELECT;       /* 选择分频时钟模式 */
    clkCtrl |=  (div & SDHCI_CLK_DIV_MASK) << SDHCI_CLK_DIV_SHIFT;
    SDHCI_WR16(pDrv, SDHCI_CLOCK_CONTROL, clkCtrl);

    /* 使能内部时钟并等待稳定（最多等待 150 ms） */
    SDHCI_SET16(pDrv, SDHCI_CLOCK_CONTROL, SDHCI_CLK_INT_EN);
    for (i = 150000; i > 0; i -= 100) {
        if (SDHCI_RD16(pDrv, SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INT_STABLE)
            break;
        sysUsDelay(100);
    }
    if (i <= 0)
        return ERROR;  /* 内部时钟超时未稳定 */

    /* 使能 PLL 和 SD 时钟输出（至少等待 74 个初始化时钟周期） */
    SDHCI_SET16(pDrv, SDHCI_CLOCK_CONTROL,
                SDHCI_CLK_PLL_EN | SDHCI_CLK_CARD_EN);
    sysUsDelay(400);  /* 200 kHz 时 74 周期 ≈ 370 µs，取 400 µs 留余量 */

    return OK;
}

/* ============================================================
 * 总线宽度设置
 * ============================================================ */

LOCAL STATUS bm1684xSdhciBusSet(VXB_SDHCI_HOST * pHost, UINT32 width)
{
    BM1684X_SDHCI_DRV * pDrv;
    UINT8               hc1;

    pDrv = (BM1684X_SDHCI_DRV *)pHost->pDrvCtrl;
    hc1  = SDHCI_RD8(pDrv, SDHCI_HOST_CONTROL);

    switch (width) {
    case 1u:
        /* 1 位模式：清除 4 位和 8 位标志 */
        hc1 &= ~SDHCI_CTRL_4BITBUS;
        hc1 &= ~SDHCI_CTRL_8BITBUS;
        break;
    case 4u:
        /* 4 位模式：置位 4 位标志，清除 8 位标志 */
        hc1 |=  SDHCI_CTRL_4BITBUS;
        hc1 &= ~SDHCI_CTRL_8BITBUS;
        break;
    case 8u:
        /* 8 位模式：清除 4 位标志，置位 8 位标志（eMMC 专用） */
        hc1 &= ~SDHCI_CTRL_4BITBUS;
        hc1 |=  SDHCI_CTRL_8BITBUS;
        break;
    default:
        return ERROR;  /* 不支持的总线宽度 */
    }

    SDHCI_WR8(pDrv, SDHCI_HOST_CONTROL, hc1);
    return OK;
}

/* ============================================================
 * 电压选择（1.8 V / 3.3 V 信号电平切换）
 * ============================================================ */

LOCAL STATUS bm1684xSdhciVoltSet(VXB_SDHCI_HOST * pHost, UINT32 vddMv)
{
    BM1684X_SDHCI_DRV * pDrv;

    pDrv = (BM1684X_SDHCI_DRV *)pHost->pDrvCtrl;

    if (vddMv == 1800u) {
        /* 切换至 1.8 V 信号电平（UHS-I / eMMC HS200/HS400 模式需要） */
        SDHCI_SET16(pDrv, SDHCI_HOST_CONTROL2, SDHCI_HC2_1V8_SIGNALING);
    } else {
        /* 恢复至 3.3 V 信号电平 */
        SDHCI_CLR16(pDrv, SDHCI_HOST_CONTROL2, SDHCI_HC2_1V8_SIGNALING);
    }

    return OK;
}

/* ============================================================
 * 卡检测
 * ============================================================ */

LOCAL BOOL bm1684xSdhciCardDetect(VXB_SDHCI_HOST * pHost)
{
    BM1684X_SDHCI_DRV * pDrv;

    pDrv = (BM1684X_SDHCI_DRV *)pHost->pDrvCtrl;

    /* eMMC 焊接在板上，固定返回 TRUE */
    if (pDrv->devIndex == BM1684X_EMMC_INDEX)
        return TRUE;

    /* SD 卡：读取当前状态寄存器的卡检测位 */
    return (SDHCI_RD32(pDrv, SDHCI_PRESENT_STATE) & SDHCI_STATE_CARD_PRESENT)
           ? TRUE : FALSE;
}

/* ============================================================
 * 命令及数据传输
 * ============================================================ */

LOCAL STATUS bm1684xSdhciCmdIssue(VXB_SDHCI_HOST * pHost,
                                  VXB_SDHCI_CMD  * pCmd,
                                  VXB_SDHCI_DATA * pData)
{
    BM1684X_SDHCI_DRV * pDrv;
    UINT16              cmdFlags  = 0u;
    UINT16              xferMode  = 0u;
    UINT32              timeout;
    UINT16              intStatus;

    pDrv = (BM1684X_SDHCI_DRV *)pHost->pDrvCtrl;

    /* 等待 CMD 和 DAT 通道均空闲（最多 100 ms） */
    for (timeout = 100000u; timeout > 0u; timeout--) {
        if (!(SDHCI_RD32(pDrv, SDHCI_PRESENT_STATE) &
              (SDHCI_STATE_CMD_INHIBIT | SDHCI_STATE_DAT_INHIBIT)))
            break;
        sysUsDelay(1);
    }
    if (timeout == 0u)
        return ERROR;  /* 总线一直忙，超时退出 */

    /* 根据响应类型构建命令标志位 */
    if (pCmd->respType == VXB_SDHCI_RSP_NONE) {
        cmdFlags = SDHCI_CMD_RESP_NONE;       /* 无响应（CMD0） */
    } else if (pCmd->respType & VXB_SDHCI_RSP_136) {
        cmdFlags = SDHCI_CMD_RESP_LONG;       /* 136 位响应（R2） */
    } else if (pCmd->respType & VXB_SDHCI_RSP_BUSY) {
        cmdFlags = SDHCI_CMD_RESP_SHORT_BUSY; /* 带忙碌信号的短响应（R1b） */
    } else {
        cmdFlags = SDHCI_CMD_RESP_SHORT;      /* 标准 48 位短响应 */
    }

    if (pCmd->respType & VXB_SDHCI_RSP_CRC)
        cmdFlags |= SDHCI_CMD_CRC;      /* 使能 CRC 校验 */
    if (pCmd->respType & VXB_SDHCI_RSP_CMDIDX)
        cmdFlags |= SDHCI_CMD_INDEX_CHK; /* 使能命令索引检查 */

    /* 若有数据传输，配置数据相关寄存器 */
    if (pData != NULL) {
        UINT32 blkCnt  = pData->blkCount;
        UINT32 blkSize = pData->blkSize;

        cmdFlags |= SDHCI_CMD_DATA;                          /* 命令携带数据 */
        xferMode  = SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_MULTI; /* 多块传输   */
        if (pData->flags & VXB_SDHCI_DATA_READ)
            xferMode |= SDHCI_TRNS_READ;                    /* 读方向       */

        /* 使用 SDMA 模式 */
        xferMode |= SDHCI_TRNS_DMA;

        /* 设置 DMA 模式为 SDMA */
        UINT8 hostCtrl = SDHCI_RD8(pDrv, SDHCI_HOST_CONTROL);
        hostCtrl = (hostCtrl & ~SDHCI_CTRL_DMA_MASK) | SDHCI_CTRL_SDMA;
        SDHCI_WR8(pDrv, SDHCI_HOST_CONTROL, hostCtrl);

        if (SDHCI_RD16(pDrv, SDHCI_HOST_CONTROL2) & SDHCI_HC2_64BIT_ADDR) {
            /* 64 位 SDMA：DMA 地址写入 ADMA_SA，块计数写入 DMA_ADDRESS */
            SDHCI_WR32(pDrv, SDHCI_ADMA_SA_LOW,
                       (UINT32)((PHYS_ADDR)pData->pBuf));
            SDHCI_WR32(pDrv, SDHCI_ADMA_SA_HIGH,
                       (UINT32)(((PHYS_ADDR)pData->pBuf) >> 32));
            SDHCI_WR32(pDrv, SDHCI_DMA_ADDRESS, blkCnt);
            SDHCI_WR16(pDrv, SDHCI_BLOCK_COUNT, 0u);
        } else {
            /* 32 位 SDMA：DMA 地址和块计数按标准方式写入 */
            SDHCI_WR32(pDrv, SDHCI_DMA_ADDRESS, (UINT32)pData->pBuf);
            SDHCI_WR16(pDrv, SDHCI_BLOCK_COUNT, (UINT16)blkCnt);
        }

        /* 块大小：SDMA 边界设为 512 KB（exponent=7） */
        SDHCI_WR16(pDrv, SDHCI_BLOCK_SIZE,
                   SDHCI_MAKE_BLKSZ(7u, blkSize));
        SDHCI_WR16(pDrv, SDHCI_TRANSFER_MODE, xferMode);
    }

    /* 写入命令参数和命令寄存器，触发命令发送 */
    SDHCI_WR32(pDrv, SDHCI_ARGUMENT, pCmd->arg);
    SDHCI_WR16(pDrv, SDHCI_COMMAND,
               SDHCI_MAKE_CMD(pCmd->cmdIndex, cmdFlags));

    /* 等待命令完成（cmdSem 由 ISR 在 CMD_COMPLETE 中断时释放，超时 1 s） */
    if (semTake(pDrv->cmdSem, sysClkRateGet()) != OK)
        return ERROR;

    /* 检查是否存在错误中断 */
    intStatus = SDHCI_RD16(pDrv, SDHCI_INT_STATUS);
    if (intStatus & SDHCI_INT_ERROR) {
        /* 清除全部中断状态位，并报告错误 */
        SDHCI_WR16(pDrv, SDHCI_INT_STATUS,     0xFFFFu);
        SDHCI_WR16(pDrv, SDHCI_ERR_INT_STATUS, 0xFFFFu);
        return ERROR;
    }

    /* 读取响应寄存器 */
    if (!(cmdFlags == SDHCI_CMD_RESP_NONE)) {
        pCmd->resp[0] = SDHCI_RD32(pDrv, SDHCI_RESPONSE_0);
        if (cmdFlags == SDHCI_CMD_RESP_LONG) {
            /* 136 位响应需读取全部 4 个响应字 */
            pCmd->resp[1] = SDHCI_RD32(pDrv, SDHCI_RESPONSE_1);
            pCmd->resp[2] = SDHCI_RD32(pDrv, SDHCI_RESPONSE_2);
            pCmd->resp[3] = SDHCI_RD32(pDrv, SDHCI_RESPONSE_3);
        }
    }

    /* 若为数据命令，等待传输完成（xferSem 由 ISR 在 XFER_COMPLETE 时释放，超时 10 s） */
    if (pData != NULL) {
        if (semTake(pDrv->xferSem, sysClkRateGet() * 10) != OK)
            return ERROR;

        intStatus = SDHCI_RD16(pDrv, SDHCI_INT_STATUS);
        if (intStatus & SDHCI_INT_ERROR) {
            SDHCI_WR16(pDrv, SDHCI_INT_STATUS,     0xFFFFu);
            SDHCI_WR16(pDrv, SDHCI_ERR_INT_STATUS, 0xFFFFu);
            return ERROR;
        }
    }

    return OK;
}

/* ============================================================
 * 中断服务例程
 * ============================================================ */

LOCAL void bm1684xSdhciIntrHandler(VXB_DEV_ID pDev)
{
    BM1684X_SDHCI_DRV * pDrv;
    UINT16              intStatus;
    UINT32              dmaAddr;

    pDrv = (BM1684X_SDHCI_DRV *)vxbDevSoftcGet(pDev);
    if (pDrv == NULL)
        return;

    /* 读取并立即清除所有待处理中断状态（写 1 清除） */
    intStatus = SDHCI_RD16(pDrv, SDHCI_INT_STATUS);
    SDHCI_WR16(pDrv, SDHCI_INT_STATUS, intStatus);

    if (intStatus & SDHCI_INT_ERROR) {
        /* 发生错误：清除错误状态，同时释放两个信号量让等待任务感知错误 */
        SDHCI_WR16(pDrv, SDHCI_ERR_INT_STATUS,
                   SDHCI_RD16(pDrv, SDHCI_ERR_INT_STATUS));
        (void)semGive(pDrv->cmdSem);
        (void)semGive(pDrv->xferSem);
        return;
    }

    /* 命令完成：唤醒 bm1684xSdhciCmdIssue 中等待命令完成的任务 */
    if (intStatus & SDHCI_INT_CMD_COMPLETE)
        (void)semGive(pDrv->cmdSem);

    if (intStatus & SDHCI_INT_DMA_END) {
        /* SDMA 512 KB 边界中断：重新写入 DMA 地址以继续传输 */
        if (SDHCI_RD16(pDrv, SDHCI_HOST_CONTROL2) & SDHCI_HC2_64BIT_ADDR) {
            /* 64 位模式：更新 ADMA_SA 寄存器 */
            dmaAddr = SDHCI_RD32(pDrv, SDHCI_ADMA_SA_LOW);
            SDHCI_WR32(pDrv, SDHCI_ADMA_SA_LOW,  dmaAddr);
            SDHCI_WR32(pDrv, SDHCI_ADMA_SA_HIGH, 0u);
        } else {
            /* 32 位模式：更新 DMA_ADDRESS 寄存器 */
            dmaAddr = SDHCI_RD32(pDrv, SDHCI_DMA_ADDRESS);
            SDHCI_WR32(pDrv, SDHCI_DMA_ADDRESS, dmaAddr);
        }
    }

    /* 数据传输完成：唤醒 bm1684xSdhciCmdIssue 中等待传输完成的任务 */
    if (intStatus & SDHCI_INT_XFER_COMPLETE)
        (void)semGive(pDrv->xferSem);
}

/* ============================================================
 * VxBus probe 回调：判断 FDT compatible 字符串是否匹配
 * ============================================================ */

LOCAL STATUS bm1684xSdhciProbe(VXB_DEV_ID pDev)
{
    return vxbFdtDevMatch(pDev, bm1684xSdhciMatchTbl, NULL);
}

/* ============================================================
 * VxBus attach 回调：资源分配、硬件初始化、向协议栈注册
 * ============================================================ */

LOCAL STATUS bm1684xSdhciAttach(VXB_DEV_ID pDev)
{
    BM1684X_SDHCI_DRV * pDrv;
    VXB_FDT_DEV       * pFdt;
    const void        * pProp;
    int                 propLen;

    /* 分配驱动私有数据结构体 */
    pDrv = (BM1684X_SDHCI_DRV *)vxbMemAlloc(sizeof(BM1684X_SDHCI_DRV));
    if (pDrv == NULL)
        return ERROR;

    pDrv->pDev = pDev;
    vxbDevSoftcSet(pDev, pDrv);  /* 将私有数据挂载到 VxBus 设备句柄 */

    /* 从 FDT 节点读取属性 */
    pFdt = vxbFdtDevGet(pDev);

    /* 读取 index 属性：0=eMMC，1=SD；缺省为 eMMC */
    pProp = vxFdtPropGet(pFdt->offset, "index", &propLen);
    pDrv->devIndex = (pProp != NULL)
                     ? (UINT32)vxFdt32ToCpu(*(const UINT32 *)pProp)
                     : BM1684X_EMMC_INDEX;

    /* 读取 max-frequency 属性；缺省为 100 MHz */
    pProp = vxFdtPropGet(pFdt->offset, "max-frequency", &propLen);
    pDrv->clkFreq = (pProp != NULL)
                    ? (UINT32)vxFdt32ToCpu(*(const UINT32 *)pProp)
                    : BM1684X_EMMC_CLK_MAX_HZ;

    /* 读取布尔属性：is_phy 表示需要 PHY 初始化，64_addressing 表示 64 位 DMA */
    pDrv->hasPhyCfg   = (vxFdtPropGet(pFdt->offset, "has_phy",       NULL) != NULL);
    pDrv->is64BitAddr = (vxFdtPropGet(pFdt->offset, "64_addressing", NULL) != NULL);

    /* 从 SoC MODE_SEL 引脚实际读取输入时钟频率（覆盖 FDT 值） */
    pDrv->clkFreq = bm1684xSdhciGetClkHz(pDrv);

    /* 分配并映射 MMIO 寄存器资源 */
    pDrv->pRegRes = vxbResourceAlloc(pDev, VXB_RES_MEMORY, 0);
    if (pDrv->pRegRes == NULL)
        goto errFree;

    pDrv->regBase = (VIRT_ADDR)VXB_RES_ADR(pDrv->pRegRes);

    /* 分配中断资源 */
    pDrv->pIrqRes = vxbResourceAlloc(pDev, VXB_RES_IRQ, 0);
    if (pDrv->pIrqRes == NULL)
        goto errFreeReg;

    /* 创建命令完成和数据传输完成信号量（二值信号量，初始为空） */
    pDrv->cmdSem  = semBCreate(SEM_Q_FIFO, SEM_EMPTY);
    pDrv->xferSem = semBCreate(SEM_Q_FIFO, SEM_EMPTY);
    if (pDrv->cmdSem == NULL || pDrv->xferSem == NULL)
        goto errFreeIrq;

    /* PHY 初始化（必须在 hwInit 之前完成） */
    if (pDrv->hasPhyCfg)
        bm1684xSdhciPhyInit(pDrv);

    /* 核心硬件初始化（配置中断、DMA、版本模式等） */
    if (bm1684xSdhciHwInit(pDrv) != OK)
        goto errFreeSem;

    /* 以识别频率（200 kHz）启动时钟，等待卡响应 */
    (void)bm1684xSdhciClkSet(&pDrv->sdhciHost, BM1684X_EMMC_CLK_INIT_HZ);

    /* 注册中断处理函数并使能中断 */
    if (vxbIntConnect(pDev, pDrv->pIrqRes,
                      (VOIDFUNCPTR)bm1684xSdhciIntrHandler, pDev) != OK)
        goto errFreeSem;

    if (vxbIntEnable(pDev, pDrv->pIrqRes) != OK)
        goto errFreeSem;

    /* 填充 VxWorks SDHCI 主机描述符 */
    pDrv->sdhciHost.pDrvCtrl = pDrv;
    pDrv->sdhciHost.pOps     = &bm1684xSdhciOps;
    pDrv->sdhciHost.clkMax   = (pDrv->devIndex == BM1684X_EMMC_INDEX)
                                ? BM1684X_EMMC_CLK_MAX_HZ  /* eMMC 最高 100 MHz */
                                : BM1684X_SD_CLK_MAX_HZ;   /* SD 最高 50 MHz    */
    pDrv->sdhciHost.clkMin   = BM1684X_EMMC_CLK_INIT_HZ;  /* 最低 200 kHz      */

    /* 向 VxWorks SD/eMMC 协议栈注册本控制器 */
    if (vxbSdhciHostRegister(pDev, &pDrv->sdhciHost) != OK)
        goto errFreeSem;

    return OK;

/* 错误处理：按分配顺序逆序释放资源 */
errFreeSem:
    if (pDrv->cmdSem  != NULL) semDelete(pDrv->cmdSem);
    if (pDrv->xferSem != NULL) semDelete(pDrv->xferSem);
errFreeIrq:
    vxbResourceFree(pDev, pDrv->pIrqRes);
errFreeReg:
    vxbResourceFree(pDev, pDrv->pRegRes);
errFree:
    vxbMemFree(pDrv);
    return ERROR;
}
