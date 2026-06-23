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
- **软检测改造后的最新反馈：错误码变成 `-1`，定位到 `bm1684xSdhciSendCmd()` 内部返回**。
  软检测确实绕开了卡检测这一关（说明卡检测位读不到 1 本身不是致命问题），但暴露出下一个
  真问题：识别序列里某条命令在引擎层超时了。问题是 `-1` 这个返回值在
  `bm1684xSdhciSendCmd()` 内部有**两处来源**，含义完全不同：
  1. 函数最开头等 `CMD_INHIBIT`/`DAT_INHIBIT` 清零超时——命令寄存器都没机会写下去，
     说明总线根本没就绪（多半是时钟没起来/卡没真正上电），和具体哪条命令无关；
  2. 命令已经写下去之后，等 `CMD_COMPLETE` 中断状态位超时——命令确实发出去了，但
     控制器硬件既没报错也没报完成，多半是总线上没有任何卡在响应。
  仅凭"在 `bm1684xSdhciSendCmd` 里返回 -1"这一句话分不清是哪一种，也不知道是初始化序列
  里哪条命令（CMD0/CMD8/ACMD41/CMD2/CMD3/CMD9/CMD7/CMD16 中的哪一条）失败的。
  **已加临时排障代码**（`bm_sd_core.c` 新增 `sdSendCmdDbg()`，包了 `bm_sd_init()`/
  `sdWaitReady()`/`sdSendAppCmd()` 里全部命令发送调用）：命令失败时打印该命令编号、
  返回值，以及当时 `PRESENT_STATE`/`INT_STATUS`/`ERR_INT_STATUS` 三个原始寄存器值。
  下次上板后把失败那行 `[bm_sd] FAIL ...` 的完整日志贴出来即可定位：
  - 日志里 `inhibit=CMD,`（或带 `DAT,`）→ 对应上面第 1 种，总线没就绪，方向是查时钟
    /PHY/供电，和具体命令无关；
  - `inhibit=` 后面空、`err=0x0000` → 对应第 2 种，命令发出去了但没人理，方向是查
    卡是否真的在线（信号完整性、CD/DATx 线是否接对）；
  - `err=` 非 0（比如 bit0 对应 Command Timeout Error）→ 控制器自己已经判定出错，
    哪条 `CMD%u` 打出来的编号就是真正卡住的那一条。
  这段代码是临时诊断用，根因定位后会整段删除，不会留在最终交付版本里。
- **上板日志实测：卡在 CMD8（SEND_IF_COND），`inhibit=CMD,`，`int=0x0000 err=0x0000`**，
  且找到了大概率根因——**供电 GPIO 那一步漏了"引脚归属"这一层**。原因是这样：CMD0 不需要
  卡应答（协议规定"无响应"），所以"CMD0 过了"只能证明控制器自己的 CLK/CMD 线电气上没死，
  并不能证明卡真的在和总线通信；CMD8 是识别序列里第一条要求卡必须应答的命令，它超时且硬件
  没有报任何错误（既非完成也非 Command Timeout Error），说明命令发出去后总线上**没有任何
  东西在响应**——最直接的解释就是卡槛压根没有电。复查 `u-boot/board/bitmain/bm1684/board.c`
  的 `pinmux_config(PINMUX_SDIO)` 才发现：U-Boot 在驱动 `SDIO_PWR_EN` 这个 GPIO 之前，
  还先在芯片顶层 pad mux 上把这颗引脚的功能选成了"GPIO"（`PINMUX_BASE(0x50010400)+0x28`，
  bit[5:4] 写 `0x1`）——这是比 GPIO 控制器自己的寄存器更底层的一道"门"：这颗物理引脚默认
  可能被路由到别的功能模块上，不先在这里选成 GPIO，GPIO 控制器内部寄存器写得再对，输出也
  到不了物理引脚。之前两版只补了 GPIO 控制器自己那三个寄存器（`0x50027400+0x0/+0x4/+0x8`），
  漏了这第一道"pad mux 选 GPIO"的门——这正好解释了为什么补完 GPIO 寄存器后供电问题表面
  上像是没解决（卡检测位读不到、CMD8 起始终没人应）：因为 GPIO 写入可能根本没生效到引脚上。
  已在 `bmSdPwrGpioInit()` 最前面补上这一步pad mux 写入，三步合并按 u-boot 真实顺序执行。
  这是目前为止证据最完整的一次根因定位，但仍需上板验证：补上这步后 CMD8 是否能拿到正确的
  校验码回显（说明卡真的通了），还是依旧超时（说明还有别的没覆盖到的环节）。
