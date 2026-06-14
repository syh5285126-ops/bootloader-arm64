/* vxbBm1684xSdhci.h - BM1684X Synopsys DesignWare SDHCI VxBus 驱动头文件 */

/*
 * 版权所有 (c) 2024 Bitmain / Sophgo
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 本文件为 BM1684X eMMC/SD 控制器的 VxWorks 7 VxBus FDT 驱动头文件。
 * FDT compatible 字符串：bitmain,synopsys-sdhc
 *
 * 编译本驱动需要以下 VxWorks SDK 头文件（用户环境须自行提供）：
 *   #include <vxWorks.h>
 *   #include <vxBus.h>
 *   #include <hwif/vxbus/vxbLib.h>
 *   #include <hwif/buslib/vxbFdtLib.h>
 *   #include <hwif/buslib/vxbSdhcLib.h>
 *   #include <semLib.h>
 */

#ifndef __VXB_BM1684X_SDHCI_H__
#define __VXB_BM1684X_SDHCI_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * BM1684X SoC 物理基地址（TOP_BASE = 0x50010000）
 * --------------------------------------------------------------------------- */

#define BM1684X_EMMC_BASE           0x50100000UL  /* eMMC 控制器物理基地址 */
#define BM1684X_SD_BASE             0x50101000UL  /* SD 控制器物理基地址   */
#define BM1684X_TOP_BASE            0x50010000UL  /* TOP 寄存器物理基地址  */

#define BM1684X_TOP_SOFT_RST0       (BM1684X_TOP_BASE + 0xC00)   /* 软复位寄存器 0   */
#define BM1684X_TOP_CLOCK_ENABLE0   (BM1684X_TOP_BASE + 0x800)   /* 时钟使能寄存器 0 */

/* 软复位位（写 0 复位，写 1 释放） */
#define BM1684X_RST0_EMMC           (1u << 20)  /* eMMC 复位位 */
#define BM1684X_RST0_SD             (1u << 21)  /* SD 复位位   */

/* eMMC 时钟使能位 */
#define BM1684X_CLK_EMMC_200M       (1u << 6)   /* eMMC 200 MHz 时钟 */
#define BM1684X_CLK_AXI_EMMC        (1u << 20)  /* eMMC AXI 总线时钟 */
#define BM1684X_CLK_100K_EMMC       (1u << 22)  /* eMMC 100 kHz 时钟 */

/* ---------------------------------------------------------------------------
 * 标准 SDHCI 寄存器偏移（JEDEC SD Host Controller spec v4）
 * --------------------------------------------------------------------------- */

