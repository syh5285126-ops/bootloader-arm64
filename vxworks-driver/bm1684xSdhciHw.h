/* bm1684xSdhciHw.h - BM1684X Synopsys DesignWare SDHCI 硬件寄存器定义 */

/*
 * 版权所有 (c) 2024 Bitmain / Sophgo
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 纯硬件层：寄存器偏移、位域定义及 MMIO 访问宏。
 * 本文件不依赖任何操作系统或 SDK 头文件，仅使用标准 C 类型
 * （unsigned int / unsigned short / unsigned char）。
 */

#ifndef __BM1684X_SDHCI_HW_H__
#define __BM1684X_SDHCI_HW_H__

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * BM1684X SoC 物理基地址
 * ========================================================================= */

#define BM1684X_EMMC_PHYS_BASE      0x50100000UL  /* eMMC 控制器基地址 */
#define BM1684X_SD_PHYS_BASE        0x50101000UL  /* SD 控制器基地址   */
#define BM1684X_TOP_PHYS_BASE       0x50010000UL  /* TOP 寄存器基地址  */

/* TOP 寄存器偏移（相对于 BM1684X_TOP_PHYS_BASE） */
#define BM1684X_TOP_CONF_INFO       0x04UL   /* [2:0] 为启动模式选择 MODE_SEL */
#define BM1684X_TOP_CLOCK_EN0       0x800UL  /* 时钟使能寄存器 0 */
#define BM1684X_TOP_SOFT_RST0       0xC00UL  /* 软复位寄存器 0   */

/* CLOCK_EN0 时钟使能位 */
#define BM1684X_CLK_EMMC_200M       (1U << 6)   /* eMMC 200 MHz 时钟 */
#define BM1684X_CLK_AXI_EMMC        (1U << 20)  /* eMMC AXI 总线时钟 */
#define BM1684X_CLK_100K_EMMC       (1U << 22)  /* eMMC 100 kHz 时钟 */
#define BM1684X_CLK_SD_200M         (1U << 7)   /* SD 200 MHz 时钟   */
#define BM1684X_CLK_AXI_SD          (1U << 21)  /* SD AXI 总线时钟   */
#define BM1684X_CLK_100K_SD         (1U << 23)  /* SD 100 kHz 时钟   */

/* SOFT_RST0 软复位位（写 0 复位，写 1 释放） */
#define BM1684X_RST_EMMC            (1U << 20)
#define BM1684X_RST_SD              (1U << 21)

/* MODE_SEL 值（CONF_INFO[2:0]） */
#define BM1684X_MODE_NORMAL         0x0U  /* 正常模式，eMMC 输入 100 MHz */
#define BM1684X_MODE_FAST           0x1U  /* 快速模式，eMMC 输入 100 MHz */
#define BM1684X_MODE_SAFE           0x2U  /* 安全模式，eMMC 输入 100 MHz */
#define BM1684X_MODE_BYPASS         0x3U  /* 旁路模式，eMMC 输入 25 MHz  */

/* 各启动模式下 eMMC 的输入时钟频率 */
#define BM1684X_EMMC_CLK_NORMAL_HZ  100000000U
#define BM1684X_EMMC_CLK_FAST_HZ    100000000U
#define BM1684X_EMMC_CLK_SAFE_HZ    100000000U
#define BM1684X_EMMC_CLK_BYPASS_HZ   25000000U

/* 运行频率限制 */
#define BM1684X_EMMC_CLK_INIT_HZ       200000U   /* 卡识别阶段 200 kHz */
#define BM1684X_EMMC_CLK_MAX_HZ     100000000U   /* eMMC 最高 100 MHz  */
#define BM1684X_SD_CLK_MAX_HZ        50000000U   /* SD 最高 50 MHz     */

/* 设备索引 */
#define BM1684X_EMMC_INDEX          0U  /* eMMC */
#define BM1684X_SD_INDEX            1U  /* SD   */