- **上板验证结果：pad mux 补丁未解决问题，CMD8 仍超时**——`state=0x03f70001`、
  `int=0x0000`、`err=0x0000`，跟补丁前几乎一样（卡在位标志位之前就一直是 1，不是这次
  补丁带来的变化）。说明供电这条线已经查到头了（GPIO 寄存器、pad mux 都已按 u-boot 原样
  补上），但问题没解决，需要换方向排查。
- **换方向排查后定位到新根因：引擎层 `bm1684xSdhci.c` 的 `phyInit()` 里 PHY 信号 pad 的
  电气参数写错了，三个挡都不对**。排查方法：用户找了一份"听说能用"的第三方天脉3 SD 驱动
  （独立实现，不是本仓库代码）供对比，连同本仓库已有的 `trusted-firmware-a/drivers/bitmain/
  bm_sd.c`（里面同时有 `bm_sd_phy_init()` 和 `bm_emmc_phy_init()` 两份真实参考实现）一起
  核对，发现这两份独立来源对 CMD/DAT/CLK/STB/RST 几个 pad 的 `RXSEL`/`TXSLEW_CTRL_P`/
  `TXSLEW_CTRL_N` 用的是同一组数值，且都跟我们引擎层原来写的值不一样；`SDCLKDL_CNFG`/
  `ATDL_CNFG` 两个延迟线的工作模式也选错了（原来选的是"旁路"，两份参考都是"固定延迟"模式）。
  这是第一个**被两个互相独立的来源同时印证**的发现，比之前任何一条线索都更有把握。而且
  `phyInit()` 是 eMMC 和 SD 两条线共用的同一份代码——这意味着它很可能不仅是 SD 这次 CMD8
  超时的根因，也可能是之前暂停排查的 eMMC 那条线"写卡失败 -1（Command Timeout Error）"
  的同一个根因。已直接在 `bm1684xSdhci.c` 的 `phyInit()` 里改正（这是本次唯一一次修改这个
  共用引擎层文件，之前的约定是只改 `bm_sd_core.c`，这次破例是因为证据强度足够）：
  - CMD/DAT/CLK/STB/RST 五个 pad 的 `RXSEL` 统一改成 2（CLK pad 原来甚至没设这个字段）；
  - 五个 pad 的 `TXSLEW_CTRL_P`/`TXSLEW_CTRL_N` 统一从 `0xA`/`6` 改成 `0x3`/`2`；
  - `SDCLKDL_CNFG` 从 `BYPASS_EN` 改成 `EXTDLY_EN`（固定延迟，配合原有的 `SDCLKDL_DC`
    默认值 `0x0A`，旁路模式下这个延迟值是被忽略的，改完才会真正生效）；
  - `ATDL_CNFG` 从 `BYPASS_EN` 改成 `INPSEL_CNFG=2`；
  - `COMMDL_CNFG`、`SMPLDL_CNFG`（eMMC 用 `INPSEL=0x2`、SD 用 `BYPASS`）这两处核对后
    跟 TF-A 一致，未改动。
  **这是目前还没上板验证的一次改动**，下次测试如果 CMD8 还过不去，下一个怀疑对象是第三方
  驱动里发现的"供电后到时钟使能之间的延时"比我们现在长得多（参考驱动是上电后 `mdelay(20)`
  → 使能总线供电 → 配置时钟分频 → `mdelay(50)` → 真正打开 `CLK_CARD_EN` → `udelay(400)`
  才发第一条命令，总共 70ms 量级；我们现在这条路径的延时是几百微秒量级），目前先不动这部分
  代码，等 PHY 参数这次改动的结果出来再决定要不要补。另外第三方驱动里还确认了 SD 中断号是
  `46`（唤醒中断是 `45`），供后续从轮询切中断模式时使用，目前仍是轮询模式，未应用此号。
