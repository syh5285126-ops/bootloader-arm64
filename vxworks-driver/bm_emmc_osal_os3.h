/******************************************************************************
 * bm_emmc_osal_os3.h
 *
 * 天脉3 (AcoreOS3) 对接 BM1684X SDHCI 引擎层（bm1684xSdhci.c）所需的
 * OSAL 回调表实例。供 bm_emmc_core.c 调用 bm1684xSdhciInit() 时使用。
 ******************************************************************************/
#ifndef _BM_EMMC_OSAL_OS3_H_
#define _BM_EMMC_OSAL_OS3_H_

#include "bm1684xSdhciOsal.h"

extern const BM1684X_SDHCI_OSAL g_bm1684xOsalOs3;

/* Optional BSP overrides for SMP-safe eMMC access. lock returns 0 on success. */
void *bmOs3EmmcMutexCreate(void);
int   bmOs3EmmcMutexLock(void *mutex, unsigned int timeoutMs);
void  bmOs3EmmcMutexUnlock(void *mutex);

#endif /* _BM_EMMC_OSAL_OS3_H_ */
