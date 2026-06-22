/******************************************************************************
 * bm_emmc_types.h
 *
 * BM1684X eMMC 驱动用到的基础类型与返回码定义。
 * 单独成文，避免直接耦合天脉3工程模板头文件，便于移植与裁剪。
 * 类比 rk3588 项目的 rk_emmc_types.h。
 ******************************************************************************/
#ifndef _BM_EMMC_TYPES_H_
#define _BM_EMMC_TYPES_H_

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

/* 驱动返回码 */
#define BM_EMMC_OK            (0)        /* 成功 */
#define BM_EMMC_EIO           (-1)       /* 读写/命令出错 */
#define BM_EMMC_ETIMEOUT      (-2)       /* 超时 */
#define BM_EMMC_ENOTRDY       (-3)       /* 卡未初始化 */
#define BM_EMMC_EPARAM        (-4)       /* 参数错误 */

#ifndef TRUE
#define TRUE   1
#endif
#ifndef FALSE
#define FALSE  0
#endif

#endif /* _BM_EMMC_TYPES_H_ */