- **上板验证：PHY 参数修复生效，`init` 成功**——CMD8 不再超时，整段卡识别+CSD 容量解析
  都走通了，证实上一条 PHY 电气参数的修复就是之前 CMD8 卡住的真正根因。这是 SD 这条线第一次
  跑通卡识别。
- **新问题：`init` 成功后，`bm_sd_selftest()` 里写成功、读失败，`read fail -1`**。
  `bm_sd_write_blocks()`（CMD24+SDMA 写数据）和紧随其后的 `sdWaitReady()`（CMD13 轮询）
  都顺利通过，证明卡此时是真的在线且总线/命令通道是好的；只有 `bm_sd_read_blocks()`
  （CMD17+SDMA 读数据）失败。但读失败之前 `bm_sd_read_blocks()` 直接调用引擎层
  `bm1684xSdhciSendCmd()`，没有走排障用的 `sdSendCmdDbg()` 包装，所以失败时什么都没打印，
  无法判断卡在"命令阶段没响应"还是"数据阶段 DMA 没等到 XFER_COMPLETE 完成"——这两类问题
  方向完全不同（前者查总线/卡在线状态，后者查 DMA/缓冲区），不能瞎猜。**已处理**：把
  `bm_sd_read_blocks()`/`bm_sd_write_blocks()` 也改走 `sdSendCmdDbg()`，并把调试打印加了
  两项新信息：命令响应寄存器 `resp0`（用于判断 R1 响应是否已经拿到，没拿到说明问题在命令
  阶段；拿到了说明命令成功，问题在后面的数据搬运阶段）和 `data=READ/WRITE` 标签（区分是
  读失败还是写失败触发的打印，因为现在两个方向共用同一个调试函数）。**这版改动还没上板
  验证**，下次测试请直接把 `read fail` 那一行的完整 `[bm_sd] FAIL READ(CMD17) ...` 日志
  贴出来，据此可以一次性判断根因方向，不用再来回猜。
- **用户追问"我们的代码和参考驱动在这块（读/写数据阶段）有什么区别"，逐行比对后找到一处
  具体的寄存器配置缺口**：参考驱动每次发数据命令前都会写一次 `SDHCI_TOUT_CTRL`（标准
  SDHCI"超时控制"寄存器，偏移 `0x2E`）为 `0x0E`（取这个寄存器允许的最大值，相当于把
  控制器自己内部判定"数据超时出错"的时间窗拉到最长）；交叉核对发现本仓库另一份现成代码
  `vxbBm1684xSdhci.c`（完整 VxWorks7 vxBus 驱动，本项目没直接复用但寄存器细节可参考）的
  `bm1684xSdhciHwInit()` 里**也**写了同样的值 `0x0E`，注释写的是"Timeout: max value (0xE)
  for 50 kHz TMCLK"。而我们天脉3 这条线实际在用的引擎层 `bm1684xSdhci.c` 的
  `controllerInit()` 里，**从来没有写过这个寄存器**，复位后是什么默认值未知。两个独立来源
  都主动配置成最大值，意味着这个寄存器的硬件复位默认值大概率偏小，留着不配大概率会让控制器
  内部更容易先判定"数据超时"。已照样在 `controllerInit()` 里补上这一行（紧跟在
  `POWER_CONTROL` 写之后，跟 `vxbBm1684xSdhci.c` 的顺序一致）。这是排障过程中第一次发现
  一个"完全没配置"的寄存器（不是数值配错），风险低（只是放宽阈值，不影响功能正确性），
  但**和读失败是否是同一个根因，仍需上板验证才能确认**——目前还不知道读失败那次的
  `err=0x00xx` 是不是非 0（如果非 0 且对应 Data Timeout Error 这一位，就能直接对上）。
  另外比对时还看到两处纯架构差异，记录但判断跟当前问题关系不大，不必改：参考驱动每次发
  数据命令前会先把 `INT_STATUS`/`ERR_INT_STATUS` 清成全 1（清掉上一次残留的状态位，更
  保守的写法，我们没有这步但目前没看到因此误判的证据）；以及参考驱动对单块读（CMD17）也
  无条件带上"多块"标志位（我们只在多块时才带），这个标志位本身对单块传输没有实际副作用，
  不是差异来源。
