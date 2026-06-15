/*
 * bm1684xEmmcDevReg.c — BM1684X eMMC 块设备注册（设备管理器层）
 *
 * 对应 nvme demo 中的 fatBlkDrvDemo.c：通过 ACoreOs 设备管理器
 * （ACoreOs_dm_install_drv + ACoreOs_dm_add_dev）把 eMMC 注册为
 * 可供 FAT 文件系统挂载的块设备，并把读写回调接到 bm1684xEmmcBlkRd/Wr。
 *
 * 注册流程（与 demo 一致）：
 *   1. 上层调用 bm1684xEmmcDriverInit() 完成硬件与卡识别，得到 g_pEmmcDrive；
 *   2. 调用 bm1684xEmmcBlkDevCreate(major) 安装驱动并添加设备节点；
 *   3. 文件系统经 open/ctrl 读取几何信息，经 read/write 收发数据。
 *
 * 版权所有 (c) 2024 Bitmain / Sophgo.  SPDX-License-Identifier: BSD-3-Clause
 */

#include <acoreos.h>
#include <sysIO.h>
#include <sysDM.h>
#include <sysTypes.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <utilsFfs.h>
#include <fsdev.h>

#include "bm1684xEmmcBlkDrv.h"

/* 前向声明（设备管理器回调，签名与 demo 保持一致） */
LOCAL INT32 emmc_dev_open(void *arg, UINT8 *fileName, INT32 flags, INT32 mode);
LOCAL INT32 emmc_dev_close(void *arg);
LOCAL INT32 emmc_dev_read(void *arg, UINT8 *buffer, UINT32 maxbytes);
LOCAL INT32 emmc_dev_write(void *arg, UINT8 *buffer, UINT32 maxbytes);
LOCAL INT32 emmc_dev_ctrl(void *value, INT32 function, INT32 arg);

/*
 * 块设备私有结构：首成员必须是 Dev_Hdr（设备管理器要求），其后挂
 * 本驱动的上下文指针。对应 demo 的 STD_BLK_DEV。
 */
typedef struct
{
    Dev_Hdr        dev;
    BM_EMMC_DRIVE *pDrive;   /* eMMC 驱动上下文 */
} EMMC_BLK_DEV;

/* -------------------------------------------------------------------------
 * 注册入口（对应 demo 的 blk_dev_create）
 * ------------------------------------------------------------------------- */
int bm1684xEmmcBlkDevCreate(int major)
{
    ACoreOs_status_code status;
    T_UWORD             drvNum;
    EMMC_BLK_DEV       *dev;
    T_BYTE              devName[255] = "emmc0";

    (void)major;

    if (g_pEmmcDrive == 0 || g_pEmmcDrive->state != BM_EMMC_DEV_OK) {
        printk("bm1684xEmmcBlkDevCreate: driver not ready, call "
               "bm1684xEmmcDriverInit() first!\n");
        return ERROR;
    }

    dev = (EMMC_BLK_DEV *)malloc(sizeof(EMMC_BLK_DEV));
    if (dev == 0) {
        printk("bm1684xEmmcBlkDevCreate: malloc Fail!\n");
        return ERROR;
    }

    /* 绑定驱动上下文 */
    dev->pDrive = g_pEmmcDrive;

    /* 安装块设备驱动（无 create/delete 钩子，与 demo 一致） */
    status = ACoreOs_dm_install_drv(
                 NULL,
                 NULL,
                 emmc_dev_open,
                 emmc_dev_close,
                 emmc_dev_read,
                 emmc_dev_write,
                 emmc_dev_ctrl,
                 &drvNum);
    if (status != ACOREOS_SUCCESSFUL) {
        printk("Install eMMC blkdev driver Error!\n");
        return ERROR;
    }

    /* 添加块设备节点供 FAT 使用 */
    status = ACoreOs_dm_add_dev((Dev_Hdr *)dev, (T_UBYTE *)devName,
                                drvNum, DM_DEV_BLOCK_RD);
    if (status != ACOREOS_SUCCESSFUL) {
        printk("Add eMMC blkdev device Error!\n");
        return ERROR;
    }

    printk("eMMC block device \"%s\" registered.\n", devName);
    return (ENONE);
}

/* -------------------------------------------------------------------------
 * open：上报设备几何信息（对应 demo 的 blk_dev_open）
 *   mode 实参为 T_PHY_DEV_INFO 指针，需填写起始块/块数/块大小。
 *   返回值作为后续 read/write 的第一个参数（即 arg）。
 * ------------------------------------------------------------------------- */
LOCAL INT32 emmc_dev_open(void *arg, UINT8 *fileName, INT32 flags, INT32 mode)
{
    T_PHY_DEV_INFO io_para;
    BM_EMMC_DRIVE *pDrive;

    if (arg == NULL) {
        printk("emmc_dev_open: arg = NULL\n");
        return -1;
    }

    pDrive  = ((EMMC_BLK_DEV *)arg)->pDrive;
    io_para = (T_PHY_DEV_INFO)mode;

    if (io_para != NULL) {
        io_para->start_block  = 0;                     /* 从设备 0 扇区起 */
        io_para->block_amount = pDrive->numOfSectors;  /* 总扇区数        */
        io_para->block_size   = pDrive->bytes;         /* 扇区字节数 512  */
    }

    return (INT32)(arg);
}

LOCAL INT32 emmc_dev_close(void *arg)
{
    return (ENONE);
}

