/* bm1684xSdhciHw.h - BM1684X Synopsys DesignWare SDHCI hardware definitions */

/*
 * Copyright (c) 2024 Bitmain / Sophgo
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Pure hardware layer: register offsets, bit definitions, and MMIO access
 * macros.  This file has NO dependency on any OS or SDK headers — it only
 * uses standard C types (unsigned int / unsigned short / unsigned char).
 */

#ifndef __BM1684X_SDHCI_HW_H__
#define __BM1684X_SDHCI_HW_H__

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * BM1684X SoC base addresses
 * ========================================================================= */

#define BM1684X_EMMC_PHYS_BASE      0x50100000UL
#define BM1684X_SD_PHYS_BASE        0x50101000UL
#define BM1684X_TOP_PHYS_BASE       0x50010000UL

/* TOP register offsets (relative to BM1684X_TOP_PHYS_BASE) */
#define BM1684X_TOP_CONF_INFO       0x04UL   /* mode-sel in bits [2:0] */
#define BM1684X_TOP_CLOCK_EN0       0x800UL
#define BM1684X_TOP_SOFT_RST0       0xC00UL

/* Clock-enable bits in CLOCK_EN0 */
#define BM1684X_CLK_EMMC_200M       (1U << 6)
#define BM1684X_CLK_AXI_EMMC        (1U << 20)
#define BM1684X_CLK_100K_EMMC       (1U << 22)
#define BM1684X_CLK_SD_200M         (1U << 7)
#define BM1684X_CLK_AXI_SD          (1U << 21)
#define BM1684X_CLK_100K_SD         (1U << 23)

/* Soft-reset bits in SOFT_RST0 (write 0 to reset, write 1 to release) */
#define BM1684X_RST_EMMC            (1U << 20)
#define BM1684X_RST_SD              (1U << 21)

/* MODE_SEL values (bits [2:0] of CONF_INFO) */
#define BM1684X_MODE_NORMAL         0x0U
#define BM1684X_MODE_FAST           0x1U
#define BM1684X_MODE_SAFE           0x2U
#define BM1684X_MODE_BYPASS         0x3U

/* Input clock frequencies per mode */
#define BM1684X_EMMC_CLK_NORMAL_HZ  100000000U
#define BM1684X_EMMC_CLK_FAST_HZ    100000000U
#define BM1684X_EMMC_CLK_SAFE_HZ    100000000U
#define BM1684X_EMMC_CLK_BYPASS_HZ   25000000U

/* Operational frequency limits */
#define BM1684X_EMMC_CLK_INIT_HZ       200000U   /* 200 kHz identification */
#define BM1684X_EMMC_CLK_MAX_HZ     100000000U   /* 100 MHz eMMC */
#define BM1684X_SD_CLK_MAX_HZ        50000000U   /* 50  MHz SD  */

/* Device index */
#define BM1684X_EMMC_INDEX          0U
#define BM1684X_SD_INDEX            1U

/* =========================================================================
 * Standard SDHCI register offsets (JEDEC SD Host Controller spec v4)
 * ========================================================================= */

#define SDHCI_DMA_ADDRESS           0x00U
#define SDHCI_BLOCK_SIZE            0x04U
#define SDHCI_BLOCK_COUNT           0x06U
#define SDHCI_ARGUMENT              0x08U
#define SDHCI_TRANSFER_MODE         0x0CU
#define SDHCI_COMMAND               0x0EU
#define SDHCI_RESPONSE_0            0x10U
#define SDHCI_RESPONSE_1            0x14U
#define SDHCI_RESPONSE_2            0x18U
#define SDHCI_RESPONSE_3            0x1CU
#define SDHCI_BUF_DATA              0x20U
#define SDHCI_PRESENT_STATE         0x24U
#define SDHCI_HOST_CONTROL          0x28U
#define SDHCI_POWER_CONTROL         0x29U
#define SDHCI_BLOCK_GAP_CONTROL     0x2AU
#define SDHCI_CLOCK_CONTROL         0x2CU
#define SDHCI_TIMEOUT_CONTROL       0x2EU
#define SDHCI_SOFTWARE_RESET        0x2FU
#define SDHCI_INT_STATUS            0x30U
#define SDHCI_ERR_INT_STATUS        0x32U
#define SDHCI_INT_STATUS_EN         0x34U
#define SDHCI_ERR_INT_STATUS_EN     0x36U
#define SDHCI_INT_SIGNAL_EN         0x38U
#define SDHCI_HOST_CONTROL2         0x3EU
#define SDHCI_CAPABILITIES          0x40U
#define SDHCI_CAPABILITIES2         0x44U
#define SDHCI_ADMA_SA_LOW           0x58U
#define SDHCI_ADMA_SA_HIGH          0x5CU
#define SDHCI_VENDOR_SPECIFIC_AREA  0xE8U
#define SDHCI_HOST_VERSION          0xFEU

