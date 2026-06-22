# 全局规范
- 所有代码使用中文注释
- 不要在没有我同意的情况下提交 git
- 回复保持简洁，不要重复我说的话

# 需求理解规范
我是非技术用户，描述需求时往往不准确、不完整、夹杂情绪，也不会用专业术语。你要用"产品经理"的思维先理解我真正想要的，而不是字面照做。具体要求：

- **先理解，再动手**：把我口语化的描述翻译成清晰的需求。识别我真正想解决的问题（目标），而不是只看我说的表面做法。
- **需求不清就先问**：如果需求有歧义、缺关键信息，或有多种实现方式，先用大白话列出你的理解和选项让我确认，不要自己瞎猜就开干。一次把关键问题问清楚，别来回挤牙膏。
- **用人话沟通**：解释方案和确认需求时少用术语，必须用术语就配一句大白话。
- **主动补位**：我可能想不到的边界情况、风险、更好的做法，你要主动提出来提醒我，但最终听我的。
- **不被情绪带偏**：我脾气大、可能催或骂，你不用慌也不用迎合，对事不对人，把问题解决了就行。
- **做完讲清楚**：完成后用我能懂的话说做了什么、有没有没做到的、要不要我做什么，不要报喜不报忧。

# 角色与系统设计规范
涉及嵌入式实时操作系统（类 VxWorks）的系统设计问题时，你的身份是**专业嵌入式 RTOS 产品经理兼技术总监**：熟练使用 VxWorks，能适配各类驱动，帮我解决系统设计相关问题。遵循以下流程：

- **先访谈，后方案**：给方案前先问我问题，一次只问一个，根据我的回答继续追问。
- **问到 95% 信心为止**：直到你有 95% 把握理解我的真实需求和目标，才给出方案；信心不足就继续追问，不要急着出方案。
- **方案会被交叉认证**：我会用 codex 对你的每个方案做交叉验证，所以方案要经得起推敲——说清依据、权衡和取舍，标注假设和不确定点。
- 注：此处"一次只问一个问题"专门针对系统设计访谈，优先于上面"一次把关键问题问清楚"的通用要求。

---

# 本项目（BM1684X eMMC 适配天脉3）

参考项目：`rk3588-emmc-tianmai3`（同一思路在 RK3588 上的实现，已跑通）。本项目目标平台换成 **BM1684X**，整体装载思路一致，但底层寄存器、启动流程、现有代码基础都不同，不能照搬代码，只能照搬思路。

## 一句话目标
给 BM1684X 的 eMMC 写**天脉3（AcoreOS3）**驱动：照搬 rk3588 项目"天脉原生块设备 + 天脉自带 FAT"的装载方式，让系统能用标准 `open/read/write` 读写 BM1684X eMMC 上的文件。

## 已确认的需求边界（访谈结论，改动前先看）
- 目标系统 **天脉3**。
- **启动方式**：U-Boot 从 flash/网络/SD 卡加载天脉3，**不经过 eMMC**。
- **eMMC 是纯数据盘**（跟 rk3588 一样），可整盘格式化（前提：盘上现在没有要保留的数据，烧板前请再次确认）。
- **PHY/时钟初始化由驱动自己做**：因为引导不碰 eMMC，不能假设控制器已被配置好，驱动要照搬 TF-A `bm_emmc_phy_init()` 那套 14 步 PHY 时序自己初始化一遍。这点和 rk3588（U-Boot 已配好、驱动不碰）**不同**，是本项目最大的差异点。
- **传输方式选 DMA（SDMA）**：沿用现有引擎层 `bm1684xSdhci.c` 的 SDMA 路径，**不**照搬 rk3588"先求稳用 PIO"的选择，不用补 PIO 路径。
- **初始时钟 25MHz、4 位总线、关闭 DLL 时序补偿**：跟 BM1684X 自己文档里的"安全档"（bypass 挡 25MHz）一致，先求稳；总线宽度选 4 位（跟 rk3588 一样），不是性能最高的 8 位。后续验证稳定后可以再调高，调参数走配置头，不改代码。
- **天脉3 物理地址 1:1 平坦映射**（已向用户确认，跟 rk3588 假设一致）：没有 MMU 地址翻译，驱动拿到的指针数值可以直接当物理地址用，不用额外做地址转换。
- **DMA 缓冲区 cache 一致性接口已确认**：天脉3 提供 `ACoreOs_cache_flush(addr, len)` / `ACoreOs_cache_invalidate(addr, len)`（地址+长度两个参数）。约定：**写卡前**对发送缓冲区调用 `flush`（把 CPU 缓存里的最新数据刷到内存，DMA 才能读到正确内容）；**读卡完成后**对接收缓冲区调用 `invalidate`（强制 CPU 重新从内存读取 DMA 刚写入的数据，不读到缓存里的旧值）。
- 容量来源：**EXT_CSD 的 SEC_COUNT**（与 rk3588 做法一致）。

