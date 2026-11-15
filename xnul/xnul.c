/*
 * xnuL (xnu loader for angelica) diagnostics: one boot answers the questions
 * that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the MTK BL31 offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 27 on MT6765)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

/* MT6765: GICv3 distributor + redistributor (DT interrupt-controller@0c000000) */
#define GICD_BASE		0x0c000000UL
#define GICR_BASE		0x0c100000UL
#define GICR_SIZE		0x200000UL
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define VTIMER_INTID		27	/* DT: <GIC_PPI 11 ...> */
#define PTIMER_INTID		30	/* DT: <GIC_PPI 14 ...> */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static uint64_t dbg_t1d, dbg_t2d, dbg_t3d, dbg_pa;

enum stage_tag {
	TAG_UNKNOWN = 0,
	TAG_BOOT,
	TAG_LOADER,
	TAG_DIAG,
	TAG_TCR,
	TAG_SMP,
	TAG_GIC_SRE,
	TAG_GIC_DIST,
	TAG_GIC_REDIST,
	TAG_GIC_CONFIG,
	TAG_GIC_VTIMER,
	TAG_GIC_SGI,
	TAG_MM_PROBE,
	TAG_DONE,
};
static volatile uint32_t stage_tag = TAG_BOOT;

static const char *decode_stage(uint32_t t)
{
	switch (t) {
	case TAG_BOOT: return "entry";
	case TAG_LOADER: return "loader";
	case TAG_DIAG: return "diag";
	case TAG_TCR: return "tcr";
	case TAG_SMP: return "smp";
	case TAG_GIC_SRE: return "gic-sre";
	case TAG_GIC_DIST: return "gic-dist";
	case TAG_GIC_REDIST: return "gic-redist";
	case TAG_GIC_CONFIG: return "gic-config";
	case TAG_GIC_VTIMER: return "gic-vtimer";
	case TAG_GIC_SGI: return "gic-sgi";
	case TAG_MM_PROBE: return "mmprobe";
	case TAG_DONE: return "done";
	default: return "unknown";
	}
}
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static const char *decode_ec(uint32_t ec)
{
	switch (ec) {
	case 0x00: return "SP alignment";
	case 0x09: return "BTI instruction";
	case 0x0b: return "Hyp entry fail";
	case 0x0c: return "BTI/PACSP fail";
	case 0x0e: return "SVE trap";
	case 0x11: return "WFx instruction";
	case 0x14: return "SVC from AArch64";
	case 0x15: return "SVC from AArch32";
	case 0x16: return "HVC from AArch64";
	case 0x17: return "SMC from AArch64";
	case 0x18: return "system reg trap";
	case 0x1c: return "SVE access trap";
	case 0x20: return "instr abort lowest EL";
	case 0x21: return "instr abort current EL";
	case 0x22: return "PC alignment";
	case 0x24: return "data abort lowest EL";
	case 0x25: return "data abort current EL";
	case 0x26: return "alignment fault";
	case 0x29: return "SError/interrupt";
	case 0x2d: return "debug exception lowest EL";
	case 0x2e: return "debug exception current EL";
	case 0x30: return "breakpoint lowest EL";
	case 0x34: return "breakpoint current EL";
	case 0x36: return "watchpoint current EL";
	case 0x38: return "watchpoint current EL";
	default:   return "unknown exception class";
	}
}

static const char *decode_dfsc(uint32_t dfsc)
{
	switch (dfsc) {
	case 0x00: return "reserved/attribution fault";
	case 0x01: return "alignment fault";
	case 0x02: return "attribution fault";
	case 0x04: return "translation fault L0";
	case 0x05: return "translation fault L1";
	case 0x06: return "translation fault L2";
	case 0x07: return "translation fault L3";
	case 0x09: return "access flag fault L1";
	case 0x0a: return "access flag fault L2";
	case 0x0b: return "access flag fault L3";
	case 0x0d: return "permission fault L1";
	case 0x0e: return "permission fault L2";
	case 0x0f: return "permission fault L3";
	case 0x11: return "synchronous external abort L0";
	case 0x13: return "synchronous external abort L1";
	case 0x15: return "synchronous external abort L2";
	case 0x19: return "synchronous external abort L3";
	case 0x21: return "asynchronous external abort L3";
	case 0x23: return "asynchronous external abort L3";
	default:   return "other fault";
	}
}

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */

	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

