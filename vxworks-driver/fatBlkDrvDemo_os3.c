/* jc :
 * === BM1684X 适配修改 ===
 * 从 rk3588-emmc-tianmai3 项目搬运（该项目又源自天脉3工程模板里复旦微样板的
 * fatBlkDrvDemo_os3.c），只改了容量获取函数（rk_emmc_get_block_count ->
 * bm_emmc_get_block_count），其余逻辑原样保留。
 *
 * 集成提醒（仓库里看不到天脉3工程结构，需要你按实际工程调整）：
 *   - 下面这行 #include "../cfg_build.h" 的相对路径假设本文件位于天脉3工程
 *     里 fmsh/acos3_sdmmc/ 同级深度；如果你把本文件放在别的目录，请把这行
 *     换成指向你工程里 cfg_build.h 的正确路径（该文件定义 TM_OS3_FAT32_FS）。
 *   - TM_OS3_FAT32_FS 是天脉3平台开 FAT 子系统的宏，不是本项目新增的宏。
 */
#include "../cfg_build.h"
#ifdef TM_OS3_FAT32_FS

/* includes */
#include <sysIO.h>
#include <sysDM.h>
#include <sysTypes.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <utilsFfs.h>
#include <fsdev.h>

// jc
//#include <errno.h>
#define ENONE 0

#define  IsStartLogNote    (0)/*是否开启日志记录功能，开启会影响文件系统性能,目前该功能只可在配置FAT文件系统 时使用*/

/* Forward Functional Declarations */
INT32 blk_dev_create(T_WORD major);
T_LONG blk_dev_open(T_VOID * arg, T_VOID * fileName, INT32 flags, T_VOID * mode);
INT32 blk_dev_close(T_VOID * arg);
INT32 blk_dev_read(T_VOID * arg, T_VOID * buffer, UINT32 maxbytes);
INT32 blk_dev_write(T_VOID * arg, T_VOID * buffer, UINT32 maxbytes);
INT32 blk_dev_ctrl(T_VOID * value, INT32 function, T_VOID * arg);

// jc
int blkDevRd(T_VOID * blkDev, u32 Start, u32 Amount, T_BYTE	* CurBuf);
int blkDevWrt(T_VOID * blkDev, u32 Start, u32 Amount, T_BYTE	* CurBuf);

typedef struct
{
	Dev_Hdr		dev;
	/* ！！！注意：此处定义设备私有结构体 */
}STD_BLK_DEV;

// jc
#define EMMC_FATFS_SIZE     (0x1000000)  // 8G : 0x100_0000 * 512 = 8G

/* === BM1684X 适配修改 ===
 * 容量改为从 eMMC 驱动自动识别的真实扇区数获取；
 * 读不到时回退到 EMMC_FATFS_SIZE，避免越界/容量写死。*/
extern u32 bm_emmc_get_block_count(void);
static u32 bm_emmc_fatfs_blocks(void)
{
	u32 n = bm_emmc_get_block_count();
	return n ? n : EMMC_FATFS_SIZE;
}



/* Function definition */
/**
 * @brief
 *      初始化块存储设备，包括注册块存储设备适配驱动、向操作系统添加块存储设备等。
 * @param[in]    T_WORD major：块存储设备在操作系统中注册的驱动号，暂未使用，默认为0。
 * @return
 *    0：初始化成功；
 *    -ENOMEM：初始化失败，包括内存申请失败。
 * @implements SDD_ITF_COMPS_COMM_DEVADAPT_FSDEV_BLKDEV_01
 * @scope   external
 * @standard    N/A
 */
