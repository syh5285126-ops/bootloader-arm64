/* vxbBm1684xSdhci.c - BM1684X Synopsys DesignWare SDHCI VxBus driver */

/*
 * Copyright (c) 2024 Bitmain / Sophgo
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * VxWorks 7 VxBus FDT driver for the BM1684X eMMC/SD host controller.
 *
 * Hardware: Synopsys DesignWare SDHCI v4 with integrated PHY
 *   - eMMC controller: 0x50100000 (index 0, has_phy, 64-bit addressing)
 *   - SD  controller:  0x50101000 (index 1, has_phy, 64-bit addressing)
 *
 * FDT compatible string: "bitmain,synopsys-sdhc"
 *
 * Build requirements (VxWorks 7 SDK):
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
 * Forward declarations
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
 * VxBus driver registration
 * ============================================================ */

LOCAL VXB_FDT_DEV_MATCH_ENTRY bm1684xSdhciMatchTbl[] = {
    { "bitmain,synopsys-sdhc", NULL },
    {}    /* sentinel */
};

LOCAL VXB_DRV_METHOD bm1684xSdhciMethods[] = {
    { VXB_DEVMETHOD_CALL(vxbDevProbe),  (FUNCPTR)bm1684xSdhciProbe  },
    { VXB_DEVMETHOD_CALL(vxbDevAttach), (FUNCPTR)bm1684xSdhciAttach },
    VXB_DEVMETHOD_END
};

VXB_DRV vxbBm1684xSdhciDrv = {
    { NULL },
    "bm1684xSdhci",
    "BM1684X Synopsys DesignWare eMMC/SD SDHCI Controller",
    VXB_BUSID_FDT,
    0,
    0,
    bm1684xSdhciMethods,
    NULL
};

VXB_DRV_DEF(vxbBm1684xSdhciDrv)

/* VxWorks SDHCI host operations (called by the SD/eMMC protocol stack) */
LOCAL VXB_SDHCI_OPS bm1684xSdhciOps = {
    .setClk       = bm1684xSdhciClkSet,
    .setBusWidth  = bm1684xSdhciBusSet,
    .setVdd       = bm1684xSdhciVoltSet,
    .issueCmd     = bm1684xSdhciCmdIssue,
    .cardDetect   = bm1684xSdhciCardDetect,
};

/* ============================================================
 * Helper: read input clock frequency from SoC mode-sel GPIO
 *
 * The BM1684X TOP register at offset 0x4 encodes MODE_SEL in bits [2:0]:
 *   0x0 = Normal  → 100 MHz
 *   0x1 = Fast    → 100 MHz
 *   0x2 = Safe    → 100 MHz
 *   0x3 = Bypass  →  25 MHz
 * ============================================================ */

LOCAL UINT32 bm1684xSdhciGetClkHz(BM1684X_SDHCI_DRV * pDrv)
{
    UINT32 confInfo;
    UINT32 modeSel;

    confInfo = vxbRead32(BM1684X_TOP_BASE + 0x04u);
    modeSel  = confInfo & 0x7u;

    if (modeSel == 0x3u)            /* bypass mode */
        return 25000000UL;

    return BM1684X_EMMC_CLK_MAX_HZ; /* 100 MHz for normal/fast/safe */
}

/* ============================================================
 * PHY initialisation
 *
 * The Synopsys PHY requires a specific PAD and delay-line setup.
 * eMMC (index 0) and SD (index 1) differ only in the SMPLDL_CNFG
 * register: eMMC uses INPSEL_CNFG=0x2 (internal feedback), while
 * SD uses BYPASS_EN=1.
 * ============================================================ */

