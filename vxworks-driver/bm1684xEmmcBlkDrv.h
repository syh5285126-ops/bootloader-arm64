/*
 * bm1684xEmmcBlkDrv.h — BM1684X eMMC 块设备驱动上下文与对外接口
 *
 * 本文件对应 nvme demo 中的 nvme.h：
 *   - 定义块设备驱动上下文 BM_EMMC_DRIVE（对应 demo 的 AHCI_DRIVE），
 *     向上层文件系统/设备管理器暴露容量（numOfSectors）与扇区字节数（bytes）。
 *   - 声明驱动入口 bm1684xEmmcDriverInit() 与块读写接口
 *     bm1684xEmmcBlkRd/Wr（对应 demo 的 nvmeDriverInit / nvmeBlkRd / nvmeBlkWr）。
 *
 * 底层寄存器访问复用自包含驱动 bm1684xSdhci.c，通过 bm1684xSdhciOsal.h
 * 暴露的命令收发接口对接 eMMC 协议。
 *
 * 版权所有 (c) 2024 Bitmain / Sophgo.  SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __BM1684X_EMMC_BLK_DRV_H__
#define __BM1684X_EMMC_BLK_DRV_H__

#include "bm1684xSdhciOsal.h"   /* BM1684X_SDHCI_DEV / 命令数据描述符 */

/*
 * ffsMsb — 返回最高有效位位置（1-indexed，bit1=LSB），兼容 VxWorks ffsLib。
 * ACoreOs 编译环境中不含 ffsLib，此处用 __builtin_clz 提供等价实现。
 */