INT32 blk_dev_create(T_WORD major)
{
	ACoreOs_status_code status;
	T_WORD drvNum;
	STD_BLK_DEV * dev;
	T_UBYTE *devName[2] = {"dev0","dev1"};

	dev = (STD_BLK_DEV *)malloc(sizeof(STD_BLK_DEV));
	if (NULL == dev)
	{
		printk("blk_dev_create failed!\n");
		return (-ENOMEM);
	}

	/* ！！！注意：此处初始化设备私有结构体 */

	/*install driver of blkDev for FAT*/
	status = ACoreOs_dm_install_drv(
			NULL,
			NULL,
			blk_dev_open,
			blk_dev_close,
			blk_dev_read,
			blk_dev_write,
			blk_dev_ctrl,
			&drvNum);
	if(status != ACOREOS_SUCCESSFUL)
	{
		printk("\nInstall blkdev driver for FAT Error!\n");
		return (-EINVAL);
	}
	else  // add by jc
	{
		printk("drvNum:%d\n",drvNum);
		extern int AcoreOs_fmsh_sdmmc_init(int sdmmcID);
		AcoreOs_fmsh_sdmmc_init(major);
	}


	/*add blkDev device for FAT*/
	status = ACoreOs_dm_add_dev((Dev_Hdr *)dev, devName[major], drvNum, DM_DEV_BLOCK_RD);
	//status = ACoreOs_dm_add_dev((Dev_Hdr *)dev, devName, drvNum, DM_DEV_BLOCK_HD);
	//status = ACoreOs_dm_add_dev((Dev_Hdr *)dev, devName, drvNum, DM_DEV_FATFS);
	if (status != ACOREOS_SUCCESSFUL)
	{
		printk("Add blkdev device for FAT Error!\n");
		return (-EINVAL);
	}
#if IsStartLogNote
	//LogNoteInit(NVramADDR,NVramSIZE);
	/*第一个参数:配置记录 数据的基地址，可以是NVRAM地址，也可以是内存地址；第二个参数：配置的地址空间大小，内部会进行记录数据检查，以防止非分配空间被冲*/
#endif

	// jc
	printk("blk_dev_create ok!\n");

	return (ENONE);
}

/**
 * @brief
 *      打开块存储设备，后续能够根据描述符访问设备。
 * @param[in]  T_VOID *arg：【块存储设备实例】；
 * @param[in]  UINT8 *filename：块存储设备的名称，暂未使用；
 * @param[in]  INT32 flags：块存储设备的打开方式，暂未使用，默认为0。
 * @param[out] T_VOID *mode：【物理设备IO结构体】，如果不为NULL，代表打开时需要获取设备参数。
 * @return
 *    0：初始化成功；
 *    -ENOMEM：初始化失败，包括内存申请失败。
 * @implements SDD_ITF_COMPS_COMM_DEVADAPT_FSDEV_BLKDEV_02
 * @scope   external
 * @standard    N/A
 */
T_LONG blk_dev_open(T_VOID * arg, T_VOID * fileName, INT32 flags, T_VOID * mode)
{
	T_PHY_DEV_INFO io_para;

	if (NULL == arg)
	{
		printk("blk_dev_open: input para error, arg = NULL\n");
		return (-EINVAL);
	}

	/* needed */
	io_para = (T_PHY_DEV_INFO)mode;
	if (io_para != NULL)
	{
		io_para->start_block = 0;/* ！！！注意：此处设置设备的起始扇区，之前的扇区均保留，默认置为0 */

		io_para->block_amount = bm_emmc_fatfs_blocks(); /* BM1684X 适配：用真实容量 */

		io_para->block_size = 512;/* ！！！注意：此处从设备私有结构体 获取扇区大小字节数 */
	}

	return (T_LONG)(arg);
}
/**
 * @brief
 *      关闭块存储设备。
 * @param[in]  T_VOID *arg：【块存储设备实例】。
 * @return
 *   0：关闭成功；
 *   -EINVAL：关闭失败，包括参数arg为NULL。
 * @implements SDD_ITF_COMPS_COMM_DEVADAPT_FSDEV_BLKDEV_03
 * @scope   external
 * @standard    N/A
 */
INT32 blk_dev_close(T_VOID * arg)
{
	if (NULL == arg)
	{
		printk("blk_dev_close: input para error, arg = NULL\n");
		return (-EINVAL);
	}

	return (ENONE);
}
/**
 * @brief
 *      对块存储设备执行控制命令。
 * @param[in]  T_VOID *value：【块存储设备实例】；
 * @param[in]  INT32 function：【设备适配控制命令;
 * @param[in]  T_VOID *arg：【设备适配控制命令】的参数。
 * @implements SDD_ITF_COMPS_COMM_DEVADAPT_FSDEV_BLKDEV_06
 * @scope   external
 * @standard    N/A
 */