/* SDHCI_EMMC_CTRL_R is at (VENDOR_SPECIFIC_AREA base + 0x2C) */
#define SDHCI_EMMC_CTRL_R_OFF       0x2CU

/* Helper: block size register encoding */
#define SDHCI_MAKE_BLKSZ(dma, sz)   ((((dma) & 0x7U) << 12) | ((sz) & 0xFFFU))

/* =========================================================================
 * Transfer mode register bits (0x0C)
 * ========================================================================= */

#define SDHCI_TRNS_DMA              (1U << 0)
#define SDHCI_TRNS_BLK_CNT_EN      (1U << 1)
#define SDHCI_TRNS_AUTO_CMD12       (1U << 2)
#define SDHCI_TRNS_READ             (1U << 4)
#define SDHCI_TRNS_MULTI            (1U << 5)
#define SDHCI_TRNS_RESP_INT         (1U << 8)

/* =========================================================================
 * Command register bits (0x0E)
 * ========================================================================= */

#define SDHCI_CMD_RESP_NONE         0x00U
#define SDHCI_CMD_RESP_LONG         0x01U
#define SDHCI_CMD_RESP_SHORT        0x02U
#define SDHCI_CMD_RESP_SHORT_BUSY   0x03U
#define SDHCI_CMD_CRC               (1U << 3)
#define SDHCI_CMD_INDEX_CHK         (1U << 4)
#define SDHCI_CMD_DATA              (1U << 5)
#define SDHCI_MAKE_CMD(c, f)        ((((c) & 0xFFU) << 8) | ((f) & 0xFFU))

/* =========================================================================
 * Present state register bits (0x24)
 * ========================================================================= */

#define SDHCI_STATE_CMD_INHIBIT     (1U << 0)
#define SDHCI_STATE_DAT_INHIBIT     (1U << 1)
#define SDHCI_STATE_BUF_WR_EN       (1U << 10)
#define SDHCI_STATE_BUF_RD_EN       (1U << 11)
#define SDHCI_STATE_CARD_PRESENT    (1U << 16)

/* =========================================================================
 * Host control register bits (0x28)
 * ========================================================================= */

#define SDHCI_CTRL_4BITBUS          (1U << 1)
#define SDHCI_CTRL_8BITBUS          (1U << 5)
#define SDHCI_CTRL_DMA_MASK         0x18U
#define SDHCI_CTRL_SDMA             0x00U
#define SDHCI_CTRL_ADMA32           0x10U
#define SDHCI_CTRL_ADMA64           0x18U

/* =========================================================================
 * Power control register bits (0x29)
 * ========================================================================= */

#define SDHCI_POWER_ON              (1U << 0)
#define SDHCI_POWER_330             0x0EU   /* 3.3V */
#define SDHCI_POWER_300             0x0CU   /* 3.0V */
#define SDHCI_POWER_180             0x0AU   /* 1.8V */

/* =========================================================================
 * Clock control register bits (0x2C)
 * ========================================================================= */

#define SDHCI_CLK_INT_EN            (1U << 0)
#define SDHCI_CLK_INT_STABLE        (1U << 1)
#define SDHCI_CLK_CARD_EN           (1U << 2)
#define SDHCI_CLK_PLL_EN            (1U << 3)
#define SDHCI_CLK_GEN_SELECT        (1U << 5)
#define SDHCI_CLK_DIV_SHIFT         8U
#define SDHCI_CLK_DIV_MASK          0xFFU

