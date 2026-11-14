# xnuL - xnu loader for angelica

**Apple's iOS 16 kernel and userland on the Redmi 9C (angelica).**

xnuL is the bare-metal xnu loader for angelica (formerly called `shim`). It takes over
from MediaTek LK, sets up its own MMU and display, loads the XNU
kernel together with a generated Apple Device Tree, and jumps into XNU.
On top of the kernel runs the **unmodified iOS 16.7.16 (20H392) userland of the
iPhone X**: launchd, the system daemons, backboardd, SpringBoard and the stock
apps.

The single target is the **Redmi 9C** (`angelica`, MediaTek MT6765 / Helio G35,
8× Cortex-A53). The kernel patch base and tooling were ported from the upstream
`dipa-studio/ios-angelica` reference; this tree does not build for or support
any other phone.

> Status as of **2026-10-21**. This is an experimental bring-up, not a usable
> phone OS. Apple's security model is mostly switched off.

## Where the Redmi 9C port stands

The loader side works and the kernel now survives the MMU handoff. XNU boots
into `arm_init` and runs early C code (the framebuffer shows the `ANGELICA EARLY`
markers 1–4), then stops at the next missing CPU feature. The iOS userland has
**not** been run on `angelica` yet.

| Area | State on `angelica` |
| --- | --- |
| Bootloader / flashing | Works: BROM → preloader → stock unlocked MediaTek LK → `fastboot flash boot` of the xnuL image. `fastboot boot` is unreliable on this LK variant; always flash. |
| xnuL | Works: EL3→EL1 drop, own page tables, display takeover (OVL/RDMA), live-FDT parsing, payload loading (Mach-O segments + ADT + boot_args), framebuffer console, watchdog disabled |
| Console | Visible on the phone panel (720×1600, fb `0x5e605000`); there is no usable serial UART |
| XNU kernel build | Works: 4K hardware page tables, 16K software pages (iOS ABI), ARMv8.0 code generation, classic non-large-memory virtual layout |
| XNU early boot | Reaches `arm_init` and markers 1–4 (`arm_init`, platform expert, topology, CPU bootstrap) |
| Barriers already fixed | 16K-page kernel on an A53 (the core has no 16K granule); TTBR1 walk from L0 with `T1SZ=23` under the 4K bootstrap tables; ARMv8.1 LSE atomics emitted by the Apple toolchain |
| Current blocker | LSE codegen (`0088`) and the PFZ assembly LL/SC fallback (`0089`) are fixed and built; the image is flashed and XNU now panics in `kmem_range_init` (`vm_kern.c:3645`) while locating kernel-map claims |
| iOS userland | Not started on `angelica` (no root filesystem flashed, no launchd/SpringBoard) |
| Storage root | MSDC/eMMC root driver exists (`0083`, polled single-block reads, untested on hardware) |
| Display/UI/touch/keys | Not implemented for `angelica` yet; the iOS graphics stack is the upstream project's |

The GUI stack is CPU-rendered and still under bring-up; this is not a usable
phone OS. Code signing, sandbox and data protection are stubbed. **Do not put
personal data on it.**

## How xnuL boots the kernel

```
BootROM (BROM)
  -> MediaTek preloader
     -> LK (stock Xiaomi bootloader, unlocked)
        -> fastboot flash boot xnul-boot.img   (roll back with the stock boot.img)
           -> xnuL: EL drop, own MMU, display takeover, live FDT
              -> payload (kernel Mach-O segments + ADT + boot-args)
                 -> XNU
```

The boot image is an Android `boot.img` v2: the gzip xnuL `Image` is the
"kernel" and the xnuL payload is the "ramdisk". On `angelica` the image uses
page size 2048, kernel address `0x40080000`, DTB/tags `0x47880000` and the
stock cmdline (`bootopt=64S3,32N2,64N2 buildvariant=user`). The kernel payload
uses `phys_base=0x60000000`, `mem_size=0x60000000` (1.5 GiB) and boots with
`rd=msdcroot0 keepsyms=1 -enable_kprintf_spam debug=0x14c`.

## Building

### Kernel (GitHub Actions)

