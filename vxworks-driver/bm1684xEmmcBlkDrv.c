/*
 * bm1684xEmmcBlkDrv.c — BM1684X eMMC 块设备驱动主体
 *
 * 对应 nvme demo 中的 nvme.c，包含三部分：
 *   1. OSAL 桥接：把自包含驱动 bm1684xSdhci.c 需要的 OS 原语映射到
 *      ACoreOs（此处仅提供 iomap/udelay/内存分配，信号量与中断字段留
 *      NULL，使 SDHCI 驱动以轮询模式运行，规避板级中断向量映射）。
 *   2. eMMC 卡识别：CMD0/1/2/3/9/7/8/16 序列，从 EXT_CSD 读取容量。
 *   3. 块读写：CMD17/18/24/25，经 DMA 弹跳缓冲区收发数据。
 *
 * 版权所有 (c) 2024 Bitmain / Sophgo.  SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <printk.h>
#include <acoreos.h>
#include <Semaphore.h>

#include "bm1684xSdhciHw.h"
#include "bm1684xEmmcBlkDrv.h"

/* timebase.c 提供的微秒级忙等待 */
extern void tbDelayus(unsigned int delayus);

#ifndef OK
#define OK    0
#endif
#ifndef ERROR
#define ERROR (-1)
#endif

/* -------------------------------------------------------------------------
 * 中断模式配置开关
 *
 *   BM_EMMC_USE_IRQ = 0：轮询模式（默认）。OSAL 不填信号量/中断字段，
 *                        bm1684xSdhci.c 自动以轮询方式收发命令。
 *   BM_EMMC_USE_IRQ = 1：中断模式。填充下方信号量与中断桥接，并在
 *                        bm1684xSdhciInit 中传入真实中断号。
 *
 *   BM1684X_EMMC_IRQ_NUM：eMMC 控制器在 GIC 上的中断号（SPI），板级
 *                        相关，启用中断模式前必须按实际硬件填写。
 * ------------------------------------------------------------------------- */
#ifndef BM_EMMC_USE_IRQ
#define BM_EMMC_USE_IRQ        0
#endif
#ifndef BM1684X_EMMC_IRQ_NUM
#define BM1684X_EMMC_IRQ_NUM   0     /* TODO: 填写实际 eMMC 中断号 */
#endif

#if BM_EMMC_USE_IRQ
/*
 * 以下符号由 RTOS 中断子系统头文件提供（与 nvme demo 一致）。若编译时
 * 报未声明，请改为包含对应的中断管理头文件而非依赖此处 extern。
 */
extern ACoreOs_status_code ACoreOs_interrupt_set_handler(
    UINT32 vector, ACoreOs_handler handler, UINT32 arg1, UINT32 arg2);
extern void enableIrqLine(UINT32 vector);
#endif

/* 全局唯一驱动实例（对应 demo 的 g_pDriver） */
BM_EMMC_DRIVE *g_pEmmcDrive = 0;

/* -------------------------------------------------------------------------
 * 第 1 部分：OSAL 桥接
 * ------------------------------------------------------------------------- */

/*
 * osalIomap — 物理地址映射。BM1684X 在 RTOS 下为平坦映射，直接
 * 将物理地址强制转换为虚拟指针返回。
 */
static void *osalIomap(unsigned long physAddr, unsigned int size)
{
    (void)size;
    return (void *)(unsigned long)physAddr;
}

/* osalUdelay — 微秒忙等待，复用 timebase 的通用计时器实现 */
static void osalUdelay(unsigned int us)
{
    tbDelayus(us);
}

/*
 * osalMemAlloc — 分配并清零（SDHCI 设备结构体非 DMA 用途，普通堆即可）。
 */
