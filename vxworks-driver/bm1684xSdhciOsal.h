/*
 * bm1684xSdhciOsal.h — BM1684X SDHCI 驱动操作系统抽象层（OSAL）
 *
 * 调用方在调用 bm1684xSdhciInit() 之前填充 BM1684X_SDHCI_OSAL 结构体。
 * 任何字段置为 NULL，驱动将对该功能回退到轮询模式。
 * 若 OSAL 全部为 NULL，驱动将以纯轮询模式运行，适用于裸机或早期启动环境。
 *
 * 版权所有 (c) 2024 Bitmain.  保留所有权利。
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BM1684X_SDHCI_OSAL_H
#define BM1684X_SDHCI_OSAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * OSAL 回调函数表
 *
 * 驱动不直接调用任何 OS API，而是通过此结构体中的函数指针与 OS 交互。
 * 集成时，将 RTOS 对应函数的地址填入各字段即可。
 * -------------------------------------------------------------------------- */
typedef struct {
    /*
     * iomap — 将物理地址映射为 CPU 可直接访问的虚拟地址。
     *   phys_addr : 寄存器块的物理基地址
     *   size      : 映射大小（字节）
     *   返回值    : 成功返回非 NULL 虚拟地址；平坦映射系统可直接将物理地址
     *               强制转换为 (void *) 返回。
     */
    void *(*iomap)(unsigned long phys_addr, unsigned int size);

    /*
     * udelay — 微秒级忙等待。
     *   us : 等待的微秒数（至少等待 us 微秒）
     */
    void (*udelay)(unsigned int us);

    /*
     * 二值信号量原语，用于命令完成通知（中断模式专用）。
     *
     *   sem_create() — 分配并返回一个初始计数为 0 的信号量句柄。
     *   sem_wait()   — 阻塞等待信号量被释放，或 timeout_ms 到期。
     *                  返回 0 表示成功，-1 表示超时。
     *   sem_signal() — 释放信号量（可从 ISR 上下文调用）。
     *
     * 注意：三个函数必须同时非 NULL，否则驱动退回轮询模式。
     */
    void *(*sem_create)(void);
    int   (*sem_wait)(void *sem, unsigned int timeout_ms);
    void  (*sem_signal)(void *sem);

    /*
     * 中断注册与使能（中断模式专用）。
     *
     *   irq_connect() — 将 isr(arg) 注册为中断号 irqn 的处理函数。
     *                   返回 0 表示成功。
     *   irq_enable()  — 解除中断线屏蔽（上挂到中断控制器）。
     *                   返回 0 表示成功。
     *
     * 注意：两个函数必须同时非 NULL，才能启用中断模式。
     */
    int (*irq_connect)(unsigned int irqn, void (*isr)(void *arg), void *arg);
    int (*irq_enable)(unsigned int irqn);

    /*
     * 堆内存分配器。
     *   mem_alloc — 分配 size 字节并清零，失败返回 NULL。
     *               若为 NULL，驱动使用内部静态设备结构体（全局唯一）。
     *   mem_free  — 释放 mem_alloc 分配的内存。
     */
    void *(*mem_alloc)(unsigned int size);
    void  (*mem_free)(void *ptr);
} BM1684X_SDHCI_OSAL;

/* --------------------------------------------------------------------------
 * MMC 命令描述符
 * -------------------------------------------------------------------------- */
typedef struct {
    unsigned int  cmdIdx;      /* 命令索引 0–63                          */
    unsigned int  cmdArg;      /* 32 位命令参数                           */
    unsigned int  respType;    /* 响应类型，取值见下方 BM1684X_RESP_* 宏 */
    unsigned int  resp[4];     /* 响应数据（由驱动填写）                  */
    int           error;       /* 0 = 成功，负值 = 错误码                */
} BM1684X_MMC_CMD;