/* =========================================================================
 * 标准 SDHCI 寄存器偏移（JEDEC SD 主机控制器规范 v4）
 * ========================================================================= */

#define SDHCI_DMA_ADDRESS           0x00U  /* SDMA 系统地址（低 32 位） */
#define SDHCI_BLOCK_SIZE            0x04U  /* 块大小                    */
#define SDHCI_BLOCK_COUNT           0x06U  /* 块计数                    */
#define SDHCI_ARGUMENT              0x08U  /* 命令参数                  */
#define SDHCI_TRANSFER_MODE         0x0CU  /* 传输模式                  */
#define SDHCI_COMMAND               0x0EU  /* 命令寄存器                */
#define SDHCI_RESPONSE_0            0x10U  /* 响应字 0                  */
#define SDHCI_RESPONSE_1            0x14U  /* 响应字 1                  */
#define SDHCI_RESPONSE_2            0x18U  /* 响应字 2                  */
#define SDHCI_RESPONSE_3            0x1CU  /* 响应字 3                  */
#define SDHCI_BUF_DATA              0x20U  /* 数据缓冲区端口            */
#define SDHCI_PRESENT_STATE         0x24U  /* 当前状态寄存器            */
#define SDHCI_HOST_CONTROL          0x28U  /* 主机控制寄存器 1（8 位）  */
#define SDHCI_POWER_CONTROL         0x29U  /* 电源控制寄存器（8 位）    */
#define SDHCI_BLOCK_GAP_CONTROL     0x2AU  /* 块间隔控制（8 位）        */
#define SDHCI_CLOCK_CONTROL         0x2CU  /* 时钟控制（16 位）         */
#define SDHCI_TIMEOUT_CONTROL       0x2EU  /* 超时控制（8 位）          */
#define SDHCI_SOFTWARE_RESET        0x2FU  /* 软件复位（8 位）          */
#define SDHCI_INT_STATUS            0x30U  /* 普通中断状态（16 位）     */
#define SDHCI_ERR_INT_STATUS        0x32U  /* 错误中断状态（16 位）     */
#define SDHCI_INT_STATUS_EN         0x34U  /* 普通中断状态使能（16 位） */
#define SDHCI_ERR_INT_STATUS_EN     0x36U  /* 错误中断状态使能（16 位） */
#define SDHCI_INT_SIGNAL_EN         0x38U  /* 普通中断信号使能（16 位） */
#define SDHCI_HOST_CONTROL2         0x3EU  /* 主机控制寄存器 2（16 位） */
#define SDHCI_CAPABILITIES          0x40U  /* 能力寄存器 1              */
#define SDHCI_CAPABILITIES2         0x44U  /* 能力寄存器 2              */
#define SDHCI_ADMA_SA_LOW           0x58U  /* ADMA 系统地址低 32 位     */
#define SDHCI_ADMA_SA_HIGH          0x5CU  /* ADMA 系统地址高 32 位     */
#define SDHCI_VENDOR_SPECIFIC_AREA  0xE8U  /* 厂商扩展区域基地址指针   */
#define SDHCI_HOST_VERSION          0xFEU  /* 主机控制器版本            */

/* eMMC 控制寄存器偏移（相对于厂商扩展区域基址 + 0x2C） */
#define SDHCI_EMMC_CTRL_R_OFF       0x2CU

/* 块大小寄存器编码辅助宏：dma 为 SDMA 边界 exponent，sz 为字节块大小 */
#define SDHCI_MAKE_BLKSZ(dma, sz)   ((((dma) & 0x7U) << 12) | ((sz) & 0xFFFU))

/* =========================================================================
 * 传输模式寄存器位域（偏移 0x0C）
 * ========================================================================= */