#define SDHCI_DMA_ADDRESS           0x00u  /* SDMA 系统地址（低 32 位） */
#define SDHCI_BLOCK_SIZE            0x04u  /* 块大小                    */
/* 块大小寄存器编码：dma 为 SDMA 边界 exponent，sz 为字节块大小 */
#define SDHCI_MAKE_BLKSZ(dma, sz)   ((((dma) & 0x7u) << 12) | ((sz) & 0xFFFu))
#define SDHCI_BLOCK_COUNT           0x06u  /* 块计数                    */
#define SDHCI_ARGUMENT              0x08u  /* 命令参数                  */
#define SDHCI_TRANSFER_MODE         0x0Cu  /* 传输模式                  */
#define SDHCI_COMMAND               0x0Eu  /* 命令寄存器                */
#define SDHCI_RESPONSE_0            0x10u  /* 响应字 0                  */
#define SDHCI_RESPONSE_1            0x14u  /* 响应字 1                  */
#define SDHCI_RESPONSE_2            0x18u  /* 响应字 2                  */
#define SDHCI_RESPONSE_3            0x1Cu  /* 响应字 3                  */
#define SDHCI_BUF_DATA              0x20u  /* 数据缓冲区端口            */
#define SDHCI_PRESENT_STATE         0x24u  /* 当前状态寄存器            */
#define SDHCI_HOST_CONTROL          0x28u  /* 主机控制寄存器 1（8 位）  */
#define SDHCI_POWER_CONTROL         0x29u  /* 电源控制寄存器（8 位）    */
#define SDHCI_BLOCK_GAP_CONTROL     0x2Au  /* 块间隔控制（8 位）        */
#define SDHCI_CLOCK_CONTROL         0x2Cu  /* 时钟控制（16 位）         */
#define SDHCI_TIMEOUT_CONTROL       0x2Eu  /* 超时控制（8 位）          */
#define SDHCI_SOFTWARE_RESET        0x2Fu  /* 软件复位（8 位）          */
#define SDHCI_INT_STATUS            0x30u  /* 普通中断状态（16 位）     */
#define SDHCI_ERR_INT_STATUS        0x32u  /* 错误中断状态（16 位）     */
#define SDHCI_INT_STATUS_EN         0x34u  /* 普通中断状态使能（16 位） */
#define SDHCI_ERR_INT_STATUS_EN     0x36u  /* 错误中断状态使能（16 位） */
#define SDHCI_INT_SIGNAL_EN         0x38u  /* 普通中断信号使能（16 位） */
#define SDHCI_HOST_CONTROL2         0x3Eu  /* 主机控制寄存器 2（16 位） */
#define SDHCI_CAPABILITIES          0x40u  /* 能力寄存器 1              */
#define SDHCI_CAPABILITIES2         0x44u  /* 能力寄存器 2              */
#define SDHCI_ADMA_SA_LOW           0x58u  /* ADMA 系统地址低 32 位     */
#define SDHCI_ADMA_SA_HIGH          0x5Cu  /* ADMA 系统地址高 32 位     */
#define SDHCI_VENDOR_SPECIFIC_AREA  0xE8u  /* 厂商扩展区域基址指针      */
#define SDHCI_HOST_VERSION          0xFEu  /* 主机控制器版本            */

/* eMMC 控制寄存器偏移（相对于厂商扩展区域基址 + 0x2C） */
#define SDHCI_EMMC_CTRL_R_OFFSET    0x2Cu

/* ---------------------------------------------------------------------------
 * 传输模式寄存器位域（偏移 0x0C）
 * --------------------------------------------------------------------------- */

#define SDHCI_TRNS_DMA              (1u << 0)  /* DMA 使能          */
#define SDHCI_TRNS_BLK_CNT_EN      (1u << 1)  /* 块计数器使能      */
#define SDHCI_TRNS_AUTO_CMD12       (1u << 2)  /* 自动发送 CMD12    */
#define SDHCI_TRNS_READ             (1u << 4)  /* 读方向（卡→主机） */
#define SDHCI_TRNS_MULTI            (1u << 5)  /* 多块传输          */
#define SDHCI_TRNS_RESP_INT         (1u << 8)  /* 响应中断使能      */

/* ---------------------------------------------------------------------------
 * 命令寄存器位域（偏移 0x0E）
 * --------------------------------------------------------------------------- */

#define SDHCI_CMD_RESP_NONE         0x00u  /* 无响应              */
#define SDHCI_CMD_RESP_LONG         0x01u  /* 136 位长响应        */
#define SDHCI_CMD_RESP_SHORT        0x02u  /* 48 位短响应         */
#define SDHCI_CMD_RESP_SHORT_BUSY   0x03u  /* 48 位短响应 + 忙碌  */
#define SDHCI_CMD_CRC               (1u << 3)  /* CRC 校验使能       */
#define SDHCI_CMD_INDEX_CHK         (1u << 4)  /* 命令索引检查使能   */
#define SDHCI_CMD_DATA              (1u << 5)  /* 带数据传输         */
/* 构建命令寄存器值：高 8 位为命令索引，低 8 位为标志 */
#define SDHCI_MAKE_CMD(c, f)        ((((c) & 0xFFu) << 8) | ((f) & 0xFFu))

/* ---------------------------------------------------------------------------
 * 当前状态寄存器位域（偏移 0x24）
 * --------------------------------------------------------------------------- */