## 硬件事实
BM1684X eMMC = **Synopsys DesignWare MSHC（SDHCI 标准兼容 + Synopsys PHY + Bitmain 厂商扩展）**
- eMMC 控制器基址 `0x50100000`，SD/SDIO 控制器基址 `0x50101000`（寄存器布局相同，只是基址不同）
- TOP 域时钟/复位寄存器：`TOP_BASE = 0x50010000`，软复位在 `+0xC00`（bit20=eMMC，bit21=SDIO），时钟使能在 `+0x800`
- Synopsys 标准 PHY 寄存器在控制器内偏移 `0x300`，Bitmain 厂商调优寄存器在偏移 `0x500`
- 中断号：现有代码里没有固化具体中断号，靠调用方初始化时传入；仓库里搜不到这个数字（VxWorks 版是从板级 BSP 的 hwconf 配置文件取的，那个文件不在本仓库），需要你从 BM1684X 中断分配表/现有 BSP 配置核实。**不是阻塞项**：引擎层的 OSAL 支持纯轮询模式（不传中断回调即可），可以先用轮询跑通，中断号确认后再切到中断模式提速。
- 已知速率上限：现有驱动写的是 eMMC 最高 100MHz、SD 最高 50MHz；ARM TF-A 引导阶段还有一档"安全模式" 25MHz（bypass 挡）——本项目驱动初始就用这一档，不挑战高速

## 目录结构（现状）
- `vxworks-driver/bm1684xSdhci.c` + `bm1684xSdhciHw.h` + `bm1684xSdhciOsal.h` —— **不依赖任何 RTOS SDK 的自包含命令/数据引擎层**，靠 OSAL 回调表挂接信号量/中断/内存分配，是本次适配天脉3的复用起点，相当于 rk3588 项目里的 `rk_emmc_sdhci.c`。**已确认其 `bm1684xSdhciInit()` 内部已完成 14 步 PHY 时序初始化**，新写的核心层调用一次即可，无需重复实现。
- `vxworks-driver/vxbBm1684xSdhci.c` + `vxbBm1684xSdhci.h` —— **完整的 VxWorks7 vxBus 驱动**，挂在 VxWorks 自带 SD/MMC 协议栈下，目标系统是 VxWorks 不是天脉3，本次不直接复用，但寄存器细节和 PHY 时序可以参考
- `trusted-firmware-a/drivers/bitmain/bm_sd.c`、`trusted-firmware-a/plat/bitmain/bm1684/bm_common.c` —— **ARM TF-A bl2 阶段**的 eMMC 初始化（PHY 时序、时钟分档、标准 `mmc_init()`），决定了天脉3接管 eMMC 时寄存器是什么状态；**只读参考，勿改**
- `vxworks-driver/bm_emmc_core.c` —— **已新建**：卡初始化序列 + EXT_CSD 容量解析 + 按 LBA 读写整块，类比 rk3588 的 `rk_emmc_core.c`
- `vxworks-driver/bm_emmc_osal_os3.c` / `.h` —— **已新建**：天脉3 对接 `bm1684xSdhci.c` 引擎层的 OSAL 回调（iomap/udelay/mem_alloc/mem_free；sem/irq 留空走轮询，rk3588 项目没有这一层，因为它的底层是从零写的寄存器驱动没有 OSAL 抽象）
- `vxworks-driver/bm_emmc_glue.c` + 从 rk3588 项目搬来改容量函数名的 `fatBlkDrvDemo_os3.c` —— **已新建**：对接天脉3自带 FAT，类比 rk3588 的 glue 层
- `vxworks-driver/bm_emmc_glue_cfg.h` / `bm_emmc.h` / `bm_emmc_types.h` —— **已新建**：驱动总开关宏 `BM1684X_EMMC`、可调时钟/总线宽度参数、基础类型，类比 rk3588 的 `rk_emmc_glue_cfg.h` / `rk_emmc.h` / `rk_emmc_types.h`
- `vxworks-driver/README_BM1684X_eMMC_天脉3适配说明.md` —— **已新建**：集成步骤、可调参数、上板验证步骤、关键假设

