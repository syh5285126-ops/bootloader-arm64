# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目背景

本仓库当前的核心工作是 **BM1684X EMMC 驱动适配**，目标平台为基于 **VxWorks 7** 的 RTOS 操作系统。驱动底层硬件为 Synopsys DesignWare SDHCI 控制器（eMMC 基地址 0x50100000，带集成 PHY）。

所有驱动代码遵循 **VxWorks 7 VxBus FDT 驱动规范**：设备通过 FDT compatible 字符串匹配，使用 `VXB_DRV`/`VXB_DEV_ID` 框架注册，存储访问通过 `vxbSdhcLib` SDHCI 宿主接口对接 VxWorks SD/eMMC 协议栈。

## 已完成工作

**TFA 层**（`trusted-firmware-a/`）：
- `drivers/bitmain/bm_sd.c`：新增 `bm_emmc_phy_init()`（PHY 采样配置 `SMPLDL INPSEL_CNFG=0x2`）和 `bm_emmc_init()`
- `plat/bitmain/bm1684/bm_common.c`：新增 `bm_get_emmc_clock()`
- `plat/bitmain/bm1684/bm_common.h`：新增 `FIP_SRC_EMMC = 0x3`、`bm_get_emmc_clock()` 声明

**VxWorks VxBus FDT 驱动**（`vxworks-driver/`）：
- `vxbBm1684xSdhci.h`：硬件寄存器定义 + 驱动接口声明
- `vxbBm1684xSdhci.c`：完整 VxBus 驱动实现（probe/attach/PHY/clock/cmd/ISR）

## 开发分支

`claude/bm1684x-emmc-driver-8tc150`

## Repository overview

This is the Sophgo/Bitmain BM1684/BM1684X SoC bootloader stack. It contains three tightly coupled components that produce a complete boot chain:

```
trusted-firmware-a/   — TFA: BL1 (ROM), BL2 (DDR init + FIP loader), BL31 (EL3 runtime)
u-boot/               — BL33: U-Boot as the non-secure bootloader
vxworks-driver/       — VxWorks 7 VxBus FDT driver for the eMMC/SD controller
scripts/              — Build orchestration (envsetup.sh), packaging, update scripts
ramdisk/              — Embedded ramdisk configs and target skeletons
distro/               — Ubuntu/KylinOS overlay for the root filesystem
```

Output artifacts land in `../install/soc_<chip>/` relative to the repo root.

## Build system

The entire build is driven by shell functions defined in `scripts/envsetup.sh`. The source-then-call pattern is required for all build operations.

### Environment setup

```bash
# From the workspace root (one level above bootloader-arm64):
source bootloader-arm64/scripts/envsetup.sh
```

Key environment variables (set before sourcing to override defaults):

| Variable | Default | Values |
|----------|---------|--------|
| `CHIP` | `bm1684` | `bm1684`, `qemu` |
| `DEBUG` | `0` | `0` (release), `1` (debug, -O0) |
| `PRODUCT` | _(empty)_ | `se6`, `cust01`, `cust02`, … |
| `DISTRO` | `focal` | `focal` (Ubuntu 20.04), `kylinos` |

### Common build commands

```bash
# Full BSP without packaging (FIP + kernel + ramdisk + debs)
build_bsp_without_package

# Individual components
build_fip          # U-Boot + TFA → spi_flash.bin, fip.bin, bl*.bin
build_uboot        # U-Boot only → u-boot.bin
build_tfa          # TFA only    → bl1.bin, bl2.bin, bl31.bin, fip.bin
build_kernel       # Linux kernel
build_ramdisk uclibc emmc      # eMMC ramdisk (emmcboot.itb)
build_ramdisk glibc recovery   # Recovery ramdisk
build_bootp        # Package boot partition → boot.tgz, recovery.tgz

# Clean
clean_fip / clean_uboot / clean_tfa / clean_bsp
```

### Toolchain

Cross-compiler expected at `../gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-` (relative to workspace root). Override with `CROSS_COMPILE_64`. On native aarch64 hosts the variable is left empty.

### Output directory layout