#define SDHCI_STATE_CMD_INHIBIT     (1u << 0)   /* CMD 通道忙   */
#define SDHCI_STATE_DAT_INHIBIT     (1u << 1)   /* DAT 通道忙   */
#define SDHCI_STATE_BUF_WR_EN       (1u << 10)  /* 写缓冲区就绪 */
#define SDHCI_STATE_BUF_RD_EN       (1u << 11)  /* 读缓冲区就绪 */
#define SDHCI_STATE_CARD_PRESENT    (1u << 16)  /* 卡已插入     */

/* ---------------------------------------------------------------------------
 * 主机控制寄存器 1 位域（偏移 0x28，8 位）
 * --------------------------------------------------------------------------- */

#define SDHCI_CTRL_4BITBUS          (1u << 1)  /* 4 位总线模式         */
#define SDHCI_CTRL_8BITBUS          (1u << 5)  /* 8 位总线模式（eMMC） */
#define SDHCI_CTRL_DMA_MASK         0x18u      /* DMA 模式位掩码       */
#define SDHCI_CTRL_SDMA             0x00u      /* 使用 SDMA            */
#define SDHCI_CTRL_ADMA32           0x10u      /* 使用 ADMA2 32 位     */
#define SDHCI_CTRL_ADMA64           0x18u      /* 使用 ADMA2 64 位     */

/* ---------------------------------------------------------------------------
 * 电源控制寄存器位域（偏移 0x29，8 位）
 * --------------------------------------------------------------------------- */

#define SDHCI_POWER_ON              (1u << 0)  /* 总线上电     */
#define SDHCI_POWER_330             0x0Eu      /* 选择 3.3 V   */
#define SDHCI_POWER_300             0x0Cu      /* 选择 3.0 V   */
#define SDHCI_POWER_180             0x0Au      /* 选择 1.8 V   */

/* ---------------------------------------------------------------------------
 * 时钟控制寄存器位域（偏移 0x2C，16 位）
 * --------------------------------------------------------------------------- */

#define SDHCI_CLK_INT_EN            (1u << 0)  /* 内部时钟使能              */
#define SDHCI_CLK_INT_STABLE        (1u << 1)  /* 内部时钟稳定标志          */
#define SDHCI_CLK_CARD_EN           (1u << 2)  /* 向卡输出时钟使能          */
#define SDHCI_CLK_PLL_EN            (1u << 3)  /* PLL 使能                  */
#define SDHCI_CLK_GEN_SELECT        (1u << 5)  /* 时钟模式：0=分频，1=可编程 */
#define SDHCI_CLK_DIV_SHIFT         8          /* 分频值字段起始位          */
#define SDHCI_CLK_DIV_MASK          0xFFu      /* 分频值字段掩码            */

/* ---------------------------------------------------------------------------
 * 软件复位寄存器位域（偏移 0x2F，8 位）
 * --------------------------------------------------------------------------- */

#define SDHCI_RESET_ALL             0x01u  /* 复位全部逻辑    */
#define SDHCI_RESET_CMD             0x02u  /* 仅复位命令通道  */
#define SDHCI_RESET_DATA            0x04u  /* 仅复位数据通道  */

/* ---------------------------------------------------------------------------
 * 中断状态 / 使能寄存器位域（偏移 0x30 / 0x34）
 * --------------------------------------------------------------------------- */

#define SDHCI_INT_CMD_COMPLETE      (1u << 0)   /* 命令完成           */
#define SDHCI_INT_XFER_COMPLETE     (1u << 1)   /* 数据传输完成       */
#define SDHCI_INT_DMA_END           (1u << 3)   /* SDMA 边界中断      */
#define SDHCI_INT_BUF_WR_READY     (1u << 4)   /* 写缓冲区就绪中断   */
#define SDHCI_INT_BUF_RD_READY     (1u << 5)   /* 读缓冲区就绪中断   */
#define SDHCI_INT_CARD_INSERT       (1u << 6)   /* 卡插入中断         */
#define SDHCI_INT_CARD_REMOVE       (1u << 7)   /* 卡拔出中断         */
#define SDHCI_INT_ERROR             (1u << 15)  /* 错误中断汇总位     */

