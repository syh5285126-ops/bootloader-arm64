# BM1684X eMMC 驱动（天脉3 / AcoreOS3）适配说明

照搬 rk3588 项目"天脉原生块设备 + 天脉自带 FAT"的装载思路，新写卡初始化层 + glue 层，
底下复用已有的 BM1684X SDHCI 命令/数据引擎（`bm1684xSdhci.c`，自带 PHY 初始化 + SDMA）。

---

## 一、调用链（本次新增的两层）

```
天脉3 FAT (open/read/write，盘符)
  → blk_dev_create / blk_dev_read / blk_dev_write   (fatBlkDrvDemo_os3.c，从 rk3588 项目搬运改容量)
  → blkDevRd / blkDevWrt
  → emmc_rd_sect0_2 / emmc_wr_sect0_2               (bm_emmc_glue.c    ← 本次新增)
  → bm_emmc_read_blocks / bm_emmc_write_blocks      (bm_emmc_core.c    ← 本次新增)
  → bm1684xSdhciSendCmd() 命令/数据引擎 (SDMA)        (已有 bm1684xSdhci.c，含 14 步 PHY 时序)
  → BM1684X eMMC 控制器 @ 0x50100000
```

## 二、新增文件（均在 `vxworks-driver/`）

| 文件 | 作用 |
|---|---|
| `bm_emmc_glue_cfg.h` | 驱动总开关 `BM1684X_EMMC` |
| `bm_emmc_types.h` | 基础类型/返回码 |
| `bm_emmc.h` | 对外接口 + 可调参数（时钟/总线宽度/DMA 地址位宽） |
| `bm_emmc_osal_os3.h` / `.c` | 天脉3 对接 `bm1684xSdhci.c` 引擎层所需的 OSAL 回调（iomap/udelay/mem_alloc/mem_free，sem/irq 留空走轮询） |
| `bm_emmc_core.c` | eMMC 卡识别、读写、EXT_CSD 容量解析 |
| `bm_emmc_glue.c` | 桥接层，提供天脉 FAT 层调用的符号 + 上板自检 `bm_emmc_selftest()` |
| `fatBlkDrvDemo_os3.c` | 天脉 FAT 块设备层，从 rk3588 项目搬运，仅改调用的容量函数名 |

## 三、集成步骤（在天脉3 IDE 工程里）

1. 把上表 7 个新文件加入工程编译（`bm1684xSdhci.c`/`.h` 原本就在仓库里，无需改动）。
2. **排查是否要排除冲突**：天脉3 工程模板里如果自带 BM1684X eMMC 驱动样板（提供
   `AcoreOs_fmsh_sdmmc_init`/`emmc_rd_sect0_2`/`emmc_wr_sect0_2` 等同名符号），需要从编译中排除，
   否则会重复定义。**本仓库内看不到天脉工程结构，这一步需要你打开天脉 IDE 工程核实**
   （对应 CLAUDE.md 缺口第 4 项）。
3. 保持天脉 FAT 块设备层启用：工程 `cfg_build.h` 中 `INCLUDE_SDMMC_AOS` 与 `TM_OS3_FAT32_FS`
   按 rk3588 项目同款配置打开；`fatBlkDrvDemo_os3.c` 顶部的 `#include "../cfg_build.h"`
   相对路径请按你工程实际目录结构调整。
4. 头文件搜索路径加入 `vxworks-driver/`。
5. 确认 `delay_us(unsigned int)` 在工程里全局可用（驱动按 rk3588 项目同款假设声明为 `extern`）。
6. 确认 `ACoreOs_cache_flush(void *addr, unsigned int len)` /
   `ACoreOs_cache_invalidate(void *addr, unsigned int len)` 在工程里全局可用（已与你确认这两个
   函数名和参数，DMA 收发缓冲区的 cache 一致性靠它们维护）。
