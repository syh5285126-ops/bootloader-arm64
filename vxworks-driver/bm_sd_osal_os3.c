/******************************************************************************
 * bm_sd_osal_os3.c
 *
 * 天脉3 (AcoreOS3) 对接 BM1684X SDHCI 引擎层（bm1684xSdhci.c）所需的
 * OSAL 回调实现（SD 卡版本）。逻辑与 bm_emmc_osal_os3.c 完全一致（地址 1:1
 * 平坦映射、堆分配清零、中断号未定先走纯轮询），只是换了开关宏和回调表的
 * 导出符号名，避免与 eMMC 版本同时编译时重复定义。
 ******************************************************************************/
#include "bm_sd_glue_cfg.h"
#ifdef BM1684X_SD

#include <stdlib.h>
#include <string.h>
#include "bm1684xSdhciOsal.h"
#include "bm_sd_osal_os3.h"

/* 天脉3 平台提供的微秒级延时函数（同 bm_emmc_osal_os3.c 假设：由天脉3 BSP
 * 全局导出，不在本仓库中，集成时如函数名不同请在此处改名对接）。*/
extern void delay_us(unsigned int time_us);

/*--------------------------------------------------------------------------
 * 地址映射：天脉3 物理地址 1:1 平坦映射（已向用户确认），iomap 直接把物理
 * 地址当指针返回，不做额外地址转换。
 *------------------------------------------------------------------------*/
static void *bmOs3SdIoMap(unsigned long physAddr, unsigned int size)
{
    (void)size;
    return (void *)physAddr;
}

static void bmOs3SdUdelay(unsigned int us)
{
    delay_us(us);
}

/*--------------------------------------------------------------------------
 * 堆内存分配：引擎层要求 mem_alloc 返回的内存已清零。
 *------------------------------------------------------------------------*/
static void *bmOs3SdMemAlloc(unsigned int size)
{
    void *p = malloc(size);
    if (p != NULL)
        memset(p, 0, size);
    return p;
}

static void bmOs3SdMemFree(void *ptr)
{
    free(ptr);
}

/*--------------------------------------------------------------------------
 * 中断号未核实，sem_*/irq_* 回调全部留空，引擎层检测到这些回调为 NULL 会
 * 自动走纯轮询模式，先求稳跑通；中断号确认后再接到天脉3 对应 API。
 *------------------------------------------------------------------------*/
const BM1684X_SDHCI_OSAL g_bm1684xOsalOs3Sd = {
    bmOs3SdIoMap,     /* iomap       */
    bmOs3SdUdelay,    /* udelay      */
    NULL,             /* sem_create  */
    NULL,             /* sem_wait    */
    NULL,             /* sem_signal  */
    NULL,             /* irq_connect */
    NULL,             /* irq_enable  */
    bmOs3SdMemAlloc,  /* mem_alloc   */
    bmOs3SdMemFree,   /* mem_free    */
};

#endif /* BM1684X_SD */
