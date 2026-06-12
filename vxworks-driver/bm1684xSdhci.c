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

    /* 3. CMD pad: RXSEL=1, WEAKPULL_EN=1, TXSLEW_CTRL_P=0xA, TXSLEW_CTRL_N=6 */
    REG_WR16(pDev->base, SDHCI_P_CMDPAD_CNFG,
             (unsigned short)(
                 (1U << PAD_CNFG_RXSEL) |
                 (1U << PAD_CNFG_WEAKPULL_EN) |
                 (0xAU << PAD_CNFG_TXSLEW_CTRL_P) |
                 (6U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 4. DAT pad: same as CMD */
    REG_WR16(pDev->base, SDHCI_P_DATPAD_CNFG,
             (unsigned short)(
                 (1U << PAD_CNFG_RXSEL) |
                 (1U << PAD_CNFG_WEAKPULL_EN) |
                 (0xAU << PAD_CNFG_TXSLEW_CTRL_P) |
                 (6U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 5. CLK pad: RXSEL=0, WEAKPULL_EN=0, TXSLEW_CTRL_P=0xA, TXSLEW_CTRL_N=6 */
    REG_WR16(pDev->base, SDHCI_P_CLKPAD_CNFG,
             (unsigned short)(
                 (0xAU << PAD_CNFG_TXSLEW_CTRL_P) |
                 (6U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 6. STB pad: RXSEL=1, WEAKPULL_EN=2 (pull-down), same slew */
    REG_WR16(pDev->base, SDHCI_P_STBPAD_CNFG,
             (unsigned short)(
                 (1U << PAD_CNFG_RXSEL) |
                 (2U << PAD_CNFG_WEAKPULL_EN) |
                 (0xAU << PAD_CNFG_TXSLEW_CTRL_P) |
                 (6U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 7. RST_N pad: RXSEL=1, WEAKPULL_EN=1, same slew */
    REG_WR16(pDev->base, SDHCI_P_RSTNPAD_CNFG,
             (unsigned short)(
                 (1U << PAD_CNFG_RXSEL) |
                 (1U << PAD_CNFG_WEAKPULL_EN) |
                 (0xAU << PAD_CNFG_TXSLEW_CTRL_P) |
                 (6U  << PAD_CNFG_TXSLEW_CTRL_N)));

    /* 8. COMMDL: bypass disabled */
    REG_WR8(pDev->base, SDHCI_P_COMMDL_CNFG, 0);

    /* 9. SDCLKDL: bypass enabled, DC = 0x0A */
    REG_WR8(pDev->base, SDHCI_P_SDCLKDL_CNFG,
            (unsigned char)(1U << SDCLKDL_BYPASS_EN));
    REG_WR8(pDev->base, SDHCI_P_SDCLKDL_DC, SDCLKDL_DC_DEFAULT);

    /* 10. SMPLDL: eMMC uses INPSEL=0x2; SD uses BYPASS */
    if (pDev->devIndex == BM1684X_EMMC_INDEX)
        REG_WR8(pDev->base, SDHCI_P_SMPLDL_CNFG,
                (unsigned char)(0x2U << SMPLDL_INPSEL_CNFG));
    else
        REG_WR8(pDev->base, SDHCI_P_SMPLDL_CNFG,
                (unsigned char)(1U << SMPLDL_BYPASS_EN));

    /* 11. ATDL: bypass */
    REG_WR8(pDev->base, SDHCI_P_ATDL_CNFG,
            (unsigned char)(1U << ATDL_BYPASS_EN));

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

    /* Set version-4 mode, optionally 64-bit addressing, CMD23 support */
    hc2 = (unsigned short)(SDHCI_HC2_VER4_ENABLE | SDHCI_HC2_CMD23_SUPPORT);
    if (pDev->is64Bit)
        hc2 |= (unsigned short)SDHCI_HC2_64BIT_ADDR;
    REG_WR16(pDev->base, SDHCI_HOST_CONTROL2, hc2);

    /* Power on: 3.3 V */
    REG_WR8(pDev->base, SDHCI_POWER_CONTROL,
            (unsigned char)(SDHCI_POWER_ON | SDHCI_POWER_330));

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
    unsigned short mode = SDHCI_TRNS_DMA | SDHCI_TRNS_BLK_CNT_EN;

    if (pData->blkCount > 1U) {
        mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
    }
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

    /* SDMA boundary: reload DMA address to resume scatter */
    if (sts & SDHCI_INT_DMA_END) {
        unsigned int sa = REG_RD32(pDev->base, SDHCI_DMA_ADDRESS);
        REG_WR32(pDev->base, SDHCI_DMA_ADDRESS, sa);
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

    /* Wait for CMD (and DAT when needed) inhibit to clear */
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

    /* Program data registers before writing the command */
    if (pData) {
        unsigned long dmaAddr = (unsigned long)(unsigned long long)(unsigned long)pData->buf;

        REG_WR16(pDev->base, SDHCI_BLOCK_SIZE,
                 (unsigned short)SDHCI_MAKE_BLKSZ(7, pData->blkSize));
        REG_WR16(pDev->base, SDHCI_BLOCK_COUNT,
                 (unsigned short)(pData->blkCount & 0xFFFFU));

        /* SDMA: write the DMA address (low 32-bit; high written separately if 64-bit) */
        REG_WR32(pDev->base, SDHCI_DMA_ADDRESS, (unsigned int)(dmaAddr & 0xFFFFFFFFUL));
        if (pDev->is64Bit)
            REG_WR32(pDev->base, SDHCI_ADMA_SA_HIGH,
                     (unsigned int)((dmaAddr >> 32) & 0xFFFFFFFFUL));

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
            /* SDMA 512 KB boundary: reload address to continue */
            unsigned int sa = REG_RD32(pDev->base, SDHCI_DMA_ADDRESS);
            REG_WR32(pDev->base, SDHCI_DMA_ADDRESS, sa);
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
