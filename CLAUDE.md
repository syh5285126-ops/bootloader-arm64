# bootloader-arm64

Bootloader and firmware distribution for Sophgo/Bitmain ARM64 SoCs (BM1684,
BM1684X) plus the RISC-V coprocessor board "mango". Builds the full boot
chain (TF-A → U-Boot → kernel → ramdisk/rootfs) and packages it into
flashable/SD-card images, and also carries a standalone VxWorks eMMC/SD
driver for BM1684X.

## Repo layout

| Path | Role |
|---|---|
| `trusted-firmware-a/` | ARM TF-A (BL1/BL2/BL31) with Bitmain/Sophgo platform port under `plat/` and low-level eMMC/clock drivers under `drivers/bitmain/` (e.g. `bm_sd.c`). Vendored upstream tree — only the platform port is project-specific. |
| `u-boot/` | U-Boot with board ports under `board/bitmain/bm1684/` (`board.c`, `bm1684_regs.h`, `mmio.h`) and `board/sophgo/`. Defconfigs: `bitmain_bm1684_defconfig`, `bitmain_qemu_defconfig`, `bitmain_antminer_s9_defconfig`, `sophgo_mango_defconfig`. Vendored upstream tree (contains unrelated upstream board ports too, e.g. generic Rockchip boards — ignore those, they are not part of this project). |
| `vxworks-driver/` | Standalone VxWorks 7 SDHCI/eMMC driver for BM1684X (see below). Not built by the scripts in `scripts/` — delivered as source for integration into a separate VxWorks BSP/IDE. |
| `scripts/` | Build orchestration. `envsetup.sh` is the entry point exposing all `build_*`/`clean_*` shell functions. Also: `bm_make_package.sh` (packaging), `local_update.sh`/`ota_update.sh` (on-device update), `gen_spi_flash.c`/`mb2h`/`mk-gpt` (image/partition tooling), `bsp-images/`, `buildroot-2019.08.2/`, `buildroot-2021.02.7/` (vendored buildroot trees for ramdisk variants). |
| `distro/` | Rootfs/package customization: `sophgo-fs/` (core Debian package skeleton), `product-se/` (SE-product Debian package), `overlay/{common,bm1684,se6,cust01,cust02,athena2,mixmode_nfs,minimum}/` (per-product/per-mode rootfs file overlays), `debs/` (drop-in dir for prebuilt `.deb`s, e.g. libsophon/ffmpeg/opencv). |
| `ramdisk/` | Initramfs build inputs: `target/` (uclibc/glibc/mini variants), `configs/` (boot scripts, device trees, per-mode overlays for emmc/pcie/recovery/mix_nfs). |
| `docs/` | Sphinx RST docs, mirrored in Chinese (`bm1684x/`) and English (`bm1684x-en/`), covering hardware/software architecture; built via `build_doc`. |

## Build workflow

Toolchain/base distro come from external `dfss` packages (see `README`), placed
as siblings of `bootloader-arm64`:
```
.
├── bootloader-arm64
├── distro/distro_focal.tgz
├── gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu
└── linux-arm64        (separate kernel repo, not in this repo)
```

Everything is driven by sourcing `scripts/envsetup.sh`, which sets up
`CHIP` (`bm1684`|`mango`|`qemu`), `VENDOR` (`bitmain`|`sophgo`), `DISTRO`
(`focal`|`kylinos`), `PRODUCT`, `KERNEL_VARIANT`, `DEBUG`, etc., then exposes
build functions. Typical end-to-end build:

```bash
source bootloader-arm64/scripts/envsetup.sh
build_bsp_without_package   # TFA + U-Boot + kernel + ramdisk, no packaging
# copy prebuilt libsophon/ffmpeg/opencv .debs into soc_<chip>/bsp-debs
build_package                # produce SD-card/TFTP update images
```

Other notable functions in `envsetup.sh`: `build_tfa`, `build_uboot`,
`build_fip` (TFA+U-Boot → BL1/BL2/FIP), `build_kernel`, `build_ramdisk`,
`build_rootp`/`build_rootp_ram` (rootfs from distro tarball + DEB install),
`build_sdimage`, `build_update`, `build_mango_bsp`/`build_mango_flash`
(RISC-V mango target), `run_qemu`, `build_doc`. Every `build_*` has a
matching `clean_*`. Output artifacts land under `install/soc_<chip>/`.

## VxWorks eMMC/SD driver (`vxworks-driver/`)

A self-contained Synopsys DesignWare SDHCI driver for the BM1684X eMMC/SD
controller, written **without depending on VxWorks SDK headers** so it can
be reviewed/modified outside a VxWorks IDE:

- `bm1684xSdhciHw.h` — register layout (controller base `0x50100000`
  eMMC / `0x50101000` SD, TOP clock/reset regs at `0x50010000`), Synopsys
  PHY block at offset `0x300`, Bitmain vendor block at offset `0x500`.
- `bm1684xSdhciOsal.h` — OS abstraction callbacks (clock, IRQ, MMIO mapping,
  semaphores) that the actual VxBus integration must supply.
- `bm1684xSdhci.c` — core driver: 14-step Synopsys PHY init sequence,
  command/data transfer pipeline, supports both interrupt-driven and
  polling operation.
- `vxbBm1684xSdhci.c/.h` — VxBus glue (FDT-bound, compatible string
  `"bitmain,synopsys-sdhc"`) that wires the core driver into VxWorks 7's
  VxBus framework; this is the layer that actually needs the real VxWorks
  SDK headers (`vxWorks.h`, `vxBus.h`, `vxbFdtLib.h`, `vxbSdhcLib.h`, ...).

This driver is delivered as source only — it is not built by anything under
`scripts/`; integration/compilation happens in the target VxWorks BSP.
TF-A's own low-level eMMC/PHY init lives separately in
`trusted-firmware-a/drivers/bitmain/bm_sd.c` and is independent of this
VxWorks driver (different boot stage, same hardware).

## Conventions

- Coding style is checked against `.checkpatch.conf` (Linux kernel
  checkpatch, max line length 120, several TFA/U-Boot-specific rules
  relaxed — see the file for the exact ignore list). Run
  `checkpatch.pl --no-signoff` against patches before submitting.
- Commit messages often use a `[vXX.XX.XX]:` version-tag prefix for
  SDK/release bumps, otherwise plain imperative summaries (`Add ...`,
  `Update ...`, `Fix ...`, `support ...`).
- `trusted-firmware-a/` and `u-boot/` are vendored upstream trees — keep
  changes scoped to the Bitmain/Sophgo platform/board ports
  (`plat/.../bitmain*`, `drivers/bitmain/`, `board/bitmain/`,
  `board/sophgo/`, `configs/bitmain_*`, `configs/sophgo_*`) rather than
  touching shared upstream code.
- No CI config or test suite is present in this repo; validation is via
  building images and flashing/booting real hardware (or `run_qemu` for the
  `qemu` chip target).
