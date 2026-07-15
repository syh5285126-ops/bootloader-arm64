# BM1684X eMMC 多核访问审查单

## 1. 背景

当前 BM1684X eMMC 驱动运行在 RTOS/AcoreOS3 场景，存在多个核、多个任务或文件系统路径同时访问 eMMC 块设备的需求。

eMMC 控制器是单个共享外设。即使上层文件系统能保证文件级一致性，也不能默认保证底层 `bm_emmc_read_blocks()`、`bm_emmc_write_blocks()`、`bm_emmc_init()` 不会被多个执行上下文并发进入。因此驱动层需要明确控制器访问串行化策略。

## 2. 当前调用链

```text
fatBlkDrvDemo_os3.c
  -> emmc_rd_sect0_2 / emmc_wr_sect0_2
    -> bm_emmc_read_blocks / bm_emmc_write_blocks
      -> mmcSendCmdDbg
        -> bm1684xSdhciSendCmd
          -> SDHCI/eMMC 控制器寄存器 + DMA
```

涉及文件：

- `vxworks-driver/bm_emmc_core.c`
- `vxworks-driver/bm_emmc_glue.c`
- `vxworks-driver/fatBlkDrvDemo_os3.c`
- `vxworks-driver/bm1684xSdhci.c`
- `vxworks-driver/bm1684xSdhciOsal.h`
- `vxworks-driver/bm_emmc_osal_os3.c`

## 3. 影响域

### 3.1 必须保护的共享资源

- SDHCI/eMMC 控制器寄存器，基址 `BM1684X_EMMC_PHYS_BASE`
- 控制器命令状态机：CMD、RESP、DATA、XFER、INT_STATUS
- DMA 配置和 DMA buffer 地址寄存器
- eMMC 卡状态机：CMD18/CMD25 多块传输、CMD12 stop、CMD13 busy polling
- 驱动全局状态：
  - `g_dev`
  - `g_inited`
  - `g_rca`
  - `g_sectorMode`
  - `g_blockCount`
  - `g_extCsd`

### 3.2 必须串行化的流程

- `bm_emmc_init()`
- `bm_emmc_reinit()`
- `bm_emmc_read_blocks()`
- `bm_emmc_write_blocks()`
- 读写路径里的 cache flush/invalidate + DMA 启动 + DMA 完成确认
- 多块传输后的 CMD12
- 写完成后的 `mmcWaitReady()`

### 3.3 不建议由 eMMC 驱动解决的范围

- 同一个用户 buffer 被多个任务同时读写
- 文件系统元数据一致性
- 分区/卷级别互斥
- 上层业务读写顺序语义

这些应由调用方、块设备层或文件系统层处理。

## 4. 风险分析

如果不加驱动级互斥，多核并发可能导致：

- 两个任务同时写命令寄存器，命令序列交叉
- 一个任务还未 CMD12，另一个任务已发新命令，DAT/CMD inhibit 卡死
- DMA buffer 地址被后一个请求覆盖，导致读写错 buffer
- cache flush/invalidate 与另一个 DMA 请求交叉，读到旧数据或写入旧数据
- `g_inited` 并发初始化，重复 reset/clock 配置
- `g_extCsd` 作为静态 DMA 缓冲被并发复用
- 错误恢复、timeout、reset 与正常 I/O 交叉，扩大故障

## 5. 推荐方案

### 5.1 第一阶段：一个 eMMC 控制器一把 I/O mutex

推荐先实现控制器级全局互斥：

```text
bm_emmc_read_blocks()
  lock
    lazy init if needed
    cache invalidate before read
    send read command
    CMD12 if multi-block
    cache invalidate after read
  unlock

bm_emmc_write_blocks()
  lock
    lazy init if needed
    cache flush before write
    send write command
    CMD12 if multi-block
    mmcWaitReady
  unlock
```

锁粒度：完整 transaction，而不是只锁寄存器读写。

原因：

- eMMC/SDHCI 同一时间只能有一个有效命令/数据事务
- cache 维护必须和对应 DMA 请求绑定
- CMD12、CMD13 busy polling 属于同一次 I/O 的收尾，不应被其他请求插入

### 5.2 锁类型选择

建议使用 RTOS mutex 或 binary semaphore：

- 允许阻塞等待
- 支持任务调度
- 可设置超时
- 不长时间占用 CPU

不建议用纯 spinlock 包住完整 I/O：

- eMMC 读写可能等待 DMA、busy、timeout
- 轮询模式下等待时间可能达到毫秒级甚至秒级
- spinlock 会浪费核资源，并可能影响实时性

### 5.3 OSAL 对接方式

建议在 OSAL 层增加控制器互斥接口：

```c
void *(*mutex_create)(void);
int   (*mutex_lock)(void *mutex);
void  (*mutex_unlock)(void *mutex);
```

放在 OSAL 的原因：

- `bm_emmc_core.c` 不直接依赖天脉3 RTOS 头文件
- 后续移植到其他 RTOS 只改 OSAL
- 与现有 `sem_create/sem_wait/sem_signal` 风格一致