INT32 blk_dev_ctrl(T_VOID * value, INT32 function, T_VOID * arg)
{
	T_PHY_DEV_INFO io_para;

	if (NULL == value)
	{
		printk("blk_dev_ctrl: input para error, value  == NULL\n");
		return (-EINVAL);
	}

	switch (function)
	{
		case DEV_GETDEVINFO:	/* Get device's information	*/
		{
			io_para = (T_PHY_DEV_INFO)arg;
			if (arg != NULL)
			{
				io_para->start_block = 0;
				io_para->block_amount = bm_emmc_fatfs_blocks();/* BM1684X 适配：用真实容量 */

				io_para->block_size = 512;/* ！！！注意：此处从设备私有结构体 获取扇区大小字节数 */
				io_para->secSizeShift = (utilsFfsMsb(512/* ！！！注意：此处从设备私有结构体 获取扇区大小字节数 */) - 1);
				break;
			}
			else
			{
				return (-EINVAL);
			}
		}

		/*是否要做丢弃操作，0不丢弃，1丢弃*/
		case FIODISCARDGET:
		{
			if (arg != NULL)
			{
				*((int *)arg) = 0;
				break;
			}
			else
			{
				return (-EINVAL);
			}
		}

		case FIODISCARD:
		{
		     break;
		}

		case DEV_SYNC:
		{
		     /*增加同步代码*/
		     break;
		}

		case DEV_TEST:
		{
		     break;
		}

		case DEV_GETPROPINFO:
		{
			if (arg != NULL)
			{
				((PHY_PROP_INFO *)arg)->properties = 0;
				((PHY_PROP_INFO *)arg)->reserved[0] = 0;
				((PHY_PROP_INFO *)arg)->reserved[1] = 0;
				((PHY_PROP_INFO *)arg)->reserved[2] = 0;
				((PHY_PROP_INFO *)arg)->reserved[3] = 0;
				break;
			}
			else
			{
				return (-EINVAL);
			}
		}

		case DEV_GETGEOMETRY:
		{
			if (arg != NULL)
			{
				((DEV_GEOMETRY *)arg)->blocksize = 0;
				((DEV_GEOMETRY *)arg)->cylinders = 0;
				((DEV_GEOMETRY *)arg)->heads = 0;
				((DEV_GEOMETRY *)arg)->secs_per_track = 0;
				((DEV_GEOMETRY *)arg)->total_blocks = 0;
				break;
			}
			else
			{
				return (-EINVAL);
			}
		}

		default:
		{
			return (-EOPNOTSUPP);
		}
	}

	return (ENONE);
}

/**
 * @brief
 *      初始化RAMDisk存储设备，包括申请内存资源、向操作系统添加RAMDisk存储设备等。
 * @param[in]  INT32 drvId：RAMDisk存储设备在操作系统中注册的驱动号。
 * @return
 *   0：初始化成功；
 *   -ENOMEM：初始化失败，包括内存申请失败。
 * @implements SDD_ITF_COMPS_COMM_DEVADAPT_FSDEV_BLKDEV_04
 * @scope   external
 * @standard    N/A
 */
INT32 blk_dev_read(T_VOID * arg, T_VOID * buffer, UINT32 maxbytes)
{
	UINT32 Start;
	UINT32 Amount;
	char* CurBuf;		/*Current buffer pointer*/
	T_PHY_DEV_INFO io_para;
	int ret = 0;

	// jc
	T_VOID * blkDev = arg;

	if ((arg == NULL) || (buffer == NULL))
	{
		printk("blk_dev_read: input para error, arg or buffer == NULL\n");
		return (-EINVAL);
	}

	io_para = (T_PHY_DEV_INFO)buffer;

	/* check buffer */
	if (!io_para->iopara_data)
	{
		printk("blk_dev_read: input para error, io_para->iopara_data error.\n");
		return (-EINVAL);
	}

	Start  = io_para->iopara_data->start_sector;
	Amount = io_para->iopara_data->sector_amount;
	CurBuf = io_para->iopara_data->data;
#if 0
	printk("[blk_dev_read]:start_sector:%ld\n",Start);
	printk("[blk_dev_read]:sector_amount:%ld\n",Amount);
	printk("[blk_dev_read]:block_size:%ld\n",io_para->iopara_data->block_size);
#endif
	if ((CurBuf == NULL) || ((Start + Amount) > bm_emmc_fatfs_blocks()/* BM1684X 适配：用真实容量 */))
	{
		return (-EINVAL);
	}

	/* ！！！注意：此处根据设备私有结构体、待读取起始扇区号、扇区数目以及缓冲调用驱动读取数据 */
	ret = blkDevRd(blkDev, Start, Amount, CurBuf);
	if (OK == ret)
	{
		return ((INT32)(Amount * 512/* ！！！注意：此处从设备私有结构体 获取扇区大小字节数 */));
	}
	else
	{
		printk("\nblk_dev_read error.\n");
		return (-EIO);
	}
}