LOCAL void bm1684xSdhciPhyInit(BM1684X_SDHCI_DRV * pDrv)
{
    UINT32 reg32;
    int    loop;

    /* Step 1: full hardware reset */
    SDHCI_WR8(pDrv, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL);
    for (loop = 100; loop > 0; loop--) {
        if (SDHCI_RD8(pDrv, SDHCI_SOFTWARE_RESET) == 0u)
            break;
        sysUsDelay(10000);  /* 10 ms */
    }

    /* Step 2: wait for PHY power-on-good */
    for (loop = 100; loop > 0; loop--) {
        if (SDHCI_RD32(pDrv, SDHCI_P_PHY_CNFG) & (1u << PHY_CNFG_PHY_PWRGOOD))
            break;
        sysUsDelay(10000);
    }

    /* Step 3: assert PHY reset (clear PHY_RSTN) */
    SDHCI_CLR32(pDrv, SDHCI_P_PHY_CNFG, (1u << PHY_CNFG_PHY_RSTN));

    /* Step 4: configure PAD slew rates */
    reg32 = (1u        << PHY_CNFG_PHY_PWRGOOD) |
            (0x9u      << PHY_CNFG_PAD_SP)       |
            (0x8u      << PHY_CNFG_PAD_SN);
    SDHCI_WR32(pDrv, SDHCI_P_PHY_CNFG, reg32);

    /* Step 5: configure command pad */
    SDHCI_WR16(pDrv, SDHCI_P_CMDPAD_CNFG,
               (0x2u << PAD_CNFG_RXSEL)         |
               (0x1u << PAD_CNFG_WEAKPULL_EN)   |
               (0x3u << PAD_CNFG_TXSLEW_CTRL_P) |
               (0x2u << PAD_CNFG_TXSLEW_CTRL_N));

    /* Step 6: configure data pad */
    SDHCI_WR16(pDrv, SDHCI_P_DATPAD_CNFG,
               (0x2u << PAD_CNFG_RXSEL)         |
               (0x1u << PAD_CNFG_WEAKPULL_EN)   |
               (0x3u << PAD_CNFG_TXSLEW_CTRL_P) |
               (0x2u << PAD_CNFG_TXSLEW_CTRL_N));

    /* Step 7: configure clock pad (no pull) */
    SDHCI_WR16(pDrv, SDHCI_P_CLKPAD_CNFG,
               (0x2u << PAD_CNFG_RXSEL)         |
               (0x3u << PAD_CNFG_TXSLEW_CTRL_P) |
               (0x2u << PAD_CNFG_TXSLEW_CTRL_N));

    /* Step 8: configure strobe pad (pull-up for HS400) */
    SDHCI_WR16(pDrv, SDHCI_P_STBPAD_CNFG,
               (0x2u << PAD_CNFG_RXSEL)         |
               (0x2u << PAD_CNFG_WEAKPULL_EN)   |
               (0x3u << PAD_CNFG_TXSLEW_CTRL_P) |
               (0x2u << PAD_CNFG_TXSLEW_CTRL_N));

    /* Step 9: configure reset pad */
    SDHCI_WR16(pDrv, SDHCI_P_RSTNPAD_CNFG,
               (0x2u << PAD_CNFG_RXSEL)         |
               (0x1u << PAD_CNFG_WEAKPULL_EN)   |
               (0x3u << PAD_CNFG_TXSLEW_CTRL_P) |
               (0x2u << PAD_CNFG_TXSLEW_CTRL_N));

    /* Step 10: enable external delay on SD clock DLL */
    SDHCI_WR8(pDrv, SDHCI_P_SDCLKDL_CNFG, (1u << SDCLKDL_EXTDLY_EN));

    /* Step 11: set clock delay (0.7 ns at 70 * 10 ps/step) */
    SDHCI_WR8(pDrv, SDHCI_P_SDCLKDL_DC, SDCLKDL_DC_DEFAULT);

    /* Step 12: sample delay - eMMC vs SD differ here */
    if (pDrv->devIndex == BM1684X_EMMC_INDEX) {
        /*
         * eMMC: use internal feedback path for sampling.
         * INPSEL_CNFG = 0x2 selects the loopback clock path,
         * which is required for reliable high-speed eMMC operation.
         */
        SDHCI_WR8(pDrv, SDHCI_P_SMPLDL_CNFG,
                  (0x2u << SMPLDL_INPSEL_CNFG));
    } else {
        /*
         * SD card: bypass the delay line entirely.
         * The card provides its own output timing reference.
         */
        SDHCI_WR8(pDrv, SDHCI_P_SMPLDL_CNFG,
                  (1u << SMPLDL_BYPASS_EN));
    }

    /* Step 13: auto-tuning delay - use internal path for init */
    SDHCI_WR8(pDrv, SDHCI_P_ATDL_CNFG, (0x2u << ATDL_INPSEL_CNFG));

    /* Step 14: release PHY reset */
    SDHCI_SET32(pDrv, SDHCI_P_PHY_CNFG, (1u << PHY_CNFG_PHY_RSTN));
}