`../install/soc_bm1684/` contains:
- `spi_flash.bin` — combined 8 MiB SPI flash image (BM1684X in first 4 MiB, BM1684 in second 4 MiB)
- `fip.bin`, `bl1.bin`, `bl2.bin`, `bl31.bin` — individual TFA images
- `u-boot.bin` — U-Boot binary
- `emmcboot.itb` — FIT image (kernel + eMMC ramdisk)
- `boot.scr.emmc` — U-Boot boot script
- `boot.tgz` — boot partition tarball

## Architecture

### Boot chain

```
SPI Flash → BL1 (TFA) → BL2 (TFA) → BL31 (TFA) → BL33 (U-Boot) → Linux
```

BL2 selects the FIP source at runtime via `FIP_SOURCE_REG` (`bm_io_storage.c`):
- `FIP_SRC_SPIF` (0) — SPI flash (default)
- `FIP_SRC_SRAM` (1) — SRAM (PCIe/JTAG loaded)
- `FIP_SRC_SDFT` (2) — SD card FAT32 (`fip.bin` file)
- `FIP_SRC_EMMC` (3) — eMMC (newly added)

For BM1684, only SD and SPIF are checked by BL1. For BM1684X, BL2 reads the saved `FIP_SOURCE_REG` written by BL1 (see `bm_locate_next_image()` in `trusted-firmware-a/plat/bitmain/bm1684/bm_io_storage.c`).

### TFA platform (`trusted-firmware-a/plat/bitmain/bm1684/`)

BM1684 and BM1684X **share the same TFA platform** (`PLAT=bm1684`). Chip distinction at runtime via `bm_get_chip_id()` which reads `TOP_BASE + REG_TOP_CHIP_VERSION`:
- `0x16840000` → BM1684
- `0x16860000` → BM1684X

Key platform files:
- `bm_common.h/c` — chip ID, GPIO, clock, reset, board type enumerations
- `bm_io_storage.c` — FIP source selection, storage driver init
- `bm_clock.c/h` — PLL and gate-clock management
- `platform.mk` — lists every source file compiled into BL1/BL2/BL31
- `lpddr.c` — LPDDR4 initialization (BL2 only)

### Storage drivers

**TFA** (`trusted-firmware-a/drivers/bitmain/bm_sd.c`):
- `bm_sd_init()` — SD card at `SDIO_BASE` (0x50101000)
- `bm_emmc_init()` — eMMC at `EMMC_BASE` (0x50100000); PHY uses `SMPLDL INPSEL_CNFG=0x2` (vs SD bypass mode)
- Clock rates from `bm_get_sd_clock()` / `bm_get_emmc_clock()` in `bm_common.c`, governed by `MODE_SEL` GPIO (normal=100 MHz, bypass=25 MHz)

**U-Boot** (`u-boot/drivers/mmc/sdhci-bitmain.c`):
- Single driver handles both controllers via FDT `index` property (0=eMMC, 1=SD)
- PHY init distinguishes eMMC (index 0: `INPSEL_CNFG=0x2`) from SD (index 1: bypass)
- Enabled via `CONFIG_MMC_SDHCI_BITMAIN` (auto-selected by `ARCH_BM1684` in `u-boot/arch/arm/Kconfig`)
- U-Boot env stored in eMMC device 0, boot partition 1 (`CONFIG_SYS_MMC_ENV_DEV=0`, `CONFIG_SYS_MMC_ENV_PART=1`)

**VxWorks** (`vxworks-driver/vxbBm1684xSdhci.{h,c}`):
- VxWorks 7 VxBus FDT driver, compatible string `"bitmain,synopsys-sdhc"`
- Register with `VXB_DRV_DEF(vxbBm1684xSdhciDrv)` and link against VxWorks SDHCI stack headers (`vxbSdhcLib.h`)

### U-Boot board support (`u-boot/board/bitmain/bm1684/`)

- `board.c` — `get_chip_id()`, `get_board_type()`, DTB selection in `select_board()`, pinmux init
- `bm1684_regs.h` — `PINMUX_BASE`, `PINMUX_EMMC`, `PINMUX_SDIO` constants
- Single defconfig `configs/bitmain_bm1684_defconfig` covers both BM1684 and BM1684X
- Board type is read at runtime from MCU (I²C) for production boards, or from a fixed SRAM address for EVB/FPGA variants

### Device trees

