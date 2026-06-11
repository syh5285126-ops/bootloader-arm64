/* vxbBm1684xSdhci.h - BM1684X Synopsys DesignWare SDHCI VxBus driver header */

/*
 * Copyright (c) 2024 Bitmain / Sophgo
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * VxWorks 7 VxBus FDT driver for the BM1684X eMMC/SD controller.
 * FDT compatible string: "bitmain,synopsys-sdhc"
 *
 * Required VxWorks SDK headers (not provided here):
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
 * BM1684X SoC addresses (TOP_BASE = 0x50010000)
 * --------------------------------------------------------------------------- */

#define BM1684X_EMMC_BASE           0x50100000UL
#define BM1684X_SD_BASE             0x50101000UL
#define BM1684X_TOP_BASE            0x50010000UL

#define BM1684X_TOP_SOFT_RST0       (BM1684X_TOP_BASE + 0xC00)
#define BM1684X_TOP_CLOCK_ENABLE0   (BM1684X_TOP_BASE + 0x800)

#define BM1684X_RST0_EMMC           (1u << 20)
#define BM1684X_RST0_SD             (1u << 21)

#define BM1684X_CLK_EMMC_200M       (1u << 6)
#define BM1684X_CLK_AXI_EMMC        (1u << 20)
#define BM1684X_CLK_100K_EMMC       (1u << 22)

/* ---------------------------------------------------------------------------
 * Standard SDHCI register offsets
 * --------------------------------------------------------------------------- */

#define SDHCI_DMA_ADDRESS           0x00u
#define SDHCI_BLOCK_SIZE            0x04u
#define SDHCI_MAKE_BLKSZ(dma, sz)   ((((dma) & 0x7u) << 12) | ((sz) & 0xFFFu))
#define SDHCI_BLOCK_COUNT           0x06u
#define SDHCI_ARGUMENT              0x08u
#define SDHCI_TRANSFER_MODE         0x0Cu
#define SDHCI_COMMAND               0x0Eu
#define SDHCI_RESPONSE_0            0x10u
#define SDHCI_RESPONSE_1            0x14u
#define SDHCI_RESPONSE_2            0x18u
#define SDHCI_RESPONSE_3            0x1Cu
#define SDHCI_BUF_DATA              0x20u
#define SDHCI_PRESENT_STATE         0x24u
#define SDHCI_HOST_CONTROL          0x28u
#define SDHCI_POWER_CONTROL         0x29u
#define SDHCI_BLOCK_GAP_CONTROL     0x2Au
#define SDHCI_CLOCK_CONTROL         0x2Cu
#define SDHCI_TIMEOUT_CONTROL       0x2Eu
#define SDHCI_SOFTWARE_RESET        0x2Fu
#define SDHCI_INT_STATUS            0x30u
#define SDHCI_ERR_INT_STATUS        0x32u
#define SDHCI_INT_STATUS_EN         0x34u
#define SDHCI_ERR_INT_STATUS_EN     0x36u
#define SDHCI_INT_SIGNAL_EN         0x38u
#define SDHCI_HOST_CONTROL2         0x3Eu
#define SDHCI_CAPABILITIES          0x40u
#define SDHCI_CAPABILITIES2         0x44u
#define SDHCI_ADMA_SA_LOW           0x58u
#define SDHCI_ADMA_SA_HIGH          0x5Cu
#define SDHCI_VENDOR_SPECIFIC_AREA  0xE8u
#define SDHCI_HOST_VERSION          0xFEu

/* SDHCI_EMMC_CTRL_R is at (vendor_specific_area_base + 0x2C) */
#define SDHCI_EMMC_CTRL_R_OFFSET    0x2Cu

/* ---------------------------------------------------------------------------
 * Transfer mode bits (0x0C)
 * --------------------------------------------------------------------------- */

#define SDHCI_TRNS_DMA              (1u << 0)
#define SDHCI_TRNS_BLK_CNT_EN      (1u << 1)
#define SDHCI_TRNS_AUTO_CMD12       (1u << 2)
#define SDHCI_TRNS_READ             (1u << 4)
#define SDHCI_TRNS_MULTI            (1u << 5)
#define SDHCI_TRNS_RESP_INT         (1u << 8)

/* ---------------------------------------------------------------------------
 * Command register (0x0E)
 * --------------------------------------------------------------------------- */