static volatile int panic_in_progress;
static void __attribute__((noreturn))
panic_hang(void)
{
	for (;;) __asm__ volatile("wfi");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (panic_in_progress)
		panic_hang();
	panic_in_progress = 1;

	/* Turn the MMU off first: the vectors may be unreachable through it. */
	uint64_t sctl = rsr("sctlr_el1");
	if (sctl & 0x1) {
		wsr("sctlr_el1", sctl & ~0x1ULL);
		isb();
	}

	if (slot == 5) {	/* IRQ from current EL, SPx */
		panic_in_progress = 0;
		irq_handler();
		return;
	}

	con_clear(WHITE);
	con_set_scheme(BLACK, BLACK);
	con_set_scale(2);
	con_set_row(0);
	con_wrap("xnuL Panic!", BLACK, 84);
	con_set_row(2);
	con_wrap("It seems something has happened. An unexpected situation has arisen, and xnuL's work has been suspended. Information regarding the reasons and other details will be provided below:",
		   BLACK, 84);
	con_skip();

	uint64_t mpidr = rsr("mpidr_el1");
	uint64_t daif  = rsr("daif");
	uint64_t sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	uint64_t cur_el = rsr("currentel") >> 2;
	uint64_t esr    = rsr("esr_el1");
	uint32_t ec     = (uint32_t)((esr >> 26) & 0x3f);
	uint32_t iss    = (uint32_t)(esr & 0x1ffffff);

	con_hex2("SLOT", "n=", slot, "cpu=", (uint32_t)(mpidr & 0xffffffffU));
	con_str("STAGE", decode_stage(stage_tag));
	con_hex("MPIDR", mpidr);
	con_hex("DAIF",  daif);
	con_hex("CEL",   cur_el);
	con_hex("VBAR",  rsr("vbar_el1"));
	con_hex("SCTLR", rsr("sctlr_el1"));
	con_hex("TCR",   rsr("tcr_el1"));
	con_hex("TTBR0", rsr("ttbr0_el1"));
	con_hex("TTBR1", rsr("ttbr1_el1"));
	con_hex("MAIR",  rsr("mair_el1"));
	con_hex("ESR",   esr);
	con_str("  EC",  decode_ec(ec));
	con_str("  DFSC", decode_dfsc(iss & 0x3f));
	con_hex("ELR",   rsr("elr_el1"));
	con_hex("FAR",   rsr("far_el1"));
	con_hex("SPSR",  rsr("spsr_el1"));
	con_hex("SP",    sp);
	con_hex("X0",    regs[0]);
	con_hex("X1",    regs[1]);
	con_hex("X2",    regs[2]);
	con_hex("X3",    regs[3]);
	con_hex("X4",    regs[4]);
	con_hex("X29",   regs[30]);
	con_hex("X30",   regs[31]);
	if (dbg_t2d || dbg_t3d) {
		if (dbg_t1d) con_hex("T1D", dbg_t1d);
		con_hex("T2D", dbg_t2d);
		con_hex("T3D", dbg_t3d);
		con_hex("PA ", dbg_pa);
	}
	con_str("STACK", "contents around SP:");
	for (int i = -4; i < 5; i++) {
		char lab[8];
		int off = i * 8, p = 0;
		if (off >= 0) { lab[p++] = '+'; } else { lab[p++] = '-'; off = -off; }
		if (off < 10) lab[p++] = (char)('0' + off);
		else { lab[p++] = (char)('0' + off / 10); lab[p++] = (char)('0' + off % 10); }
		lab[p] = 0;
		con_hex(lab, *(volatile uint64_t *)(sp + (int64_t)i * 8));
	}
	reset(40);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

void secondary_main(uint64_t idx)
{
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			wr8(r + GICR_IPRIORITYR + 19, 0xa0);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

/* The LK logo is live somewhere above 0x60000000: find the active DDP
 * layer buffer from its address register and paint it white. */
static void fill_white(uint32_t addr)
{
	if (addr < 0x60000000U || addr >= 0xc0000000U || (addr & 3))
		return;
	volatile uint32_t *p = (volatile uint32_t *)addr;
	for (uint32_t i = 0; i < 0x600000 / 4; i++)
		p[i] = 0xffffffffU;
}

static void disp_probe(void)
{
	static const uintptr_t bases[] = { 0x1400b000, 0x1400c000 };

	for (int b = 0; b < 2; b++) {
		uint32_t src = rd32(bases[b] + 0x02c);
		for (int l = 0; l < 4; l++) {
			if (!(src & (1U << l)))
				continue;
			uint32_t a = rd32(bases[b] + 0xf40 + l * 0x20);
			if (a >= 0x60000000U && a < 0xc0000000U)
				wr32(bases[b] + 0xf40 + l * 0x20, 0x5e605000U);
		}
	}
	uint32_t a = rd32(0x1400d000 + 0xf00);
	if (a >= 0x60000000U && a < 0xc0000000U)
		wr32(0x1400d000 + 0xf00, 0x5e605000U);
}

static void __attribute__((unused)) test_smp(void)
{
	stage_tag = TAG_SMP;
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++) {
		/* angelica: MPIDR Aff0 = core (0..7). Probe Aff0 first, then the
		 * Aff1 (i << 8) encoding as a fallback; bit31 marks the retry. */
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i, (uintptr_t)secondary_entry, i);
		if ((uint32_t)ret[i] != 0)
			ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8,
				     (uintptr_t)secondary_entry, i) | 0x80000000;
	}
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if ((uint64_t)i == self || ((uint64_t)i << 8) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage_tag = TAG_GIC_SRE;
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage_tag = TAG_GIC_DIST;
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage_tag = TAG_GIC_REDIST;
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage_tag = TAG_GIC_CONFIG;
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage_tag = TAG_GIC_VTIMER;
	irq_window("VTIMER", r, VTIMER_INTID);
	stage_tag = TAG_GIC_SGI;
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}