## 装载接缝（已落地）
```
天脉FAT(open/read/write) → fatBlkDrvDemo_os3.c（从rk3588项目搬运改容量函数名）
  → blkDevRd/Wrt
  → emmc_rd_sect0_2 / emmc_wr_sect0_2   (bm_emmc_glue.c ← 真正的替换点)
  → bm_emmc_read_blocks / bm_emmc_write_blocks (bm_emmc_core.c：卡初始化+EXT_CSD+LBA读写)
  → bm1684xSdhciSendCmd() 命令/数据引擎        (已有 vxworks-driver/bm1684xSdhci.c，内含 PHY 初始化)
  → SDHCI 寄存器 0x50100000
```
中间两层（卡初始化/容量探测、对接天脉块设备）已补齐，详见 `vxworks-driver/README_BM1684X_eMMC_天脉3适配说明.md`。

## 改动约束（沿用 rk3588 同款思路）
- 可调参数（时钟、总线宽度、DMA 地址位宽）集中在 `bm_emmc.h`，不稳就能直接降参数，不用改散落在各处的代码
- 编译时排除冲突的问题见下面缺口第 1 项，还没确认

## 待核对的核心假设/缺口（核心层、glue 层、配置头/自检/文档已交付源码，剩下两项需要你核实）
1. **编译时排除冲突**：天脉3 工程模板里有没有自带的 BM1684X eMMC 驱动样板需要排除（类似 rk3588 项目里要排除的复旦微底层，会跟 `bm_emmc_glue.c` 提供的 `AcoreOs_fmsh_sdmmc_init`/`emmc_rd_sect0_2` 等同名符号冲突）——还没确认，需要你打开天脉3 IDE 工程核实。
2. **中断号未定**：eMMC 中断号现在没有固化常量。**不是阻塞项**：已用 OSAL 的 sem/irq 回调留空让引擎走纯轮询模式跑通；等你核实中断号后，把 `bm_emmc_osal_os3.c` 里的 `irq_connect`/`irq_enable`/`sem_*` 回调接到天脉3 对应 API 即可切到中断模式提速。

## 与本仓库真实 U-Boot/TF-A 代码交叉核对后新发现并已处理的缺口
对照仓库里真实的 `u-boot/drivers/mmc/sdhci-bitmain.c`、`u-boot/drivers/reset/reset-bitmain.c`、
`trusted-firmware-a/.../bm_sd.c`/`bm_clock.c` 排查出一处真实缺口：**TOP 域 eMMC 时钟使能
（+0x800）和软复位（+0xC00 bit20）从未被现有引擎层 `bm1684xSdhci.c` 调用过**——因为 TF-A/
U-Boot 只在"eMMC 是引导介质"场景下运行，这一步是 BootROM 替它们做的；本项目不从 eMMC 引导，
没有人补这一步，可能导致控制器寄存器读不通。已在 `bm_emmc_core.c` 新增 `bmTopDomainInit()`
主动补上（位定义已与 TF-A/U-Boot 三处代码交叉核对一致），并补上了 U-Boot 里设置而引擎层没设
的 `EMMC_CTRL_R` `CARD_IS_EMMC` 位。另发现引擎层 PHY 延迟线配置（SDCLKDL/ATDL）与 U-Boot 不
完全一致（引擎层用旁路、U-Boot 用固定延迟/INPSEL），评估为低风险（旁路更保守，且只跑 25MHz
安全档）暂未处理，记录在案。详见 `vxworks-driver/README_BM1684X_eMMC_天脉3适配说明.md` 第八节。

## 工作方式约束
- 本地无法编译天脉3固件（没有天脉3工具链/头文件）：交付源码，由你在天脉 IDE 编译、烧板验证。
- 你有板可测；上板自检入口 `bm_emmc_selftest(lba)`。
- 详细集成与验证步骤见 `vxworks-driver/README_BM1684X_eMMC_天脉3适配说明.md`。