#define SDHCI_CMD_RESP_NONE         0x00u
#define SDHCI_CMD_RESP_LONG         0x01u
#define SDHCI_CMD_RESP_SHORT        0x02u
#define SDHCI_CMD_RESP_SHORT_BUSY   0x03u
#define SDHCI_CMD_CRC               (1u << 3)
#define SDHCI_CMD_INDEX_CHK         (1u << 4)
#define SDHCI_CMD_DATA              (1u << 5)
#define SDHCI_MAKE_CMD(c, f)        ((((c) & 0xFFu) << 8) | ((f) & 0xFFu))

/* ---------------------------------------------------------------------------
 * Present state bits (0x24)
 * --------------------------------------------------------------------------- */

#define SDHCI_STATE_CMD_INHIBIT     (1u << 0)
#define SDHCI_STATE_DAT_INHIBIT     (1u << 1)
#define SDHCI_STATE_BUF_WR_EN       (1u << 10)
#define SDHCI_STATE_BUF_RD_EN       (1u << 11)
#define SDHCI_STATE_CARD_PRESENT    (1u << 16)

/* ---------------------------------------------------------------------------
 * Host control bits (0x28)
 * --------------------------------------------------------------------------- */

#define SDHCI_CTRL_4BITBUS          (1u << 1)
#define SDHCI_CTRL_8BITBUS          (1u << 5)
#define SDHCI_CTRL_DMA_MASK         0x18u
#define SDHCI_CTRL_SDMA             0x00u
#define SDHCI_CTRL_ADMA32           0x10u
#define SDHCI_CTRL_ADMA64           0x18u

/* ---------------------------------------------------------------------------
 * Power control bits (0x29)
 * --------------------------------------------------------------------------- */

#define SDHCI_POWER_ON              (1u << 0)
#define SDHCI_POWER_330             0x0Eu   /* 3.3V */
#define SDHCI_POWER_300             0x0Cu   /* 3.0V */
#define SDHCI_POWER_180             0x0Au   /* 1.8V */

/* ---------------------------------------------------------------------------
 * Clock control bits (0x2C)
 * --------------------------------------------------------------------------- */

#define SDHCI_CLK_INT_EN            (1u << 0)
#define SDHCI_CLK_INT_STABLE        (1u << 1)
#define SDHCI_CLK_CARD_EN           (1u << 2)
#define SDHCI_CLK_PLL_EN            (1u << 3)
#define SDHCI_CLK_GEN_SELECT        (1u << 5)   /* 0=divided, 1=programmable */
#define SDHCI_CLK_DIV_SHIFT         8
#define SDHCI_CLK_DIV_MASK          0xFFu

/* ---------------------------------------------------------------------------
 * Software reset bits (0x2F)
 * --------------------------------------------------------------------------- */

#define SDHCI_RESET_ALL             0x01u
#define SDHCI_RESET_CMD             0x02u
#define SDHCI_RESET_DATA            0x04u

/* ---------------------------------------------------------------------------
 * Interrupt status / enable bits (0x30 / 0x34)
 * --------------------------------------------------------------------------- */

#define SDHCI_INT_CMD_COMPLETE      (1u << 0)
#define SDHCI_INT_XFER_COMPLETE     (1u << 1)
#define SDHCI_INT_DMA_END           (1u << 3)
#define SDHCI_INT_BUF_WR_READY     (1u << 4)
#define SDHCI_INT_BUF_RD_READY     (1u << 5)
#define SDHCI_INT_CARD_INSERT       (1u << 6)
#define SDHCI_INT_CARD_REMOVE       (1u << 7)
#define SDHCI_INT_ERROR             (1u << 15)

#define SDHCI_INT_DATA_MASK         (SDHCI_INT_XFER_COMPLETE | \
                                     SDHCI_INT_DMA_END       | \
                                     SDHCI_INT_BUF_WR_READY  | \
                                     SDHCI_INT_BUF_RD_READY)

#define SDHCI_INT_NORMAL_MASK       0x00FFu
#define SDHCI_INT_ERROR_MASK        0xFFFFu

/* ---------------------------------------------------------------------------
 * Host control 2 bits (0x3E)
 * --------------------------------------------------------------------------- */