7. 中断号：当前 `g_bm1684xOsalOs3` 的 `irq_connect`/`irq_enable` 回调留空，引擎层会自动走纯轮询，
   **可以先这样跑通**。等你从 BM1684X 中断分配表确认了 eMMC 中断号后，再补上对应的天脉3
   中断挂接函数，把这两个回调接上即可切到中断模式提速。

## 四、可调参数（`bm_emmc.h`，"先求稳"默认值）

| 宏 | 默认 | 说明 |
|---|---|---|
| `BM_EMMC_TRAN_CLK_HZ` | 25MHz | 工作时钟，对齐 BM1684X 文档"安全档"（bypass 挡）。不稳可继续下调 |
| `BM_EMMC_BUS_WIDTH` | 4 | 总线位宽，与 rk3588 项目同款选择，不挑战 8 位 |
| `BM_EMMC_USE_64BIT_DMA` | 0 | SDMA 地址位宽，0=32位。BM1684X DRAM 物理地址若确认不超 4GB 可保持默认 |

## 五、上板验证步骤

1. **底层自检**（不依赖 FAT，先验证驱动本身）：天脉 shell 调用
   `bm_emmc_selftest 1000` —— 在第 1000 扇区写-读-比对一个扇区。
   预期打印：`[bm_emmc] selftest PASS at lba 1000, capacity=XXXX sectors`，
   且 capacity 与 eMMC 实际容量相符。
2. **FAT 验证**：调用 `blk_dev_create(0)` 注册块设备（沿用天脉启动流程里已有调用点），
   首次空盘会自动格式化为 FAT32；随后用标准文件操作读写。
3. 反复读写大文件并比对，确认 DMA 读写在你的天脉3 环境下稳定（这是本项目相对 rk3588 项目
   唯一多出来的风险点：cache 一致性）。如出现"偶发读出脏数据"，优先检查
   `ACoreOs_cache_flush`/`ACoreOs_cache_invalidate` 的调用时机和参数是否符合你工程的真实语义。

## 六、关键假设（请连同 codex 一起核对）

1. **天脉3 为物理地址 1:1 平坦映射**（已与你确认），`bmOs3IoMap()` 直接把物理地址当指针返回，
   不做地址翻译。若实际开了 MMU 且非 1:1，需要改这个函数做真正的地址映射。
2. **引导不经过 eMMC，控制器/PHY 状态未知**：依赖 `bm1684xSdhciInit()` 内部的 14 步 PHY 时序
   把控制器从"未知态"初始化到可用态，本次没有重新实现这套时序，是直接复用已有引擎代码。
3. **DMA 缓冲区物理连续**：调用方（FAT 层/上层应用）传入的读写缓冲区需要物理连续，本驱动
   不做拼接或二次拷贝；这一点需要在天脉3 实际环境里验证缓冲区分配方式是否满足。
4. **容量取 EXT_CSD SEC_COUNT**：覆盖所有 ≥2GB 的现代 eMMC。
5. eMMC 为纯数据盘，整盘可格式化（已与你确认，烧板前请再次确认盘上没有要保留的数据）。

## 八、与 BM1684X 真实 U-Boot/TF-A 代码的交叉核对结果

为了排查现有引擎层（`bm1684xSdhci.c`）是否有遗漏，对照了本仓库里真实的
`u-boot/drivers/mmc/sdhci-bitmain.c`、`u-boot/drivers/reset/reset-bitmain.c`、
`trusted-firmware-a/drivers/bitmain/bm_sd.c`、`trusted-firmware-a/plat/bitmain/bm1684/{bm_clock.h,bm_clock.c,include/platform_def.h}`，
发现并处理了一处真实缺口，另外记录了两处暂不处理的差异：