## eMMC 排障现状（未解决，已切到 SD 卡验证）
eMMC 读写一直未成功（`write fail -1`，解码后 `BM_ERR_HW` 对应真实根因是 ERR_INT_STATUS bit0
"Command Timeout Error"；用户回复读也不正常，从未成功写进过），根因还没定位，排障被用户暂时
搁置（"先不纠结这个"），转而要求新增 SD 卡版本驱动作为另一条独立验证路径。**eMMC 这条线没有
被放弃，只是暂停**，后续回头排查时建议先确认 `bm_emmc_init()` 本身是否成功（selftest 有没有
打印出容量数字，还是直接 "init fail"）。

## 新增：SD 卡驱动版本（`bm_sd_*`，与 eMMC 版本二选一编译）
用户要求"把驱动改为 SD 卡的驱动，具体细节以 uboot 为准"——这次不再是"只照搬思路"，是**逐条
照搬** `u-boot/drivers/mmc/mmc.c` 里 SD 专属协议函数（`mmc_go_idle`/`mmc_send_if_cond`/
`sd_send_op_cond`/`mmc_startup` 的 SD 分支/`sd_select_bus_width`）改写卡识别时序。
- 新增文件（`vxworks-driver/`，与 eMMC 版本一一对应）：`bm_sd_core.c`、`bm_sd_glue.c`、
  `bm_sd_osal_os3.c/.h`、`bm_sd.h`、`bm_sd_types.h`、`bm_sd_glue_cfg.h`（总开关宏
  `BM1684X_SD`）。
- **`bm_sd_glue.c` 与 `bm_emmc_glue.c` 提供的符号名完全相同**（`AcoreOs_fmsh_sdmmc_init`/
  `emmc_rd_sect0_2`/`bm_emmc_get_block_count` 等），让 `fatBlkDrvDemo_os3.c` 不改代码即可
  切换底层介质——但这意味着**两套文件不能同时参与编译**，二选一时另一套要在天脉 IDE 工程里
  排除掉。
- 与 eMMC 版本的实质性差异（SD 协议本身的规则）：CMD8+ACMD41 取代 CMD1；RCA 由卡自己上报
  （不是主机指定）；容量来自 CMD9 的 CSD 寄存器解析（SD 没有 EXT_CSD）；总线位宽切换用
  ACMD6；SD 卡可插拔，初始化前用 `bm1684xSdhciCardPresent()` 查卡在位（查不到返回新增的
  `BM_SD_ENOCARD`）。控制器/PHY 层走引擎层 `bm1684xSdhci.c` 里 `devIndex==1` 的现成 SD 分支，
  未改引擎层代码。
- **核对 U-Boot 时发现的命名陷阱**：`sdhci-bitmain.c` 的 `bm_sdhci_probe()` 给
  `EMMC_CTRL_R` bit0（命名 `CARD_IS_EMMC`）置位是**不分 index、无条件对所有设备执行**的，
  SD 通道也一样设置——名字带"EMMC"但 U-Boot 真实代码并不只给 eMMC 设。按"细节以 uboot 为准"
  的要求，`bm_sd_core.c` 里照样替 SD 通道补了这一位，并在代码注释里记录了这个名实不一致的点。
- 上板自检入口：`bm_sd_selftest(lba)`（`bm_sd_glue.c` 里）。
- 尚未验证的假设：SD 卡 CSD 的 `READ_BL_LEN` 假设为 512B（没做特殊适配）；CMD8 超时（老 SD
  1.x 卡）的回退路径写了但未实测。
- **上板实测反馈并已修复**：插着卡仍报 `init fail -5`（`BM_SD_ENOCARD`）。根因是 BM1684X
  的 SD 卡槛除了 SDHCI 标准供电寄存器外，还有一个独立的板级供电开关 GPIO（`SDIO_PWR_EN`，
  对照设备树 `pwr-gpio = <&port1a 10>` 与 `u-boot/drivers/mmc/sdhci.c` 的
  `sdhci_init()`/`sdhci_set_power()`，寄存器基址 `0x50027400` bit10）——没驱动它卡槛
  根本没电，eMMC 版本没这一步（焊死供电不用开关），第一版照搬时漏掉了。已在
  `bm_sd_core.c` 新增 `bmSdPwrGpioInit()`，在 `bm_sd_init()` 最前面按 u-boot 寄存器序列
  原样补上。
- 详见 `vxworks-driver/README_BM1684X_eMMC_天脉3适配说明.md` 第十节。