#define SDHCI_HC2_UHS_SDR12         0x0000u
#define SDHCI_HC2_UHS_SDR25         0x0001u
#define SDHCI_HC2_UHS_SDR50         0x0002u
#define SDHCI_HC2_UHS_SDR104        0x0003u
#define SDHCI_HC2_UHS_DDR50         0x0004u
#define SDHCI_HC2_HS400             0x0005u
#define SDHCI_HC2_UHS_MASK          0x0007u
#define SDHCI_HC2_1V8_SIGNALING     (1u << 3)
#define SDHCI_HC2_DRV_TYPE_A        (1u << 4)
#define SDHCI_HC2_DRV_TYPE_C        (2u << 4)
#define SDHCI_HC2_DRV_TYPE_D        (3u << 4)
#define SDHCI_HC2_EXEC_TUNING       (1u << 6)
#define SDHCI_HC2_TUNED_CLK         (1u << 7)
#define SDHCI_HC2_CMD23_SUPPORT     (1u << 11)
#define SDHCI_HC2_VER4_ENABLE       (1u << 12)
#define SDHCI_HC2_64BIT_ADDR        (1u << 13)
#define SDHCI_HC2_ASYNC_INT         (1u << 14)
#define SDHCI_HC2_PRESET_VAL_EN     (1u << 15)

/* ---------------------------------------------------------------------------
 * Synopsys PHY registers (base offset 0x300)
 * --------------------------------------------------------------------------- */

#define SDHCI_PHY_BASE              0x300u

#define SDHCI_P_PHY_CNFG            (SDHCI_PHY_BASE + 0x00u)   /* 32-bit */
#define SDHCI_P_CMDPAD_CNFG         (SDHCI_PHY_BASE + 0x04u)   /* 16-bit */
#define SDHCI_P_DATPAD_CNFG         (SDHCI_PHY_BASE + 0x06u)   /* 16-bit */
#define SDHCI_P_CLKPAD_CNFG         (SDHCI_PHY_BASE + 0x08u)   /* 16-bit */
#define SDHCI_P_STBPAD_CNFG         (SDHCI_PHY_BASE + 0x0Au)   /* 16-bit */
#define SDHCI_P_RSTNPAD_CNFG        (SDHCI_PHY_BASE + 0x0Cu)   /* 16-bit */
#define SDHCI_P_COMMDL_CNFG         (SDHCI_PHY_BASE + 0x1Cu)   /* 8-bit */
#define SDHCI_P_SDCLKDL_CNFG        (SDHCI_PHY_BASE + 0x1Du)   /* 8-bit */
#define SDHCI_P_SDCLKDL_DC          (SDHCI_PHY_BASE + 0x1Eu)   /* 8-bit */
#define SDHCI_P_SMPLDL_CNFG         (SDHCI_PHY_BASE + 0x20u)   /* 8-bit */
#define SDHCI_P_ATDL_CNFG           (SDHCI_PHY_BASE + 0x21u)   /* 8-bit */
#define SDHCI_P_DLL_CTRL            (SDHCI_PHY_BASE + 0x24u)
#define SDHCI_P_DLL_CNFG1           (SDHCI_PHY_BASE + 0x25u)
#define SDHCI_P_DLL_CNFG2           (SDHCI_PHY_BASE + 0x26u)
#define SDHCI_P_DLL_STATUS          (SDHCI_PHY_BASE + 0x2Eu)

/* PHY_CNFG field positions */
#define PHY_CNFG_PHY_RSTN           0u
#define PHY_CNFG_PHY_PWRGOOD        1u
#define PHY_CNFG_PAD_SP             16u     /* 4-bit slew P */
#define PHY_CNFG_PAD_SN             20u     /* 4-bit slew N */

/* PAD_CNFG field positions (CMDPAD, DATPAD, CLKPAD, STBPAD, RSTNPAD) */
#define PAD_CNFG_RXSEL              0u      /* 3-bit */
#define PAD_CNFG_WEAKPULL_EN        3u      /* 2-bit */
#define PAD_CNFG_TXSLEW_CTRL_P      5u      /* 4-bit */
#define PAD_CNFG_TXSLEW_CTRL_N      9u      /* 4-bit */

/* SDCLKDL_CNFG field positions */
#define SDCLKDL_EXTDLY_EN           0u
#define SDCLKDL_BYPASS_EN           1u
#define SDCLKDL_INPSEL_CNFG         2u     /* 2-bit */
#define SDCLKDL_UPDATE_DC           4u

/* SMPLDL_CNFG field positions */
#define SMPLDL_EXTDLY_EN            0u
#define SMPLDL_BYPASS_EN            1u
#define SMPLDL_INPSEL_CNFG          2u     /* 2-bit */