/* ============================================================
 * Hardware initialisation (called once after PHY init)
 * ============================================================ */

LOCAL STATUS bm1684xSdhciHwInit(BM1684X_SDHCI_DRV * pDrv)
{
    UINT16 hc2;
    UINT16 vendorOffset;

    /* Reset command and data lines */
    SDHCI_WR8(pDrv, SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);

    /* Power on at 3.3V */
    SDHCI_WR8(pDrv, SDHCI_POWER_CONTROL, SDHCI_POWER_330 | SDHCI_POWER_ON);

    /* Timeout: max value (0xE) for 50 kHz TMCLK */
    SDHCI_WR8(pDrv, SDHCI_TIMEOUT_CONTROL, 0x0Eu);

    /* Host Control 2: version 4, CMD23 support */
    hc2 = SDHCI_RD16(pDrv, SDHCI_HOST_CONTROL2);
    hc2 |= SDHCI_HC2_CMD23_SUPPORT;
    hc2 |= SDHCI_HC2_VER4_ENABLE;
    SDHCI_WR16(pDrv, SDHCI_HOST_CONTROL2, hc2);

    /* Enable 64-bit DMA addressing if requested */
    if (pDrv->is64BitAddr) {
        UINT32 caps = SDHCI_RD32(pDrv, SDHCI_CAPABILITIES2);
        if (caps & (1u << 27)) {   /* sys_addr_64 capability bit */
            SDHCI_SET16(pDrv, SDHCI_HOST_CONTROL2, SDHCI_HC2_64BIT_ADDR);
        }
    }

    /* Enable async interrupt if supported */
    if (SDHCI_RD32(pDrv, SDHCI_CAPABILITIES2) & (1u << 29))
        SDHCI_SET16(pDrv, SDHCI_HOST_CONTROL2, SDHCI_HC2_ASYNC_INT);

    /* Enable all normal and error interrupt status */
    SDHCI_WR16(pDrv, SDHCI_INT_STATUS_EN,     0xFFFFu);
    SDHCI_WR16(pDrv, SDHCI_ERR_INT_STATUS_EN, 0xFFFFu);

    /* Enable interrupt signals for cmd/xfer/dma/error */
    SDHCI_WR16(pDrv, SDHCI_INT_SIGNAL_EN,
               SDHCI_INT_CMD_COMPLETE  |
               SDHCI_INT_XFER_COMPLETE |
               SDHCI_INT_DMA_END       |
               SDHCI_INT_ERROR);

    /* Mark device as eMMC in vendor control register */
    vendorOffset = SDHCI_RD16(pDrv, SDHCI_VENDOR_SPECIFIC_AREA) & 0x0FFFu;
    pDrv->vendorBase = vendorOffset;
    SDHCI_SET16(pDrv, vendorOffset + SDHCI_EMMC_CTRL_R_OFFSET, 0x1u);

    return OK;
}

/* ============================================================
 * Clock management
 * ============================================================ */