/* =========================================================================
 * Software reset register bits (0x2F)
 * ========================================================================= */

#define SDHCI_RESET_ALL             0x01U
#define SDHCI_RESET_CMD             0x02U
#define SDHCI_RESET_DATA            0x04U

/* =========================================================================
 * Interrupt status / enable register bits (0x30 / 0x34 / 0x38)
 * ========================================================================= */

#define SDHCI_INT_CMD_COMPLETE      (1U << 0)
#define SDHCI_INT_XFER_COMPLETE     (1U << 1)
#define SDHCI_INT_DMA_END           (1U << 3)
#define SDHCI_INT_BUF_WR_READY     (1U << 4)
#define SDHCI_INT_BUF_RD_READY     (1U << 5)
#define SDHCI_INT_CARD_INSERT       (1U << 6)
#define SDHCI_INT_CARD_REMOVE       (1U << 7)
#define SDHCI_INT_ERROR             (1U << 15)

#define SDHCI_INT_ALL_NORMAL        0x00FFU
#define SDHCI_INT_ALL_ERROR         0xFFFFU

/* Interrupt mask for normal command+data completion */
#define SDHCI_INT_CORE_MASK  (SDHCI_INT_CMD_COMPLETE  | \
                              SDHCI_INT_XFER_COMPLETE  | \
                              SDHCI_INT_DMA_END        | \
                              SDHCI_INT_ERROR)

/* =========================================================================
 * Host control 2 register bits (0x3E)
 * ========================================================================= */

#define SDHCI_HC2_UHS_SDR12         0x0000U
#define SDHCI_HC2_UHS_SDR25         0x0001U
#define SDHCI_HC2_UHS_SDR50         0x0002U
#define SDHCI_HC2_UHS_SDR104        0x0003U
#define SDHCI_HC2_UHS_DDR50         0x0004U
#define SDHCI_HC2_HS400             0x0005U
#define SDHCI_HC2_UHS_MASK          0x0007U
#define SDHCI_HC2_1V8_SIGNALING     (1U << 3)
#define SDHCI_HC2_DRV_TYPE_C        (2U << 4)
#define SDHCI_HC2_CMD23_SUPPORT     (1U << 11)
#define SDHCI_HC2_VER4_ENABLE       (1U << 12)
#define SDHCI_HC2_64BIT_ADDR        (1U << 13)
#define SDHCI_HC2_ASYNC_INT         (1U << 14)
#define SDHCI_HC2_PRESET_VAL_EN     (1U << 15)

/* Capabilities2 register bits (0x44) */
#define SDHCI_CAP2_SYS_ADDR_64      (1U << 27)
#define SDHCI_CAP2_ASYNC_INT_SUP    (1U << 29)

/* =========================================================================
 * Synopsys PHY registers (base offset 0x300 from controller base)
 * ========================================================================= */

#define SDHCI_PHY_BASE              0x300U

#define SDHCI_P_PHY_CNFG            (SDHCI_PHY_BASE + 0x00U)  /* 32-bit */
#define SDHCI_P_CMDPAD_CNFG         (SDHCI_PHY_BASE + 0x04U)  /* 16-bit */
#define SDHCI_P_DATPAD_CNFG         (SDHCI_PHY_BASE + 0x06U)  /* 16-bit */
#define SDHCI_P_CLKPAD_CNFG         (SDHCI_PHY_BASE + 0x08U)  /* 16-bit */
#define SDHCI_P_STBPAD_CNFG         (SDHCI_PHY_BASE + 0x0AU)  /* 16-bit */
#define SDHCI_P_RSTNPAD_CNFG        (SDHCI_PHY_BASE + 0x0CU)  /* 16-bit */
#define SDHCI_P_COMMDL_CNFG         (SDHCI_PHY_BASE + 0x1CU)  /* 8-bit  */
#define SDHCI_P_SDCLKDL_CNFG        (SDHCI_PHY_BASE + 0x1DU)  /* 8-bit  */
#define SDHCI_P_SDCLKDL_DC          (SDHCI_PHY_BASE + 0x1EU)  /* 8-bit  */
#define SDHCI_P_SMPLDL_CNFG         (SDHCI_PHY_BASE + 0x20U)  /* 8-bit  */
#define SDHCI_P_ATDL_CNFG           (SDHCI_PHY_BASE + 0x21U)  /* 8-bit  */
#define SDHCI_P_DLL_CTRL            (SDHCI_PHY_BASE + 0x24U)
#define SDHCI_P_DLL_CNFG1           (SDHCI_PHY_BASE + 0x25U)
#define SDHCI_P_DLL_STATUS          (SDHCI_PHY_BASE + 0x2EU)