/* ATDL_CNFG field positions */
#define ATDL_EXTDLY_EN              0u
#define ATDL_BYPASS_EN              1u
#define ATDL_INPSEL_CNFG            2u     /* 2-bit */

/* SDCLKDL_DC: default clock delay (70 * 10 ps = 0.7 ns) */
#define SDCLKDL_DC_DEFAULT          0x0Au

/* ---------------------------------------------------------------------------
 * Vendor-specific (Bitmain tuning) registers, at base 0x500
 * --------------------------------------------------------------------------- */

#define BM_VENDOR_BASE              0x500u
#define BM_VENDOR_MSHC_CTRL         (BM_VENDOR_BASE + 0x08u)   /* 16-bit */
#define BM_VENDOR_A_CTRL            (BM_VENDOR_BASE + 0x40u)   /* 16-bit */
#define BM_VENDOR_A_STAT            (BM_VENDOR_BASE + 0x44u)   /* 16-bit */

/* ---------------------------------------------------------------------------
 * Device index
 * --------------------------------------------------------------------------- */

#define BM1684X_EMMC_INDEX          0u
#define BM1684X_SD_INDEX            1u

/* ---------------------------------------------------------------------------
 * Driver frequency limits
 * --------------------------------------------------------------------------- */

#define BM1684X_EMMC_CLK_INIT_HZ    200000UL       /* 200 kHz */
#define BM1684X_EMMC_CLK_MAX_HZ     100000000UL    /* 100 MHz */
#define BM1684X_SD_CLK_MAX_HZ       50000000UL     /* 50 MHz */

/* ---------------------------------------------------------------------------
 * Driver private data structure
 * --------------------------------------------------------------------------- */

typedef struct bm1684xSdhciDrv {
    VXB_DEV_ID      pDev;           /* VxBus device handle */
    VIRT_ADDR       regBase;        /* MMIO base (mapped) */
    VXB_RESOURCE  * pRegRes;        /* MMIO resource descriptor */
    VXB_RESOURCE  * pIrqRes;        /* IRQ resource descriptor */
    VXB_SDHCI_HOST  sdhciHost;      /* VxWorks SDHCI host (SD stack IF) */
    UINT32          clkFreq;        /* current host input clock (Hz) */
    UINT32          devIndex;       /* 0 = eMMC, 1 = SD */
    BOOL            hasPhyCfg;      /* TRUE when PHY initialisation needed */
    BOOL            is64BitAddr;    /* TRUE when 64-bit DMA addressing used */
    SEM_ID          cmdSem;         /* released by ISR on CMD_COMPLETE */
    SEM_ID          xferSem;        /* released by ISR on XFER_COMPLETE */
    UINT32          vendorBase;     /* cached vendor-specific area offset */
} BM1684X_SDHCI_DRV;

/* ---------------------------------------------------------------------------
 * Register access helpers (thin wrappers around VxBus MMIO)
 * --------------------------------------------------------------------------- */

#define SDHCI_RD32(d, off)      vxbRead32((d)->regBase + (off))
#define SDHCI_RD16(d, off)      vxbRead16((d)->regBase + (off))
#define SDHCI_RD8(d, off)       vxbRead8 ((d)->regBase + (off))
#define SDHCI_WR32(d, off, v)   vxbWrite32((d)->regBase + (off), (v))
#define SDHCI_WR16(d, off, v)   vxbWrite16((d)->regBase + (off), (v))
#define SDHCI_WR8(d, off, v)    vxbWrite8 ((d)->regBase + (off), (v))

#define SDHCI_SET32(d, off, m)  SDHCI_WR32(d, off, SDHCI_RD32(d, off) | (m))
#define SDHCI_CLR32(d, off, m)  SDHCI_WR32(d, off, SDHCI_RD32(d, off) & ~(m))
#define SDHCI_SET16(d, off, m)  SDHCI_WR16(d, off, SDHCI_RD16(d, off) | (m))
#define SDHCI_CLR16(d, off, m)  SDHCI_WR16(d, off, SDHCI_RD16(d, off) & ~(m))

/* ---------------------------------------------------------------------------
 * VxBus driver public symbol
 * --------------------------------------------------------------------------- */

extern VXB_DRV vxbBm1684xSdhciDrv;

#ifdef __cplusplus
}
#endif

#endif /* __VXB_BM1684X_SDHCI_H__ */