1. **已修复 —— TOP 域时钟使能/软复位完全没人调用**：`bm1684xSdhciHw.h` 里早就定义好了
   `BM1684X_TOP_CLOCK_EN0`(+0x800)/`BM1684X_TOP_SOFT_RST0`(+0xC00) 以及对应的 eMMC 位
   （`BM1684X_CLK_AXI_EMMC`=bit20、`BM1684X_CLK_EMMC_200M`=bit6、`BM1684X_CLK_100K_EMMC`=bit22、
   `BM1684X_RST_EMMC`=bit20），这些位定义和 TF-A 的 `bm_clock.h`（`GATE_CLK_AXI_EMMC`=20 等）、
   `platform_def.h`（`BIT_MASK_TOP_SOFT_RST0_EMMC`=BIT(20)）完全吻合，但**这两个寄存器在
   `bm1684xSdhci.c` 引擎层里从未被实际读写过**。
   原因是 TF-A 的 `bm_sd.c`、U-Boot 的 `sdhci-bitmain.c` 走的都是"eMMC 是引导介质"的场景：
   BootROM 选中 eMMC 引导时已经替它们把时钟开了、复位放了，所以这两份代码也从来不需要显式
   调用。**本项目场景不同**（天脉3不从 eMMC 引导），没有任何更早期的代码能保证这一步已经做
   过，所以已经在 `bm_emmc_core.c` 新增 `bmTopDomainInit()`，在调用 `bm1684xSdhciInit()` 之前
   先把这两个寄存器配好（时钟开 + 复位拉低再放开，复位手法照搬 `reset-bitmain.c`）。
   **上板验证重点**：如果发现初始化一进去就卡住、或寄存器读出来全是 `0xFFFFFFFF`，先怀疑是
   这一步没生效（比如实际 TOP_BASE 地址或位定义跟你板子的芯片版本不一致）。
2. **已补 —— `EMMC_CTRL_R` 的 `CARD_IS_EMMC` 位**：`sdhci-bitmain.c` 的 `probe()` 里在控制器初
   始化之后会置位厂商区 `EMMC_CTRL_R`(+0x2C) 的 bit0，让控制器按 eMMC 卡的专属行为处理（涉及
   `RST_N` 复位脚等）；`bm1684xSdhci.c` 没有设这一位，已在 `bm_emmc_core.c` 的 `bm_emmc_init()`
   里补上（紧跟在 `bm1684xSdhciInit()` 成功之后）。
3. **记录但未处理 —— PHY 延迟线（SDCLKDL/ATDL）配置与 U-Boot 不完全一致**：
   `bm1684xSdhci.c` 的 `phyInit()` 把 `SDCLKDL_CNFG`/`ATDL_CNFG` 都配成"bypass"（旁路延迟线）；
   而 U-Boot 的 `bm_sdhci_phy_init()` 是把 `SDCLKDL_CNFG` 配成"外部固定延迟，DC=0x0A"
   （不是旁路），`ATDL_CNFG` 配成 `INPSEL=0x2`（也不是旁路）。两者不是同一档配置。
   **没有在这次改动里动 `bm1684xSdhci.c`**：一是它是已有的共享引擎代码，改之前想先问你；
   二是这两个延迟线主要影响高速时序（HS200/调优阶段），本项目目标只是 25MHz 安全档，
   旁路延迟线理论上更保守、风险更低，先不动。**如果上板后发现读写偶发出错或时钟不稳**，
   这是第一个该回头检查的点——需要的话我可以照 U-Boot 那份配置改 `bm1684xSdhci.c`，但这是
   对共享引擎层的修改，会影响 SD 控制器那一路，需要你先确认。
4. **记录 —— eMMC 时钟门控位在 TF-A 自己的代码里也是"定义了但没启用"状态**：`bm_clock.c`
   里 `bm1684_gate_clks[]` 表只填了 `APB_ROM`/`AXISRAM` 两项，eMMC 相关的
   `GATE_CLK_EMMC_200M`/`AXI_EMMC`/`100K_EMMC` 三个 ID 在这份代码里也没有对应的表项（说明
   TF-A 自己也没有显式启用过这几个门控，进一步印证"eMMC 引导场景下 BootROM 已经替它做好"
   这个推断）。第 1 条里我们启用这几个位用的是 `bm1684xSdhciHw.h` 里现成的位定义，这些位定义
   的数值（bit6/20/22）与 TF-A 的 ID 编号一致，可信度较高，但**这是本仓库代码里唯一能找到的
   依据，没有看到任何实际跑通的代码路径验证过它**，上板是第一次真正验证，请重点关注。

