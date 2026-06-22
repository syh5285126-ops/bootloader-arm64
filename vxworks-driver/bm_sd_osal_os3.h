/******************************************************************************
 * bm_sd_osal_os3.h
 *
 * 天脉3 (AcoreOS3) 对接 BM1684X SDHCI 引擎层（bm1684xSdhci.c）所需的
 * OSAL 回调表实例（SD 卡版本）。供 bm_sd_core.c 调用 bm1684xSdhciInit() 时使用。
 * 内容与 bm_emmc_osal_os3.h 完全相同，单独成文只是为了让 SD/eMMC 两套驱动各自
 * 独立、可单独排除编译（见 bm_sd_glue_cfg.h 的二选一说明）。
 ******************************************************************************/
#ifndef _BM_SD_OSAL_OS3_H_
#define _BM_SD_OSAL_OS3_H_

#include "bm1684xSdhciOsal.h"

extern const BM1684X_SDHCI_OSAL g_bm1684xOsalOs3Sd;

#endif /* _BM_SD_OSAL_OS3_H_ */