#define SDHCI_TRNS_DMA              (1U << 0)  /* DMA 使能            */
#define SDHCI_TRNS_BLK_CNT_EN      (1U << 1)  /* 块计数器使能        */
#define SDHCI_TRNS_AUTO_CMD12       (1U << 2)  /* 自动发送 CMD12      */
#define SDHCI_TRNS_READ             (1U << 4)  /* 读方向（卡→主机）   */
#define SDHCI_TRNS_MULTI            (1U << 5)  /* 多块传输            */
#define SDHCI_TRNS_RESP_INT         (1U << 8)  /* 响应中断使能        */

/* =========================================================================
 * 命令寄存器位域（偏移 0x0E）
 * ========================================================================= */

#define SDHCI_CMD_RESP_NONE         0x00U  /* 无响应            */
#define SDHCI_CMD_RESP_LONG         0x01U  /* 136 位长响应      */
#define SDHCI_CMD_RESP_SHORT        0x02U  /* 48 位短响应       */
#define SDHCI_CMD_RESP_SHORT_BUSY   0x03U  /* 48 位短响应 + 忙  */
#define SDHCI_CMD_CRC               (1U << 3)  /* CRC 校验使能      */
#define SDHCI_CMD_INDEX_CHK         (1U << 4)  /* 命令索引检查使能  */
#define SDHCI_CMD_DATA              (1U << 5)  /* 带数据传输        */
/* 构建命令寄存器值：高 8 位为命令索引，低 8 位为标志 */
#define SDHCI_MAKE_CMD(c, f)        ((((c) & 0xFFU) << 8) | ((f) & 0xFFU))

/* =========================================================================
 * 当前状态寄存器位域（偏移 0x24）
 * ========================================================================= */

#define SDHCI_STATE_CMD_INHIBIT     (1U << 0)   /* CMD 通道忙     */
#define SDHCI_STATE_DAT_INHIBIT     (1U << 1)   /* DAT 通道忙     */
#define SDHCI_STATE_BUF_WR_EN       (1U << 10)  /* 写缓冲区就绪   */
#define SDHCI_STATE_BUF_RD_EN       (1U << 11)  /* 读缓冲区就绪   */
#define SDHCI_STATE_CARD_PRESENT    (1U << 16)  /* 卡已插入       */

/* =========================================================================
 * 主机控制寄存器 1 位域（偏移 0x28，8 位）
 * ========================================================================= */

#define SDHCI_CTRL_4BITBUS          (1U << 1)  /* 4 位总线模式          */
#define SDHCI_CTRL_8BITBUS          (1U << 5)  /* 8 位总线模式（eMMC）  */
#define SDHCI_CTRL_DMA_MASK         0x18U      /* DMA 模式位掩码        */
#define SDHCI_CTRL_SDMA             0x00U      /* 使用 SDMA             */
#define SDHCI_CTRL_ADMA32           0x10U      /* 使用 ADMA2 32 位      */
#define SDHCI_CTRL_ADMA64           0x18U      /* 使用 ADMA2 64 位      */

/* =========================================================================
 * 电源控制寄存器位域（偏移 0x29，8 位）
 * ========================================================================= */

#define SDHCI_POWER_ON              (1U << 0)  /* 总线上电        */
#define SDHCI_POWER_330             0x0EU      /* 选择 3.3 V      */
#define SDHCI_POWER_300             0x0CU      /* 选择 3.0 V      */
#define SDHCI_POWER_180             0x0AU      /* 选择 1.8 V      */

/* =========================================================================
 * 时钟控制寄存器位域（偏移 0x2C，16 位）
 * ========================================================================= */

#define SDHCI_CLK_INT_EN            (1U << 0)  /* 内部时钟使能        */
#define SDHCI_CLK_INT_STABLE        (1U << 1)  /* 内部时钟稳定标志    */
#define SDHCI_CLK_CARD_EN           (1U << 2)  /* 向卡输出时钟使能    */
#define SDHCI_CLK_PLL_EN            (1U << 3)  /* PLL 使能            */
#define SDHCI_CLK_GEN_SELECT        (1U << 5)  /* 时钟发生器选择      */
#define SDHCI_CLK_DIV_SHIFT         8U         /* 分频值字段起始位    */
#define SDHCI_CLK_DIV_MASK          0xFFU      /* 分频值字段掩码      */