/* PHY_CNFG field bit positions */
#define PHY_CNFG_PHY_RSTN           0U
#define PHY_CNFG_PHY_PWRGOOD        1U
#define PHY_CNFG_PAD_SP             16U
#define PHY_CNFG_PAD_SN             20U

/* PAD_CNFG field bit positions (all PAD registers share this layout) */
#define PAD_CNFG_RXSEL              0U
#define PAD_CNFG_WEAKPULL_EN        3U
#define PAD_CNFG_TXSLEW_CTRL_P      5U
#define PAD_CNFG_TXSLEW_CTRL_N      9U

/* SDCLKDL_CNFG field bit positions */
#define SDCLKDL_EXTDLY_EN           0U
#define SDCLKDL_BYPASS_EN           1U
#define SDCLKDL_INPSEL_CNFG         2U

/* SMPLDL_CNFG field bit positions */
#define SMPLDL_EXTDLY_EN            0U
#define SMPLDL_BYPASS_EN            1U
#define SMPLDL_INPSEL_CNFG          2U

/* ATDL_CNFG field bit positions */
#define ATDL_EXTDLY_EN              0U
#define ATDL_BYPASS_EN              1U
#define ATDL_INPSEL_CNFG            2U

/* Default SD-clock delay: 70 steps × 10 ps/step = 0.7 ns */
#define SDCLKDL_DC_DEFAULT          0x0AU

/* =========================================================================
 * Bitmain vendor-specific tuning registers (at base + 0x500)
 * ========================================================================= */

#define BM_VENDOR_BASE              0x500U
#define BM_VENDOR_MSHC_CTRL         (BM_VENDOR_BASE + 0x08U)  /* 16-bit */
#define BM_VENDOR_A_CTRL            (BM_VENDOR_BASE + 0x40U)  /* 16-bit */
#define BM_VENDOR_A_STAT            (BM_VENDOR_BASE + 0x44U)  /* 16-bit */

/* =========================================================================
 * MMIO register access macros
 *
 * `base` is the virtual address (uintptr_t) returned by OSAL iomap().
 * These operate directly on the mapped pointer — no OS call needed.
 * ========================================================================= */

#define REG_RD32(base, off) \
    (*((volatile unsigned int   *)(((unsigned char *)(base)) + (off))))
#define REG_RD16(base, off) \
    (*((volatile unsigned short *)(((unsigned char *)(base)) + (off))))
#define REG_RD8(base, off)  \
    (*((volatile unsigned char  *)(((unsigned char *)(base)) + (off))))

#define REG_WR32(base, off, v) \
    (*((volatile unsigned int   *)(((unsigned char *)(base)) + (off))) = (unsigned int)(v))
#define REG_WR16(base, off, v) \
    (*((volatile unsigned short *)(((unsigned char *)(base)) + (off))) = (unsigned short)(v))
#define REG_WR8(base, off, v)  \
    (*((volatile unsigned char  *)(((unsigned char *)(base)) + (off))) = (unsigned char)(v))

#define REG_SET32(base, off, m) REG_WR32(base, off, REG_RD32(base, off) | (m))
#define REG_CLR32(base, off, m) REG_WR32(base, off, REG_RD32(base, off) & ~(unsigned int)(m))
#define REG_SET16(base, off, m) REG_WR16(base, off, REG_RD16(base, off) | (m))
#define REG_CLR16(base, off, m) REG_WR16(base, off, (unsigned short)(REG_RD16(base, off) & ~(unsigned int)(m)))

#ifdef __cplusplus
}
#endif

#endif /* __BM1684X_SDHCI_HW_H__ */