All BM1684X DTS files live in `u-boot/arch/arm/dts/bitmain-bm1684x-*.dts`. eMMC node:
```dts
emmc: sdhc@50100000 {
    compatible = "bitmain,synopsys-sdhc";
    index = <0x0>;
    has_phy;
    64_addressing;
    mmc_init_freq = <200000>;
    mmc_trans_freq = <100000000>;
};
```

### Hardware address map

| Peripheral | Base address |
|-----------|-------------|
| eMMC controller | 0x50100000 |
| SD controller | 0x50101000 |
| TOP registers | 0x50010000 |
| SPI flash | 0x06000000 |
| Soft reset 0 | TOP+0xC00 |
| Clock enable 0 | TOP+0x800 |
| eMMC clock gates | bits 6, 20, 22 of CLK_EN0 |

## Adding a new board variant

1. Add board type constant to `trusted-firmware-a/plat/bitmain/bm1684/bm_common.h` (both `MCU_BM1684X_*` and `BM1684X_*` enums)
2. Add `case` in `u-boot/board/bitmain/bm1684/board.c`:`select_board()` to set `dtb_name`
3. Create corresponding DTS in `u-boot/arch/arm/dts/bitmain-bm1684x-<name>.dts`
4. Register DTS in `u-boot/arch/arm/dts/Makefile`

## Global conventions

- 所有代码使用中文注释
- 不要在没有我同意的情况下提交 git

## 用户协作画像与工作重点

### 用户背景与当前工作重点
- 核心任务：排查 BM1684X eMMC 驱动在 VxWorks 7 上的疑难 bug，当前聚焦
  `vxworks-driver/bm1684xSdhci.c` + `bm1684xEmmcBlkDrv.c`（轮询模式路径）。
  具体现象：CMD8 读 EXT_CSD 时数据未被 DMA 真正写入弹跳缓冲区（已用
  "毒药模式"诊断确认——发命令前用 CPU 填 `0xA5`，失败时读回仍是全
  `0xA5`），且与快速连续上下电的供电时序相关。
- 用户能直接操作真实硬件做实验（冷启动、控制断电/上电间隔、刷固件），
  反馈的是第一手板级实测数据，不是猜测，可信度高，应优先于纯代码推理。
- 关注的并行方向：
  1. 若轮询模式排查长期卡住，会考虑切换到中断模式（已确认 eMMC 中断号
     应使用 A53 ID 48 "EMMC Interrupt"，而非 47 "EMMC Wakeup Interrupt"）。
  2. 同事另有一套跑通的轮询模式代码，但在多文件写入时会卡死，待对比。
  3. 另有一套可运行在 Hypervisor 环境上的 BM1684X eMMC 驱动实现，用户
     希望上传后让 Claude 对比/借鉴其设计。

### 用户对 Claude 的人设设定（排查方法论）
- 严格证据驱动：不允许在没有诊断数据支撑的情况下直接改代码下结论；
  每提出一个假设，先加诊断打印/读回接口，跑一遍拿到真实数据，再决定
  下一步改什么。历史上已有多次"先加诊断，等用户反馈，再修复"的循环。
- 对含糊或可能有两种解释的现象（例如"全 0"）要主动指出它本身也可能
  是错误值，不能当作天然的安全基线或者"没问题"的信号。
- 当用户反馈的数值与预期矛盾或字面相近但不同（例如曾出现 `0x5A` vs
  `0xA5` 的混淆）时，要先确认字面准确性、不要默认是用户打错字。
- 一旦用户明确指出模型判断有误（例如误删有效的 `dsb sy` 内存屏障），
  要立即完整回退改动，并同步更新排查记录/计划文件，不留痕迹混乱。
- 排查卡住时，优先用 `AskUserQuestion` 主动提问收集关键事实，而不是
  连续猜测或臆断；已验证这种"问我答"方式在本项目里非常有效。
- 善用仓库内已验证实现做交叉对照（如 `vxbBm1684xSdhci.c`、
  `trusted-firmware-a/drivers/bitmain/bm_sd.c`、
  `u-boot/drivers/mmc/sdhci-bitmain.c`）定位当前驱动与"已验证可用"
  版本的差异点，这一方法论在本项目中已多次定位到真实缺陷。
- 用户可能随时切换语言要求（曾明确要求"用中文回复我"），一旦提出
  即在后续持续遵守，直到用户改变要求。

