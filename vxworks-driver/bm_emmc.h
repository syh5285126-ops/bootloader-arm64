/******************************************************************************
 * bm_emmc.h
 *
 * BM1684X eMMC 驱动对外接口（块设备级：以 512 字节扇区为单位读写）。
 *
 * 层次：
 *   天脉3 FAT 块设备(fatBlkDrvDemo_os3.c)
 *     -> bm_emmc_rd_sect0_2 / bm_emmc_wr_sect0_2  (bm_emmc_glue.c 桥接)
 *       -> bm_emmc_read_blocks / bm_emmc_write_blocks  (本接口)
 *         -> bm1684xSdhciSendCmd() 命令/数据引擎 (vxworks-driver/bm1684xSdhci.c)
 *           -> SDHCI 寄存器 0x50100000
 ******************************************************************************/
#ifndef _BM_EMMC_H_
#define _BM_EMMC_H_

#include "bm_emmc_types.h"

/*--------------------------------------------------------------------------
 * 可调参数（“先求稳”默认值：25MHz 安全档 + 4 位总线，不稳可继续下调）
 *------------------------------------------------------------------------*/

/* 数据传输工作时钟（Hz）。默认 25MHz，对齐 BM1684X 文档里的 bypass 安全档 */
#ifndef BM_EMMC_TRAN_CLK_HZ
#define BM_EMMC_TRAN_CLK_HZ     25000000U
#endif

/* 总线位宽：4 或 1。当前方案选 4 位（跟 rk3588 一样），不挑战 8 位 */
#ifndef BM_EMMC_BUS_WIDTH
#define BM_EMMC_BUS_WIDTH       4
#endif

/* SDMA 地址位宽：0=32位地址  1=64位地址。BM1684X DRAM 物理地址范围未确认
 * 超过 4GB 的场景，先按 32 位求稳，需要时再打开 */
#ifndef BM_EMMC_USE_64BIT_DMA
#define BM_EMMC_USE_64BIT_DMA   0
#endif

#define BM_EMMC_BLOCK_SIZE      512U

/*--------------------------------------------------------------------------
 * 接口函数
 *------------------------------------------------------------------------*/

/* 初始化控制器并完成 eMMC 卡识别。可重复调用（内部有“已初始化”标志）。
 * 返回 BM_EMMC_OK 表示成功。*/
int  bm_emmc_init(void);

/* 强制重新初始化（清除已初始化标志后重新执行 bm_emmc_init）。*/
int  bm_emmc_reinit(void);

/* 读若干扇区。lba：起始扇区号；count：扇区数；buf：接收缓冲（>= count*512）。*/
int  bm_emmc_read_blocks(u32 lba, u32 count, void *buf);

/* 写若干扇区。lba：起始扇区号；count：扇区数；buf：数据源（>= count*512）。*/
int  bm_emmc_write_blocks(u32 lba, u32 count, const void *buf);

/* 获取 eMMC 用户区总扇区数（每扇区 512 字节）。未初始化返回 0。*/
u32  bm_emmc_get_block_count(void);

#endif /* _BM_EMMC_H_ */