/* =========================================================================
 * 软件复位寄存器位域（偏移 0x2F，8 位）
 * ========================================================================= */

#define SDHCI_RESET_ALL             0x01U  /* 复位全部逻辑      */
#define SDHCI_RESET_CMD             0x02U  /* 仅复位命令通道    */
#define SDHCI_RESET_DATA            0x04U  /* 仅复位数据通道    */

/* =========================================================================
 * 中断状态 / 使能寄存器位域（偏移 0x30 / 0x34 / 0x38）
 * ========================================================================= */

#define SDHCI_INT_CMD_COMPLETE      (1U << 0)   /* 命令完成          */
#define SDHCI_INT_XFER_COMPLETE     (1U << 1)   /* 数据传输完成      */
#define SDHCI_INT_DMA_END           (1U << 3)   /* SDMA 边界中断     */
#define SDHCI_INT_BUF_WR_READY     (1U << 4)   /* 写缓冲区就绪      */
#define SDHCI_INT_BUF_RD_READY     (1U << 5)   /* 读缓冲区就绪      */
#define SDHCI_INT_CARD_INSERT       (1U << 6)   /* 卡插入            */
#define SDHCI_INT_CARD_REMOVE       (1U << 7)   /* 卡拔出            */
#define SDHCI_INT_ERROR             (1U << 15)  /* 错误中断汇总位    */

#define SDHCI_INT_ALL_NORMAL        0x00FFU  /* 全部普通中断状态位 */
#define SDHCI_INT_ALL_ERROR         0xFFFFU  /* 全部错误中断状态位 */

/* 命令/数据传输完成的核心中断掩码 */
#define SDHCI_INT_CORE_MASK  (SDHCI_INT_CMD_COMPLETE  | \
                              SDHCI_INT_XFER_COMPLETE  | \
                              SDHCI_INT_DMA_END        | \
                              SDHCI_INT_ERROR)

/* =========================================================================
 * 主机控制寄存器 2 位域（偏移 0x3E，16 位）
 * ========================================================================= */

#define SDHCI_HC2_UHS_SDR12         0x0000U       /* UHS-I SDR12 模式       */
#define SDHCI_HC2_UHS_SDR25         0x0001U       /* UHS-I SDR25 模式       */
#define SDHCI_HC2_UHS_SDR50         0x0002U       /* UHS-I SDR50 模式       */
#define SDHCI_HC2_UHS_SDR104        0x0003U       /* UHS-I SDR104 模式      */
#define SDHCI_HC2_UHS_DDR50         0x0004U       /* UHS-I DDR50 模式       */
#define SDHCI_HC2_HS400             0x0005U       /* eMMC HS400 模式        */
#define SDHCI_HC2_UHS_MASK          0x0007U       /* 速度模式位掩码         */
#define SDHCI_HC2_1V8_SIGNALING     (1U << 3)     /* 1.8 V 信号切换         */
#define SDHCI_HC2_DRV_TYPE_C        (2U << 4)     /* 驱动类型 C             */
#define SDHCI_HC2_CMD23_SUPPORT     (1U << 11)    /* CMD23 预设块计数使能   */
#define SDHCI_HC2_VER4_ENABLE       (1U << 12)    /* 版本 4 模式使能        */
#define SDHCI_HC2_64BIT_ADDR        (1U << 13)    /* 64 位系统地址使能      */
#define SDHCI_HC2_ASYNC_INT         (1U << 14)    /* 异步中断使能           */
#define SDHCI_HC2_PRESET_VAL_EN     (1U << 15)    /* 预设值使能             */