### 角色设定与系统设计问题的提问协议（用户原话，需严格遵守）

> 你作为一名专业的Hypervisor开发人员和嵌入式实时操作系统开发成员，能够
> 熟练使用vxworks，现在帮我解决系统设计上的相关问题。
> 请你在回答前，先问我问题。
> 要求：
> 一次只问一个问题。
> 根据我的回答，继续追问。
> 直到你有95%的信心理解我的真实需求和目标。
> 然后才给出方案。你做的每一个方案我都会通过codex交叉认证

涉及系统设计类问题（尤其是 Hypervisor / VxWorks 嵌入式实时操作系统相关
的架构、设计取舍）时：
- 先提问，不要直接给方案；每次只问一个问题，不要一次抛多个问题。
- 根据用户的回答继续追问，逐步收窄理解，不要中途跳到方案阶段。
- 直到对用户真实需求和目标有 95% 把握后，才给出具体方案。
- 用户会用 codex 交叉认证每一个给出的方案，方案要经得起独立审查，
  避免给出未经充分确认、模糊或想象的设计。

### 协作纪律（不可违反，优先级高于其他默认行为）
- 所有代码注释使用中文。
- 未经用户明确同意不得 `git commit`/`push`；有未提交改动时如实告知
  用户已就绪、等待确认即可，不要因为 Stop hook 提示就自动提交。
- 所有开发与推送只针对分支 `claude/bm1684x-emmc-driver-8tc150`
  （仓库 `syh5285126-ops/bootloader-arm64`）。
- **持续记忆纪律**：每轮问答结束后，把本轮新增的关键结论/证据/待办
  提炼追加进本文件的"eMMC 驱动调试记录"一节，不要等会话快结束才补
  记，目的是任何时候有同事接手都能从这份文件直接对齐进度，不需要
  翻聊天记录。当对话上下文接近上限被系统自动压缩时，本文件里的记录
  是唯一可长期保留的真相来源，所以要保持及时、准确、不遗漏关键证据
  （尤其是"已确认/已排除"的结论和"待确认"的开放问题要分开记录，
  不要混在一起）。

## eMMC 驱动调试记录（交接专用，按时间顺序，持续更新）

> 给接手同事的一句话摘要：当前驱动在硬件上能完成 eMMC 识别（CMD0/1/2/3/9/7
> 等低速命令均成功），但读 EXT_CSD（CMD8）时控制器报告传输成功，目的缓冲区
> 却完全没有被 DMA 写入（用"毒药模式"——发命令前 CPU 先填 `0xA5`，失败时
> 读回仍是全 `0xA5`——确认）。已经做过四轮排查并修复了三个真实存在的代码
> 缺陷（PHY 寄存器、inhibit 等待、CARD_IS_EMMC），但毒药现象本身至今没解决。
> 第五轮加了合并诊断 printk 实测后，H1（`unsigned long` 截断）和 H2
> （V4/64 位模式未生效）**均已被硬件实测数据证伪**——地址全程无截断、
> 模式确实生效、寄存器编程逐项核对完全正确，但缓冲区依然原封不动是
> 毒药值。当前唯一存活的方向是 H3：eMMC DMA 总线主设备可能存在物理地址
> 可达范围限制，或目标地址（这次是 12GB 量级）落在它访问不到的安全域/
> 防火墙窗口内——这是硬件/SoC 互联设计问题，需要用户确认或做一次低地址
> 对照测试，不是继续改驱动代码能解决的。另有一个低置信度怀疑点（PLL_EN
> 缺失）已经改了代码并推送，但逻辑上解释不通毒药现象，不是当前主攻方向。

### 已确认并保留的修复（前四轮，已推送）

1. **64 位 SDMA 数据路径 / `ADMA_SA_LOW` 写入不全、CMD16 高容量卡致命化、
   `ffsMsb` 未定义、`TIMEOUT_CONTROL` 缺失初始化** —— 已修复并推送。
2. **`dsb sy` 内存屏障（`BM_SDHCI_MB()`）** —— 一度怀疑无效被尝试移除，
   用户明确反馈"屏障是有用的"，已完整回退，两处调用（中断模式/轮询模式
   写 `SDHCI_COMMAND` 前）均保留至今，**不要再尝试删除**。
