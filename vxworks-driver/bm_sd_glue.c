/******************************************************************************
 * bm_sd_glue.c
 *
 * 桥接层：向天脉3 FAT 块设备层（fatBlkDrvDemo_os3.c）提供它所调用的符号，
 * 内部转调 BM1684X SD 卡驱动。类比 bm_emmc_glue.c——【提供的符号名完全相同】，
 * 这样 fatBlkDrvDemo_os3.c 不用改一行代码就能从挂载 eMMC 切换成挂载 SD 卡，
 * 也正因为符号同名，本文件与 bm_emmc_glue.c 不能同时参与编译（见
 * bm_sd_glue_cfg.h 顶部的二选一说明）。
 *
 * 提供的符号：
 *   - int  AcoreOs_fmsh_sdmmc_init(int sdmmcID)
 *   - void emmc_rd_sect0   (int  block_idx, void *dst)
 *   - void emmc_wr_sect0   (int  block_idx, void *src)
 *   - void emmc_rd_sect0_2 (u32  block_idx, void *dst, u32 blk_cnt)
 *   - void emmc_wr_sect0_2 (u32  block_idx, void *src, u32 blk_cnt)
 *   - void emmc_rd_sect0_3 (int  block_idx, void *dst, u32 ulLength)
 *   - void emmc_wr_sect0_3 (int  block_idx, void *src, u32 ulLength)
 *   - u32  bm_emmc_get_block_count(void)   （fatBlkDrvDemo_os3.c 里 extern
 *     声明的就是这个名字，即使现在底下转调的是 SD 卡，符号名不能改）
 ******************************************************************************/
#include "bm_sd_glue_cfg.h"
#ifdef BM1684X_SD

#include <stdio.h>
#include "bm_sd.h"

/*--------------------------------------------------------------------------
 * 初始化入口：天脉 blk_dev_create() 会调用 AcoreOs_fmsh_sdmmc_init(major)
 *------------------------------------------------------------------------*/
int AcoreOs_fmsh_sdmmc_init(int sdmmcID)
{
    int ret;

    (void)sdmmcID;   /* 本项目只用一个 SD 控制器，忽略编号 */

    ret = bm_sd_init();
    if (ret != BM_SD_OK)
    {
        printf("[bm_sd] init FAILED, ret=%d\r\n", ret);
        return ret;
    }

    printf("[bm_sd] init OK, capacity = %u sectors (%u MB)\r\n",
           bm_sd_get_block_count(),
           (unsigned)((u64)bm_sd_get_block_count() / 2048U));
    return BM_SD_OK;
}

/*--------------------------------------------------------------------------
 * 单扇区读写（兼容旧接口/测试）
 *------------------------------------------------------------------------*/
void emmc_rd_sect0(int block_idx, void *dst)
{
    (void)bm_sd_read_blocks((u32)block_idx, 1U, dst);
}

void emmc_wr_sect0(int block_idx, void *src)
{
    (void)bm_sd_write_blocks((u32)block_idx, 1U, src);
}

/*--------------------------------------------------------------------------
 * 多扇区读写（天脉 FAT 块设备实际走这两个）
 *------------------------------------------------------------------------*/
void emmc_rd_sect0_2(u32 block_idx, void *dst, u32 blk_cnt)
{
    (void)bm_sd_read_blocks(block_idx, blk_cnt, dst);
}

void emmc_wr_sect0_2(u32 block_idx, void *src, u32 blk_cnt)
{
    (void)bm_sd_write_blocks(block_idx, blk_cnt, src);
}

/*--------------------------------------------------------------------------
 * 按字节长度读写（兼容旧接口/测试），长度按 512 折算扇区数
 *------------------------------------------------------------------------*/
void emmc_rd_sect0_3(int block_idx, void *dst, u32 ulLength)
{
    (void)bm_sd_read_blocks((u32)block_idx, ulLength / BM_SD_BLOCK_SIZE, dst);
}

void emmc_wr_sect0_3(int block_idx, void *src, u32 ulLength)
{
    (void)bm_sd_write_blocks((u32)block_idx, ulLength / BM_SD_BLOCK_SIZE, src);
}

/*--------------------------------------------------------------------------
 * 容量查询：符号名沿用 bm_emmc_get_block_count，理由见文件头注释
 * （fatBlkDrvDemo_os3.c 里 extern 声明的就是这个名字）。
 *------------------------------------------------------------------------*/
u32 bm_emmc_get_block_count(void)
{
    return bm_sd_get_block_count();
}

/*--------------------------------------------------------------------------
 * 上板自检：写-读-比对一个扇区，并打印容量。可在天脉 shell 里手动调用。
 * 注意：会改写所传扇区号的数据；SD 卡若有要保留的数据，请用空闲扇区测试。
 *------------------------------------------------------------------------*/
int bm_sd_selftest(u32 test_lba)
{
    static u8 wbuf[BM_SD_BLOCK_SIZE];
    static u8 rbuf[BM_SD_BLOCK_SIZE];
    int ret;
    u32 i;

    ret = bm_sd_init();
    if (ret != BM_SD_OK)
    {
        printf("[bm_sd] selftest: init fail %d\r\n", ret);
        return ret;
    }

    for (i = 0; i < BM_SD_BLOCK_SIZE; i++)
        wbuf[i] = (u8)(i + test_lba);

    ret = bm_sd_write_blocks(test_lba, 1U, wbuf);
    if (ret != BM_SD_OK)
    {
        printf("[bm_sd] selftest: write fail %d\r\n", ret);
        return ret;
    }

    for (i = 0; i < BM_SD_BLOCK_SIZE; i++)
        rbuf[i] = 0;

    ret = bm_sd_read_blocks(test_lba, 1U, rbuf);
    if (ret != BM_SD_OK)
    {
        printf("[bm_sd] selftest: read fail %d\r\n", ret);
        return ret;
    }

    for (i = 0; i < BM_SD_BLOCK_SIZE; i++)
    {
        if (rbuf[i] != wbuf[i])
        {
            printf("[bm_sd] selftest: MISMATCH at byte %u (w=0x%02X r=0x%02X)\r\n",
                   i, wbuf[i], rbuf[i]);
            return BM_SD_EIO;
        }
    }

    printf("[bm_sd] selftest PASS at lba %u, capacity=%u sectors\r\n",
           test_lba, bm_sd_get_block_count());
    return BM_SD_OK;
}

#endif /* BM1684X_SD */