/* -------------------------------------------------------------------------
 * ctrl：响应设备管理器/文件系统的控制请求（对应 demo 的 blk_dev_ctrl）
 * ------------------------------------------------------------------------- */
LOCAL INT32 emmc_dev_ctrl(void *value, INT32 function, INT32 arg)
{
    T_PHY_DEV_INFO io_para;
    BM_EMMC_DRIVE *pDrive;

    if (value == NULL) {
        printk("emmc_dev_ctrl: value == NULL\n");
        return -1;
    }

    pDrive = ((EMMC_BLK_DEV *)value)->pDrive;

    switch (function)
    {
        case DEV_GETDEVINFO:    /* 获取设备信息 */
        {
            io_para = (T_PHY_DEV_INFO)arg;
            if (arg != NULL) {
                io_para->start_block  = 0;
                io_para->block_amount = pDrive->numOfSectors;
                io_para->block_size   = pDrive->bytes;
                io_para->secSizeShift = (ffsMsb(pDrive->bytes) - 1);
                break;
            } else {
                return (-EINVAL);
            }
        }

        case FIODISCARDGET:     /* 是否支持 discard：eMMC 暂不上报 */
        {
            if (arg != NULL) {
                *((int *)arg) = 0;
                break;
            } else {
                return (-EINVAL);
            }
        }

        case FIODISCARD:
        {
            break;
        }

        case DEV_SYNC:          /* 同步：弹跳缓冲区已即时落盘，无需额外操作 */
        {
            break;
        }

        case DEV_TEST:
        {
            break;
        }

        case DEV_GETPROPINFO:
        {
            if (arg != NULL) {
                ((PHY_PROP_INFO *)arg)->properties = 0;
                ((PHY_PROP_INFO *)arg)->reserved[0] = 0;
                ((PHY_PROP_INFO *)arg)->reserved[1] = 0;
                ((PHY_PROP_INFO *)arg)->reserved[2] = 0;
                ((PHY_PROP_INFO *)arg)->reserved[3] = 0;
                break;
            } else {
                return (-EINVAL);
            }
        }

        case DEV_GETGEOMETRY:
        {
            if (arg != NULL) {
                ((DEV_GEOMETRY *)arg)->blocksize      = 0;
                ((DEV_GEOMETRY *)arg)->cylinders      = 0;
                ((DEV_GEOMETRY *)arg)->heads          = 0;
                ((DEV_GEOMETRY *)arg)->secs_per_track = 0;
                ((DEV_GEOMETRY *)arg)->total_blocks   = 0;
                break;
            } else {
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

/* -------------------------------------------------------------------------
 * read：从 io_para 取出起始扇区/扇区数/目标缓冲，调用块读
 *        （对应 demo 的 blk_dev_read）
 * ------------------------------------------------------------------------- */
LOCAL INT32 emmc_dev_read(void *arg, UINT8 *buffer, UINT32 maxbytes)
{
    UINT32         Start;
    UINT32         Amount;
    char          *CurBuf;
    T_PHY_DEV_INFO io_para;
    int            ret;
    BM_EMMC_DRIVE *pDrive;

    if ((arg == NULL) || (buffer == NULL)) {
        printk("emmc_dev_read: arg or buffer == NULL\n");
        return -1;
    }

    pDrive  = ((EMMC_BLK_DEV *)arg)->pDrive;
    io_para = (T_PHY_DEV_INFO)buffer;

    if (!io_para->iopara_data) {
        printk("emmc_dev_read: io_para->iopara_data error.\n");
        return (-ENXIO);
    }

    Start  = io_para->iopara_data->start_sector;
    Amount = io_para->iopara_data->sector_amount;
    CurBuf = io_para->iopara_data->data;

    if ((CurBuf == NULL) || ((Start + Amount) > pDrive->numOfSectors))
        return (-ENXIO);

    ret = bm1684xEmmcBlkRd(pDrive, Start, Amount, CurBuf);
    if (OK == ret) {
        return (ENONE);
    } else {
        printk("emmc_dev_read error.\n");
        return (-ENXIO);
    }
}

/* -------------------------------------------------------------------------
 * write：从 io_para 取出参数，调用块写（对应 demo 的 blk_dev_write）
 * ------------------------------------------------------------------------- */
LOCAL INT32 emmc_dev_write(void *arg, UINT8 *buffer, UINT32 maxbytes)
{
    UINT32         Start;
    UINT32         Amount;
    char          *CurBuf;
    T_PHY_DEV_INFO io_para;
    int            ret;
    BM_EMMC_DRIVE *pDrive;

    if ((arg == NULL) || (buffer == NULL)) {
        printk("emmc_dev_write: arg or buffer == NULL.\n");
        return -1;
    }

    pDrive  = ((EMMC_BLK_DEV *)arg)->pDrive;
    io_para = (T_PHY_DEV_INFO)buffer;

    if (!io_para->iopara_data) {
        printk("emmc_dev_write: io_para->iopara_data error.\n");
        return (-ENXIO);
    }

    Start  = io_para->iopara_data->start_sector;
    Amount = io_para->iopara_data->sector_amount;
    CurBuf = io_para->iopara_data->data;

    if ((CurBuf == NULL) || ((Start + Amount) > pDrive->numOfSectors))
        return (-ENXIO);

    ret = bm1684xEmmcBlkWr(pDrive, Start, Amount, CurBuf);
    if (OK == ret) {
        return (ENONE);
    } else {
        printk("emmc_dev_write error.\n");
        return (-ENXIO);
    }
}