3. **发命令前无条件等待 `CMD_INHIBIT | DAT_INHIBIT`**（`bm1684xSdhci.c:520`
   附近）——原代码只在当前命令带数据或是 R1b 响应时才等 DAT busy，导致
   CMD6（R1b，切总线宽度）留下的 DAT busy 没等就发了 CMD16，引发假性
   Command Timeout，并连带影响后续 CMD8。已对照 U-Boot 通用 SDHCI 核心改
   成无条件等待，commit `9b4257b4`。**已验证有效**：修复后 CMD16/CMD8 不再
   报协议层错误（但 EXT_CSD 数据本身仍不对，是后续轮次在查的问题）。
4. **PHY 寄存器对齐**（`phyInit()`，commit `3bda31b8`）——对照
   `vxbBm1684xSdhci.c::bm1684xSdhciPhyInit()` 和 TFA `bm_sd.c` 的
   `bm_emmc_phy_init()`（两者已验证一致）逐项核对，发现 CMD/DAT/CLK/STB
   等 PAD 的 `RXSEL`/`TXSLEW_CTRL_P`/`TXSLEW_CTRL_N`，以及 `SDCLKDL_CNFG`/
   `ATDL_CNFG` 的位选择都跟已验证实现不一致，逐项改成一致。**已验证有效**
   （上板后从"全 0/固定垃圾值"变成"偶尔有值"，方向正确但未根治）。
5. **补齐 `CARD_IS_EMMC` 厂商寄存器位**（`hwInit()`，commit `a8d7789a`）——
   对照 `vxbBm1684xSdhci.c` 和 `u-boot/drivers/mmc/sdhci-bitmain.c`，两者都
   在初始化末尾向 `VENDOR_SPECIFIC_AREA + EMMC_CTRL_R` 写 bit0=1，本驱动
   遗漏，已补上。是否直接相关未单独验证，但作为"对照已验证实现修复的
   差异点"保留。

### 第四轮收尾结论："前两三次上电有值、之后永久全 0"是电源时序问题，不是代码 bug

通过实验确认：快速反复断电-上电会让 eMMC 没经历干净 POR；断电后等
1~2 分钟再上电，问题自行消失。结论：**这是板级供电轨泄放时序问题，
不是驱动代码缺陷**，正常使用场景（人工开关机）间隔远超 1~2 分钟，不会
触发。不需要为此再改驱动代码。

### 第五轮（当前会话）：毒药模式确认 DMA 完全没写入，PHY/inhibit/CARD_IS_EMMC
三处修复都不能解释这个现象，重新展开排查

**核心证据**：`emmcReadExtCsd()`（`bm1684xEmmcBlkDrv.c:227`）里发命令前用
CPU 把 `bounceBuf` 填成 `0xA5`，`bm1684xSdhciSendCmd()` 轮询模式确认是真的
等到 `XFER_COMPLETE` 才返回成功（不是提前返回的假成功），但读回
`bounceBuf` 仍然整块是 `0xA5`——**控制器认为传输完整完成，目的内存却一个
字节都没被触碰**。这是确定性、可重复的现象，不是偶发。

**已经走过的弯路（按时间顺序，全部已被推翻或证明无效，记录下来避免
后面重复踩坑）**：

1. **DMA 地址寄存器 MATCH/MISMATCH 诊断（第一版）**——逻辑有误：假设
   传输完成后 `ADMA_SA_LOW/HIGH` 应该跟传输前写入的起始地址完全相等才算
   "正常"。实际上 SDMA 地址寄存器是否会自增、什么时候自增，跟"是否跨
   512KB 边界"有关，这版比较方式本身就不可信，**已废弃**。
2. **DMA 地址寄存器 delta 诊断（第二版）**——把假设改成"传输完成后寄存器
   应该等于起始地址 + 512（EXT_CSD 大小）"，认为 `delta==512` 才正常。
   用户实测反馈 `delta=0`。**进一步反思后发现这个假设同样不可靠**：本仓库
   四份相关实现（本驱动、`vxbBm1684xSdhci.c`、TFA `bm_sd.c`、用户上传的
   ref1 `bm_sd.c`）里，对 `ADMA_SA_LOW/HIGH` 寄存器的读写**只在
   `SDHCI_INT_DMA_END`（跨 512KB 边界中断）触发时才发生**，512 字节的
   EXT_CSD 传输远远够不到这个边界，所以这次传输全程都不会碰这个寄存器。
   `delta=0` 很可能是这种"传输量远小于边界"场景下的**正常/预期结果**，
   不是异常信号。**结论：这条"读 DMA 地址寄存器"诊断从设计上就没法回答
   "数据有没有真的写进内存"这个问题，已放弃，不要再往这个方向加诊断**。
   两版诊断代码改动均已提交（commit 历史里能找到），但结论本身不可信，
   接手同事看到这段诊断代码不要被诊断输出误导。