## 九、内存屏障（memory barrier）核对结果

对照发现：U-Boot 在 ARM64 上的 `writel/readl` 每次都带 `dmb` 屏障（`__iowmb()/__iormb()`，
见 `u-boot/arch/arm/include/asm/io.h`）；而本项目复用的引擎层 `bm1684xSdhci.c` 用的是纯
`volatile` 访问（`REG_RD/WR_*`，无任何屏障），程 DMA 地址、踢传输也都是纯 `volatile` 写。
引擎层这么写隐含假设"MMIO 区映射成 Device 内存"——Device 访问之间 CPU 本就保序，在原
VxWorks 平台成立。

本次新增代码（`bm_emmc_core.c`）**没有照搬引擎层的"无屏障"风格**，而是在两类真正要紧的边界
上显式补了 `dsb`（用内联汇编 `BM_DSB()` 实现，不依赖天脉3 SDK 头文件）：

- **TOP 域时钟/复位时序点**：`bmTopDomainInit()` 里每次控制寄存器写之后都补 `dsb`，保证"写
  真正生效"排在后面的固定延时之前——否则写还压在写缓冲里就开始计复位保持/恢复时间，会缩短
  实际复位窗口。
- **DMA 跨域定序**：写卡 `cache_flush` 之后、读卡 `cache_invalidate` 之前各补一道 `dsb`，保证
  普通可缓存内存（数据缓冲）与 Device 内存（控制器寄存器）之间的先后顺序——这是 DMA 最经典
  的出错点，`volatile` 管不了跨域定序。

说明：正确实现的 `ACoreOs_cache_flush/invalidate` 内部本就应带 `dsb`（这正是这类 cache 维护
API 的应有之义），所以 DMA 路径上我补的这几道 `dsb` 严格说是**冗余但零风险的保险**；之所以
还是补上，是因为本地无法确认你天脉3 那两个函数的具体实现，万一它是个不带屏障的薄封装（甚至
空实现），这道保险就成了必需。**反过来如果上板后偶发读到脏数据**，第一件事就是确认这两个
cache 函数确实做了真正的 clean/invalidate（而不是空壳），其次再看本节这几道屏障。

引擎层 `bm1684xSdhci.c` 本身的"无屏障 volatile"我**没有改**：一是它是共享引擎代码（也走 SD
那一路），二是只要 MMIO 区是 Device 映射（天脉3 对外设寄存器几乎必然如此）就够用。若你那边
把 0x50100000 这段映射成了普通非缓存内存（少见），则需要回头给引擎层的 `REG_*` 宏也加屏障，
这属于对共享层的改动，要先跟你确认。

## 七、取舍与后续优化

- 本版 **SDMA + 25MHz + 4位总线 + 轮询模式**：先求稳，规避 HS200/HS400 时序校准和中断接线两个
  暂时没核实清楚的环节。
- 后续如需提速：确认中断号后切中断模式（减少 CPU 占用）；确认时序余量后可尝试调高
  `BM_EMMC_TRAN_CLK_HZ`（但引擎层未实现 HS_TIMING 切换，无脑提频有失败风险，需先确认是否要
  补这部分代码）。

## 十、SD 卡驱动版本（`bm_sd_*`，本次新增）

因 eMMC 读写一直未跑通（排查中），用户要求新增一套**SD 卡版本**的驱动，作为另一条独立验证
路径，方便用插拔方便、问题更容易隔离的 SD 卡来判断"是引擎层/控制器本身有问题"还是"eMMC 这
颗卡/这条信号链路有问题"。与原 eMMC 版本的关系：