/* 能力寄存器 2 位域（偏移 0x44） */
#define SDHCI_CAP2_SYS_ADDR_64      (1U << 27)   /* 支持 64 位系统地址     */
#define SDHCI_CAP2_ASYNC_INT_SUP    (1U << 29)   /* 支持异步中断           */

/* =========================================================================
 * Synopsys PHY 寄存器（控制器基址 + 0x300）
 * ========================================================================= */

#define SDHCI_PHY_BASE              0x300U  /* PHY 寄存器区域起始偏移 */

#define SDHCI_P_PHY_CNFG            (SDHCI_PHY_BASE + 0x00U)  /* PHY 配置（32 位） */
#define SDHCI_P_CMDPAD_CNFG         (SDHCI_PHY_BASE + 0x04U)  /* CMD PAD 配置（16 位） */
#define SDHCI_P_DATPAD_CNFG         (SDHCI_PHY_BASE + 0x06U)  /* DAT PAD 配置（16 位） */
#define SDHCI_P_CLKPAD_CNFG         (SDHCI_PHY_BASE + 0x08U)  /* CLK PAD 配置（16 位） */
#define SDHCI_P_STBPAD_CNFG         (SDHCI_PHY_BASE + 0x0AU)  /* STB PAD 配置（16 位） */
#define SDHCI_P_RSTNPAD_CNFG        (SDHCI_PHY_BASE + 0x0CU)  /* RST_N PAD 配置（16 位）*/
#define SDHCI_P_COMMDL_CNFG         (SDHCI_PHY_BASE + 0x1CU)  /* CMD 延迟配置（8 位）  */
#define SDHCI_P_SDCLKDL_CNFG        (SDHCI_PHY_BASE + 0x1DU)  /* SD 时钟延迟配置（8 位）*/
#define SDHCI_P_SDCLKDL_DC          (SDHCI_PHY_BASE + 0x1EU)  /* SD 时钟延迟步数（8 位）*/
#define SDHCI_P_SMPLDL_CNFG         (SDHCI_PHY_BASE + 0x20U)  /* 采样延迟配置（8 位）  */
#define SDHCI_P_ATDL_CNFG           (SDHCI_PHY_BASE + 0x21U)  /* AT 延迟配置（8 位）   */
#define SDHCI_P_DLL_CTRL            (SDHCI_PHY_BASE + 0x24U)  /* DLL 控制              */
#define SDHCI_P_DLL_CNFG1           (SDHCI_PHY_BASE + 0x25U)  /* DLL 配置 1            */
#define SDHCI_P_DLL_STATUS          (SDHCI_PHY_BASE + 0x2EU)  /* DLL 状态              */

/* PHY_CNFG 字段位偏移 */
#define PHY_CNFG_PHY_RSTN           0U   /* PHY 复位（低有效）      */
#define PHY_CNFG_PHY_PWRGOOD        1U   /* PHY 上电完成标志        */
#define PHY_CNFG_PAD_SP             16U  /* PAD P 型晶体管斜率控制  */
#define PHY_CNFG_PAD_SN             20U  /* PAD N 型晶体管斜率控制  */

/* PAD_CNFG 字段位偏移（CMD/DAT/CLK/STB/RSTN 共用此布局） */
#define PAD_CNFG_RXSEL              0U   /* 接收器选择        */
#define PAD_CNFG_WEAKPULL_EN        3U   /* 弱上/下拉使能     */
#define PAD_CNFG_TXSLEW_CTRL_P      5U   /* P 型驱动斜率控制  */
#define PAD_CNFG_TXSLEW_CTRL_N      9U   /* N 型驱动斜率控制  */

/* SDCLKDL_CNFG 字段位偏移 */
#define SDCLKDL_EXTDLY_EN           0U   /* 扩展延迟使能      */
#define SDCLKDL_BYPASS_EN           1U   /* 旁路延迟链        */
#define SDCLKDL_INPSEL_CNFG         2U   /* 输入路径选择      */