- **上板验证：读失败日志已拿到，定位到真正根因——不在 `SDHCI_TOUT_CTRL`，是 `sdWaitReady()`
  给出了"假就绪"**。日志关键信息：`state=0x03F70106`（DAT_INHIBIT 这一位是 1）、
  `int=0x0040`（命令完成/数据完成相关的位全是 0）、`err=0x0000`（硬件没报任何错误，
  排除"数据超时"，上一条 `SDHCI_TOUT_CTRL` 缺口跟这次读失败无关，但仍是个值得保留的
  改进）、`resp0=0x00000000`（命令的响应寄存器是空的，说明 CMD17 这条命令根本没有发到
  总线上）。结合 `bm1684xSdhciSendCmd()` 的代码逻辑：发命令前会先等
  `PRESENT_STATE` 里的忙位清除，等不到才会直接返回超时、连 `ARGUMENT`/`COMMAND`
  寄存器都不会去写——`resp0` 是空的正好对应这条路径。也就是说 **CMD17 卡在"发命令前
  等总线空出来"这一步，一直没等到，超时放弃**。

  再往前查为什么会等不到：对照真实的 `u-boot/drivers/mmc/sdhci.c` 的
  `sdhci_send_command()`（第 252 行 `mask = SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT;`），
  u-boot 对**几乎所有命令**（只有"停止传输"命令例外）在发出前都会同时等两个忙位——
  包括查询卡状态的 CMD13。而我们引擎层 `bm1684xSdhciSendCmd()` 里这道等待逻辑，
  原来写的是"只有带数据的命令、或者带忙信号响应的命令才等 DAT 忙位"，CMD13 两条都不
  占，所以之前 `sdWaitReady()`（写完一块卡后内部靠轮询 CMD13 判断"卡是否空闲"）发
  CMD13 时根本没去看 DAT 忙位，只看卡自己在 CMD13 响应里报的"我准备好了"——卡口头说
  好了，但 SD 总线 DAT 线上的忙信号实际还没真正撤掉，于是 `sdWaitReady()` 提前放行，
  紧接着的 CMD17（读）一发，自己的等待逻辑（带数据，本来就该等 DAT 忙位）才第一次
  真去看这个忙位，结果发现还在忙，干等到 100ms 超时直接放弃——这就是"写完之后看似一切
  正常，紧接着的读却超时"的真正原因：**不是数据搬运卡住了，是下一条命令压根没发出去**。

  **已修复**：把引擎层的等待逻辑改成跟 u-boot 一致的口径——除了"停止传输"命令（CMD12，
  本项目目前单块读写也用不到这条），所有命令发出前都同时等 CMD 和 DAT 两个忙位清除。
  这样 CMD13 自己会先等真正的总线忙位清掉才发命令，给出的"卡已就绪"才是跟硬件状态对得上
  的，不会再有这个"卡口头说好但总线没好"的时间差。改动在共用引擎层
  `vxworks-driver/bm1684xSdhci.c` 的 `bm1684xSdhciSendCmd()` 里，eMMC 和 SD 两条线
  共用这段代码，理论上也会顺带影响之前暂停排查的 eMMC 读写问题（同一份代码、同一类
  时序假设），但 eMMC 那条线还没重新上板验证，不确定是不是同一根因。**这版改动还没
  上板验证**，下次测试请直接跑 `bm_sd_selftest()`，确认读是否成功；如果还失败，请把
  新的失败日志贴出来。
