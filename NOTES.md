# xnuL — working notes (angelica)

Last updated **2026-11-14**. This file is the current handoff: status, active
blocker, next actions, hardware facts and the commands that reproduce the last
test. The long boot-by-boot bring-up log (2026-10-14 … 2026-11-01) is in git
history.

## 2026-11-14 — rebrand broke CI's patch application (fixed)

- The sweet→angelica rebrand (b696e98…c13b2fc, 2026-11-14) was done with
  hand-edited, inconsistent substitutions. CI's `git apply` is atomic per
  patch, so most of the series was silently skipped in CI (the kernel still
  built and ran, but without MSDC/USB/Framebuffer/SMP patches, ANGELICA.h and
  the MASTER config fixes).
- Fixed in **0a23547**: uniform global rename everywhere; MASTER.arm64.angelica
  created exactly once (0006) with the final content; 0082 contributes only
  ANGELICA.h (single ANGELICA define, SMP_MAX_CPUS=8, VTIMER_INTID=27, IPI_SGI=1);
  obsolete SMP_MAX_CPUS 6/8 hunks in 0065/0078 dropped; 0090 keeps only the real
  rename edits; 0181 (unbuffered log ring, previously always skipped) now applies.
- Verified exactly as CI does (`git apply` on a pristine xnu-8796.141.3 tarball,
  filename order): **172 of 174 patches apply**. The only two that cannot apply are
  0031/0041 (HFS read-write / catalog link notes for `bsd/angelica_hfs/core/*.c`,
  sources this tree never adds) — inert, keep them as-is or delete later.
- **0181/0183/0184 were never in any built image**: CI silently skipped them all.
  0184 already removed the FB-serial and fixed the log-ring path (committed
  0f96d63); the new kernel will contain it, so the missing `ANGELICA pmap_expand`
  / `ANGELICA EXC` prints should reach the USB ring via the console path.
- **CI run 43585212213** (kakaed3000/nexredmi-xnul, branch angelica, commit
  0a23547) is building the first kernel with the full series actually applied.
- The pre-rebrand verified series applied 171/177; only 0031/0041/0181 were
  skipped then as well. `backup-rebrand-broken` (c13b2fc) preserves the broken
  rebrand for reference.

## 2026-11-14 (cont.) — user-visible status to watch after this CI build

- Boot args unchanged (see below). Watch port **5140** (`udplog.py` must run
  before boot). Expected: same chain as the 0182 boot, plus now now-working
  unbuffered kprintf ring entries (`ANGELICA pmap_expand` path) via 0181+0184.
- The image to flash will need rebuilding from the new `xnu-kernel` artifact
  (`tools/mkpayload.py <kernel> <adt> out/xnul-boot-0a23547.img --cmdline "..."`),
  then `fastboot flash boot` (never `fastboot boot`).

Target: **Redmi 9C only** (`angelica`, MediaTek MT6765 / Helio G35, 8× Cortex-A53,
4K hardware page tables, 16K iOS software page ABI). No other phone is in scope.

## Current status

- The boot chain works end to end: stock LK → `fastboot flash boot` → xnuL →
  XNU 8796.141.3 (iOS 16.7.16 / 20H392) → iPhone X userland on internal eMMC.
- Kernel **0146** (`2fd5d86`) enables the big-cluster ARMPLL before `PSCI
  CPU_ON`; hardware test showed `requested=8 selected=8 start=8`,
  `ANGELICA PREPLL` for CPU4–7 and **8/8 CPUs initialized** in the first pass.
  There is no late-start thread when `startcpus` is absent.
- **Dynamic 4GB RAM works (kernel 0147).** `0147-angelica-bootstrap-table-size`
  raises `BOOTSTRAP_TABLE_SIZE` from 8 to 34 pages. The 8-page table only
  covered 2GB; the 3.5GB managed range overflowed into `topology_info` at
  `bootstrap_pagetables + 0x85a0` and panicked with
  `topology_info.boot_cpu == NULL`. Hardware log
  `logs/live-0147-bootstraptable.txt` shows `vm_page_bootstrap: 226084 free
  pages` (3.5GiB managed including wired), `requested=8 selected=8 start=8`,
  and all 8 CPUs alive. The earlier 2GB cap in xnuL was only an interim
  workaround and is removed.