/* SMPLDL_CNFG 字段位偏移 */
#define SMPLDL_EXTDLY_EN            0U   /* 扩展延迟使能      */
#define SMPLDL_BYPASS_EN            1U   /* 旁路采样延迟链    */
#define SMPLDL_INPSEL_CNFG          2U   /* 采样路径选择      */

/* ATDL_CNFG 字段位偏移 */
#define ATDL_EXTDLY_EN              0U   /* 扩展延迟使能      */
#define ATDL_BYPASS_EN              1U   /* 旁路 AT 延迟链    */
#define ATDL_INPSEL_CNFG            2U   /* AT 路径选择       */

/* SD 时钟默认延迟步数：10 步 × 10 ps/步 = 0.1 ns（可根据 PCB 调整） */
#define SDCLKDL_DC_DEFAULT          0x0AU

/* =========================================================================
 * Bitmain 厂商专用调谐寄存器（控制器基址 + 0x500）
 * ========================================================================= */

#define BM_VENDOR_BASE              0x500U
#define BM_VENDOR_MSHC_CTRL         (BM_VENDOR_BASE + 0x08U)  /* 控制寄存器（16 位）   */
#define BM_VENDOR_A_CTRL            (BM_VENDOR_BASE + 0x40U)  /* 自动调谐控制（16 位） */
#define BM_VENDOR_A_STAT            (BM_VENDOR_BASE + 0x44U)  /* 自动调谐状态（16 位） */

/* =========================================================================
 * MMIO 寄存器访问宏
 *
 * base 为 OSAL iomap() 返回的虚拟地址指针，直接通过 volatile 指针访问，
 * 不依赖任何操作系统调用。
 * ========================================================================= */

/* 读操作：按 32/16/8 位宽度读取指定偏移处的寄存器 */
#define REG_RD32(base, off) \
    (*((volatile unsigned int   *)(((unsigned char *)(base)) + (off))))
#define REG_RD16(base, off) \
    (*((volatile unsigned short *)(((unsigned char *)(base)) + (off))))
#define REG_RD8(base, off)  \
    (*((volatile unsigned char  *)(((unsigned char *)(base)) + (off))))

/* 写操作：按 32/16/8 位宽度写入指定偏移处的寄存器 */
#define REG_WR32(base, off, v) \
    (*((volatile unsigned int   *)(((unsigned char *)(base)) + (off))) = (unsigned int)(v))
#define REG_WR16(base, off, v) \
    (*((volatile unsigned short *)(((unsigned char *)(base)) + (off))) = (unsigned short)(v))
#define REG_WR8(base, off, v)  \
    (*((volatile unsigned char  *)(((unsigned char *)(base)) + (off))) = (unsigned char)(v))

/* 读-改-写辅助宏：置位 / 清位 */
#define REG_SET32(base, off, m) REG_WR32(base, off, REG_RD32(base, off) | (m))
#define REG_CLR32(base, off, m) REG_WR32(base, off, REG_RD32(base, off) & ~(unsigned int)(m))
#define REG_SET16(base, off, m) REG_WR16(base, off, REG_RD16(base, off) | (m))
#define REG_CLR16(base, off, m) REG_WR16(base, off, (unsigned short)(REG_RD16(base, off) & ~(unsigned int)(m)))

/* -------------------------------------------------------------------------
 * 内存屏障：确保此前所有 MMIO 写操作在屏障之后的访问发出前真正到达硬件。
 * 目标平台固定为 AArch64，直接使用 dsb 指令，不做架构可移植性处理。
 * ------------------------------------------------------------------------- */
#define BM_SDHCI_MB()  __asm__ __volatile__ ("dsb sy" : : : "memory")

#ifdef __cplusplus
}
#endif

#endif /* __BM1684X_SDHCI_HW_H__ */