The kernel builds on GitHub's macOS runners with
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build):
xnu-8796.141.3 is fetched, `xnu/patches/` is applied in order, and the pinned
open-source libpthread/HFS/BearSSL sources are staged. A push that touches
`xnu/patches/**` on the `angelica` branch builds `DEVELOPMENT`/`ANGELICA`
automatically; a manual run needs `machine_config=ANGELICA`.

```sh
gh workflow run xnu.yml -R YOU/nexredmi-xnul -f machine_config=ANGELICA
gh run download RUN_ID -R YOU/nexredmi-xnul -n xnu-kernel -D xnu/out
```

Artifacts: `xnu-kernel` (`kernel.development.angelica`, the dSYM/link info and
`SHA256SUMS`) and `xnu-buildtree`.

### Boot image

Needs Python 3, `mkbootimg`, and for xnuL itself `clang` + `ld.lld` (the host
build script targets `aarch64-linux-gnu` and packs the image with `mkbootimg`).

```sh
python3 tools/mkadt.py xnul/out/angelica.adt --angelica
python3 tools/mkpayload.py xnu/out/kernel.development.angelica \
    xnul/out/angelica.adt xnul/out/payload.bin \
    --cmdline 'rd=msdcroot0 keepsyms=1 -enable_kprintf_spam debug=0x14c'
cd xnul && OUT=out/xnul-boot-angelica.img ./build.sh ../xnul/out/payload.bin
```

The builder checks that the ELF has no absolute relocations, converts it to a
raw arm64 `Image`, gzips it, appends the DTB (stock `dtb` by default, override
with `DTB=...`), and verifies the unpacked image round-trips.

### Flash

```sh
fastboot flash boot xnul/out/xnul-boot-angelica.img
fastboot reboot
```

Never use `fastboot boot` on this device. Keep the stock `boot.img` around for
rollback. Flashing `boot` does not touch `userdata`; **no iOS root image has
been flashed on `angelica` yet**.

### Diagnostics

There is no serial console. xnuL draws on the panel from the first instruction,
and the kernel console uses the same framebuffer (`0084`); panics are printed
there. The display shows the early markers, so a photo of the screen is the
primary log.

## Repository layout

| Path | What |
| --- | --- |
| `xnul/` | xnuL, the bare-metal loader (C + asm) packed as an Android `boot.img` v2 |
| `xnu/patches/` | XNU patch series, applied in order by CI |
| `.github/workflows/xnu.yml` | Kernel CI build |
| `tools/mkadt.py` | Generates the Apple Device Tree (MMIO, CPU, display, identity) for the Redmi 9C |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the xnuL payload |
| `tools/unpack_bootv2.py` | Android `boot.img` v2 unpacker (headers, DTB, round-trip checks) |
| `tools/mkiosroot.sh` | Builds the HFS+ iOS root from IPSW images |
| `tools/angelica-root-tweaks.py` | Bring-up changes to that root (Setup skip, activation, daemons) |
| `tools/angelica-log-pull.sh`, `angelica-log.py` | Read the persistent kernel log from recovery |
| `tools/angelica-crash.py` | Symbolicate console crash reports |
| `tools/xnu-cc.py` | Local single-file compile check from a CI log |
| `tools/qemu-angelica.py` | QEMU virt test boots |
| `tools/adt.py`, `remotezip.py`, `sparsediff.py` | Helpers |
| `NOTES.md` | Detailed engineering log: every boot, crash, decision and hardware fact |

Not in git: partition backups, live device trees, recovery images, Apple
firmware/root images, and generated build trees. They contain device serials and
calibration data, or Apple's copyrighted files.

## Patch series (high level)