- **No current GUI reference exists.** An earlier 2026-10-18 observation
  (kernel 0069 + image v14, `cpus=6`, `angelicafb_noconsole=1
  angelicafb_hz=30`) is historical only; that image/kernel combination is not
  preserved and cannot be used as a baseline.
- Current image is **v15**. It has not shown a GUI in the current tree. The
  current kernel is post-0069 and includes the 4K hardware-page port.
- Current boot is slow but not hung: `mount-phase-2` ~00:06:34, SpringBoard
  spawn ~00:10:56, backboardd ~00:10:58, boot tasks continue past 00:22.
  `ignition boot failed: 25` (no preboot) is non-fatal.
- **GUI failure:** SpringBoard and backboardd stall in the same early
  libsystem call; no `ANGELICA SURF create`, no `ANGELICA FB: swap`, no
  IOMobileFramebuffer setup from backboardd. The FB driver is not the wall.
- 6-CPU test (`diag6`, kernel 0146) reached the **same relative libsystem PC**
  as the 8-CPU run, so cluster1, CPU6–7 and first-pass 8-core startup are not
  the cause. The stall predates 0145.

## MSDC status after 0161 (static bounce + proper init/CRC)

- **0154** (`b3a311d`) added fast mode (div clock, 4-bit, 64K bounce, CMD18
  multi-block). Every fast build (0154-0160) panicked: apps died with
  PAC-farts inside the dyld cache, then the kernel lost a freed 48-byte
  zone element (`[task_id_token] element modified after free`, later
  `[data.kalloc.16384]`, `[kalloc.type2.48]`, `[kalloc.type13.48]`).
  0152b (slow, 1-bit, div 0x40) was stable for 26+ minutes.
- **Root cause was twofold, but only one was real:**
  1. **DMA scribble into arbitrary physical pages** - the 64K bounce was a
     plain kalloc'd buffer; `kmem_alloc_contig` does not guarantee
     contiguity, so DMA wrote into unrelated kernel pages
     (`compressed PTE ... corrupted?` panic).
  2. **Marginal MSDC timing** at high divider-free speed (div=4/8) corrupts
     data without the host CRC flagging, so iOS dies inside the dyld cache
     (`[task_id_token]`/`[data.kalloc.16384]` UAF afterwards).