/* 数据传输相关中断的组合掩码 */
#define SDHCI_INT_DATA_MASK         (SDHCI_INT_XFER_COMPLETE | \
                                     SDHCI_INT_DMA_END       | \
                                     SDHCI_INT_BUF_WR_READY  | \
                                     SDHCI_INT_BUF_RD_READY)

#define SDHCI_INT_NORMAL_MASK       0x00FFu  /* 全部普通中断状态位 */
#define SDHCI_INT_ERROR_MASK        0xFFFFu  /* 全部错误中断状态位 */

/* ---------------------------------------------------------------------------
 * 主机控制寄存器 2 位域（偏移 0x3E，16 位）
 * --------------------------------------------------------------------------- */

#define SDHCI_HC2_UHS_SDR12         0x0000u     /* UHS-I SDR12 模式        */
#define SDHCI_HC2_UHS_SDR25         0x0001u     /* UHS-I SDR25 模式        */
#define SDHCI_HC2_UHS_SDR50         0x0002u     /* UHS-I SDR50 模式        */
#define SDHCI_HC2_UHS_SDR104        0x0003u     /* UHS-I SDR104 模式       */
#define SDHCI_HC2_UHS_DDR50         0x0004u     /* UHS-I DDR50 模式        */
#define SDHCI_HC2_HS400             0x0005u     /* eMMC HS400 模式         */
#define SDHCI_HC2_UHS_MASK          0x0007u     /* 速度模式位掩码          */
#define SDHCI_HC2_1V8_SIGNALING     (1u << 3)   /* 切换至 1.8 V 信号电平   */
#define SDHCI_HC2_DRV_TYPE_A        (1u << 4)   /* 驱动类型 A              */
#define SDHCI_HC2_DRV_TYPE_C        (2u << 4)   /* 驱动类型 C              */
#define SDHCI_HC2_DRV_TYPE_D        (3u << 4)   /* 驱动类型 D              */
#define SDHCI_HC2_EXEC_TUNING       (1u << 6)   /* 执行调谐序列            */
#define SDHCI_HC2_TUNED_CLK         (1u << 7)   /* 采样时钟已调谐完成      */
#define SDHCI_HC2_CMD23_SUPPORT     (1u << 11)  /* CMD23 预设块计数使能    */
#define SDHCI_HC2_VER4_ENABLE       (1u << 12)  /* 版本 4 模式使能         */
#define SDHCI_HC2_64BIT_ADDR        (1u << 13)  /* 64 位系统地址使能       */
#define SDHCI_HC2_ASYNC_INT         (1u << 14)  /* 异步中断使能            */
#define SDHCI_HC2_PRESET_VAL_EN     (1u << 15)  /* 预设值使能              */

/* ---------------------------------------------------------------------------
 * Synopsys PHY 寄存器（控制器基址 + 0x300）
 * --------------------------------------------------------------------------- */

#define SDHCI_PHY_BASE              0x300u  /* PHY 寄存器区域起始偏移 */

#define SDHCI_P_PHY_CNFG            (SDHCI_PHY_BASE + 0x00u)   /* PHY 总体配置（32 位）  */
#define SDHCI_P_CMDPAD_CNFG         (SDHCI_PHY_BASE + 0x04u)   /* CMD PAD 配置（16 位）  */
#define SDHCI_P_DATPAD_CNFG         (SDHCI_PHY_BASE + 0x06u)   /* DAT PAD 配置（16 位）  */
#define SDHCI_P_CLKPAD_CNFG         (SDHCI_PHY_BASE + 0x08u)   /* CLK PAD 配置（16 位）  */
#define SDHCI_P_STBPAD_CNFG         (SDHCI_PHY_BASE + 0x0Au)   /* STB PAD 配置（16 位）  */
#define SDHCI_P_RSTNPAD_CNFG        (SDHCI_PHY_BASE + 0x0Cu)   /* RST_N PAD 配置（16 位）*/
#define SDHCI_P_COMMDL_CNFG         (SDHCI_PHY_BASE + 0x1Cu)   /* CMD 延迟配置（8 位）   */
#define SDHCI_P_SDCLKDL_CNFG        (SDHCI_PHY_BASE + 0x1Du)   /* SD 时钟延迟配置（8 位）*/
#define SDHCI_P_SDCLKDL_DC          (SDHCI_PHY_BASE + 0x1Eu)   /* SD 时钟延迟步数（8 位）*/
#define SDHCI_P_SMPLDL_CNFG         (SDHCI_PHY_BASE + 0x20u)   /* 采样延迟配置（8 位）   */
#define SDHCI_P_ATDL_CNFG           (SDHCI_PHY_BASE + 0x21u)   /* AT 延迟配置（8 位）    */
#define SDHCI_P_DLL_CTRL            (SDHCI_PHY_BASE + 0x24u)   /* DLL 控制               */
#define SDHCI_P_DLL_CNFG1           (SDHCI_PHY_BASE + 0x25u)   /* DLL 配置 1             */
#define SDHCI_P_DLL_CNFG2           (SDHCI_PHY_BASE + 0x26u)   /* DLL 配置 2             */
#define SDHCI_P_DLL_STATUS          (SDHCI_PHY_BASE + 0x2Eu)   /* DLL 状态               */