| # | What |
| --- | --- |
| 0001–0009 | Toolchain compatibility, ANGELICA machine config, non-PAC arm64, stubs for Apple-internal pieces |
| 0010–0018 | Early framebuffer console, physmap fix, PRNG, device-tree platform expert, timer PPI, boot CPU |
| 0019–0027 | libpthread provider, BearSSL AES, RAM-disk root, mockfs, SHA-256, minimal AMFI, HFS+ |
| 0028–0034 | UFS host driver and root device, read/write root, DMA above 4 GiB |
| 0035–0045 | Entitlements, bindfs, AppleKeyStore stub, IOWatchdog, crash reports |
| 0046 | IOSurface + IOMobileFramebuffer (`AppleCLCD`) services |
| 0047–0057 | Diagnostics, sandbox permissiveness, HFS fixes, throttled panel copy |
| 0058–0069 | HID family, iPhone X identity, SPMI keys, GENI touch, RPMh, SMP, persistent log |
| 0082 | ANGELICA machine config (dual board config, A53 feature set) |
| 0083 | ANGELICA MSDC/eMMC read-only root device (untested) |
| 0084 | ANGELICA early framebuffer markers/panic console |
| 0085 | 4K hardware page tables (A53 has no 16K granule) |
| 0086 | Commpage assert exception for the first 4K build (superseded by 0087) |
| 0087 | Classic non-large-memory layout for 4K (TTBR1 roots at L1, `T1SZ=25`) |
| 0088 | Build ANGELICA with `-mcpu=cortex-a53+crypto` (no ARMv8.1 LSE atomics) |
| 0089 | PFZ commpage trylock: `ldaxr`/`stxr` loop instead of LSE `casa` on non-LSE CPUs |
| 0090 | Final single-name rebrand of the tree (legacy dual-name layer removed) |
| 0091 | Fix the 8-byte ANGELICA log magic and add `kmem_range_init` failure diagnostics |
| 0092 | Put the `kmem_range_init` claim layout into the panic string and retry a failed claim from the map start |
| 0093 | Use the 24 GB zone map and the iOS compressor cap on ANGELICA instead of the macOS 128 GB/16x sizes |
| 0094 | Switch the ANGELICA MSDC host to polled PIO, clear the FIFO and print controller state on failure |
| 0095 | Report the caller and virtual address when the 16K/4K PTE alignment guard fires |
| 0096 | Power up the MSDC clock and report the exact CMD17 timeout phase |
| 0097 | Identify the VA and pmap when page-protect hits an unaligned PTE |
| 0098 | Identify the caller when an unaligned pmap_enter is attempted |
| 0099 | Align the randomized ARM commpage text mapping to the VM page size |
| 0100 | Set the ANGELICA MSDC data timeout and panic with the failing phase |
| 0101 | Initialize the ANGELICA eMMC card in SDR/1-bit instead of inheriting LK state |
| 0102 | Pass the eMMC RCA in CMD3 and verify the response |
| 0103 | Print BSD autoconf/kminit stage markers |
| 0104 | Enable BSD init stage logs and mark setconf/vfs_mountroot |
| 0105 | Skip the IOBSD wait for built-in ANGELICA HFS and mark root lookup |
| 0106 | Add ANGELICA MSDC entry/prepare/init stage markers |
| 0107 | Match msdcroot0 by prefix and trace rootdev call |
| 0108 | Fix goto crossing declaration in IOFindBSDRoot |
| 0109 | Add kprintf at ANGELICA MSDC rootdev entry |
| 0110 | Use kprintf for all ANGELICA MSDC messages |
| 0111 | Use kprintf when reporting the ANGELICA root device |
| 0112 | Fix ANGELICA MSDC CMD17 data loop timeout path |
| 0113 | Program SDC_BLK_NUM for ANGELICA MSDC CMD17 |
| 0114 | Set CMD16 block length and clear FIFO before CMD17 |
| 0115 | Use basic DMA for ANGELICA CMD17 reads |
| 0116 | Fix ANGELICA DMA phys-address type |
| 0117 | Allow HFS+ to set a 4K logical block size |

## Warnings

- Experimental. Expect hangs, panics and data loss; the upstream ios-angelica build
  already erases `userdata`, and this port will do the same once an iOS root is flashed.
- Security is deliberately off: permissive AMFI, stub AppleKeyStore, no data
  protection, no sandbox profiles. Never store personal data.
- Only flash `boot` on `angelica` until the port reaches userland. Keep stock
  `boot`, `dtbo`, `vbmeta` and partition backups (persist, modemst1/2, fsg,
  fsc); never touch bootloader/recovery/modem/persist.
- Apple's iOS images and the iPhone X device tree are not distributed here.

## Credits

Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), the
[dipa-studio/ios-angelica](https://github.com/dipa-studio/ios-angelica) project (the
upstream ios-angelica work this tree started from),
[BearSSL](https://bearssl.org),
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build),
[ipsw](https://github.com/blacktop/ipsw), MediaTek/Xiaomi's published Linux
kernel sources, and [OrangeFox Recovery](https://orangefox.download). iOS itself
is Apple's and is not distributed here.