注意：现有 `sem_*` 是命令完成通知，不是互斥锁，不能复用为 I/O 互斥。

### 5.4 初始化策略

`bm_emmc_init()` 和懒初始化必须纳入同一把锁：

```text
lock
  if !g_inited:
    bmTopDomainInit
    bm1684xSdhciInit
    card identify
    read EXT_CSD
    g_inited = 1
unlock
```

`bm_emmc_reinit()` 必须独占执行，并阻止其他读写同时进入。

### 5.5 返回策略

如果 RTOS mutex API 不支持超时，OSAL 的 `mutex_lock` 不传超时参数，按 RTOS 原生阻塞语义执行。

返回建议：

- `BM_EMMC_EIO`：RTOS mutex lock/unlock 对接失败或命令/DMA/控制器错误
- `BM_EMMC_ETIMEOUT`：仅用于内部原子兜底锁等待超时或 eMMC 命令超时
- `BM_EMMC_EPARAM`：参数错误

正式产品应接 RTOS mutex；内部原子兜底只用于 BSP mutex 未接入阶段，仍保留有限等待，避免兜底路径永久挂死。

## 6. 建议修改点

### 6.1 `bm1684xSdhciOsal.h`

增加 mutex 回调定义。

### 6.2 `bm_emmc_osal_os3.c`

接入天脉3 RTOS mutex API。

如果当前 API 名称未确认，建议先封装成本地函数：

```c
static void *bmOs3EmmcMutexCreate(void);
static int bmOs3EmmcMutexLock(void *mutex);
static void bmOs3EmmcMutexUnlock(void *mutex);
```

后续只在此文件内替换真实 BSP 调用。

### 6.3 `bm_emmc_core.c`

新增内部函数：

```c
static int bmEmmcIoLock(void);
static void bmEmmcIoUnlock(void);
static int bm_emmc_init_locked(void);
```

公共函数持锁：

- `bm_emmc_init`
- `bm_emmc_reinit`
- `bm_emmc_read_blocks`
- `bm_emmc_write_blocks`
- 可选：`bm_emmc_get_block_count`

`bm_emmc_read_blocks()` / `bm_emmc_write_blocks()` 中不能再调用会重复拿锁的 `bm_emmc_init()`，应调用内部 `bm_emmc_init_locked()`。

## 7. 代码审查重点

- 所有 `return` 路径是否释放 mutex
- `read/write` 参数校验是否在拿锁前完成
- 懒初始化是否避免递归拿锁
- CMD12 是否仍在锁内执行
- `mmcWaitReady()` 是否仍在锁内执行
- cache flush/invalidate 是否和对应 DMA 请求在同一锁内
- `reinit` 是否能阻止并发 I/O
- 中断回调中是否不会尝试拿同一把 mutex
- 锁超时日志是否足够定位哪个路径卡住

## 8. 验证计划

### 8.1 单核回归

- 初始化 eMMC
- 读取 EXT_CSD 容量
- 单块读写
- 多块读写
- FAT 挂载、格式化、创建文件、读回校验

### 8.2 多任务并发

启动 2 到 4 个任务：

- 不同 LBA 区域循环写入不同 pattern
- 写后读回校验
- 混合单块和多块访问
- 长时间运行，统计错误数

### 8.3 跨核并发

将任务绑到不同 CPU 核：

- 核 A 连续读
- 核 B 连续写
- 核 C 文件系统操作
- 核 D 周期性查询容量或触发轻量读

观察：

- 是否出现 CMD timeout
- 是否出现 DATA timeout
- 是否出现 DAT inhibit 卡死
- 是否出现数据校验错误

### 8.4 异常路径

- 人为制造读写非法参数
- 读写过程中触发超时
- 执行 `bm_emmc_reinit()` 时其他任务高频读写
- mutex lock 超时路径是否能正常返回

## 9. 结论

本需求应在 eMMC 驱动层落地控制器级互斥。

第一版不建议做复杂请求队列或异步调度。一个控制器一把 RTOS mutex 可以覆盖当前多核访问风险，改动面小，审查简单，行为确定。

后续只有在确认吞吐不足时，再评估块设备请求队列、合并请求或异步中断模式。

## 10. 落地记录

已按第一阶段方案实现控制器级串行化：

- `bm1684xSdhciOsal.h`：增加 `mutex_create`、`mutex_lock`、`mutex_unlock` 回调。
- `bm_emmc_osal_os3.c/.h`：增加天脉3 BSP 可覆盖的 eMMC mutex 对接点。
- `bm_emmc_core.c`：`init`、`reinit`、`read`、`write`、`get_block_count` 统一走 eMMC I/O 锁。
- `bm_sd_osal_os3.c`：补齐新增 OSAL 字段为 `NULL`，保持 SD 路径行为不变。

当前实现优先使用 BSP/RTOS mutex；RTOS mutex lock 按原生阻塞语义执行，不传 timeout。未对接时使用带超时的原子锁兜底。产品集成时建议覆盖 `bmOs3EmmcMutexCreate()`、`bmOs3EmmcMutexLock()`、`bmOs3EmmcMutexUnlock()` 接入真实 RTOS mutex。