/**
 * @brief
 *      向块存储设备以扇区为单位写入数据。
 * @param[in]  T_VOID *arg：【块存储设备实例】；
 * @param[in] UINT8 *buffer：【物理设备IO结构体】，存储待写入的起始扇区号、扇区数目以及数据；
 * @param[in] UINT32 maxbytes：待写入的字节数目，暂不使用，默认为0。
 * @return
 *   成功写入的字节数目：写入成功；
 *   -EINVAL：写入失败，包括参数arg为NULL或者参数buffer为NULL。
 * @implements SDD_ITF_COMPS_COMM_DEVADAPT_FSDEV_BLKDEV_05
 * @scope   external
 * @standard    N/A
 */
INT32 blk_dev_write(T_VOID * arg, T_VOID * buffer, UINT32 maxbytes)
{
	UINT32 Start;
	UINT32 Amount;
	char* CurBuf;		/*Current buffer pointer*/
	T_PHY_DEV_INFO io_para;
	int ret = 0;

	// jc
	T_VOID * blkDev = arg;

	if ((arg == NULL) || (buffer == NULL))
	{
		printk("blk_dev_write: input para error, arg or buffer == NULL. \n");
		printk("blk_dev_write: arg-0x%X, buffer-0x%X! \n", arg, buffer);
		return (-EINVAL);
	}

	io_para = (T_PHY_DEV_INFO)buffer;

	/* check buffer */
	if (!io_para->iopara_data)
	{
		printk("blk_dev_write: input para error, io_para->iopara_data error.\n");
		return (-EINVAL);
	}

	Start  = io_para->iopara_data->start_sector;
	Amount = io_para->iopara_data->sector_amount;
	CurBuf = io_para->iopara_data->data;

#if 0
	printk("[blk_dev_write]:start_sector:%ld\n",Start);
	printk("[blk_dev_write]:sector_amount:%ld\n",Amount);
	printk("[blk_dev_write]:block_size:%ld\n",io_para->iopara_data->block_size);
#endif

	if ((CurBuf == NULL) || ((Start + Amount) > bm_emmc_fatfs_blocks()/* BM1684X 适配：用真实容量 */))
	{
		return (-EINVAL);
	}

	/* ！！！注意：此处根据设备私有结构体、待写入起始扇区号、扇区数目以及缓冲调用驱动写入数据 */
	ret = blkDevWrt(blkDev, Start, Amount, CurBuf);
	if (OK == ret)
	{
		return ((INT32)(Amount * 512/* ！！！注意：此处从设备私有结构体 获取扇区大小字节数 */));
	}
	else
	{
		printk("\nblk_dev_write error.\n");
		return (-EIO);
	}
}

#if 1
extern void emmc_rd_sect0_2(u32 block_idx, void *dst, u32 blk_cnt);
extern void emmc_wr_sect0_2(u32 block_idx, void *src, u32 blk_cnt);

int blkDevRd(T_VOID * blkDev, u32 Start, u32 Amount, T_BYTE	* CurBuf)
{
	emmc_rd_sect0_2(Start, CurBuf, Amount);
	return OK;
}

int blkDevWrt(T_VOID * blkDev, u32 Start, u32 Amount, T_BYTE	* CurBuf)
{
	emmc_wr_sect0_2(Start, CurBuf, Amount);
	return OK;
}
#endif

#endif  /* #ifdef TM_OS3_FAT32_FS */