- **0159 (`01cb63a`) and 0160 (`f956021`) were silently skipped in CI.**
  The DXB build applies `xnu/patches/*.patch` in order but tolerates patch
  failures and keeps going; a hand-built context in the 0159 hunk was
  mis-placed, both patches failed to apply ("Skipping patch ... patch
  failed: bsd/dev/angelica_msdc.c:1060"), and the build produced the
  un-fixed driver. **All div-test boots (div4/8/16, nomulti, safemsdc,
  w1fastdiv, 0159-div8/div4) therefore ran a driver without any of these
  fixes.** The panics we saw on those boots are explained by that.
- **0161** (`0161-angelica-msdc-fix.patch`) combines both fixes and is
  generated against the real upstream source so it applies cleanly and is
  verified locally with `patch -p1 --dry-run`. It contains:
  - the static, physically contiguous 64K bounce buffer (via `kvtophys`);
  - `MSDC_CFG_MODE = SDMMC` (was 0);
  - `msdc_set_mclk()` (mode 0 divisor / mode 1 no-divisor, hclk = 400 MHz
    from DTS `clk_src=1`);
  - sampling-edge boot-args (`angelicamscmd_edge`,
    `angelicamsdrdata_edge`, `angelicamdswdata_edge`, 0=rising 1=falling,
    default 0/0/0 as stock DTS);
  - a 3-retry loop on `MSDC_INT_CMDTMO` / `MSDC_INT_RSPCRCERR` /
    `MSDC_INT_DATCRCERR` / `MSDC_INT_DATTMO` (before, a CRC error silently
    returned EIO; a fast data error was never retried);
  - default operating clock 26 MHz (`angelicamsdcclk`), 1-bit width by
    default. Full HS200/HS400 autok is not implemented.
- **`boot` images must be rebuilt** after this lands; the previously
  flashed 0159-div4/div8/div16 images contain none of these fixes.

- **0150-0158 panic logger** (`0155-0158-*.patch`) catches the panic text
  over USB via `angelica_early_panic` + `angelica_usb_panic_log`: prints
  `ANGELICA PANIC: op=.. str=.. caller=.. msg=[..]` and the last 1 KB of
  the console ring. Empty text means `debugger_panic_str == NULL`, i.e.
  WDT/`Debugger()` rather than `panic()`; with real panics it shows the
  full format string.

## Active blocker: early libsystem stall

Sampler evidence from the 8-CPU run (`live-0146*.txt`):

```
SpringBoard[26]  pc=lr=0x1b53ebd08
  bt 0x1b54397b4 0x1b54061d8 0x1b53fdb3c 0x1b53fc2e0
backboardd[56]  pc=lr=0x1b53ec5b0 / 0x1b53efedc  (same caller chain)
```

From the 6-CPU `diag6` run (`live-diag6.txt`):

```
backboardd  pc=lr=0x1e5fe7d08
  bt 0x1e60357b4 0x1e60021d8 0x1e5ff9b3c 0x1e5ff82e0
```

The relative offsets are identical to the 8-CPU case (`+0x4daac` from the same
caller chain). `pc == lr` and the stable backtrace mean the threads are blocked
inside a syscall, not spinning. backboardd never reaches its main binary or
IOMKit, so the display path has no chance to create a surface.

Variables relative to the historical 2026-10-18 observation (not currently
reproducible):

1. image **v15** instead of v14;
2. kernel post-0069 (4K hardware pages, non-large layout, rebuild, AMFI/logging
   shims, 8-CPU ARMPLL path);
3. current boot flags (`cpus=8`, `angelicafb_noconsole=1`, `angelicafb_hz=30`);
4. userland shared-cache/page-in path, which cannot be inspected locally until
   the dyld cache is available again.

The stalled PC must be symbolized before guessing further. There is no local
dyld shared cache: the v1 root image contains 0-byte placeholders. The `ipsw`
tool can remote-extract the iPhone X 20H392 dyld cache; the earlier attempt
died with `/tmp`. Retry it into `/mnt/data`, never `/tmp`.

### 2026-11-03 correction: the old sampler PC is not a live syscall

The sampler reads `machine.upcb`, which is the **last saved user state**, not
the live PC of a running thread. `pc == lr` at a constant address is therefore
consistent with a preemption point; it does not by itself prove a syscall.
For the 8-CPU log (`slide=0x1270000`), `0x1b53ebd08 - 0x1270000` =
`0x1a4613d08`, which symbolizes to **dyld `MachOFile::forEachSection+52`**,
not libsystem. `libignition`/preboot is still non-fatal, and the display path
still never reaches surface creation.

Patch **0148** (`xnu/patches/0148-angelica-sampler-state.patch`, CI run
`41625485325`) adds `state`, `reason`, `block_hint`, `wait_event`,
`wait_result`, `kpcb/kpc/klr/ksp` to the same sampler so the next working
8-CPU boot can distinguish runnable/preempted from genuinely blocked threads.
Do not use the `ld64.lld` relink as a base: it changes segment order and
trips `arm_vm_init`/`topology_info`.

## Next actions

1. **Symbolize the stalled libsystem address.** Use the `ipsw` tool against
   iPhone10,3 build `20H392` (iOS 16.7.16) and extract the dyld shared cache to
   `/mnt/data`. Resolve `0x1b53ebd08` (8-CPU) and `0x1e5fe7d08` (6-CPU) and
   identify the syscall/function. If the cache cannot be fetched, add
   kernel-side all-thread PC/syscall diagnostics instead of changing FB.
2. **Separate the regression variables** with the smallest possible test on
   the current image v15 (with and without the 0069-style UI flags). Do not
   assume a v14 image is available; there is no current GUI reference. Keep 4K
   hardware pages: 16K hardware is impossible on this SoC.
3. **Rebuild the UI image.** The prepared `final8` image (kernel 0146, `cpus=8`,
   `angelicafb_noconsole=1 angelicafb_hz=30`) was lost with `/tmp`; rebuild it
   from HEAD before the next hardware test.
4. **Redo the heartbeat removal (0147).** `0121-angelica-heartbeat-forever` is
   still in the tree; the prepared 0147 patch/diff was lost with `/tmp`. Tested
   logs no longer show `ANGELICA alive`, but the source heartbeat remains.

## Reproduce the last boot

Kernel: HEAD `2fd5d86` (patch 0146). Build via CI and download
`kernel.development.angelica`.

```sh
python3 tools/mkadt.py xnul/out/angelica.adt
python3 tools/mkpayload.py xnu/out/kernel.development.angelica \
    xnul/out/angelica.adt xnul/out/payload.bin \
    --cmdline 'cpus=8 rd=msdcroot0 keepsyms=1 -enable_kprintf_spam debug=0x14c rootrw=1 angelicanull=all angelicausb=2 launchd_log_serial=1 angelicasample=SpringBoard,backboardd,launchd angelicascan=backboardd,SpringBoard angelicaiolog=backboardd angelicatouch=1 angelicafb_noconsole=1 angelicafb_hz=30'
cd xnul && OUT=out/xnul-boot-angelica.img ./build.sh ../xnul/out/payload.bin
```

Flash only `boot`:

```sh
fastboot devices                     # exactly one angelica
fastboot flash boot xnul/out/xnul-boot-angelica.img
fastboot reboot
```

Never `fastboot boot`. The iOS root is in `userdata` and is not touched by
flashing `boot`. The last tested phone state is unknown: a host/phone reboot
interrupted the `diag6` sample series, so check `fastboot devices` before
planning anything.

Logs: panel console + USB CDC-ECM UDP to host `192.168.7.1:5140` (phone
`192.168.7.2`). Saved under `/mnt/data/redmi9c-ios/logs/`.

## Hardware facts (project-relevant)

- **SoC:** MT6765, 8× Cortex-A53, no 16K hardware page granule. Use 4K hardware
  page tables with the 16K software page ABI.
- **Clusters/CPUs:** big cluster needs ARMPLL before CPU4–7 `PSCI CPU_ON`.
  `ANGELICA PREPLL` reads `CON0/CON3` and enables `CON0.PLL_EN`; without it
  CPU4–7 fail. Patch 0146 does this in `AngelicaCPU.cpp` before the SMC.
- **RAM:** xnuL loads at `phys_base=0x60000000`; the payload's fallback
  `mem_size` is 1.5 GiB, but `fdt_memory()` now derives the live span and XNU
  manages 3.5 GiB (4 GiB actual). Keep the framebuffer and firmware carve-outs
  below the kernel.
- **Display:** 720×1600 panel, LK video FB at `0x5e605000`, `rowBytes=2880`,
  32 bpp; XNU presents it as an iPhone X-sized display (1125×2436).
- **Storage:** internal eMMC via MSDC; iOS root is an HFS+ image in the existing
  `userdata` partition. Flashing `boot` leaves it alone.
- **Interrupts:** GICv3; timer is INTID 27 (PPI), routed through the IRQ path
  (`ICC_IAR1`/`ICC_EOIR1`), not FIQ.
- **Debug:** no serial UART. Framebuffer console plus USB CDC-ECM log; the
  loader also has early FB marker bands.
- **Userland:** iPhone X, iOS 16.7.16 `20H392`; ARMv8.0, plain arm64, no PAC,
  4K hardware pages / 16K software pages.

## Kernel / patch state

- HEAD: `7fbe9a5` — `xnu: size bootstrap page tables for 3.5GB angelica`
  (bootstrap-table 0147); parent `2fd5d86` is the ARMPLL 0146 change.
- `0144-angelica-sip-cluster-down.patch` — power cluster1 down through SIP
  before bring-up.
- `0145-angelica-late-cluster1.patch` — late cluster1 start experiment; not
  used in the final 8-CPU first-pass path.
- `0146-angelica-armpll-before-cpu-on.patch` — final ARMPLL fix; CI run
  `41315892846` succeeded.
- `0147-angelica-bootstrap-table-size.patch` — 34-page bootstrap tables; CI
  run `41454813125` succeeded.
- Heartbeat removal (the older planned 0147) is **not** in HEAD; `0121`
  heartbeat remains.
- The 0151–0153 framebuffer experiments were dropped; do not resurrect them.
- Kernel CI: `.github/workflows/xnu.yml`, branch `angelica`, artifact
  `xnu-kernel`; buildtree artifact `xnu-buildtree`.

## Artifacts and paths

- Last hardware log: `/mnt/data/redmi9c-ios/logs/live-0147-bootstraptable.txt`
  (dynamic-RAM test; `vm_page_bootstrap: 226084`, 8/8 CPUs).
- Earlier logs: `/mnt/data/redmi9c-ios/logs/live-diag6.txt` (6-CPU),
  `live-0146.txt`, `live-0146-0146d-full.txt.gz`,
  `live-0146-archive-0146abc.txt.gz`, `live-0146-freemb-2g.txt` (2GB cap).
- Kernel artifacts: `/mnt/data/redmi9c-ios/artifacts/2026-11-02/0147/kernel/`
  contains the 0147 build (sha256 `55df41f3…`).
- Surviving build tree: `/mnt/data/redmi9c-ios/artifacts/2026-10-31/xnu-buildtree-0143/`.
- Reconstructed 0146 source: `/mnt/data/redmi9c-ios/AngelicaCPU.cpp` (from the
  0143 build tree plus 0144→0145→0146). The byte-identical CI build tree was
  not preserved.
- Root image: `/mnt/data/redmi9c-ios/nexredmi-xnul/xnul/out/ios16-root-20H392-nojournal-v1.img`
  (v1, HFS+, 16 GB). v15 lives on the device; its source image is not in git.
- Current images: `xnul/out/payload-0147-bootstraptable-a53-rw.bin` and
  `xnul/out/xnul-boot-0147-bootstraptable-8cpu-a53-rw.img` (sha256
  `02c4cdc6…`). `payload-0146-a53-rw.bin` predates the 0146 ARMPLL change;
  older `xnul/out/` images are stale.
- The `diag6` and `final8` images and the 0146d image were in `/tmp` and are
  gone. Do not reference them as available.
- `ipsw` tool: `/mnt/data/redmi9c-ios/tmp/opencode/bin/ipsw`. iOS root/dyld
  source: iPhone10,3 build `20H392` (iOS 16.7.16); dyld extraction should be
  `--dyld --dyld-arch arm64` and must write to persistent storage.
- GitHub auth (do not print): `GIT_ASKPASS=/mnt/data/redmi9c-ios/tmp/opencode/askpass.sh`,
  token in `/mnt/data/redmi9c-ios/tmp/opencode/token-kakaed`.

## Constraints and safety

- User requirement: all 8 CPUs must work; any usable desktop is acceptable;
  do not change the FB driver (it is correct; iOS never reaches surface
  creation); keep kprintf spam in the USB log.
- Russian for conversation. Avoid long sleeps in automation.
- Flash only `boot`; never touch bootloader/recovery/modem/persist/partition
  table. Keep stock `boot.img` for rollback.
- Security model is intentionally open: permissive AMFI, stub AppleKeyStore,
  no sandbox/data protection. No personal data on the device.
- Commit/push only with explicit permission. `NOTES.md` is currently modified;
  do not commit it as part of an unrelated change.