- **上板验证：读确认修好了，但同一次改动又带出一个新问题——写失败，报错落在 CMD13
  上**。根因和上一条是同一类问题的另一面：上一条把 CMD13 也纳入"要等 DAT 忙位清除
  才发命令"的范围是对的（跟 u-boot 一致），但等待这个忙位的**超时时长**留的是旧值
  ——引擎层原来写死等 100ms（100000 次 1us 轮询）就放弃，这个数字是按"正常情况下
  忙位几乎瞬间清除"给的粗略估计，从来没有对照过 u-boot 真实的超时预算。回去对照
  `u-boot/drivers/mmc/sdhci.c` 的 `sdhci_send_command()`（第 248~275 行）发现
  u-boot 这里并不是固定等 100ms：它从 `SDHCI_CMD_DEFAULT_TIMEOUT`（100ms）起步，
  每等满一轮就把超时翻倍重试，直到 `SDHCI_CMD_MAX_TIMEOUT`（3200ms）才真正放弃
  ——也就是 u-boot 实际能容忍的卡内部"编程忙"时间最长是 **3.2 秒**，比我们原来的
  100ms 短了整整 32 倍。SD 卡写完一块之后内部要做"编程"（把数据从缓存真正写入
  闪存介质），这个过程在 SD 规范里没有硬性上限，部分卡（尤其老卡/廉价卡）超过
  100ms 很常见，但极少会超过 3.2 秒。上一条修复让 CMD13 开始如实等待这个忙位后，
  如果这张卡的编程时间正好落在"超过 100ms、但远小于 3.2s"这个区间，就会精确踩中
  "忙位真实存在、但我们的超时给得太短"这个坑——`sdWaitReady()` 内部调用 CMD13
  时在引擎层的等待循环里就直接超时返回，根本等不到 `sdWaitReady()` 自己外层那套
  ~1 秒重试逻辑生效（说明详见 `vxworks-driver/bm_sd_core.c` 的 `sdWaitReady()`：
  它对"CMD13 本身发送失败"是直接报错退出，不会重试——这一点也跟 u-boot
  `mmc_poll_for_busy()` 的真实逻辑一致，不是我们独有的缺陷，所以不需要改这部分，
  只需要让超时预算跟 u-boot 对上）。
  **已修复**：把 `vxworks-driver/bm1684xSdhci.c` 里等 CMD/DAT 忙位清除的超时，
  从写死的 100ms 改成跟 u-boot 放弃前的等待总量一致的 3.2 秒（新增具名常量
  `SDHCI_INHIBIT_TIMEOUT_US`，不再是裸的 `100000U` 魔法数字）。没有照搬 u-boot
  那套"边等边打印边翻倍"的写法，因为对裸机驱动没有额外价值，翻倍只是 u-boot 为了
  日志友好，最终效果（放弃前能等多久）跟直接拉满上限是一样的。
  **影响范围提醒**：这个等待循环是每条命令发出前都会过一次的公共逻辑，正常情况
  （忙位很快清除）几乎不增加耗时；唯一会变慢的场景是"卡确实不在线/总线异常"——
  以前最多卡 100ms 就报超时，现在最多可能卡到 3.2 秒才报超时。SD 卡可插拔，
  之前已经把"卡检测"那一关从硬性失败改成了"软检测"（查不到也继续往下走），
  这个改动会让"真的没插卡"这种情况下，每条命令的失败反馈从 100ms 延长到最多
  3.2 秒，仅此而已，不影响功能正确性。
  **这版改动还没上板验证**，下次测试请直接跑 `bm_sd_selftest()`，确认写是否成功；
  如果还失败，请把新的 `[bm_sd] FAIL CMD13 ...` 完整日志贴出来。
- **上板验证：把超时拉到 3.2s 后 CMD13 仍然超时——这条结果很关键，反而推翻了"卡需要
  更多时间"的假设，并指向真正的根因。** 拿到的日志：`state=0x03f70106`（DAT_INHIBIT
  位=1，且 Write Transfer Active 位=1，但 DAT0 信号线电平=高、即卡已经不忙了）、
  `err=0x0000`（硬件零报错）、`resp0=0x00000000`（CMD13 根本没发出去，卡在发命令前的
  等忙位那步）。**关键含义**：卡其实已经编程完了（DAT0 高），可控制器的 DAT 占用位
  死活不清——这不是"等得不够久"，等再久也没用。而且这个 `state` 值跟最早那次读失败
  的值【一模一样】，说明 **从一开始的"读失败"到现在的"写报错在 CMD13"，根子是同一个：
  写完一块之后控制器的 DAT 线被卡住没释放**；之前两次改动（让 CMD13 也等 DAT 忙位、
  再把超时拉长）都只是在症状之间搬来搬去（症状从 CMD17 挪到 CMD13），没碰到真正的病根。
  这两次方向走错了，已纠正。
