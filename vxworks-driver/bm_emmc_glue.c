/******************************************************************************
 * bm_emmc_glue.c
 *
 * 桥接层：向天脉3 FAT 块设备层（fatBlkDrvDemo_os3.c）提供它所调用的符号，
 * 内部转调 BM1684X eMMC 驱动。类比 rk3588 项目的 rk_emmc_glue.c。
 *
 * 提供的符号（与天脉3工程模板/rk3588 项目一致的命名，保证天脉 FAT 层无需改动）：
 *   - int  AcoreOs_fmsh_sdmmc_init(int sdmmcID)
 *   - void emmc_rd_sect0   (int  block_idx, void *dst)
 *   - void emmc_wr_sect0   (int  block_idx, void *src)
 *   - void emmc_rd_sect0_2 (u32  block_idx, void *dst, u32 blk_cnt)
 *   - void emmc_wr_sect0_2 (u32  block_idx, void *src, u32 blk_cnt)
 *   - void emmc_rd_sect0_3 (int  block_idx, void *dst, u32 ulLength)
 *   - void emmc_wr_sect0_3 (int  block_idx, void *src, u32 ulLength)
 ******************************************************************************/
#include "bm_emmc_glue_cfg.h"
#ifdef BM1684X_EMMC

#include <stdio.h>
#include "bm_emmc.h"

/*--------------------------------------------------------------------------
 * 初始化入口：天脉 blk_dev_create() 会调用 AcoreOs_fmsh_sdmmc_init(major)
 *------------------------------------------------------------------------*/
int AcoreOs_fmsh_sdmmc_init(int sdmmcID)
{
    int ret;

    (void)sdmmcID;   /* 本项目只用一个 eMMC 控制器，忽略编号 */

    ret = bm_emmc_init();
    if (ret != BM_EMMC_OK)
    {
        printf("[bm_emmc] init FAILED, ret=%d\r\n", ret);
        return ret;
    }

    printf("[bm_emmc] init OK, capacity = %u sectors (%u MB)\r\n",
           bm_emmc_get_block_count(),
           (unsigned)((u64)bm_emmc_get_block_count() / 2048U));
    return BM_EMMC_OK;
}

/*--------------------------------------------------------------------------
 * 单扇区读写（兼容旧接口/测试）
 *------------------------------------------------------------------------*/
void emmc_rd_sect0(int block_idx, void *dst)
{
    (void)bm_emmc_read_blocks((u32)block_idx, 1U, dst);
}

void emmc_wr_sect0(int block_idx, void *src)
{
    (void)bm_emmc_write_blocks((u32)block_idx, 1U, src);
}

/*--------------------------------------------------------------------------
 * 多扇区读写（天脉 FAT 块设备实际走这两个）
 *------------------------------------------------------------------------*/
void emmc_rd_sect0_2(u32 block_idx, void *dst, u32 blk_cnt)
{
    (void)bm_emmc_read_blocks(block_idx, blk_cnt, dst);
}

void emmc_wr_sect0_2(u32 block_idx, void *src, u32 blk_cnt)
{
    (void)bm_emmc_write_blocks(block_idx, blk_cnt, src);
}

/*--------------------------------------------------------------------------
 * 按字节长度读写（兼容旧接口/测试），长度按 512 折算扇区数
 *------------------------------------------------------------------------*/
void emmc_rd_sect0_3(int block_idx, void *dst, u32 ulLength)
{
    (void)bm_emmc_read_blocks((u32)block_idx, ulLength / BM_EMMC_BLOCK_SIZE, dst);
}

void emmc_wr_sect0_3(int block_idx, void *src, u32 ulLength)
{
    (void)bm_emmc_write_blocks((u32)block_idx, ulLength / BM_EMMC_BLOCK_SIZE, src);
}

/*--------------------------------------------------------------------------
 * 上板自检：写-读-比对一个扇区，并打印容量。可在天脉 shell 里手动调用。
 * 注意：会改写所传扇区号的数据；eMMC 为纯数据盘，建议用较大的空闲扇区。
 *------------------------------------------------------------------------*/
