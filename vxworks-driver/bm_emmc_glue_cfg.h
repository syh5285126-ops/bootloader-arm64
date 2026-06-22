/******************************************************************************
 * bm_emmc_glue_cfg.h
 *
 * BM1684X eMMC 驱动总开关。
 *
 * 用法：
 *   - 启用本驱动：保持下面 BM1684X_EMMC 定义打开；
 *   - 同时务必在天脉 IDE 工程里【排除】天脉3工程模板自带的 BM1684X eMMC 驱动
 *     样板（如果有的话），避免符号冲突：
 *       AcoreOs_fmsh_sdmmc_init / emmc_rd_sect0_* / emmc_wr_sect0_* 等符号
 *       已由本目录 bm_emmc_glue.c 重新实现，不能同时存在两份。
 *     【待确认】天脉3工程模板里是否自带这类样板，需要你打开天脉 IDE 工程核实，
 *     本仓库里看不到天脉工程结构（CLAUDE.md 缺口第 4 项）。
 *   - 天脉 FAT 块设备层 fatBlkDrvDemo_os3.c（本目录，从 rk3588 项目搬运改容量）
 *     依赖天脉3平台宏 TM_OS3_FAT32_FS 打开 FAT 子系统，该宏定义在天脉3工程自带
 *     的 cfg_build.h 里，不在本仓库中，集成时请按你工程实际路径调整 #include。
 ******************************************************************************/
#ifndef _BM_EMMC_GLUE_CFG_H_
#define _BM_EMMC_GLUE_CFG_H_

/* 打开本宏即启用 BM1684X eMMC 驱动 */
#define BM1684X_EMMC

#endif /* _BM_EMMC_GLUE_CFG_H_ */