- **回到【本芯片专用、已在本硬件跑通】的第三方参考驱动 `bm_sd.c` 逐行比对，找到两处真实差异
  并据此改回/改对（本节为最新结论，优先于上面几条的过程性判断）**：
  1. **CMD13 这类纯命令本就不该等 DAT 忙位**：参考驱动把命令分成两类——带数据的
     （`bm_sd_send_cmd_with_data`）等 `CMD_INHIBIT | DAT_INHIBIT`；不带数据的
     （`bm_sd_send_cmd_without_data`，CMD13 走这里）**只等 `CMD_INHIBIT`**。这恰好是
     本驱动【最初】的写法。之前照搬 u-boot 通用 `sdhci.c`"除 CMD12 外所有命令都等
     DAT 忙位"是把这条改坏了，直接造成"CMD13 报错"。**已改回**参考驱动的口径
     （`bm1684xSdhciSendCmd()` 里 `if (pData || respType==R1B)` 才加 DAT 忙位）。
  2. **真正的病根大概率在传输模式：参考驱动对读/写一律带 `TRNS_MULTI`，连单块也带；
     我们原来只在块数>1 时才带。** 这是本颗 Synopsys/比特大陆控制器的硬件特性：单块
     模式（MULTI=0）下，写命令做完数据后卡进入"编程忙"，控制器的 DAT 忙位疑似清不
     干净，于是单块写之后 DAT 线一直被占，紧跟的命令发不出去而超时；读因为没有"写后
     编程忙"，单块也能过——这正好解释了"读好了、写不行"的不对称。参考驱动用"一律
     MULTI"绕开了这个单块模式的坑。**已照搬**：`buildXferMode()` 单块也置 `MULTI`，
     `AUTO_CMD12` 仅在真正多块时才用。
     **⚠ 这一条与"细节以 u-boot 为准"冲突**：u-boot 通用驱动只在块数>1 时置 MULTI，
     跟我们改之前一致。但 u-boot 通用驱动不认识这颗芯片的这个单块怪癖，而这份参考驱动
     是在【这块板子】上真跑通的。两相权衡，这一处我按"本芯片跑通的参考驱动"为准，没按
     u-boot。**如果你更希望这一处严格照 u-boot（只多块才 MULTI），告诉我，我把这一处
     改回去。**
  3. **顺带补齐一处两份权威来源都做、而我们漏了的整清**：发每条命令前把
     `INT_STATUS`/`ERR_INT_STATUS` 整体写 `0xFFFF` 清零（u-boot 在等完忙位后清、参考
     驱动在命令最前面清），避免上一条命令残留的状态位让下一条命令的等待逻辑误判
     "已完成"。我们原来只在 `pollWaitStatus` 里清匹配到的那几位。已补在
     `bm1684xSdhciSendCmd()` 等完忙位之后、写命令寄存器之前。
  以上三处都改在共用引擎层 `vxworks-driver/bm1684xSdhci.c`（eMMC/SD 共用），其中第 2 条
  最可能是真正的修复点。**这版改动还没上板验证**，下次请直接跑 `bm_sd_selftest()`，看
  写/读是否都过；若还失败，请把完整失败日志贴出来（含 `state`/`int`/`err`/`resp0`）。
