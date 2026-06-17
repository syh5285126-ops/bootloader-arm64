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

## 会话备注

我仍然遵守你之前的指示，没有在你同意之前提交代码。本次诊断改动
（`bm1684xSdhci.c`/`bm1684xSdhciOsal.h` 中新增的 DMA 地址读回接口，以及
`bm1684xEmmcBlkDrv.c` 中的毒药模式校验）已经在本地准备好——等你想要
提交/推送的时候告诉我，或者等你收集到下一轮测试数据、想要把修复一起
合并进提交时再说。