/* respType 取值 */
#define BM1684X_RESP_NONE    0  /* 无响应                   */
#define BM1684X_RESP_R1      1  /* R1：48 位普通响应        */
#define BM1684X_RESP_R2      2  /* R2：136 位 CSD/CID 响应  */
#define BM1684X_RESP_R3      3  /* R3：48 位 OCR（无 CRC）  */
#define BM1684X_RESP_R4      4  /* R4：48 位（SDIO）        */
#define BM1684X_RESP_R5      5  /* R5：48 位（SDIO）        */
#define BM1684X_RESP_R6      6  /* R6：48 位 RCA            */
#define BM1684X_RESP_R7      7  /* R7：48 位 IF_COND        */
#define BM1684X_RESP_R1B     8  /* R1b：48 位 + 忙碌信号    */

/* --------------------------------------------------------------------------
 * MMC 数据描述符
 * -------------------------------------------------------------------------- */
typedef struct {
    void         *buf;         /* 数据缓冲区指针（必须为 DMA 可访问地址）  */
    unsigned int  blkSize;     /* 块大小（字节，通常为 512）               */
    unsigned int  blkCount;    /* 块数量                                   */
    unsigned int  flags;       /* 传输方向，见下方 BM1684X_DATA_* 宏       */
} BM1684X_MMC_DATA;

#define BM1684X_DATA_READ    (1u << 0)  /* 读操作（卡 → 主机） */
#define BM1684X_DATA_WRITE   (1u << 1)  /* 写操作（主机 → 卡） */

/* --------------------------------------------------------------------------
 * 设备句柄（对调用方不透明，由 bm1684xSdhciInit 分配）
 * -------------------------------------------------------------------------- */
typedef struct BM1684X_SDHCI_DEV BM1684X_SDHCI_DEV;

/* --------------------------------------------------------------------------
 * 公开 API 声明
 * -------------------------------------------------------------------------- */

/*
 * bm1684xSdhciInit — 初始化一个 SDHCI 控制器实例。
 *
 *   pOsal       : OSAL 回调表（任意字段可为 NULL 以启用轮询模式）
 *   regPhysBase : SDHCI 寄存器块的物理基地址
 *                   eMMC：0x50100000   SD：0x50101000
 *   irqNum      : 中断号（irq_connect 为 NULL 时忽略）
 *   devIndex    : 0 = eMMC，1 = SD/SDIO
 *   is64BitAddr : 1 = 使用 64 位 DMA 地址（推荐）
 *
 *   返回值      : 成功返回设备句柄，失败返回 NULL。
 */
BM1684X_SDHCI_DEV *bm1684xSdhciInit(
    const BM1684X_SDHCI_OSAL *pOsal,
    unsigned long              regPhysBase,
    unsigned int               irqNum,
    unsigned int               devIndex,
    unsigned int               is64BitAddr);

/*
 * bm1684xSdhciSendCmd — 发送一条 MMC/eMMC 命令，可选带数据传输。
 *   pData 为 NULL 时执行纯命令传输。
 *   返回 0 表示成功，负值表示错误。
 */
int bm1684xSdhciSendCmd(BM1684X_SDHCI_DEV *pDev,
                        BM1684X_MMC_CMD   *pCmd,
                        BM1684X_MMC_DATA  *pData);

/*
 * bm1684xSdhciSetClk — 设置 SD/eMMC 总线时钟频率（Hz）。
 *   返回 0 表示成功。
 */
int bm1684xSdhciSetClk(BM1684X_SDHCI_DEV *pDev, unsigned int clkHz);

/*
 * bm1684xSdhciSetBusWidth — 设置总线宽度：1、4 或 8 位。
 *   返回 0 表示成功。
 */
int bm1684xSdhciSetBusWidth(BM1684X_SDHCI_DEV *pDev, unsigned int width);

/*
 * bm1684xSdhciIsr — 中断服务例程入口。
 *   当 SDHCI 中断触发时，在 RTOS ISR 中调用此函数。
 *   轮询模式下调用为空操作（无副作用）。
 */
void bm1684xSdhciIsr(BM1684X_SDHCI_DEV *pDev);

/*
 * bm1684xSdhciCardPresent — 检测卡是否在位。
 *   eMMC 固定返回 1（始终在位）；SD 读取检测引脚状态。
 *   返回 1 表示卡在位，0 表示未插卡。
 */
int bm1684xSdhciCardPresent(BM1684X_SDHCI_DEV *pDev);

#ifdef __cplusplus
}
#endif

#endif /* BM1684X_SDHCI_OSAL_H */