- **上板验证：上一版改完后"错误又改变位置了"——CMD13 不再报错，错误挪到了 CMD24（写
  命令）本身的数据阶段。** 解码新日志：CMD24 的命令响应 `resp0=0x00000900`（按 SD R1
  状态字解码：`CURRENT_STATE=4`=`tran`、`READY_FOR_DATA=1`、无错误位）说明**命令本身
  发送成功、卡也应答正常**；卡死在紧随其后的数据搬运阶段（DAT 忙位再也不清，跟之前
  读失败/CMD13 超时时看到的 `state` 同一种症状，但这次卡死的位置往后挪到了 CMD24 自己
  身上）。命令阶段没问题、数据阶段卡死 → 把怀疑方向从"等待忙位的逻辑/超时"转移到
  "数据搬运用的寄存器有没有配对"。
  逐一核对 `bm1684xSdhciSendCmd()` 里真正往硬件写 DMA 地址/块数的那几行（紧跟在等忙位
  之后、写 `COMMAND` 寄存器之前），同时核对 `hwInit()`：发现 `hwInit()` 给
  `HOST_CONTROL2` **始终无条件**置位 `SDHCI_HC2_VER4_ENABLE`（"Host Version 4 Enable"）。
  按 SDHCI 规范，这个位一旦置位，原来偏移 `0x00` 的 legacy"SDMA 系统地址"寄存器
  （也就是 `SDHCI_DMA_ADDRESS`）的含义会被**复用成"32位块计数"寄存器**，真正搬数据用
  的系统地址改放到偏移 `0x58`/`0x5C` 的 `ADMA_SA_LOW`/`ADMA_SA_HIGH`。而我们发命令时
  写数据寄存器的代码一直按"没开 V4"的旧布局写：把真实的缓冲区地址写进了已经被复用成
  "块计数"的那个寄存器（控制器会把这个地址数值当块数解释，是个天文数字），`ADMA_SA_LOW`
  则从来没人写过、是上电或上一次操作留下的脏值——这正好解释了"命令成功、数据阶段卡死"：
  控制器压根不知道该往哪个地址搬数据。
  交叉核对【本芯片专用、已跑通的】参考驱动 `bm_sd.c` 的 `bm_sd_prepare()` 函数，确认它
  正是按这个 V4 位分支处理的（开 V4 时写 `ADMA_SA_LOW/HIGH` + 把块数写进复用后的
  `SDHCI_DMA_ADDRESS` + `BLOCK_COUNT` 清零；不开 V4 才走旧布局），跟 SDHCI 规范的说法
  互相印证。**已修复**：`bm1684xSdhciSendCmd()` 里准备数据寄存器那段改成按
  `HOST_CONTROL2` 的 `VER4_ENABLE` 位分支，跟参考驱动 `bm_sd_prepare()` 写法一致；顺带
  把另外两处"SDMA 512KB 边界中断时重新加载地址寄存器"的代码（IRQ 模式和轮询模式各一处）
  也按同样的 V4 分支改对，否则多块传输跨边界时会用错寄存器（虽然目前单块场景不会触发，
  但为一致性一起修了）。**这是第一次发现的、跟"等忙位逻辑"完全不同类型的根因，理论上
  也是之前 eMMC 那条线读写一直失败的同一个原因（eMMC 和 SD 共用这同一段引擎层代码），
  但 eMMC 那条线还没回头重新上板验证。**
- **上板验证：V4/ADMA 地址修复没解决问题，"和之前一样"；用户反馈一个关键线索——把
  发命令前清 `INT_STATUS`/`ERR_INT_STATUS` 那两行屏蔽掉，写能过了，但读又坏了。**
  这条线索没有顺着去猜"清不清这两个寄存器"本身的因果，而是借机把参考驱动 `bm_sd.c`
  的 `bm_sd_send_cmd_without_data()`（CMD13 等不带数据的命令走这个函数）逐行重新读了
  一遍，发现**上一轮"CMD13 只等 CMD_INHIBIT、不等 DAT_INHIBIT"的结论是误读**：那次
  只看了函数开头第一段等待循环（`mask = SDHCI_CMD_INHIBIT`），漏看了同一个函数后面
  （第288~294行）还单独有一段——算完命令的响应类型标志后，只要标志不是"无响应"
  （也就是除 CMD0 外的所有命令，CMD13 当然在内），**还会再等一次 DAT_INHIBIT 清除**。
  两段等待加起来，参考驱动的真实规则是：**除 CMD0 外，所有命令都要等 DAT 忙位清除**，
  跟最早从 u-boot 抄来的"除停止传输命令外都等"几乎是同一个规则，只是例外的命令不同
  （CMD0 而不是 CMD12）——上一轮"已改回参考驱动口径"的判断从根上就是错的，等于一直在
  错误的规则上原地打转。
  另外顺手核对了清寄存器的【顺序】：参考驱动是"先清 INT_STATUS/ERR_INT_STATUS，再等
  忙位"，我们之前的代码顺序刚好反过来（先等忙位、再清），这次也按参考驱动调整为先清
  后等；并补上参考驱动里"每条命令都重写一次超时门限寄存器"（我们之前只在初始化时写
  一次）。
  **已修复**：`bm1684xSdhciSendCmd()` 里等待 DAT_INHIBIT 的条件从"只有带数据/R1B忙
  响应才等"改成"除 CMD0 外都等"，并把清状态寄存器的顺序、超时门限寄存器的重写时机都
  跟参考驱动对齐。**这版改动尚未上板验证**。请用户先去掉本地"屏蔽清寄存器"那个临时
  改动（已经不需要了，新代码顺序已经调整），直接用这版新代码重新跑
  `bm_sd_selftest()`，看读写是否都能过；若还失败，请把完整日志贴出来（含
  `state`/`int`/`err`/`resp0`，以及是哪条命令报的）。