LOCAL STATUS bm1684xSdhciClkSet(VXB_SDHCI_HOST * pHost, UINT32 clkHz)
{
    BM1684X_SDHCI_DRV * pDrv;
    UINT32              div;
    UINT16              clkCtrl;
    int                 i;

    pDrv = (BM1684X_SDHCI_DRV *)pHost->pDrvCtrl;

    if (clkHz == 0u) {
        /* Stop the SD clock */
        SDHCI_CLR16(pDrv, SDHCI_CLOCK_CONTROL, SDHCI_CLK_CARD_EN);
        return OK;
    }

    /* Calculate divider: SDHCI v3 10-bit divided clock mode */
    if (pDrv->clkFreq <= clkHz) {
        div = 0u;
    } else {
        for (div = 1u; div <= 0xFFu; div++) {
            if ((pDrv->clkFreq / (2u * div)) <= clkHz)
                break;
        }
    }

    /* Disable clocks before changing divider */
    clkCtrl  = SDHCI_RD16(pDrv, SDHCI_CLOCK_CONTROL);
    clkCtrl &= ~(SDHCI_CLK_CARD_EN | SDHCI_CLK_PLL_EN | SDHCI_CLK_INT_EN);
    clkCtrl &= ~(SDHCI_CLK_DIV_MASK << SDHCI_CLK_DIV_SHIFT);
    clkCtrl &= ~SDHCI_CLK_GEN_SELECT;       /* divided clock mode */
    clkCtrl |=  (div & SDHCI_CLK_DIV_MASK) << SDHCI_CLK_DIV_SHIFT;
    SDHCI_WR16(pDrv, SDHCI_CLOCK_CONTROL, clkCtrl);

    /* Enable internal clock and wait for stable */
    SDHCI_SET16(pDrv, SDHCI_CLOCK_CONTROL, SDHCI_CLK_INT_EN);
    for (i = 150000; i > 0; i -= 100) {
        if (SDHCI_RD16(pDrv, SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INT_STABLE)
            break;
        sysUsDelay(100);
    }
    if (i <= 0)
        return ERROR;

    /* Enable PLL and SD clock output */
    SDHCI_SET16(pDrv, SDHCI_CLOCK_CONTROL,
                SDHCI_CLK_PLL_EN | SDHCI_CLK_CARD_EN);
    sysUsDelay(400); /* wait ≥ 74 clock cycles at 200 kHz init freq */

    return OK;
}

/* ============================================================
 * Bus width
 * ============================================================ */

LOCAL STATUS bm1684xSdhciBusSet(VXB_SDHCI_HOST * pHost, UINT32 width)
{
    BM1684X_SDHCI_DRV * pDrv;
    UINT8               hc1;

    pDrv = (BM1684X_SDHCI_DRV *)pHost->pDrvCtrl;
    hc1  = SDHCI_RD8(pDrv, SDHCI_HOST_CONTROL);

    switch (width) {
    case 1u:
        hc1 &= ~SDHCI_CTRL_4BITBUS;
        hc1 &= ~SDHCI_CTRL_8BITBUS;
        break;
    case 4u:
        hc1 |=  SDHCI_CTRL_4BITBUS;
        hc1 &= ~SDHCI_CTRL_8BITBUS;
        break;
    case 8u:
        hc1 &= ~SDHCI_CTRL_4BITBUS;
        hc1 |=  SDHCI_CTRL_8BITBUS;
        break;
    default:
        return ERROR;
    }

    SDHCI_WR8(pDrv, SDHCI_HOST_CONTROL, hc1);
    return OK;
}

/* ============================================================
 * Voltage selection (1.8V / 3.3V signalling)
 * ============================================================ */

LOCAL STATUS bm1684xSdhciVoltSet(VXB_SDHCI_HOST * pHost, UINT32 vddMv)
{
    BM1684X_SDHCI_DRV * pDrv;

    pDrv = (BM1684X_SDHCI_DRV *)pHost->pDrvCtrl;

    if (vddMv == 1800u) {
        SDHCI_SET16(pDrv, SDHCI_HOST_CONTROL2, SDHCI_HC2_1V8_SIGNALING);
    } else {
        SDHCI_CLR16(pDrv, SDHCI_HOST_CONTROL2, SDHCI_HC2_1V8_SIGNALING);
    }

    return OK;
}

/* ============================================================
 * Card detect
 * ============================================================ */

LOCAL BOOL bm1684xSdhciCardDetect(VXB_SDHCI_HOST * pHost)
{
    BM1684X_SDHCI_DRV * pDrv;

    pDrv = (BM1684X_SDHCI_DRV *)pHost->pDrvCtrl;

    /* eMMC is non-removable: always present */
    if (pDrv->devIndex == BM1684X_EMMC_INDEX)
        return TRUE;

    /* SD: read card-detect bit in present-state register */
    return (SDHCI_RD32(pDrv, SDHCI_PRESENT_STATE) & SDHCI_STATE_CARD_PRESENT)
           ? TRUE : FALSE;
}

/* ============================================================
 * Command and data transfer
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

    /* Wait until CMD and DAT lines are idle */
    for (timeout = 100000u; timeout > 0u; timeout--) {
        if (!(SDHCI_RD32(pDrv, SDHCI_PRESENT_STATE) &
              (SDHCI_STATE_CMD_INHIBIT | SDHCI_STATE_DAT_INHIBIT)))
            break;
        sysUsDelay(1);
    }
    if (timeout == 0u)
        return ERROR;

    /* Build response type flags */
    if (pCmd->respType == VXB_SDHCI_RSP_NONE) {
        cmdFlags = SDHCI_CMD_RESP_NONE;
    } else if (pCmd->respType & VXB_SDHCI_RSP_136) {
        cmdFlags = SDHCI_CMD_RESP_LONG;
    } else if (pCmd->respType & VXB_SDHCI_RSP_BUSY) {
        cmdFlags = SDHCI_CMD_RESP_SHORT_BUSY;
    } else {
        cmdFlags = SDHCI_CMD_RESP_SHORT;
    }

    if (pCmd->respType & VXB_SDHCI_RSP_CRC)
        cmdFlags |= SDHCI_CMD_CRC;
    if (pCmd->respType & VXB_SDHCI_RSP_CMDIDX)
        cmdFlags |= SDHCI_CMD_INDEX_CHK;

    /* Prepare data transfer if present */
    if (pData != NULL) {
        UINT32 blkCnt  = pData->blkCount;
        UINT32 blkSize = pData->blkSize;

        cmdFlags |= SDHCI_CMD_DATA;
        xferMode  = SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_MULTI;
        if (pData->flags & VXB_SDHCI_DATA_READ)
            xferMode |= SDHCI_TRNS_READ;

        /* SDMA or 64-bit SDMA DMA mode */
        xferMode |= SDHCI_TRNS_DMA;

        UINT8 hostCtrl = SDHCI_RD8(pDrv, SDHCI_HOST_CONTROL);
        hostCtrl = (hostCtrl & ~SDHCI_CTRL_DMA_MASK) | SDHCI_CTRL_SDMA;
        SDHCI_WR8(pDrv, SDHCI_HOST_CONTROL, hostCtrl);

        if (SDHCI_RD16(pDrv, SDHCI_HOST_CONTROL2) & SDHCI_HC2_64BIT_ADDR) {
            /* 64-bit SDMA: address in ADMA_SA, count in DMA_ADDRESS */
            SDHCI_WR32(pDrv, SDHCI_ADMA_SA_LOW,
                       (UINT32)((PHYS_ADDR)pData->pBuf));
            SDHCI_WR32(pDrv, SDHCI_ADMA_SA_HIGH,
                       (UINT32)(((PHYS_ADDR)pData->pBuf) >> 32));
            SDHCI_WR32(pDrv, SDHCI_DMA_ADDRESS, blkCnt);
            SDHCI_WR16(pDrv, SDHCI_BLOCK_COUNT, 0u);
        } else {
            SDHCI_WR32(pDrv, SDHCI_DMA_ADDRESS, (UINT32)pData->pBuf);
            SDHCI_WR16(pDrv, SDHCI_BLOCK_COUNT, (UINT16)blkCnt);
        }

        SDHCI_WR16(pDrv, SDHCI_BLOCK_SIZE,
                   SDHCI_MAKE_BLKSZ(7u /* 512K boundary */, blkSize));
        SDHCI_WR16(pDrv, SDHCI_TRANSFER_MODE, xferMode);
    }

    /* Issue command */
    SDHCI_WR32(pDrv, SDHCI_ARGUMENT, pCmd->arg);
    SDHCI_WR16(pDrv, SDHCI_COMMAND,
               SDHCI_MAKE_CMD(pCmd->cmdIndex, cmdFlags));

    /* Wait for command complete (semaphore released by ISR) */
    if (semTake(pDrv->cmdSem, sysClkRateGet() /* 1 s timeout */) != OK)
        return ERROR;

    /* Check for error */
    intStatus = SDHCI_RD16(pDrv, SDHCI_INT_STATUS);
    if (intStatus & SDHCI_INT_ERROR) {
        SDHCI_WR16(pDrv, SDHCI_INT_STATUS,     0xFFFFu);
        SDHCI_WR16(pDrv, SDHCI_ERR_INT_STATUS, 0xFFFFu);
        return ERROR;
    }

    /* Read response */
    if (!(cmdFlags == SDHCI_CMD_RESP_NONE)) {
        pCmd->resp[0] = SDHCI_RD32(pDrv, SDHCI_RESPONSE_0);
        if (cmdFlags == SDHCI_CMD_RESP_LONG) {
            pCmd->resp[1] = SDHCI_RD32(pDrv, SDHCI_RESPONSE_1);
            pCmd->resp[2] = SDHCI_RD32(pDrv, SDHCI_RESPONSE_2);
            pCmd->resp[3] = SDHCI_RD32(pDrv, SDHCI_RESPONSE_3);
        }
    }

    /* For data commands, wait for transfer complete */
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
 * Interrupt service routine
 * ============================================================ */

LOCAL void bm1684xSdhciIntrHandler(VXB_DEV_ID pDev)
{
    BM1684X_SDHCI_DRV * pDrv;
    UINT16              intStatus;
    UINT32              dmaAddr;

    pDrv = (BM1684X_SDHCI_DRV *)vxbDevSoftcGet(pDev);
    if (pDrv == NULL)
        return;

    intStatus = SDHCI_RD16(pDrv, SDHCI_INT_STATUS);

    /* Acknowledge all pending interrupts immediately */
    SDHCI_WR16(pDrv, SDHCI_INT_STATUS, intStatus);

    if (intStatus & SDHCI_INT_ERROR) {
        /* Signal both semaphores so waiting task can handle the error */
        SDHCI_WR16(pDrv, SDHCI_ERR_INT_STATUS,
                   SDHCI_RD16(pDrv, SDHCI_ERR_INT_STATUS));
        (void)semGive(pDrv->cmdSem);
        (void)semGive(pDrv->xferSem);
        return;
    }

    if (intStatus & SDHCI_INT_CMD_COMPLETE)
        (void)semGive(pDrv->cmdSem);

    if (intStatus & SDHCI_INT_DMA_END) {
        /* Reload DMA address to continue scatter-gather transfer */
        if (SDHCI_RD16(pDrv, SDHCI_HOST_CONTROL2) & SDHCI_HC2_64BIT_ADDR) {
            dmaAddr = SDHCI_RD32(pDrv, SDHCI_ADMA_SA_LOW);
            SDHCI_WR32(pDrv, SDHCI_ADMA_SA_LOW,  dmaAddr);
            SDHCI_WR32(pDrv, SDHCI_ADMA_SA_HIGH, 0u);
        } else {
            dmaAddr = SDHCI_RD32(pDrv, SDHCI_DMA_ADDRESS);
            SDHCI_WR32(pDrv, SDHCI_DMA_ADDRESS, dmaAddr);
        }
    }

    if (intStatus & SDHCI_INT_XFER_COMPLETE)
        (void)semGive(pDrv->xferSem);
}

/* ============================================================
 * VxBus probe: accept devices whose FDT compatible matches
 * ============================================================ */

LOCAL STATUS bm1684xSdhciProbe(VXB_DEV_ID pDev)
{
    return vxbFdtDevMatch(pDev, bm1684xSdhciMatchTbl, NULL);
}

/* ============================================================
 * VxBus attach: resource allocation, hardware init, registration
 * ============================================================ */

LOCAL STATUS bm1684xSdhciAttach(VXB_DEV_ID pDev)
{
    BM1684X_SDHCI_DRV * pDrv;
    VXB_FDT_DEV       * pFdt;
    const void        * pProp;
    int                 propLen;

    /* Allocate driver private data */
    pDrv = (BM1684X_SDHCI_DRV *)vxbMemAlloc(sizeof(BM1684X_SDHCI_DRV));
    if (pDrv == NULL)
        return ERROR;

    pDrv->pDev = pDev;
    vxbDevSoftcSet(pDev, pDrv);

    /* Read FDT properties */
    pFdt = vxbFdtDevGet(pDev);

    pProp = vxFdtPropGet(pFdt->offset, "index", &propLen);
    pDrv->devIndex = (pProp != NULL)
                     ? (UINT32)vxFdt32ToCpu(*(const UINT32 *)pProp)
                     : BM1684X_EMMC_INDEX;

    pProp = vxFdtPropGet(pFdt->offset, "max-frequency", &propLen);
    pDrv->clkFreq = (pProp != NULL)
                    ? (UINT32)vxFdt32ToCpu(*(const UINT32 *)pProp)
                    : BM1684X_EMMC_CLK_MAX_HZ;

    pDrv->hasPhyCfg   = (vxFdtPropGet(pFdt->offset, "has_phy",       NULL) != NULL);
    pDrv->is64BitAddr = (vxFdtPropGet(pFdt->offset, "64_addressing", NULL) != NULL);

    /* Get actual SoC input clock */
    pDrv->clkFreq = bm1684xSdhciGetClkHz(pDrv);

    /* Allocate and map MMIO register resource */
    pDrv->pRegRes = vxbResourceAlloc(pDev, VXB_RES_MEMORY, 0);
    if (pDrv->pRegRes == NULL)
        goto errFree;

    pDrv->regBase = (VIRT_ADDR)VXB_RES_ADR(pDrv->pRegRes);

    /* Allocate interrupt resource */
    pDrv->pIrqRes = vxbResourceAlloc(pDev, VXB_RES_IRQ, 0);
    if (pDrv->pIrqRes == NULL)
        goto errFreeReg;

    /* Create completion semaphores (binary) */
    pDrv->cmdSem  = semBCreate(SEM_Q_FIFO, SEM_EMPTY);
    pDrv->xferSem = semBCreate(SEM_Q_FIFO, SEM_EMPTY);
    if (pDrv->cmdSem == NULL || pDrv->xferSem == NULL)
        goto errFreeIrq;

    /* PHY initialisation (must come before hwInit) */
    if (pDrv->hasPhyCfg)
        bm1684xSdhciPhyInit(pDrv);

    /* Core hardware initialisation */
    if (bm1684xSdhciHwInit(pDrv) != OK)
        goto errFreeSem;

    /* Set initial clock (200 kHz identification) */
    (void)bm1684xSdhciClkSet(&pDrv->sdhciHost, BM1684X_EMMC_CLK_INIT_HZ);

    /* Connect and enable interrupt */
    if (vxbIntConnect(pDev, pDrv->pIrqRes,
                      (VOIDFUNCPTR)bm1684xSdhciIntrHandler, pDev) != OK)
        goto errFreeSem;

    if (vxbIntEnable(pDev, pDrv->pIrqRes) != OK)
        goto errFreeSem;

    /* Populate VxWorks SDHCI host descriptor */
    pDrv->sdhciHost.pDrvCtrl = pDrv;
    pDrv->sdhciHost.pOps     = &bm1684xSdhciOps;
    pDrv->sdhciHost.clkMax   = (pDrv->devIndex == BM1684X_EMMC_INDEX)
                                ? BM1684X_EMMC_CLK_MAX_HZ
                                : BM1684X_SD_CLK_MAX_HZ;
    pDrv->sdhciHost.clkMin   = BM1684X_EMMC_CLK_INIT_HZ;

    /* Register the host with the VxWorks SD/eMMC protocol stack */
    if (vxbSdhciHostRegister(pDev, &pDrv->sdhciHost) != OK)
        goto errFreeSem;

    return OK;

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
