/*
 * bm1684xSdhciOsal.h — OS Abstraction Layer for BM1684X SDHCI driver
 *
 * The caller fills in BM1684X_SDHCI_OSAL before calling bm1684xSdhciInit().
 * Any pointer left NULL causes the driver to fall back to polling mode for
 * that capability.  A fully NULL OSAL runs entirely in polling mode (suitable
 * for bare-metal or early-boot environments).
 *
 * Copyright (c) 2024 Bitmain.  All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BM1684X_SDHCI_OSAL_H
#define BM1684X_SDHCI_OSAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * OSAL callback table
 * -------------------------------------------------------------------------- */
typedef struct {
    /*
     * Map a physical address range into a CPU-accessible virtual address.
     * Must return a non-NULL pointer on success.
     * May return phys_addr cast to (void *) on flat-mapped systems.
     */
    void *(*iomap)(unsigned long phys_addr, unsigned int size);

    /* Busy-wait for at least 'us' microseconds. */
    void (*udelay)(unsigned int us);

    /*
     * Binary semaphore primitives for command-completion notification.
     * sem_create()  – allocate and return a semaphore handle (initial count 0).
     * sem_wait()    – block until signalled or timeout_ms expires.
     *                 Returns 0 on success, -1 on timeout.
     * sem_signal()  – release the semaphore (called from ISR context).
     * All three must be non-NULL together; if any is NULL the driver uses
     * polling mode instead of interrupt mode.
     */
    void *(*sem_create)(void);
    int   (*sem_wait)(void *sem, unsigned int timeout_ms);
    void  (*sem_signal)(void *sem);

    /*
     * Connect and enable an interrupt line.
     * irq_connect() – registers isr(arg) for interrupt number irqn.
     *                 Returns 0 on success.
     * irq_enable()  – unmasks the interrupt line.
     *                 Returns 0 on success.
     * Both must be non-NULL together to enable interrupt mode.
     */
    int (*irq_connect)(unsigned int irqn, void (*isr)(void *arg), void *arg);
    int (*irq_enable)(unsigned int irqn);

    /*
     * Heap allocator.  mem_alloc returns zero-initialised memory or NULL.
     * If NULL, the driver uses a single statically allocated device struct.
     */
    void *(*mem_alloc)(unsigned int size);
    void  (*mem_free)(void *ptr);

    /*
     * Controller-wide mutex for RTOS/SMP callers. This is separate from
     * sem_* above: sem_* only waits for command completion; this mutex
     * serializes full read/write/init transactions across cores.
     */
    void *(*mutex_create)(void);
    int   (*mutex_lock)(void *mutex);
    void  (*mutex_unlock)(void *mutex);
} BM1684X_SDHCI_OSAL;

/* --------------------------------------------------------------------------
 * MMC command descriptor
 * -------------------------------------------------------------------------- */
typedef struct {
    unsigned int  cmdIdx;      /* command index 0–63              */
    unsigned int  cmdArg;      /* 32-bit argument                  */
    unsigned int  respType;    /* BM1684X_RESP_* below             */
    unsigned int  resp[4];     /* response words filled by driver  */
    int           error;       /* 0 = OK, negative = error code    */
} BM1684X_MMC_CMD;

/* respType values */
#define BM1684X_RESP_NONE    0  /* no response          */
#define BM1684X_RESP_R1      1  /* 48-bit normal        */
#define BM1684X_RESP_R2      2  /* 136-bit CSD/CID      */
#define BM1684X_RESP_R3      3  /* 48-bit OCR (no CRC)  */
#define BM1684X_RESP_R4      4  /* 48-bit (SDIO)        */
#define BM1684X_RESP_R5      5  /* 48-bit (SDIO)        */
#define BM1684X_RESP_R6      6  /* 48-bit RCA           */
#define BM1684X_RESP_R7      7  /* 48-bit IF_COND       */
#define BM1684X_RESP_R1B     8  /* 48-bit + busy        */

/* --------------------------------------------------------------------------
 * MMC data descriptor
 * -------------------------------------------------------------------------- */
typedef struct {
    void         *buf;         /* data buffer (must be DMA-accessible)  */
    unsigned int  blkSize;     /* block size in bytes (typically 512)   */
    unsigned int  blkCount;    /* number of blocks                      */
    unsigned int  flags;       /* BM1684X_DATA_* below                  */
} BM1684X_MMC_DATA;

#define BM1684X_DATA_READ    (1u << 0)  /* host←card */
#define BM1684X_DATA_WRITE   (1u << 1)  /* host→card */

/* --------------------------------------------------------------------------
 * Device handle (opaque to caller, allocated by bm1684xSdhciInit)
 * -------------------------------------------------------------------------- */
typedef struct BM1684X_SDHCI_DEV BM1684X_SDHCI_DEV;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/*
 * Initialise one SDHCI controller.
 *
 *  pOsal       – OSAL callbacks (may contain NULLs for polling mode)
 *  regPhysBase – physical base address of SDHCI register block
 *                  eMMC: 0x50100000   SD: 0x50101000
 *  irqNum      – interrupt number (ignored when irq_connect is NULL)
 *  devIndex    – 0 = eMMC, 1 = SD/SDIO
 *  is64BitAddr – set 1 for 64-bit DMA addressing (recommended)
 *
 * Returns a device handle on success, NULL on failure.
 */
BM1684X_SDHCI_DEV *bm1684xSdhciInit(
    const BM1684X_SDHCI_OSAL *pOsal,
    unsigned long              regPhysBase,
    unsigned int               irqNum,
    unsigned int               devIndex,
    unsigned int               is64BitAddr);

/*
 * Send one MMC/eMMC command, optionally with data transfer.
 * pData may be NULL for command-only transfers.
 * Returns 0 on success, negative error code on failure.
 */
int bm1684xSdhciSendCmd(BM1684X_SDHCI_DEV *pDev,
                        BM1684X_MMC_CMD   *pCmd,
                        BM1684X_MMC_DATA  *pData);

/* Set clock frequency in Hz.  Returns 0 on success. */
int bm1684xSdhciSetClk(BM1684X_SDHCI_DEV *pDev, unsigned int clkHz);

/* Set bus width: 1, 4, or 8 bits.  Returns 0 on success. */
int bm1684xSdhciSetBusWidth(BM1684X_SDHCI_DEV *pDev, unsigned int width);

/*
 * Interrupt service routine — call this from your ISR when the SDHCI
 * interrupt fires.  Safe to call even in polling mode (will be a no-op).
 */
void bm1684xSdhciIsr(BM1684X_SDHCI_DEV *pDev);

/*
 * Returns 1 if a card is present, 0 otherwise.
 * eMMC always returns 1.
 */
int bm1684xSdhciCardPresent(BM1684X_SDHCI_DEV *pDev);

/*
 * 取最近一次 bm1684xSdhciSendCmd() 失败时刻、清空前捕获到的原始状态：
 *   GetLastIntStatus  – INT_STATUS | (ERR_INT_STATUS << 16)
 *   GetLastErrStatus  – 仅 ERR_INT_STATUS（具体哪类硬件错误）
 * 失败后立即调用才有意义，下一次 SendCmd 会覆盖。
 */
unsigned int bm1684xSdhciGetLastIntStatus(BM1684X_SDHCI_DEV *pDev);
unsigned int bm1684xSdhciGetLastErrStatus(BM1684X_SDHCI_DEV *pDev);

#ifdef __cplusplus
}
#endif

#endif /* BM1684X_SDHCI_OSAL_H */
