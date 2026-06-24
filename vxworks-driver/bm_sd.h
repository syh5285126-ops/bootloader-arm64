/******************************************************************************
 * bm_sd.h
 *
 * BM1684X SD 卡驱动对外接口（块设备级：以 512 字节扇区为单位读写）。
 * 由 bm_emmc_core.c（eMMC 版本）改造而来，协议时序细节改为照搬 U-Boot
 * drivers/mmc/mmc.c 里 SD 卡专属路径（sd_send_op_cond/mmc_send_if_cond/
 * mmc_startup 的 SD 分支/sd_select_bus_width），不再是"照搬思路、不照搬代码"，
 * 而是按用户明确要求"具体细节以 uboot 为准"。
 *
 * 层次：
 *   天脉3 FAT 块设备(fatBlkDrvDemo_os3.c)
 *     -> emmc_rd_sect0_2 / emmc_wr_sect0_2  (bm_sd_glue.c 桥接，符号名与 eMMC
 *        版本相同，二者二选一编译，见 bm_sd_glue_cfg.h)
 *       -> bm_sd_read_blocks / bm_sd_write_blocks  (本接口)
 *         -> bm1684xSdhciSendCmd() 命令/数据引擎 (vxworks-driver/bm1684xSdhci.c)
 *           -> SDHCI 寄存器 0x50101000（SD/SDIO 控制器，devIndex=1）
 ******************************************************************************/
#ifndef _BM_SD_H_
#define _BM_SD_H_

#include "bm_sd_types.h"

/*--------------------------------------------------------------------------
 * 可调参数（“先求稳”默认值：25MHz 安全档 + 4 位总线，不稳可继续下调）
 *------------------------------------------------------------------------*/

/* 数据传输工作时钟（Hz）。默认 25MHz，与 eMMC 版本同一安全档，
 * 低于 BM1684X_SD_CLK_MAX_HZ（SD 标称上限 50MHz），先求稳不挑战上限 */
#ifndef BM_SD_TRAN_CLK_HZ
#define BM_SD_TRAN_CLK_HZ       25000000U
#endif

/* 总线位宽：4 或 1。当前方案选 4 位，跟 eMMC 版本一致，不挑战性能 */
#ifndef BM_SD_BUS_WIDTH
#define BM_SD_BUS_WIDTH         4
#endif

/* SDMA 地址位宽：0=32位地址  1=64位地址。
 * 【已不再起决定作用，仅作初值】引擎层 hwInit() 现在照参考驱动按硬件能力
 * （CAPABILITIES1 bit27）自动判定并开启 64 位寻址，不依赖这个开关。
 * 上板实测确认本板 DRAM 在 4GB 以上（DMA 缓冲区物理地址高 32 位非 0），
 * 必须用 64 位寻址，否则 DMA 地址被截断、写卡数据阶段超时——这就是之前
 * 写失败的真正根因，详见 README 第十二节。 */
#ifndef BM_SD_USE_64BIT_DMA
#define BM_SD_USE_64BIT_DMA     0
#endif

#define BM_SD_BLOCK_SIZE        512U

/*--------------------------------------------------------------------------
 * 接口函数
 *------------------------------------------------------------------------*/

/* 初始化控制器并完成 SD 卡识别。可重复调用（内部有"已初始化"标志）。
 * 返回 BM_SD_OK 表示成功；卡未插入返回 BM_SD_ENOCARD。*/
int  bm_sd_init(void);

/* 强制重新初始化（清除已初始化标志后重新执行 bm_sd_init）。
 * 用于卡被拔出重新插入后的重新识别。*/
int  bm_sd_reinit(void);

/* 读若干扇区。lba：起始扇区号；count：扇区数；buf：接收缓冲（>= count*512）。*/
int  bm_sd_read_blocks(u32 lba, u32 count, void *buf);

/* 写若干扇区。lba：起始扇区号；count：扇区数；buf：数据源（>= count*512）。*/
int  bm_sd_write_blocks(u32 lba, u32 count, const void *buf);

/* 获取 SD 卡用户区总扇区数（每扇区 512 字节）。未初始化返回 0。*/
u32  bm_sd_get_block_count(void);

#endif /* _BM_SD_H_ */