#ifndef FFSMB_COMPAT_DEFINED
#define FFSMB_COMPAT_DEFINED
static inline int ffsMsb(unsigned int x) {
    if (x == 0u) return 0;
    return 32 - __builtin_clz(x);
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * 设备状态（对应 demo nvme.h 中的 AHCI_DEV_* 系列）
 * ------------------------------------------------------------------------- */
#define BM_EMMC_DEV_INIT      255   /* 未初始化            */
#define BM_EMMC_DEV_OK          0   /* 设备就绪            */
#define BM_EMMC_DEV_NONE        1   /* 设备无响应          */
#define BM_EMMC_DEV_ID_FAIL     2   /* 卡识别失败          */

/* -------------------------------------------------------------------------
 * eMMC 固定参数
 * ------------------------------------------------------------------------- */
#define BM_EMMC_BLK_SIZE      512U  /* eMMC 扇区固定 512 字节        */
#define BM_EMMC_EXTCSD_SIZE   512U  /* EXT_CSD 寄存器长度（字节）    */

/*
 * 弹跳缓冲区可容纳的扇区数。块读写按此粒度分段，既绕开上层缓冲区
 * 的 cache 一致性问题（统一走 DMA 相干内存），又能支撑任意大小请求。
 */
#define BM_EMMC_BOUNCE_BLOCKS 128U  /* 128 × 512 = 64 KiB           */

/* -------------------------------------------------------------------------
 * eMMC 命令索引（JEDEC JESD84）
 * ------------------------------------------------------------------------- */
#define MMC_CMD_GO_IDLE_STATE        0   /* 复位至 idle             */
#define MMC_CMD_SEND_OP_COND         1   /* 查询/设置 OCR（R3）     */
#define MMC_CMD_ALL_SEND_CID         2   /* 读取 CID（R2）          */
#define MMC_CMD_SET_RELATIVE_ADDR    3   /* 主机分配 RCA（R1）      */
#define MMC_CMD_SWITCH               6   /* 修改 EXT_CSD（R1b）     */
#define MMC_CMD_SELECT_CARD          7   /* 选中卡（R1b）           */
#define MMC_CMD_SEND_EXT_CSD         8   /* 读 EXT_CSD（带数据,R1） */
#define MMC_CMD_SEND_CSD             9   /* 读取 CSD（R2）          */
#define MMC_CMD_SET_BLOCKLEN        16   /* 设置块长度（R1）        */
#define MMC_CMD_READ_SINGLE_BLOCK   17   /* 单块读（R1）            */
#define MMC_CMD_READ_MULTIPLE_BLOCK 18   /* 多块读（R1）            */
#define MMC_CMD_WRITE_SINGLE_BLOCK  24   /* 单块写（R1）            */
#define MMC_CMD_WRITE_MULTIPLE_BLOCK 25  /* 多块写（R1）            */

/* CMD1 OCR 参数：扇区寻址（bit30）+ 全电压窗口（bit23:7） */
#define MMC_OCR_SECTOR_MODE    0x40000000U
#define MMC_OCR_VOLTAGE_WIN    0x00FF8000U
#define MMC_OCR_BUSY           0x80000000U  /* 上电完成标志（R3 bit31） */

/*
 * eMMC 数据总线宽度（位），按板级实际连线配置，可选 1 / 4 / 8。
 * 例如 EVB 设备树为 bus-width=<4>，生产板多为 8。
 */
#ifndef BM_EMMC_BUS_WIDTH
#define BM_EMMC_BUS_WIDTH      8
#endif

/* EXT_CSD[183] BUS_WIDTH 字段取值 */
#define EXT_CSD_BUS_WIDTH_1BIT 0
#define EXT_CSD_BUS_WIDTH_4BIT 1
#define EXT_CSD_BUS_WIDTH_8BIT 2

/*
 * CMD6 SWITCH 参数生成宏：WRITE_BYTE(0x03) 写 EXT_CSD index=183(BUS_WIDTH)，
 * 值取上面的 EXT_CSD_BUS_WIDTH_* 之一。
 */
#define MMC_SWITCH_BUS_WIDTH(val) \
    (0x03B70000U | (((unsigned int)(val)) << 8))

/* EXT_CSD 字段偏移 */
#define EXT_CSD_SEC_COUNT      212U  /* 扇区总数（4 字节小端）        */

/* -------------------------------------------------------------------------
 * eMMC 块设备驱动上下文（对应 demo 的 AHCI_DRIVE）
 *
 * 设备管理器回调（bm1684xEmmcDevReg.c）只读取 numOfSectors 与 bytes
 * 两个字段向文件系统上报几何信息，其余为驱动内部状态。
 * ------------------------------------------------------------------------- */
typedef struct bm1684xEmmcDrive
{
    BM1684X_SDHCI_DEV  *pSdhci;        /* 底层 SDHCI 控制器句柄          */
    unsigned int        rca;           /* 卡相对地址（主机分配）        */
    unsigned int        highCap;       /* 1=扇区寻址（>2GB）            */
    unsigned int        numOfSectors;  /* 容量（扇区数）— 上报文件系统  */
    unsigned int        bytes;         /* 扇区字节数（512）— 上报       */
    unsigned char       state;         /* BM_EMMC_DEV_*                 */

    void               *muteSem;       /* 读写互斥信号量                */
    void               *bounceBuf;     /* DMA 相干弹跳缓冲区            */
} BM_EMMC_DRIVE;

/* 全局唯一驱动实例指针（对应 demo 的 g_pDriver） */
extern BM_EMMC_DRIVE *g_pEmmcDrive;

/* -------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

/*
 * bm1684xEmmcDriverInit — eMMC 驱动入口（对应 demo 的 nvmeDriverInit）。
 *   初始化 SDHCI 控制器、执行 eMMC 卡识别、填充 g_pEmmcDrive 容量信息。
 *   返回 0 成功，非 0 失败。
 */
int bm1684xEmmcDriverInit(void);

/*
 * bm1684xEmmcBlkRd / bm1684xEmmcBlkWr — 块读写（对应 demo 的 nvmeBlkRd/Wr）。
 *   pDrive   : 驱动上下文
 *   startBlk : 起始扇区号
 *   nBlks    : 扇区数量
 *   pBuf     : 用户数据缓冲区
 *   返回 0 成功，非 0 失败。
 */
int bm1684xEmmcBlkRd(BM_EMMC_DRIVE *pDrive, unsigned int startBlk,
                     unsigned int nBlks, char *pBuf);
int bm1684xEmmcBlkWr(BM_EMMC_DRIVE *pDrive, unsigned int startBlk,
                     unsigned int nBlks, char *pBuf);

/*
 * bm1684xEmmcBlkDevCreate — 注册块设备到设备管理器（对应 demo blk_dev_create）。
 *   实现在 bm1684xEmmcDevReg.c。
 */
int bm1684xEmmcBlkDevCreate(int major);

#ifdef __cplusplus
}
#endif

#endif /* __BM1684X_EMMC_BLK_DRV_H__ */