- **二者二选一编译，不能同时打开**：`bm_sd_glue.c` 与 `bm_emmc_glue.c` 提供的符号名【完全
  相同】（`AcoreOs_fmsh_sdmmc_init`/`emmc_rd_sect0_2`/`emmc_wr_sect0_2`/`bm_emmc_get_block_count`
  等），这是有意设计成的——这样 `fatBlkDrvDemo_os3.c` 不用改一行代码，换底层介质时直接在天脉
  IDE 工程里切换参与编译的文件组即可。详细二选一说明见 `bm_sd_glue_cfg.h` 顶部注释。
- **协议时序的依据不同**：原 eMMC 版本是"照搬 rk3588 项目的思路，细节按 JEDEC eMMC 规范自己
  写"；本 SD 版本按用户明确要求**逐条照搬 U-Boot 通用协议层** `u-boot/drivers/mmc/mmc.c`
  里 SD 专属的函数改写（`mmc_go_idle`/`mmc_send_if_cond`/`sd_send_op_cond`/`mmc_startup`
  的 SD 分支/`sd_select_bus_width`），命令参数、响应类型、CSD 容量解析公式都与这几个函数
  逐行对照过。
- **新增文件**（均在 `vxworks-driver/`，与 eMMC 版本一一对应）：
  - `bm_sd_core.c` —— SD 卡协议层：CMD0/CMD8(SEND_IF_COND)/CMD55+ACMD41(SD_SEND_OP_COND)/
    CMD2/CMD3(卡自报 RCA)/CMD9(CSD 解析容量)/CMD7/CMD55+ACMD6(切4位总线)，按 LBA 读写整块；
    类比 `bm_emmc_core.c`。
  - `bm_sd_glue.c` —— 对接天脉3 FAT 的桥接层，符号名与 `bm_emmc_glue.c` 相同（见上）；
    类比 `bm_emmc_glue.c`。
  - `bm_sd_osal_os3.c`/`.h` —— OSAL 回调表，内容与 `bm_emmc_osal_os3.c` 完全一致，只是换了
    导出符号名（`g_bm1684xOsalOs3Sd`）避免重复定义；类比 `bm_emmc_osal_os3.c`/`.h`。
  - `bm_sd.h`/`bm_sd_types.h`/`bm_sd_glue_cfg.h` —— 对外接口/基础类型/总开关宏
    `BM1684X_SD`；类比 `bm_emmc.h`/`bm_emmc_types.h`/`bm_emmc_glue_cfg.h`。
- **与 eMMC 版本的实质性差异**（均为 SD 协议本身的规则，不是本项目自创）：
  1. 没有 CMD1，识别态走 **CMD8 + ACMD41**（不是 CMD1 OCR 轮询）；
  2. **RCA 由卡自己上报**（CMD3 响应里取），不是主机指定固定值；
  3. **容量来自 CMD9 的 CSD 寄存器**（按 U-Boot 公式解析），SD 卡没有 EXT_CSD；
  4. 总线位宽切换用 **ACMD6**（标准命令），不是 eMMC 的 CMD6 改 EXT_CSD 字段；
  5. **SD 卡可插拔**，初始化前先用 `bm1684xSdhciCardPresent()` 查卡在位，查不到卡返回新增的
     `BM_SD_ENOCARD`；eMMC 焊死在板上没有这一步。
  6. 控制器/PHY 层走的是引擎层 `bm1684xSdhci.c` 里 `devIndex==BM1684X_SD_INDEX(=1)` 的分支
     （基址 0x50101000，PHY 的 SMPLDL 配置等已按 U-Boot `bm_sdhci_phy_init()` 的 SD 分支
     原样实现，本次未改），这部分引擎层早就支持，无需新写代码。