## 十一、全驱动一致性审计（应用户"重新梳理、别拆东墙补西墙"的要求）
把整条链路（引擎层 `bm1684xSdhci.c` → 核心层 `bm_sd_core.c` → 桥接/自检
`bm_sd_glue.c` → 配置头 `bm_sd.h` → OSAL `bm_sd_osal_os3.c`）逐文件读了一遍，
并把引擎层关键寄存器操作与本芯片已跑通的参考驱动 `bm_sd.c` 逐项核对。结论：
**多轮改动没有留下互相打架的地方，关键点都对得上**：
- 寄存器偏移/位定义与参考驱动 `bm_sd.h` 逐条一致（`ADMA_SA_LOW=0x58`、
  `HOST_VER4_ENABLE=bit12`、`CMD/DAT_INHIBIT=bit0/1`、`TRNS_*` 等，已 grep 比对）。
- 等忙位逻辑（除 CMD0 外都等 `CMD_INHIBIT|DAT_INHIBIT`）= 参考驱动
  `bm_sd_send_cmd_without_data()` 两段等待合起来的真实效果。
- V4 模式地址编程（写 `ADMA_SA_LOW/HIGH` + 块数写进复用后的 `DMA_ADDRESS` +
  `BLOCK_COUNT=0`）= 参考驱动 `bm_sd_prepare()`。
- `buildXferMode()` 单块也置 `TRNS_MULTI`、`AUTO_CMD12` 仅多块用——自检走的就是
  单扇区（`bm_sd_selftest()` 用 `1U`），这一路与参考驱动单块写法完全一致。
- 数据方向标志（READ/WRITE）在核心层与引擎层之间对应正确。
- 当前是纯轮询模式（OSAL 的 sem/irq 全 NULL，但 `udelay=delay_us` 有提供），
  所以 3.2s 等待超时是真实的、引擎层走的是轮询分支（中断分支当前是死代码）。
- 本轮唯一补的真实差异：参考驱动每次传输前都重选一次 SDMA 模式
  （`HOST_CONTROL` DMA 位），我们原来只在 `hwInit()` 选一次；已在
  `bm1684xSdhciSendCmd()` 的数据分支里按参考驱动补上（只读改写 DMA 选择位，
  不动总线位宽位）。

### 关于"屏蔽清状态寄存器后写就过了"这条线索的判断（重要）
**这不是真正的修复，是把写的失败给"假过"了，强烈建议保留清零、不要去掉。**
理由：发每条命令前清 `INT_STATUS`/`ERR_INT_STATUS`，参考驱动 `bm_sd.c` 和 u-boot
通用 `sdhci.c` 两份权威来源都这么做。把它屏蔽掉，等于让上一条命令的残留状态位漏
进下一条命令的等待循环——`pollWaitStatus()` 一看到残留的"完成位"就可能立刻返回
"成功"，于是写命令在数据其实还没真正搬完时就被判成功（写"假过"），紧接着读回来
自然对不上（数据没真写进去/读到旧值）。所以"屏蔽清零让写过了"恰恰反过来印证了
清零是对的：去掉它只是掩盖了写的真实失败，没有解决任何问题。

### 下一步怎么打破"读好写坏/写好读坏"的来回（给用户的明确请求）
代码已审计为自洽且忠于参考驱动，不宜再凭猜测加改动。请在保留清零的前提下，用当前
这版代码上板跑一次 `bm_sd_selftest()`，并把**完整串口日志整段贴回来**（不要只截
一行）——尤其是 `sdSendCmdDbg()` 为 WRITE 和 READ 两条打印的
`state/int/err/resp0/inhibit/clk` 各字段。有了"写在哪一步死、读在哪一步死"的并排
原始寄存器值，才能从"症状在命令间搬家"的循环里跳出来、定位到真正的硬件根因。