static void *osalMemAlloc(unsigned int size)
{
    void *p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static void osalMemFree(void *ptr)
{
    free(ptr);
}

#if BM_EMMC_USE_IRQ
/* ---- 中断模式专用：信号量桥接 ---- */

/* 为每个信号量生成唯一名字 EMMC_SEM0 / EMMC_SEM1 ... */
static int g_emmcSemSeq = 0;

/*
 * osalSemCreate — 创建初值 0 的二值信号量，供命令/传输完成通知。
 *   ISR 端 sem_signal 释放，调用方 sem_wait 阻塞获取。
 */
static void *osalSemCreate(void)
{
    char  name[16];
    Sem_ID sem;

    sprintf(name, "EMMC_SEM%d", g_emmcSemSeq++);
    sem = ACoreOsMP_semaphore_create(
        name, 0,
        ACOREOS_FIFO | ACOREOS_BINARY_SEMAPHORE,
        ACOREOS_NO_INHERIT_PRIORITY);
    return (void *)sem;
}

/*
 * osalSemWait — 阻塞等待信号量，timeout_ms 到期返回 -1。
 *   注意：ACoreOs_semaphore_obtain 的超时单位若为 tick 而非 ms，需在
 *   此处做 ms→tick 换算（取决于系统节拍频率）。
 */
static int osalSemWait(void *sem, unsigned int timeout_ms)
{
    ACoreOs_status_code r =
        ACoreOs_semaphore_obtain((Sem_ID)sem, ACOREOS_WAIT, timeout_ms);
    return (r == ACOREOS_SUCCESSFUL) ? 0 : -1;
}

/* osalSemSignal — 释放信号量（可从 ISR 上下文调用） */
static void osalSemSignal(void *sem)
{
    ACoreOs_semaphore_release((Sem_ID)sem);
}

/* ---- 中断模式专用：中断桥接 ---- */

/*
 * SDHCI 的 ISR 回调签名为 void(*)(void*)，而 ACoreOs 中断处理函数不带
 * 参数。此处用静态变量保存回调及其参数，注册一个无参 trampoline 转接，
 * 与 demo 中 nvmeMsiISR 的注册方式一致。
 */
static void (*g_emmcIsr)(void *arg) = 0;
static void  *g_emmcIsrArg          = 0;

static void emmcIrqTrampoline(void)
{
    if (g_emmcIsr)
        g_emmcIsr(g_emmcIsrArg);
}

static int osalIrqConnect(unsigned int irqn,
                          void (*isr)(void *arg), void *arg)
{
    g_emmcIsr    = isr;
    g_emmcIsrArg = arg;
    if (ACoreOs_interrupt_set_handler(
            (UINT32)irqn, (ACoreOs_handler)emmcIrqTrampoline, 0, 0) != OK)
        return -1;
    return 0;
}

static int osalIrqEnable(unsigned int irqn)
{
    enableIrqLine((UINT32)irqn);
    return 0;
}
#endif /* BM_EMMC_USE_IRQ */

/*
 * 构造 OSAL 回调表。
 *   轮询模式：仅填 iomap/udelay/内存，信号量与中断字段留 NULL，
 *             bm1684xSdhciInit 检测到后自动以轮询模式工作。
 *   中断模式：额外填充信号量与中断桥接，启用 ISR 驱动收发。
 */
static void emmcBuildOsal(BM1684X_SDHCI_OSAL *pOsal)
{
    memset(pOsal, 0, sizeof(*pOsal));
    pOsal->iomap     = osalIomap;
    pOsal->udelay    = osalUdelay;
    pOsal->mem_alloc = osalMemAlloc;
    pOsal->mem_free  = osalMemFree;

#if BM_EMMC_USE_IRQ
    pOsal->sem_create  = osalSemCreate;
    pOsal->sem_wait    = osalSemWait;
    pOsal->sem_signal  = osalSemSignal;
    pOsal->irq_connect = osalIrqConnect;
    pOsal->irq_enable  = osalIrqEnable;
#endif
}

/* -------------------------------------------------------------------------
 * 第 2 部分：eMMC 卡识别
 * ------------------------------------------------------------------------- */

/* 发送一条无数据命令的薄封装 */
static int emmcCmd(BM_EMMC_DRIVE *pDrive, unsigned int idx,
                   unsigned int arg, unsigned int respType)
{
    BM1684X_MMC_CMD cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdIdx   = idx;
    cmd.cmdArg   = arg;
    cmd.respType = respType;
    return bm1684xSdhciSendCmd(pDrive->pSdhci, &cmd, 0);
}

/*
 * 诊断专用：CMD8 发送前先用 CPU 把 bounceBuf 填成这个"毒药"模式（不经过
 * DMA）。如果读回来的内容仍然是这个模式，说明 DMA 这次根本没有把卡返回
 * 的数据写进来（残留的是诊断填充值，不是上一次真实数据，也不是真的全
 * 0），借此把"DMA 真的没传数据"和"DRAM/卡上一次的残留数据被重复读到"
 * 这两种情况区分开。排查结束后随本轮诊断代码一起删除。
 */
#define BM_EMMC_DIAG_POISON   0xA5U

/*
 * emmcReadExtCsd — 通过 CMD8 读取 512 字节 EXT_CSD 到弹跳缓冲区。
 * 返回 0 成功。
 */
static int emmcReadExtCsd(BM_EMMC_DRIVE *pDrive, unsigned char *pExtCsd)
{
    BM1684X_MMC_CMD  cmd;
    BM1684X_MMC_DATA data;
    int rc;

    memset(&cmd, 0, sizeof(cmd));
    memset(&data, 0, sizeof(data));

    /* 诊断：发命令前先用 CPU 写一遍毒药模式，覆盖掉 DRAM 里可能残留的
     * 上一次数据，确保事后能分辨这次 DMA 是否真的写入了新数据 */
    memset(pDrive->bounceBuf, (int)BM_EMMC_DIAG_POISON, BM_EMMC_EXTCSD_SIZE);

    cmd.cmdIdx   = MMC_CMD_SEND_EXT_CSD;
    cmd.cmdArg   = 0;
    cmd.respType = BM1684X_RESP_R1;

    data.buf      = pDrive->bounceBuf;     /* DMA 相干缓冲区 */
    data.blkSize  = BM_EMMC_EXTCSD_SIZE;
    data.blkCount = 1;
    data.flags    = BM1684X_DATA_READ;

    rc = bm1684xSdhciSendCmd(pDrive->pSdhci, &cmd, &data);
    if (rc != 0)
        return rc;

    /* 诊断：控制器报告传输成功后，立即读回 DMA 地址寄存器实际值。
     *
     * 注意：SDMA 系统地址寄存器（64 位模式下复用为 ADMA_SA_LOW/HIGH）在传输
     * 过程中会随每次搬运的字节数硬件自增——一次成功的 512 字节传输完成后，
     * 寄存器读回值应该是"起始地址 + 512"，而不是起始地址本身。之前那版诊断
     * 直接拿寄存器值跟传输前的起始地址比较，即便传输完全正常也会报
     * MISMATCH，结论不可信。这里改成同时打印寄存器原始值、起始地址、以及
     * 二者的差值（delta），由 delta 是否恰好等于 BM_EMMC_EXTCSD_SIZE 来判断
     * 寄存器自增行为是否正常，而不是简单比较是否相等。 */
    {
        unsigned int addrLow = 0, addrHigh = 0;
        unsigned long long start = (unsigned long long)(unsigned long)pDrive->bounceBuf;
        unsigned long long reg;
        long long delta;
        bm1684xSdhciGetLastDmaAddr(pDrive->pSdhci, &addrLow, &addrHigh);
        reg   = ((unsigned long long)addrHigh << 32) | addrLow;
        delta = (long long)(reg - start);
        printk("eMMC: extCsd diag: dmaAddrReg=0x%08x_%08x start=0x%08x_%08x delta=%lld (expectDelta=%u) %s\n",
               addrHigh, addrLow,
               (unsigned int)(start >> 32), (unsigned int)(start & 0xFFFFFFFFUL),
               delta, (unsigned int)BM_EMMC_EXTCSD_SIZE,
               (delta == (long long)BM_EMMC_EXTCSD_SIZE) ? "DELTA-OK(自增正常)" : "DELTA-UNEXPECTED(需进一步排查)");
    }

    memcpy(pExtCsd, pDrive->bounceBuf, BM_EMMC_EXTCSD_SIZE);
    return 0;
}

/*
 * emmcCardIdentify — 完整 eMMC 上电与识别序列，结束后卡处于 transfer
 * 状态，pDrive 中容量信息已填好。返回 0 成功。
 */
static int emmcCardIdentify(BM_EMMC_DRIVE *pDrive)
{
    int          rc;
    unsigned int i;
    unsigned int ocr = 0;
    unsigned char *extCsd;

    /* CMD0：复位卡到 idle 状态 */
    rc = emmcCmd(pDrive, MMC_CMD_GO_IDLE_STATE, 0, BM1684X_RESP_NONE);
    if (rc != 0) { printk("eMMC: CMD0 fail %d\n", rc); return ERROR; }
    tbDelayus(2000);

    /* CMD1：循环发送 OCR，等待卡完成上电（busy 位置 1） */
    for (i = 0; i < 1000U; i++) {
        BM1684X_MMC_CMD cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmdIdx   = MMC_CMD_SEND_OP_COND;
        cmd.cmdArg   = MMC_OCR_SECTOR_MODE | MMC_OCR_VOLTAGE_WIN;
        cmd.respType = BM1684X_RESP_R3;
        rc = bm1684xSdhciSendCmd(pDrive->pSdhci, &cmd, 0);
        if (rc != 0) { printk("eMMC: CMD1 fail %d\n", rc); return ERROR; }

        ocr = cmd.resp[0];
        if (ocr & MMC_OCR_BUSY)
            break;          /* 上电完成 */
        tbDelayus(1000);
    }
    if (!(ocr & MMC_OCR_BUSY)) {
        printk("eMMC: CMD1 power-up timeout\n");
        return ERROR;
    }
    /* OCR bit30=1 表示卡支持扇区寻址（容量 >2GB） */
    pDrive->highCap = (ocr & MMC_OCR_SECTOR_MODE) ? 1U : 0U;

    /* CMD2：读取 CID（仅完成识别流程，内容此处不解析） */
    rc = emmcCmd(pDrive, MMC_CMD_ALL_SEND_CID, 0, BM1684X_RESP_R2);
    if (rc != 0) { printk("eMMC: CMD2 fail %d\n", rc); return ERROR; }

    /* CMD3：主机为 eMMC 分配 RCA（约定使用 1） */
    pDrive->rca = 1;
    rc = emmcCmd(pDrive, MMC_CMD_SET_RELATIVE_ADDR,
                 pDrive->rca << 16, BM1684X_RESP_R1);
    if (rc != 0) { printk("eMMC: CMD3 fail %d\n", rc); return ERROR; }

    /* CMD9：读取 CSD（识别流程必需；容量以 EXT_CSD 为准） */
    rc = emmcCmd(pDrive, MMC_CMD_SEND_CSD,
                 pDrive->rca << 16, BM1684X_RESP_R2);
    if (rc != 0) { printk("eMMC: CMD9 fail %d\n", rc); return ERROR; }

    /* CMD7：选中卡，进入 transfer 状态 */
    rc = emmcCmd(pDrive, MMC_CMD_SELECT_CARD,
                 pDrive->rca << 16, BM1684X_RESP_R1B);
    if (rc != 0) { printk("eMMC: CMD7 fail %d\n", rc); return ERROR; }

    /* 按 BM_EMMC_BUS_WIDTH 切换总线宽度，并提升时钟到工作频率 */
#if   BM_EMMC_BUS_WIDTH == 8
    rc = emmcCmd(pDrive, MMC_CMD_SWITCH,
                 MMC_SWITCH_BUS_WIDTH(EXT_CSD_BUS_WIDTH_8BIT), BM1684X_RESP_R1B);
#elif BM_EMMC_BUS_WIDTH == 4
    rc = emmcCmd(pDrive, MMC_CMD_SWITCH,
                 MMC_SWITCH_BUS_WIDTH(EXT_CSD_BUS_WIDTH_4BIT), BM1684X_RESP_R1B);
#else  /* 1 位 */
    rc = emmcCmd(pDrive, MMC_CMD_SWITCH,
                 MMC_SWITCH_BUS_WIDTH(EXT_CSD_BUS_WIDTH_1BIT), BM1684X_RESP_R1B);
#endif
    if (rc == 0)
        bm1684xSdhciSetBusWidth(pDrive->pSdhci, BM_EMMC_BUS_WIDTH);
    else
        printk("eMMC: CMD6 set bus width(%d) fail %d (continue 1-bit)\n",
               BM_EMMC_BUS_WIDTH, rc);

    bm1684xSdhciSetClk(pDrive->pSdhci, BM1684X_EMMC_CLK_MAX_HZ);

    /* CMD16：设置块长度 512（高容量卡固定 512，仍按规范下发） */
    rc = emmcCmd(pDrive, MMC_CMD_SET_BLOCKLEN, BM_EMMC_BLK_SIZE,
                 BM1684X_RESP_R1);
    if (rc != 0) {
        if (pDrive->highCap) {
            /* 高容量 eMMC 块大小固定 512，CMD16 为可选，忽略错误继续识别 */
            printk("eMMC: CMD16 ignored for high-capacity eMMC\n");
        } else {
            printk("eMMC: CMD16 fail %d\n", rc);
            return ERROR;
        }
    }

    /* CMD8：读取 EXT_CSD，从 SEC_COUNT 解析总扇区数 */
    extCsd = (unsigned char *)malloc(BM_EMMC_EXTCSD_SIZE);
    if (!extCsd) { printk("eMMC: extCsd malloc fail\n"); return ERROR; }

    rc = emmcReadExtCsd(pDrive, extCsd);
    if (rc != 0) {
        printk("eMMC: CMD8 read EXT_CSD fail %d\n", rc);
        free(extCsd);
        return ERROR;
    }

    /* 诊断：确认 EXT_CSD 是否真的收到了卡返回的数据，而不是全 0 的陈旧缓冲区 */
    printk("eMMC: extCsd dump: rev=%d csdStruct=%d devType=0x%02x "
           "secCount=%02x%02x%02x%02x bytes[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x\n",
           extCsd[192], extCsd[194], extCsd[196],
           extCsd[215], extCsd[214], extCsd[213], extCsd[212],
           extCsd[0], extCsd[1], extCsd[2], extCsd[3],
           extCsd[4], extCsd[5], extCsd[6], extCsd[7]);

    /* 诊断：判断这次读到的内容是否仍是发命令前 CPU 填的毒药模式（0xA5）。
     * 是 → DMA 这次根本没有写入新数据，控制器/卡没有真正完成这次传输；
     * 不是 → 这次确实发生了一次真实的数据写入（不论内容对不对）。 */
    {
        unsigned int j;
        int allPoison = 1;
        for (j = 0; j < BM_EMMC_EXTCSD_SIZE; j++) {
            if (extCsd[j] != (unsigned char)BM_EMMC_DIAG_POISON) {
                allPoison = 0;
                break;
            }
        }
        if (allPoison)
            printk("eMMC: extCsd diag: buffer UNCHANGED from poison 0x%02x "
                   "-> DMA did NOT write new data this time\n", BM_EMMC_DIAG_POISON);
        else
            printk("eMMC: extCsd diag: buffer WAS overwritten by DMA this time\n");
    }

    /* SEC_COUNT 为 4 字节小端，单位为 512 字节扇区 */
    pDrive->numOfSectors =
        ((unsigned int)extCsd[EXT_CSD_SEC_COUNT + 0]      ) |
        ((unsigned int)extCsd[EXT_CSD_SEC_COUNT + 1] <<  8) |
        ((unsigned int)extCsd[EXT_CSD_SEC_COUNT + 2] << 16) |
        ((unsigned int)extCsd[EXT_CSD_SEC_COUNT + 3] << 24);
    pDrive->bytes = BM_EMMC_BLK_SIZE;

    free(extCsd);
    return 0;
}

/* -------------------------------------------------------------------------
 * 第 3 部分：块读写
 * ------------------------------------------------------------------------- */

/*
 * emmcXfer — 单段（≤ BM_EMMC_BOUNCE_BLOCKS 扇区）读或写。
 * 数据统一经 DMA 相干弹跳缓冲区收发，绕开上层缓冲区 cache 问题。
 */
static int emmcXfer(BM_EMMC_DRIVE *pDrive, unsigned int startBlk,
                    unsigned int nBlks, char *pBuf, int isWrite)
{
    BM1684X_MMC_CMD  cmd;
    BM1684X_MMC_DATA data;
    unsigned int     arg;
    int              rc;

    /* 高容量卡使用扇区寻址；低容量卡参数为字节地址 */
    arg = pDrive->highCap ? startBlk : (startBlk * BM_EMMC_BLK_SIZE);

    memset(&cmd, 0, sizeof(cmd));
    memset(&data, 0, sizeof(data));

    if (isWrite) {
        /* 写：先把用户数据搬入弹跳缓冲区 */
        memcpy(pDrive->bounceBuf, pBuf, nBlks * BM_EMMC_BLK_SIZE);
        cmd.cmdIdx = (nBlks > 1) ? MMC_CMD_WRITE_MULTIPLE_BLOCK
                                 : MMC_CMD_WRITE_SINGLE_BLOCK;
        data.flags = BM1684X_DATA_WRITE;
    } else {
        cmd.cmdIdx = (nBlks > 1) ? MMC_CMD_READ_MULTIPLE_BLOCK
                                 : MMC_CMD_READ_SINGLE_BLOCK;
        data.flags = BM1684X_DATA_READ;
    }

    cmd.cmdArg   = arg;
    cmd.respType = BM1684X_RESP_R1;

    data.buf      = pDrive->bounceBuf;
    data.blkSize  = BM_EMMC_BLK_SIZE;
    data.blkCount = nBlks;

    rc = bm1684xSdhciSendCmd(pDrive->pSdhci, &cmd, &data);
    if (rc != 0)
        return rc;

    if (!isWrite)
        memcpy(pBuf, pDrive->bounceBuf, nBlks * BM_EMMC_BLK_SIZE);

    return 0;
}

/* 公共读写主体：加互斥锁，分段循环 */
static int emmcBlkRw(BM_EMMC_DRIVE *pDrive, unsigned int startBlk,
                     unsigned int nBlks, char *pBuf, int isWrite)
{
    int rc = 0;
    ACoreOs_status_code semret;

    if (!pDrive || !pBuf)
        return ERROR;
    if ((startBlk + nBlks) > pDrive->numOfSectors)
        return ERROR;

    /* 进入临界区：读写互斥 */
    semret = ACoreOs_semaphore_obtain(pDrive->muteSem, ACOREOS_WAIT, 0);
    if (semret != ACOREOS_SUCCESSFUL) {
        printk("eMMC: obtain muteSem ERROR %d\n", semret);
        return ERROR;
    }

    while (nBlks > 0) {
        unsigned int chunk = (nBlks > BM_EMMC_BOUNCE_BLOCKS)
                             ? BM_EMMC_BOUNCE_BLOCKS : nBlks;

        rc = emmcXfer(pDrive, startBlk, chunk, pBuf, isWrite);
        if (rc != 0)
            break;

        startBlk += chunk;
        nBlks    -= chunk;
        pBuf     += chunk * BM_EMMC_BLK_SIZE;
    }

    semret = ACoreOs_semaphore_release(pDrive->muteSem);
    if (semret != ACOREOS_SUCCESSFUL)
        printk("eMMC: release muteSem ERROR %d\n", semret);

    return rc;
}

int bm1684xEmmcBlkRd(BM_EMMC_DRIVE *pDrive, unsigned int startBlk,
                     unsigned int nBlks, char *pBuf)
{
    return emmcBlkRw(pDrive, startBlk, nBlks, pBuf, 0);
}

int bm1684xEmmcBlkWr(BM_EMMC_DRIVE *pDrive, unsigned int startBlk,
                     unsigned int nBlks, char *pBuf)
{
    return emmcBlkRw(pDrive, startBlk, nBlks, pBuf, 1);
}

/* -------------------------------------------------------------------------
 * 驱动入口（对应 demo 的 nvmeDriverInit）
 * ------------------------------------------------------------------------- */

int bm1684xEmmcDriverInit(void)
{
    BM1684X_SDHCI_OSAL osal;

    /* 1. 分配驱动上下文 */
    g_pEmmcDrive = (BM_EMMC_DRIVE *)malloc(sizeof(BM_EMMC_DRIVE));
    if (g_pEmmcDrive == 0) {
        printk("eMMC: g_pEmmcDrive malloc Fail!\n");
        return ERROR;
    }
    memset(g_pEmmcDrive, 0, sizeof(BM_EMMC_DRIVE));
    g_pEmmcDrive->state = BM_EMMC_DEV_INIT;

    /* 2. 创建读写互斥信号量（初值 1，二值互斥） */
    g_pEmmcDrive->muteSem = ACoreOsMP_semaphore_create(
        "EMMC_MUTE", 1,
        ACOREOS_FIFO | ACOREOS_MUTEX_SEMAPHORE,
        ACOREOS_NO_INHERIT_PRIORITY);
    if (g_pEmmcDrive->muteSem == 0) {
        printk("eMMC: create EMMC_MUTE Fail!\n");
        return ERROR;
    }

    /* 3. 分配 DMA 相干弹跳缓冲区 */
    g_pEmmcDrive->bounceBuf =
        ACoreOs_cache_dmamalloc(BM_EMMC_BOUNCE_BLOCKS * BM_EMMC_BLK_SIZE);
    if (g_pEmmcDrive->bounceBuf == 0) {
        printk("eMMC: bounceBuf dmamalloc Fail!\n");
        return ERROR;
    }

    /* 4. 初始化底层 SDHCI 控制器（轮询模式，64 位 DMA） */
    emmcBuildOsal(&osal);
    g_pEmmcDrive->pSdhci = bm1684xSdhciInit(
        &osal,
        BM1684X_EMMC_PHYS_BASE,
        BM1684X_EMMC_IRQ_NUM,   /* 中断模式使用真实中断号；轮询模式忽略 */
        BM1684X_EMMC_INDEX,
        1);                     /* 使用 64 位 DMA 地址 */
    if (g_pEmmcDrive->pSdhci == 0) {
        printk("eMMC: bm1684xSdhciInit Fail!\n");
        g_pEmmcDrive->state = BM_EMMC_DEV_NONE;
        return ERROR;
    }

    /* 5. 执行 eMMC 卡识别，获取容量 */
    if (emmcCardIdentify(g_pEmmcDrive) != 0) {
        printk("eMMC: card identify Fail!\n");
        g_pEmmcDrive->state = BM_EMMC_DEV_ID_FAIL;
        return ERROR;
    }

    g_pEmmcDrive->state = BM_EMMC_DEV_OK;
    printk("eMMC ready: numOfSectors=0x%x, blocksize=%d bytes (%d MB)\n",
           g_pEmmcDrive->numOfSectors, g_pEmmcDrive->bytes,
           g_pEmmcDrive->numOfSectors / 2048U);

    return OK;
}