- **核对 U-Boot 源码时发现并已照办的一个细节**：`u-boot/drivers/mmc/sdhci-bitmain.c` 的
  `bm_sdhci_probe()` 里，`EMMC_CTRL_R` 寄存器 bit0（命名是 `CARD_IS_EMMC`）是**不分
  `host->index`、对任何使用这份驱动的设备都无条件置位**的，SD/SDIO 通道也一样设置——名字
  看起来像"只给 eMMC 用"，但 U-Boot 实际代码并没有按名字这么做。本 SD 版本按"具体细节以
  U-Boot 为准"的要求，在 `bm_sd_core.c` 里原样替 SD 通道也补上了这一位（已在文件内注释处
  说明依据）。这是一个**命名和行为不一致**的真实细节，留痕方便后续核对。
- **沿用 eMMC 版本未变的部分**：TOP 域时钟使能/软复位手法（只是换成 SD 对应的位定义
  `BM1684X_CLK_AXI_SD`/`BM1684X_RST_SD` 等）、DMA cache 维护时序、`先求稳`参数默认值
  （25MHz、4位总线、轮询模式），原因和注意事项都与第八/九节一致，不再重复。
- **上板自检入口**：`bm_sd_selftest(lba)`（在 `bm_sd_glue.c` 里），用法和打印格式与
  `bm_emmc_selftest()` 一致。
- **尚未验证的假设**（同样需要上板核实）：SD 卡的 CSD 里 `READ_BL_LEN` 字段假设为 512B
  （绝大多数现代卡如此），本版本没有读取该字段做特殊适配，遇到块长不是 512 的老卡会算错容量；
  CMD8 超时（老的 SD 1.x 卡）路径写了但未实测，理论上会回退到字节寻址，不保证所有老卡都兼容。
- **上板实测反馈并已修复的真实缺口**：第一版插着卡仍报 `init fail -5`（`BM_SD_ENOCARD`，
  卡检测不到）。交叉核对 `u-boot/drivers/mmc/sdhci.c` 后发现：BM1684X 平台的 SD 卡槛除了
  SDHCI 标准的 `POWER_CONTROL` 寄存器外，**还有一个独立的板级供电开关 GPIO**（即设备树
  `sdhc@50101000` 节点的 `pwr-gpio = <&port1a 10 GPIO_ACTIVE_HIGH>;`，对应 u-boot
  `sdhci_init()`/`sdhci_set_power()` 里裸写 `BM_PORTB_BASE(0x50027400)+0x0/+0x4/+0x8`、
  bit10 的那段代码，名字叫 `SDIO_PWR_EN`/GPIO42）。没驱动这个 GPIO，卡槛物理上完全没电，
  插着卡控制器也检测不到——这是 eMMC 版本没有的步骤（eMMC 焊死供电，不需要开关），第一版
  照搬 eMMC 思路时漏掉了。已在 `bm_sd_core.c` 新增 `bmSdPwrGpioInit()`，在 `bm_sd_init()`
  最前面（TOP 域时钟之后、引擎初始化之前）按 u-boot 的寄存器序列原样补上：选软件模式
  （+0x8 清 bit10）→ 设输出方向（+0x4 置 bit10）→ 驱动高电平上电（+0x0 置 bit10）。
- **补供电 GPIO 后仍报 `-5`，已把卡检测改为"软检测"**：补上供电 GPIO 后再上板，`init fail -5`
  依旧。说明 SDHCI 的"卡在位"状态位（Present State bit16）在本板上插着卡也读不到 1——
  可能是卡检测(CD#)信号没接到控制器、控制器全复位后该位消抖未完成、或该引脚复用未到位。
  既然卡是确实插着的，硬卡在这一位上没有意义。已把 `bm_sd_init()` 里原来"读不到就直接
  返回 `BM_SD_ENOCARD`"改为**软检测**：先轮询约 1s 给消抖留机会，仍读不到就打印一条告警
  后【继续往下走】识别流程。这样做安全（引擎层发命令有 1s 超时保护，真没卡/没电时后续
  CMD55/ACMD41 会超时返回别的错误码，比死活停在 `-5` 更能定位真正卡点）。下次上板看告警
  后面跟着的是"识别成功打印容量"还是"某条命令超时"，据此再决定查供电/信号链路还是查时序。