/* PHY_CNFG 字段位偏移 */
#define PHY_CNFG_PHY_RSTN           0u    /* PHY 复位（低有效）       */
#define PHY_CNFG_PHY_PWRGOOD        1u    /* PHY 上电完成标志         */
#define PHY_CNFG_PAD_SP             16u   /* PAD P 型晶体管斜率（4位）*/
#define PHY_CNFG_PAD_SN             20u   /* PAD N 型晶体管斜率（4位）*/

/* PAD_CNFG 字段位偏移（CMD/DAT/CLK/STB/RSTN PAD 共用此布局） */
#define PAD_CNFG_RXSEL              0u    /* 接收器选择（3 位）       */
#define PAD_CNFG_WEAKPULL_EN        3u    /* 弱上/下拉使能（2 位）    */
#define PAD_CNFG_TXSLEW_CTRL_P      5u    /* P 型驱动斜率控制（4 位） */
#define PAD_CNFG_TXSLEW_CTRL_N      9u    /* N 型驱动斜率控制（4 位） */

/* SDCLKDL_CNFG 字段位偏移 */
#define SDCLKDL_EXTDLY_EN           0u    /* 扩展延迟使能             */
#define SDCLKDL_BYPASS_EN           1u    /* 旁路延迟链               */
#define SDCLKDL_INPSEL_CNFG         2u    /* 输入路径选择（2 位）     */
#define SDCLKDL_UPDATE_DC           4u    /* 更新延迟步数触发位       */

/* SMPLDL_CNFG 字段位偏移 */
#define SMPLDL_EXTDLY_EN            0u    /* 扩展延迟使能             */
#define SMPLDL_BYPASS_EN            1u    /* 旁路采样延迟链           */
#define SMPLDL_INPSEL_CNFG          2u    /* 采样路径选择（2 位）     */

/* ATDL_CNFG 字段位偏移 */
#define ATDL_EXTDLY_EN              0u    /* 扩展延迟使能             */
#define ATDL_BYPASS_EN              1u    /* 旁路 AT 延迟链           */
#define ATDL_INPSEL_CNFG            2u    /* AT 路径选择（2 位）      */

/* SD 时钟默认延迟步数：10 步 × 10 ps/步 = 0.1 ns */
#define SDCLKDL_DC_DEFAULT          0x0Au

/* ---------------------------------------------------------------------------
 * Bitmain 厂商专用调谐寄存器（控制器基址 + 0x500）
 * --------------------------------------------------------------------------- */

#define BM_VENDOR_BASE              0x500u
#define BM_VENDOR_MSHC_CTRL         (BM_VENDOR_BASE + 0x08u)   /* 控制寄存器（16 位）   */
#define BM_VENDOR_A_CTRL            (BM_VENDOR_BASE + 0x40u)   /* 自动调谐控制（16 位） */
#define BM_VENDOR_A_STAT            (BM_VENDOR_BASE + 0x44u)   /* 自动调谐状态（16 位） */

/* ---------------------------------------------------------------------------
 * 设备索引
 * --------------------------------------------------------------------------- */