/* ---- minimal 4K-granule high-half MMU probe (39-bit VA, T1SZ=25) ---- */
extern char _head[]; extern char _start[]; extern char _end[];
extern char _bss_start[]; extern char _bss_end[]; extern char _stack_top[];
extern char vectors[];

static uint64_t probe_id_l1_4k[512]  __attribute__((aligned(4096)));
static uint64_t probe_l1_4k[512]   __attribute__((aligned(4096)));
static uint64_t probe_l2_4k[512]   __attribute__((aligned(4096)));
static uint64_t probe_l3_4k[512]   __attribute__((aligned(4096)));
static uint8_t  probe_page[4096]      __attribute__((aligned(4096)));

static void
probe16k(void)
{
	stage_tag = TAG_MM_PROBE;
	uint64_t sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	for (uint32_t i = 0; i < sizeof(probe_page); i++) probe_page[i] = (uint8_t)(0x5a + (i & 0xff));

	con_hex("lohead", (uint64_t)(uintptr_t)_head);
	con_hex("start ", (uint64_t)(uintptr_t)_start);
	con_hex("vectors", (uint64_t)(uintptr_t)vectors);
	con_hex("_end  ", (uint64_t)(uintptr_t)_end);
	con_hex("_stack", (uint64_t)(uintptr_t)_stack_top);
	con_hex("sp    ", sp);
	con_hex("ppage ", (uint64_t)(uintptr_t)probe_page);

	/* low-half identity map: 4K granule, 1GB SECT blocks at L1 (covers VA
	 * 0..512 GB, enough for xnuL Image + FB) */
	for (uint32_t i = 0; i < 512; i++) probe_id_l1_4k[i] = 0;
	for (uint32_t i = 0; i < 512; i++) probe_id_l1_4k[i] = ((uint64_t)i << 30) | 0x1 | (3ULL << 8) | (1ULL << 10);

	/* high half 4K granule test page; T1SZ=25 => VA_BITS=39, initial lookup at L1 (L0 skipped) */
	for (uint32_t i = 0; i < 512; i++) probe_l1_4k[i] = 0;
	for (uint32_t i = 0; i < 512; i++) probe_l2_4k[i] = 0;
	for (uint32_t i = 0; i < 512; i++) probe_l3_4k[i] = 0;
	uint64_t va = 0xffffffffff523040ULL;
	uint64_t i1 = (va >> 30) & 0x1ffULL;
	uint64_t i2 = (va >> 21) & 0x1ffULL;
	uint64_t i3 = (va >> 12) & 0x1ffULL;
	uint64_t pa = (uint64_t)(uintptr_t)probe_page;
	probe_l1_4k[i1] = (uint64_t)(uintptr_t)probe_l2_4k | 0x3 | (1ULL << 10);
	probe_l2_4k[i2] = (uint64_t)(uintptr_t)probe_l3_4k | 0x3 | (1ULL << 10);
	probe_l3_4k[i3] = pa | 0x3 | (1ULL << 10) | (3ULL << 8);
	dbg_t1d = probe_l1_4k[i1];
	dbg_t2d = probe_l2_4k[i2];
	dbg_t3d = probe_l3_4k[i3];
	dbg_pa  = pa;
	con_hex("T1D", dbg_t1d);
	con_hex("T2D", dbg_t2d);
	con_hex("T3D", dbg_t3d);
	con_hex("PA ", dbg_pa);
	con_hex2("IDX", "i1=", (uint32_t)i1, "i2=", (uint32_t)i2);
	con_hex2("     ", "i3=", (uint32_t)i3, "off=", (uint32_t)(va & 0xfff));

	/* MAIR_EL1 */
	wsr("mair_el1", 0xffULL | (0x04ULL << 8));
	/* TCR: 39-bit VA, TG0=0b00 (4K low), TG1=0b10 (4K high), Linux-exact */
	uint64_t parange = rsr("id_aa64mmfr0_el1") & 0xfULL;
	uint64_t ips = (parange & 0x7ULL) << 32;
	uint64_t tcr = ((uint64_t)25)            /* T0SZ = 25 (39-bit VA) */
		| ((uint64_t)25 << 16)              /* T1SZ */
		| ((uint64_t)1 << 8) | ((uint64_t)1 << 24)    /* IRGN0/1 WBWA */
		| ((uint64_t)1 << 10) | ((uint64_t)1 << 26)   /* ORGN0/1 WBWA */
		| ((uint64_t)3 << 12) | ((uint64_t)3 << 28)   /* SH0/1 inner */
		| ((uint64_t)2 << 30)                                 /* TG1 = 4K */
		| ((uint64_t)1 << 22)                                 /* A1 */
		| ((uint64_t)1 << 36)                                 /* ASID16 */
		| ((uint64_t)1 << 37)                                 /* TBI0 */
		| ips;
	wsr("tcr_el1", tcr);
	/* TTBR0 stays the working 64K-granule low-half identity map */
	wsr("ttbr0_el1", (uint64_t)(uintptr_t)probe_id_l1_4k);
	wsr("ttbr1_el1", (uint64_t)(uintptr_t)probe_l1_4k);
	isb();
	__asm__ volatile("tlbi vmalle1is" ::: "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("isb" ::: "memory");

	uint64_t sctl = rsr("sctlr_el1");
	con_hex("SCLTR  ", sctl);
	wsr("sctlr_el1", sctl | 0x1ULL);
	isb();
	con_hex("MMU_ON", rsr("sctlr_el1"));

	uint64_t lval = *(volatile uint64_t *)(uintptr_t)probe_page;
	con_hex("L_VAL  ", lval);

	uint64_t hval = *(volatile uint64_t *)(uint64_t)va;
	con_hex("H_VAL  ", hval);

	wsr("sctlr_el1", sctl);
	isb();
	con_hex("MMU_OFF", rsr("sctlr_el1"));

	for (;;) __asm__ volatile("wfi");
}

/* ---- main ---- */

void xnul_main(uint64_t dtb)
{
	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage_tag = TAG_LOADER;
	con_clear(BLACK);
	disp_probe();
	con_str("xnu loader for angelica", "");
	con_hex2("MIDR", "id=", (uint32_t)rsr("midr_el1"),
		 "mmfr0=", (uint32_t)rsr("id_aa64mmfr0_el1"));
	con_hex2("MMFR", "mmfr0=", (uint32_t)rsr("id_aa64mmfr0_el1"),
		 "mmfr1=", (uint32_t)rsr("id_aa64mmfr1_el1"));
	con_hex2("PFR", "pfr0=", (uint32_t)rsr("id_aa64pfr0_el1"),
		 "pfr1=", (uint32_t)rsr("id_aa64pfr1_el1"));
	probe16k();
	con_skip();
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_skip();

	/* No payload: hardware facts, then the SMP and GIC timer tests. */
	stage_tag = TAG_DIAG;
	con_hex2("EL", "cur=", (uint32_t)(rsr("currentel") >> 2),
		 "mpidr=", (uint32_t)rsr("mpidr_el1"));
	con_hex2("DTB", "at=", (uint32_t)dtb, "magic=",
		 dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex("CNTFRQ", rsr("cntfrq_el0"));
	con_hex2("MIDR", "id=", (uint32_t)rsr("midr_el1"),
		 "mmfr0=", (uint32_t)rsr("id_aa64mmfr0_el1"));
	con_skip();
	{
		stage_tag = TAG_TCR;
		uint64_t t0 = rsr("tcr_el1");
		wsr("tcr_el1", (t0 & ~(((uint64_t)0x3 << 14) | ((uint64_t)0x3 << 30))) | ((uint64_t)2 << 14) | ((uint64_t)1 << 30));
		isb();
		uint64_t t16k = rsr("tcr_el1");
		wsr("tcr_el1", (t16k & ~(((uint64_t)0x3 << 14) | ((uint64_t)0x3 << 30))) | ((uint64_t)1 << 14) | ((uint64_t)3 << 30));
		isb();
		uint64_t t64k = rsr("tcr_el1");
		wsr("tcr_el1", (t64k & ~(((uint64_t)0xff << 14) | ((uint64_t)0xff << 30))) | t0);
		isb();
		uint64_t tback = rsr("tcr_el1");
		con_hex2("TCR", "init=", (uint32_t)t0, "16K=", (uint32_t)t16k);
		con_hex2("TCR", "64K=", (uint32_t)t64k, "rst=", (uint32_t)tback);
	}
	con_skip();
	con_hex2("PSCI", "ver=", (uint32_t)smc(PSCI_VERSION, 0, 0, 0),
		 "smccc=", (uint32_t)smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	stage_tag = TAG_DONE;
	con_str("DIAG", "display takeover done");
	for (;;)
		__asm__ volatile("wfe");
}