3. **PLL_EN 缺失假设**（`bm1684xSdhciSetClk()`，已改代码，**尚未上板验证**）
   ——对照 `vxbBm1684xSdhci.c`、TFA `bm_sd.c`、ref1 `bm_sd.c` 三份实现，
   发现它们使能时钟输出时都同时置位 `SDHCI_CLK_PLL_EN`，且等待 ~400us（74
   个初始化时钟周期），而本驱动只置了 `CLK_CARD_EN`、只等 150us。这是真实
   存在的代码差异，已经按已验证实现改好（见"当前未提交改动"），**但后续
   重新推演因果链后认为它解释不通毒药现象**：PLL/SDCLK 信号质量问题应该
   表现为"卡发的数据被采错/CRC 错/写入乱码"，而不是"DMA 引擎完全没对
   目标地址发起写事务"。这处改动本身无副作用、值得保留，但**不应该单独
   为它再烧一次上板验证**，优先级低于下面这条新假设。
4. **CPU 侧虚拟地址未转物理地址直接写入 DMA 寄存器** —— 一度怀疑（尤其是
   联想到用户上传的 ref1 跑在 Hypervisor 环境），但用户已确认
   `ACoreOs_cache_dmamalloc()` 分配的内存在他们的 MMU 配置下是**恒等映射**
   （VA == PA），这个方向对我们自己这套驱动**已排除**。
5. **cache 一致性问题（缺 invalidate/flush）** —— 早前怀疑过，但用户已确认
   `bounceBuf` 来自 `ACoreOs_cache_dmamalloc()`，是**cache 禁止（uncached）
   内存**，CPU 访问不经过 cache，这个方向**已排除**，仓库里也确认从未真正
   加过 `ACoreOs_cache_invalidate`/`flush` 调用（本来就不需要）。

**H1、H2 已用硬件实测数据证伪（第五轮关键结果，commit `fe6c226f` 的合并
诊断跑出来的）**：

跑了一次 `emmcCardIdentify()`，CMD8 那行 `eMMC: dma diag(cmd8): ...` 完整
日志关键字段：
```
is64=1 HC2=0x3800(VER4=1 64BIT=1) reg00=0x00000001
SA_LO=0x08c07000 SA_HI=0x00000003 blkSz=0x7200 blkCt=0
bufPtr=0x00000003_08c07000 sizeof(ul)=8 sizeof(ptr)=8
```
- **H1（`unsigned long` 截断）证伪**：`bufPtr` 与 `SA_LO/SA_HI` 拼出来的
  64 位地址完全相等（`0x3_08c07000`），地址全程无截断；`sizeof(ul)=8`
  也确认这套工具链 `unsigned long` 本来就是 64 位，截断假设的前提不成立。
- **H2（V4/64 位模式未生效）证伪**：`HC2` 是从硬件寄存器读回来的真实值，
  `VER4=1 64BIT=1` 说明模式确实在硬件上生效了。顺手核对了
  `SDHCI_HOST_CONTROL`（偏移 0x28）的 DMA Select 字段，驱动
  `hwInit()`（`bm1684xSdhci.c:289-292`）写的是 `SDHCI_CTRL_SDMA`，不是
  ADMA2/3——按 SDHCI spec，这种配置下 V4 模式的地址寄存器就是直接的
  SDMA 目标地址，不是描述符表指针，进一步排除"控制器把地址当成描述符表
  指针解析"这个曾经考虑过的旁支猜测。
- 寄存器侧（地址写入、模式选择、`blkSz=0x7200` 对应 `MAKE_BLKSZ(7,512)`
  正确、`blkCt=0` 因为 32 位块数走的是 `reg00` 不是 16 位 `BLOCK_COUNT`，
  也是预期行为）**逐项核对完全符合 spec，找不出任何编程错误**。