#define BM1684X_EMMC_INDEX          0u   /* eMMC 控制器（物理地址 0x50100000） */
#define BM1684X_SD_INDEX            1u   /* SD 控制器（物理地址 0x50101000）   */

/* ---------------------------------------------------------------------------
 * 驱动频率限制
 * --------------------------------------------------------------------------- */

#define BM1684X_EMMC_CLK_INIT_HZ    200000UL       /* 卡识别阶段 200 kHz   */
#define BM1684X_EMMC_CLK_MAX_HZ     100000000UL    /* eMMC 最高 100 MHz    */
#define BM1684X_SD_CLK_MAX_HZ       50000000UL     /* SD 最高 50 MHz       */

/* ---------------------------------------------------------------------------
 * 驱动私有数据结构体
 *
 * 每个控制器实例（eMMC 或 SD）拥有一个独立的 BM1684X_SDHCI_DRV，
 * 由 bm1684xSdhciAttach() 在系统启动时动态分配。
 * --------------------------------------------------------------------------- */

typedef struct bm1684xSdhciDrv {
    VXB_DEV_ID      pDev;           /* VxBus 设备句柄                     */
    VIRT_ADDR       regBase;        /* SDHCI 寄存器虚拟基地址（已映射）   */
    VXB_RESOURCE  * pRegRes;        /* MMIO 资源描述符                    */
    VXB_RESOURCE  * pIrqRes;        /* IRQ 资源描述符                     */
    VXB_SDHCI_HOST  sdhciHost;      /* VxWorks SDHCI 主机描述符（协议栈接口）*/
    UINT32          clkFreq;        /* 当前主机输入时钟频率（Hz）          */
    UINT32          devIndex;       /* 设备索引：0=eMMC，1=SD             */
    BOOL            hasPhyCfg;      /* TRUE 表示需要执行 PHY 初始化       */
    BOOL            is64BitAddr;    /* TRUE 表示使用 64 位 DMA 地址       */
    SEM_ID          cmdSem;         /* 命令完成信号量（ISR 中释放）        */
    SEM_ID          xferSem;        /* 数据传输完成信号量（ISR 中释放）    */
    UINT32          vendorBase;     /* 厂商扩展区域基地址缓存偏移          */
} BM1684X_SDHCI_DRV;

/* ---------------------------------------------------------------------------
 * 寄存器访问宏（对 VxBus MMIO 读写函数的薄封装）
 *
 * 参数 d 为 BM1684X_SDHCI_DRV 指针，off 为相对于 regBase 的偏移。
 * --------------------------------------------------------------------------- */

#define SDHCI_RD32(d, off)      vxbRead32((d)->regBase + (off))
#define SDHCI_RD16(d, off)      vxbRead16((d)->regBase + (off))
#define SDHCI_RD8(d, off)       vxbRead8 ((d)->regBase + (off))
#define SDHCI_WR32(d, off, v)   vxbWrite32((d)->regBase + (off), (v))
#define SDHCI_WR16(d, off, v)   vxbWrite16((d)->regBase + (off), (v))
#define SDHCI_WR8(d, off, v)    vxbWrite8 ((d)->regBase + (off), (v))

/* 读-改-写辅助宏：置位 / 清位 */
#define SDHCI_SET32(d, off, m)  SDHCI_WR32(d, off, SDHCI_RD32(d, off) | (m))
#define SDHCI_CLR32(d, off, m)  SDHCI_WR32(d, off, SDHCI_RD32(d, off) & ~(m))
#define SDHCI_SET16(d, off, m)  SDHCI_WR16(d, off, SDHCI_RD16(d, off) | (m))
#define SDHCI_CLR16(d, off, m)  SDHCI_WR16(d, off, SDHCI_RD16(d, off) & ~(m))

/* ---------------------------------------------------------------------------
 * VxBus 驱动公开符号（供内核驱动注册表引用）
 * --------------------------------------------------------------------------- */

extern VXB_DRV vxbBm1684xSdhciDrv;

#ifdef __cplusplus
}
#endif

#endif /* __VXB_BM1684X_SDHCI_H__ */
