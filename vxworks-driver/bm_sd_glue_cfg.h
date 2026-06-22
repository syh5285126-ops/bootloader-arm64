/******************************************************************************
 * bm_sd_glue_cfg.h
 *
 * BM1684X SD 卡驱动总开关。
 *
 * 重要：本文件提供的符号（AcoreOs_fmsh_sdmmc_init / emmc_rd_sect0_* /
 * emmc_wr_sect0_* / bm_emmc_get_block_count）与 bm_emmc_glue.c 提供的符号
 * 【完全同名】——这是有意为之：fatBlkDrvDemo_os3.c 固定调用这些符号名，让 SD
 * 版本可以直接顶替 eMMC 版本，FAT 层代码不用改一行。
 *
 * 因此 BM1684X_EMMC 和 BM1684X_SD 两个宏【不能同时打开】，否则链接时符号
 * 重复定义。二选一：
 *   - 测 eMMC：打开 bm_emmc_glue_cfg.h 里的 BM1684X_EMMC，本文件 BM1684X_SD
 *     注释掉（或在天脉 IDE 工程里把 bm_sd_*.c 整组排除出编译）。
 *   - 测 SD 卡：打开本文件的 BM1684X_SD，并在天脉 IDE 工程里把 bm_emmc_*.c
 *     （bm_emmc_core.c / bm_emmc_glue.c / bm_emmc_osal_os3.c）整组排除出编译，
 *     bm_emmc.h / bm_emmc_types.h / bm_emmc_glue_cfg.h 这三个纯头文件不冲突，
 *     可以留着。
 *   - 同样别忘了排除天脉3工程模板自带的样板驱动（如果有的话），见
 *     bm_emmc_glue_cfg.h 里的说明，原因相同（符号冲突）。
 ******************************************************************************/
#ifndef _BM_SD_GLUE_CFG_H_
#define _BM_SD_GLUE_CFG_H_

/* 打开本宏即启用 BM1684X SD 卡驱动（与 BM1684X_EMMC 二选一，见上方说明） */
#define BM1684X_SD

#endif /* _BM_SD_GLUE_CFG_H_ */
