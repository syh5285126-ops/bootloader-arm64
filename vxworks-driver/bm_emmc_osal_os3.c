/******************************************************************************
 * bm_emmc_osal_os3.c
 *
 * 天脉3 (AcoreOS3) 对接 BM1684X SDHCI 引擎层（bm1684xSdhci.c）所需的
 * OSAL 回调实现。引擎层本身不依赖任何 RTOS SDK，靠这张回调表挂接
 * 延时/内存分配/信号量/中断，本文件就是这层“挂接”。
 *
 * 与 rk3588 项目的差异：rk3588 的底层（rk_emmc_sdhci.c）是直接从零写的
 * 寄存器级驱动，没有这层 OSAL 抽象；BM1684X 复用现成引擎层，所以多了
 * 这一个适配文件。
 ******************************************************************************/
#include "bm_emmc_glue_cfg.h"
#ifdef BM1684X_EMMC

#include <stdlib.h>
#include <string.h>
#include "bm1684xSdhciOsal.h"
#include "bm_emmc_osal_os3.h"

/* 天脉3 平台提供的微秒级延时函数（同 rk3588 项目假设：由天脉3 BSP 全局导出，
 * 不在本仓库中，集成时如函数名不同请在此处改名对接）。*/
extern void delay_us(unsigned int time_us);

/*--------------------------------------------------------------------------
 * 地址映射：天脉3 物理地址 1:1 平坦映射（已向用户确认），iomap 直接把物理
 * 地址当指针返回，不做额外地址转换。
 *------------------------------------------------------------------------*/
static void *bmOs3IoMap(unsigned long physAddr, unsigned int size)
{
    (void)size;
    return (void *)physAddr;
}

static void bmOs3Udelay(unsigned int us)
{
    delay_us(us);
}

/*--------------------------------------------------------------------------
 * 堆内存分配：引擎层要求 mem_alloc 返回的内存已清零。
 *------------------------------------------------------------------------*/
static void *bmOs3MemAlloc(unsigned int size)
{
    void *p = malloc(size);
    if (p != NULL)
        memset(p, 0, size);
    return p;
}

static void bmOs3MemFree(void *ptr)
{
    free(ptr);
}

/*
 * 天脉3工程可用 BSP/RTOS mutex 覆盖这三个弱符号。
 * 默认返回 NULL，core 会退到原子锁兜底；产品形态建议接真 mutex。
 */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void *bmOs3EmmcMutexCreate(void)
{
    return NULL;
}

#if defined(__GNUC__)
__attribute__((weak))
#endif
int bmOs3EmmcMutexLock(void *mutex)
{
    (void)mutex;
    return -1;
}

#if defined(__GNUC__)
__attribute__((weak))
#endif
void bmOs3EmmcMutexUnlock(void *mutex)
{
    (void)mutex;
}

/*--------------------------------------------------------------------------
 * 中断号未核实（CLAUDE.md 已记录为非阻塞缺口），sem_xxx/irq_xxx 回调全部留空，
 * 引擎层检测到这些回调为 NULL 会自动走纯轮询模式，先求稳跑通；
 * 中断号确认后，把 sem_create/sem_wait/sem_signal/irq_connect/irq_enable
 * 接到天脉3 对应的信号量/中断 API 即可切换到中断模式提速。
 *------------------------------------------------------------------------*/
const BM1684X_SDHCI_OSAL g_bm1684xOsalOs3 = {
    bmOs3IoMap,     /* iomap       */
    bmOs3Udelay,    /* udelay      */
    NULL,           /* sem_create  */
    NULL,           /* sem_wait    */
    NULL,           /* sem_signal  */
    NULL,           /* irq_connect */
    NULL,           /* irq_enable  */
    bmOs3MemAlloc,  /* mem_alloc   */
    bmOs3MemFree,   /* mem_free    */
    bmOs3EmmcMutexCreate, /* mutex_create */
    bmOs3EmmcMutexLock,   /* mutex_lock   */
    bmOs3EmmcMutexUnlock, /* mutex_unlock */
};

#endif /* BM1684X_EMMC */