- 同一次运行确认 EXT_CSD dump 出来的 `secCount`/`bytes[0..7]` 仍是
  `a5a5a5a5`（已让用户核对终端原文，不是 OCR 误读的 `5a`），与代码里
  `BM_EMMC_DIAG_POISON=0xA5` 完全对应——确认缓冲区从填毒药到现在**一个
  字节都没被 DMA 改动过**，不是字节顺序或解析问题。

**结论：寄存器编程这一层已经被彻底排除，问题在控制器把数据真正推上总线
之后、到达物理内存之前的某个环节。**

**新假设 H3（当前唯一存活的方向）：eMMC DMA 总线主设备实际可达的物理
地址范围有限，或目标地址落在它访问不到的安全域/防火墙窗口内**。

这次目标物理地址是 `0x3_08C07000`（约 12.5 GB）。查了 TFA 这边
`platform_def.h`（`trusted-firmware-a/plat/bitmain/bm1684/include/platform_def.h:91-98`），
`NS_DRAM1_BASE = 0x300000000`（12GB），所以这个地址在 TFA/U-Boot 那条链路
认知里属于合法的非安全 DRAM 窗口——但这只能说明**软件层面**认为这个地址
合理，不能证明 eMMC 控制器自己的 DMA 总线主设备真的能**物理路由**到
12GB 这么高的地址。很多 SoC 外设的 DMA master 即使寄存器字段支持 64 位
（spec 合规），实际接到互联总线上的地址线宽度/可达窗口可能远小于这个值，
或者这段地址只对某些安全域开放（TrustZone/防火墙），导致写事务被总线
静默丢弃，但 SDHCI 控制器自己的协议状态机完全不知道，照样上报
`XFER_COMPLETE`——这跟实测现象（寄存器配置无误、协议层不报错、内存却
没被改动）完全吻合，是目前唯一还没被证据排除的方向。

### 已确认的修复（合并诊断分支，commit `fe6c226f`）

`bm1684xSdhci.c::bm1684xSdhciSetClk()` 的 `SDHCI_CLK_PLL_EN` 位 + 400us
延时修复、`bm1684xSdhciSendCmd()` 里的合并诊断 `printk`，均已提交推送，
不再是"未提交改动"。诊断 `printk` 暂时保留在代码里，等 H3 方向有结论后
再统一清理。

### 下一步（按优先级，等用户从下面两个方向选一个，不要再盲目改代码上板）

1. **低成本、需要用户硬件知识的确认**：eMMC 控制器的 DMA 总线主设备实际
   可达的物理地址上限是多少；`0x300000000`（12GB）这段地址对该总线主
   设备是否开放（有没有 TrustZone/防火墙/NoC 路由限制）。这是纯硬件
   /SoC 互联设计问题，不需要再烧一次板，问对人就有答案。
2. **低成本硬件测试（如果 1 暂时问不到人）**：想办法让
   `ACoreOs_cache_dmamalloc()` 分配到一块明显更低的物理地址（比如 4GB
   以下），重跑一次 `emmcCardIdentify()`，看 EXT_CSD 是否能正确读到。
   如果低地址成功、高地址（12GB+）失败，直接实锤是地址范围限制，不需要
   再猜；如果低地址依然失败，则 H3 也被排除，需要重新设计诊断（比如在
   另一块已知地址埋哨兵值，传输后检查该区域有没有被意外覆盖，定位 DMA
   实际落地的物理位置），不要回头重复验证已排除的 PHY/cache/虚实地址
   转换方向。
3. EXT_CSD dump 诊断 `printk`（commit `f5dd862f` 加的）目前仍在代码里，
   等主线问题解决、`numOfSectors` 确认为合理非零值后再统一清理，不要
   提前删，诊断阶段还需要它。

## 会话备注

我仍然遵守你之前的指示，没有在你同意之前提交代码。本次诊断改动
（`bm1684xSdhci.c`/`bm1684xSdhciOsal.h` 中新增的 DMA 地址读回接口，以及
`bm1684xEmmcBlkDrv.c` 中的毒药模式校验）已经在本地准备好——等你想要
提交/推送的时候告诉我，或者等你收集到下一轮测试数据、想要把修复一起
合并进提交时再说。