int bm_emmc_selftest(u32 test_lba)
{
    static u8 wbuf[BM_EMMC_BLOCK_SIZE];
    static u8 rbuf[BM_EMMC_BLOCK_SIZE];
    int ret;
    u32 i;

    ret = bm_emmc_init();
    if (ret != BM_EMMC_OK)
    {
        printf("[bm_emmc] selftest: init fail %d\r\n", ret);
        return ret;
    }

    for (i = 0; i < BM_EMMC_BLOCK_SIZE; i++)
        wbuf[i] = (u8)(i + test_lba);

    ret = bm_emmc_write_blocks(test_lba, 1U, wbuf);
    if (ret != BM_EMMC_OK)
    {
        printf("[bm_emmc] selftest: write fail %d\r\n", ret);
        return ret;
    }

    for (i = 0; i < BM_EMMC_BLOCK_SIZE; i++)
        rbuf[i] = 0;

    ret = bm_emmc_read_blocks(test_lba, 1U, rbuf);
    if (ret != BM_EMMC_OK)
    {
        printf("[bm_emmc] selftest: read fail %d\r\n", ret);
        return ret;
    }

    for (i = 0; i < BM_EMMC_BLOCK_SIZE; i++)
    {
        if (rbuf[i] != wbuf[i])
        {
            printf("[bm_emmc] selftest: MISMATCH at byte %u (w=0x%02X r=0x%02X)\r\n",
                   i, wbuf[i], rbuf[i]);
            return BM_EMMC_EIO;
        }
    }

    printf("[bm_emmc] selftest PASS at lba %u, capacity=%u sectors\r\n",
           test_lba, bm_emmc_get_block_count());
    return BM_EMMC_OK;
}

/*--------------------------------------------------------------------------
 * 多核互斥验证 demo
 *
 * 用法：
 *   核A 的任务里跑 lock_demo_writer_A(sector, 200)
 *   核B 的任务里跑 lock_demo_writer_B(sector, 200)
 *   两个任务都结束后跑 lock_demo_verify(sector)
 *
 * 原理：两个核对同一扇区反复写不同 pattern（0xAA vs 0x55，每位相反）。
 *       有锁 → 扇区全是一种 pattern。没锁 → 两种混在一起。
 *------------------------------------------------------------------------*/
void lock_demo_writer_A(u32 lba, int repeat)
{
	u8 buf[BM_EMMC_BLOCK_SIZE];
	memset(buf, 0xAA, sizeof(buf));
	while (repeat-- > 0)
		bm_emmc_write_blocks(lba, 1U, buf);
}

void lock_demo_writer_B(u32 lba, int repeat)
{
	u8 buf[BM_EMMC_BLOCK_SIZE];
	memset(buf, 0x55, sizeof(buf));
	while (repeat-- > 0)
		bm_emmc_write_blocks(lba, 1U, buf);
}

int lock_demo_verify(u32 lba)
{
	u8 buf[BM_EMMC_BLOCK_SIZE];
	u8 first;
	u32 i;

	if (bm_emmc_read_blocks(lba, 1U, buf) != BM_EMMC_OK)
	{
		printf("[bm_emmc] lock_demo: read fail\r\n");
		return -1;
	}

	first = buf[0];
	for (i = 1; i < sizeof(buf); i++)
	{
		if (buf[i] != first)
		{
			printf("[bm_emmc] lock_demo FAIL: byte %u = 0x%02X, "
			       "expected all 0x%02X (mixed!)\r\n", i, buf[i], first);
			return -2;
		}
	}

	if (first == 0xAA || first == 0x55)
	{
		printf("[bm_emmc] lock_demo PASS: all 0x%02X\r\n", first);
		return 0;
	}

	printf("[bm_emmc] lock_demo FAIL: unknown pattern 0x%02X\r\n", first);
	return -3;
}

#endif /* BM1684X_EMMC */
