/*
 * The Wi-Fi's PCIe controller on the Redmi K40 (alioth), poked one step at a time.
 *
 * The K40's radio is a QCA6390 on PCIe0 (vendor 0x17cb, device 0x1101), behind the root complex
 * qcom,pcie@1c00000 -- not the AHB radio (WCN6750) the rest of this tree's Wi-Fi driver was
 * written for (redwood_wifi.c). Everything below is this phone's: the device tree's node
 * qcom,pcie@1c00000 (reg-names parf, phy, dm_core, elbi, iatu, conf), the clock controller's
 * PCIe0 entries (gcc-sm8250.c) and the PCIe block's offsets (pcie-qcom.c).
 *
 * Addresses, from the phone's own device tree (read from it with `dtc -I dtb -O dts`):
 *   parf   0x01c00000 + 0x3000       phy   0x01c06000 + 0x1000
 *   dm_core 0x60000000 + 0xf1d (ELBI at 0x60000f20 + 0xa8)   iatu 0x60001000 + 0x1000
 *   conf   0x60100000 + 0x100000 (the configuration space, bus 0 onwards; this
 *          file reads only bus 0 device 0 function 0, so it maps 4 KiB of it)
 *   memory 0x60300000 + 0x3d00000, i/o 0x60200000 + 0x100000 (both the same on the bus side)
 *   perst-gpio is TLMM 0x4f (79), wake-gpio 0x51 (81); root complex 0
 *
 * From the clock controller (gcc-sm8250.c; all of these are offsets from its 0x100000):
 *   pcie_0_gdscr 0x6b004 (the controller's own power switch), the five clock votes in
 *   gcc_pcie_0_axi_clk_misc 0x52008 (bit 0 slv_axi, 1 mstr_axi, 2 cfg_ahb, 3 aux, 4 pipe, 5
 *   slv_q2a_axi), the phy's own two clocks at 0x6f004 and 0x6f02c, the bus clock at 0x9000c,
 *   and the two block resets 0x6b000 (core) and 0x6c01c (phy).
 *
 * The steps are how this is to be found out, and one that fails says so and stops the sequence:
 *   1  reads   what the boot loader and Android left of all of that: the power switch, the clock
 *              votes and their halt state, the two resets, the PHY's status word -- and, only if
 *              the controller is actually on, a few of its own registers (reading them while its
 *              clocks are off is a bus access that never returns)
 *   2  writes  the controller's power and clocks on and its two resets out, then reads it back
 *   3  writes  the PHY's own sequence (the device tree's qcom,phy-sequence) and asks it for the
 *              "ready" bit
 *   4  writes  the endpoint's reset line out (TLMM gpio 79) and waits for the link to come up
 *   5  reads   the configuration space: the root complex's and the QCA6390's vendor and device
 *
 * Boot-arg pcie=N performs steps 1 to N and stops; pcie_pause=MS is how long a step waits after
 * announcing a group of accesses (default 2200, as the loader notes the log every two seconds).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_BCM2837)
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void thread_terminate(thread_t thread);
/*
 * bsd/dev/sweet_ufs.c: the persistent log. printf already appends to its ring
 * (osfmk/console/serial_console.c _cnputs), so a line only needs the flush that
 * writes the ring out to the partition. This replaces the loader log the
 * reference used (loader_return.c), which this tree does not have.
 */
extern int sweet_log_flush(void);
/* This board's console. printf() does not reach it (a DEVELOPMENT kernel leaves
 * disable_kprintf_output set); this is the channel that does. */
extern size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));

static uint32_t pcie_last_step, pcie_step, pcie_pause_ms = 2200;
static uint32_t pcie_skip_steps;

static void run_steps(void);
static void pcie_steps_thread(void *argument, wait_result_t wait_result);

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
announce(const char *what);

static void
pause_group(void)
{
	if (pcie_step == pcie_last_step && pcie_pause_ms) {
		sleep_ms(pcie_pause_ms);
	}
}

#define BLOCK_NAMES 7
static struct {
	const char *name;
	uint64_t base;
	uint32_t size;
	vm_offset_t va;
} blocks[BLOCK_NAMES] = {
	{ "gcc", 0x00100000, 0x00100000 },   /* highest offset read: 0x9000c */
	{ "parf", 0x01c00000, 0x3000 },
	{ "phy", 0x01c06000, 0x1000 },
	{ "elbi", 0x60000f20, 0xa8 },
	{ "iatu", 0x60001000, 0x1000 },
	{ "conf", 0x60100000, 0x1000 },      /* bus 0 device 0 function 0 only */
	{ "dbi", 0x60000000, 0x1000 },   /* the core's own 4 KB: DBI at its start, ELBI at 0xf20 */
};

static uint64_t
reg(int which, uint32_t offset)
{
	if (blocks[which].va == 0 || offset >= blocks[which].size) {
		return 0;
	}
	return blocks[which].va + offset;
}

static uint32_t
rd(uint64_t address)
{
	return address ? *(volatile uint32_t *)address : 0;
}

static void
wr(uint64_t address, uint32_t value)
{
	if (address) {
		*(volatile uint32_t *)address = value;
	}
}

static void
say(const char *fmt, ...)
{
	__builtin_va_list ap;

	printf("pcie: %u: ", pcie_step);
	__builtin_va_start(ap, fmt);
	vprintf(fmt, ap);
	__builtin_va_end(ap);
}

/*
 * A line goes into the console, which keeps it in the log ring in memory. It
 * reaches the partition only when something flushes the ring, so anything
 * printed just before a register access is flushed first (see risky()): a
 * processor that stops on an access that never returns takes whatever it
 * printed last with it otherwise.
 */
static void
line(const char *fmt, ...)
{
	__builtin_va_list ap;

	printf("pcie: %u: ", pcie_step);
	__builtin_va_start(ap, fmt);
	vprintf(fmt, ap);
	__builtin_va_end(ap);
	printf("\n");
	if (&qcom_usb_debug_write) {
		char body[192], out[224];
		int n;
		__builtin_va_start(ap, fmt);
		n = vsnprintf(body, sizeof body, fmt, ap);
		__builtin_va_end(ap);
		if (n > 0) {
			int m = snprintf(out, sizeof out, "PCIE %u: %s\n", pcie_step, body);
			if (m > 0) qcom_usb_debug_write(out, (size_t)m < sizeof(out) ? (size_t)m : sizeof(out) - 1);
		}
	}
}

/* Put the ring on the medium now: the access that follows may never return. */
static void
risky(void)
{
	(void)sweet_log_flush();
}

static void
announce(const char *what)
{
	if (pcie_step != pcie_last_step) {
		return;
	}
	line("%s", what);
}

static uint32_t
get(int which, const char *name, uint32_t offset)
{
	uint64_t address = reg(which, offset);
	uint32_t value;

	line("about to read %s %s at %#llx", blocks[which].name, name, address);
	risky();
	value = rd(address);
	line("%s %s (%#llx) = %#x", blocks[which].name, name, address, value);
	return value;
}

static void
put(int which, const char *name, uint32_t offset, uint32_t value)
{
	uint64_t address = reg(which, offset);

	line("about to write %s %s at %#llx: %#x", blocks[which].name, name, address, value);
	risky();
	wr(address, value);
}

/* Where the clock controller's registers are, as an address (its block base plus the offset). */
#define GCC(off)                reg(0, (off))

static bool
map_all(void)
{
	if (blocks[0].va != 0) {
		return true;    /* already mapped: io_submap is 8 MiB and this window is not free */
	}
	for (int i = 0; i < BLOCK_NAMES; i++) {
		blocks[i].va = ml_io_map(blocks[i].base, blocks[i].size);
		if (blocks[i].va == 0) {
			printf("pcie: NO MEMORY FOR THE %s BLOCK AT %#llx\n", blocks[i].name, blocks[i].base);
			return false;
		}
	}
	return true;
}

/* ---- step 1: what is left of the controller ------------------------------------------------ */

#define GCC_PCIE_0_GDSCR        0x6b004
#define GCC_PCIE_0_AXI_MISC     0x52008
#define GCC_PCIE_0_Q2A_AXI_HALT 0x6b010
#define GCC_PCIE_0_SLV_AXI_HALT 0x6b014
#define GCC_PCIE_0_MSTR_HALT    0x6b01c
#define GCC_PCIE_0_CFG_AHB_HALT 0x6b024
#define GCC_PCIE_0_AUX_HALT     0x6b028
#define GCC_PCIE_0_PIPE_HALT    0x6b02c
#define GCC_PCIE_PHY_AUX        0x6f004
#define GCC_PCIE_0_PHY_REFGEN   0x6f02c
#define GCC_PCIE_TBU            0x9000c
#define GCC_PCIE_0_BCR          0x6b000
#define GCC_PCIE_0_PHY_BCR      0x6c01c
#define GCC_PCIE_PHY_BCR        0x6f000
#define GCC_PCIE_PHY_COM_BCR    0x6f010

#define GDSC_POWER_ON           (1u << 31)
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_RETAIN_FF_ENABLE   (1u << 11)

#define PHY_STATUS              0x814           /* dt: qcom,phy-status-offset; bit 6: ready */
#define PHY_POWER_DOWN          0x840           /* dt: qcom,phy-power-down-offset */

#define GDSC_PWR_ON             (1u << 31)      /* gdsc.c: PWR_ON_MASK */
#define CLOCK_VOTES             0x2fu           /* slv_axi, mstr_axi, cfg_ahb, aux, slv_q2a (pipe comes with the PHY) */

static bool
step_look(void)
{
	uint32_t misc, gdscr;

	announce("read what the boot loader and Android left of the Wi-Fi's PCIe controller: its power switch, the clock "
	    "votes and their halt state, its two resets, and the PHY's status word");
	gdscr = get(0, "pcie_0_gdscr", GCC_PCIE_0_GDSCR);
	misc = get(0, "pcie_0_axi_misc", GCC_PCIE_0_AXI_MISC);
	get(0, "pcie_0_slv_axi_halt", GCC_PCIE_0_SLV_AXI_HALT);
	get(0, "pcie_0_mstr_axi_halt", GCC_PCIE_0_MSTR_HALT);
	get(0, "pcie_0_cfg_ahb_halt", GCC_PCIE_0_CFG_AHB_HALT);
	get(0, "pcie_0_aux_halt", GCC_PCIE_0_AUX_HALT);
	get(0, "pcie_0_pipe_halt", GCC_PCIE_0_PIPE_HALT);
	get(0, "pcie_0_slv_q2a_halt", GCC_PCIE_0_Q2A_AXI_HALT);
	get(0, "pcie_phy_aux", GCC_PCIE_PHY_AUX);
	get(0, "pcie0_phy_refgen", GCC_PCIE_0_PHY_REFGEN);
	get(0, "aggre_noc_pcie_tbu", GCC_PCIE_TBU);
	get(0, "pcie_0_bcr (core reset)", GCC_PCIE_0_BCR);
	get(0, "pcie_0_phy_bcr (phy reset)", GCC_PCIE_0_PHY_BCR);
	printf("pcie: %u: the controller is %s (gdscr %#x), its clock votes %#x\n", pcie_step,
	    (gdscr & GDSC_POWER_ON) ? "on" : "off", gdscr, misc);
	if ((gdscr & GDSC_POWER_ON) == 0 || (misc & 0x3f) != 0x3f) {
		say("its clocks are not all voted for, so neither its PHY's nor its own registers are read here\n");
		return true;
	}
	pause_group();
	/* parf is block 1 and phy is block 2; the reference had these two swapped here. */
	get(2, "phy_status", PHY_STATUS);
	get(2, "phy_power_down", PHY_POWER_DOWN);
	get(1, "parf_sys_ctrl", 0x00);
	get(1, "parf_phy_ctrl", 0x40);
	get(1, "parf_phy_refclk", 0x4c);
	get(1, "parf_dbi_base", 0x168);
	get(1, "parf_slv_addr_space_size (2.3.2, 0x16c)", 0x16c);
	get(1, "parf_slv_addr_space_size (2.7.0, 0x358)", 0x358);
	get(1, "parf_ltssm", 0x1b0);
	get(1, "parf_q2a_flush", 0x1ac);
	get(1, "parf_bdf_translate", 0x24c);
	get(3, "elbi_sys_ctrl", 0x04);
	return true;
}

static bool
step_gdsc(void)
{
	uint32_t gdscr, waited;

	announce("switch the controller's own power on: the software collapse bit out of its power switch, then wait for its "
	    "power-on bit");
	gdscr = get(0, "pcie_0_gdscr", GCC_PCIE_0_GDSCR);
	put(0, "pcie_0_gdscr", GCC_PCIE_0_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
	for (waited = 0; waited < 100; waited++) {
		if (rd(GCC(GCC_PCIE_0_GDSCR)) & GDSC_PWR_ON) {
			break;
		}
		sleep_ms(1);
	}
	gdscr = get(0, "pcie_0_gdscr after", GCC_PCIE_0_GDSCR);
	if ((gdscr & GDSC_PWR_ON) == 0) {
		printf("pcie: %u: THE CONTROLLER DID NOT COME ON (gdscr %#x)\n", pcie_step, gdscr);
		return false;
	}
	line("THE CONTROLLER HAS POWER (gdscr %#x, %u ms)", gdscr, waited);
	return true;
}

static bool
step_clocks(void)
{
	announce("vote for its clocks (slv_axi, mstr_axi, cfg_ahb, aux, slv_q2a; the pipe clock comes with the PHY) and read "
	    "the halt state of each");
	put(0, "pcie_0_axi_misc (vote for its clocks)", GCC_PCIE_0_AXI_MISC, rd(GCC(GCC_PCIE_0_AXI_MISC)) | CLOCK_VOTES);
	sleep_ms(1);
	get(0, "pcie_0_axi_misc after", GCC_PCIE_0_AXI_MISC);
	get(0, "pcie_0_slv_axi_halt", GCC_PCIE_0_SLV_AXI_HALT);
	get(0, "pcie_0_mstr_axi_halt", GCC_PCIE_0_MSTR_HALT);
	get(0, "pcie_0_cfg_ahb_halt", GCC_PCIE_0_CFG_AHB_HALT);
	get(0, "pcie_0_aux_halt", GCC_PCIE_0_AUX_HALT);
	get(0, "pcie_0_slv_q2a_halt", GCC_PCIE_0_Q2A_AXI_HALT);
	return true;
}

static bool
step_phy(void)
{
	uint32_t status = 0;

	announce("wake the PHY: its own auxiliary clock on, its power-down word cleared, and its status bit that says it is "
	    "ready");
	put(0, "pcie_phy_aux (the PHY's clock)", GCC_PCIE_PHY_AUX, rd(GCC(GCC_PCIE_PHY_AUX)) | 1u);
	sleep_ms(1);
	get(0, "pcie_phy_aux after", GCC_PCIE_PHY_AUX);
	pause_group();
	get(2, "phy_power_down", PHY_POWER_DOWN);
	get(2, "phy_status", PHY_STATUS);
	put(2, "phy_power_down (clear)", PHY_POWER_DOWN, rd(reg(2, PHY_POWER_DOWN)) & ~1u);
	for (uint32_t waited = 0; waited < 100; waited++) {
		status = rd(reg(2, PHY_STATUS));
		if (status & (1u << 6)) {
			break;
		}
		sleep_ms(1);
	}
	line("the PHY %s (status %#x, bit 6 is its ready)", (status & (1u << 6)) ? "IS READY" : "IS NOT READY", status);
	return true;
}

static bool
step_phy_seq(void)
{
	/* the phone's own sequence, from the device tree's qcom,phy-sequence (offset, value) */
	static const struct {
		uint16_t reg, value;
	} phy_sequence[] = {
	{ 0x840, 0x03 }, { 0x094, 0x08 }, { 0x154, 0x34 }, { 0x16c, 0x08 },
	{ 0x058, 0x0f }, { 0x0a4, 0x42 }, { 0x110, 0x24 }, { 0x11c, 0x03 },
	{ 0x118, 0xb4 }, { 0x10c, 0x02 }, { 0x1bc, 0x11 }, { 0x0bc, 0x82 },
	{ 0x0d4, 0x03 }, { 0x0d0, 0x55 }, { 0x0cc, 0x55 }, { 0x0b0, 0x1a },
	{ 0x0ac, 0x0a }, { 0x0c4, 0x68 }, { 0x0e0, 0x02 }, { 0x0dc, 0xaa },
	{ 0x0d8, 0xab }, { 0x0b8, 0x34 }, { 0x0b4, 0x14 }, { 0x158, 0x01 },
	{ 0x074, 0x06 }, { 0x07c, 0x16 }, { 0x084, 0x36 }, { 0x078, 0x06 },
	{ 0x080, 0x16 }, { 0x088, 0x36 }, { 0x1b0, 0x1e }, { 0x1ac, 0xca },
	{ 0x1b8, 0x18 }, { 0x1b4, 0xa2 }, { 0x050, 0x07 }, { 0x010, 0x01 },
	{ 0x01c, 0x31 }, { 0x020, 0x01 }, { 0x024, 0xde }, { 0x028, 0x07 },
	{ 0x030, 0x4c }, { 0x034, 0x06 }, { 0x29c, 0x12 }, { 0x284, 0x35 },
	{ 0x23c, 0x11 }, { 0x51c, 0x03 }, { 0x518, 0x1c }, { 0x524, 0x1e },
	{ 0x4e8, 0x00 }, { 0x4ec, 0x0e }, { 0x4f0, 0x4a }, { 0x4f4, 0x0f },
	{ 0x5b4, 0x04 }, { 0x434, 0x7f }, { 0x444, 0x70 }, { 0x510, 0x17 },
	{ 0x4d4, 0x04 }, { 0x4d8, 0x07 }, { 0x598, 0xd4 }, { 0x59c, 0x54 },
	{ 0x5a0, 0xdb }, { 0x5a4, 0x3b }, { 0x5a8, 0x31 }, { 0x584, 0x24 },
	{ 0x588, 0xe4 }, { 0x58c, 0xec }, { 0x590, 0x3b }, { 0x594, 0x36 },
	{ 0x570, 0x3f }, { 0x574, 0x3f }, { 0x578, 0xff }, { 0x57c, 0x7f },
	{ 0x580, 0x14 }, { 0x4fc, 0x00 }, { 0x4f8, 0xc0 }, { 0x460, 0x30 },
	{ 0x464, 0x00 }, { 0x5bc, 0x0c }, { 0x4dc, 0x1b }, { 0x408, 0x0c },
	{ 0x414, 0x03 }, { 0x5b8, 0x30 }, { 0x9a4, 0x01 }, { 0xc90, 0x00 },
	{ 0xc40, 0x01 }, { 0xc48, 0x01 }, { 0xc50, 0x00 }, { 0xcb4, 0x33 },
	{ 0xcbc, 0x00 }, { 0xce0, 0x58 }, { 0xca4, 0x0f }, { 0x048, 0x90 },
	{ 0xc1c, 0xc1 }, { 0x988, 0x77 }, { 0x998, 0x0b }, { 0x8dc, 0x0d },
	{ 0x9ec, 0x12 }, { 0x800, 0x00 }, { 0x844, 0x03 },
	};

	announce("write the PHY's own sequence, the pairs the device tree keeps (qcom,phy-sequence: register and value, all of "
	    "them in the PHY's own 4 KB), then wait for its ready bit");
	/*
	 * A reset pulse of the PHY's own two blocks first (not the common one: releasing that one after the PHY's
	 * own stopped the machine), and its auxiliary clock voted again once they are out -- a block reset takes
	 * the clock branch with it.
	 */
	put(0, "pcie_0_phy_bcr (assert)", GCC_PCIE_0_PHY_BCR, 1);
	put(0, "pcie_phy_bcr (assert)", GCC_PCIE_PHY_BCR, 1);
	sleep_ms(2);
	put(0, "pcie_0_phy_bcr (release)", GCC_PCIE_0_PHY_BCR, 0);
	put(0, "pcie_phy_bcr (release)", GCC_PCIE_PHY_BCR, 0);
	sleep_ms(20);
	put(0, "pcie_phy_aux again", GCC_PCIE_PHY_AUX, rd(GCC(GCC_PCIE_PHY_AUX)) | 1u);
	/* its reference generator at the rate the device tree gives it, before its own sequence is written */
	put(0, "phy_refgen_src cfg (100 MHz)", 0x6f018, 0x501);
	put(0, "phy_refgen_src root enable", 0x6f014, 0x2);
	put(0, "pcie0_phy_refgen again", GCC_PCIE_0_PHY_REFGEN, rd(GCC(GCC_PCIE_0_PHY_REFGEN)) | 1u);
	sleep_ms(100);          /* qcom_pcie_phy_power_on() waits before it touches the PHY again */
	get(0, "pcie_phy_aux after the reset", GCC_PCIE_PHY_AUX);
	get(0, "pcie_0_phy_bcr after", GCC_PCIE_0_PHY_BCR);
	/*
	 * The common PHY block's own reset, released only now: it was released before the PHY's own in the run
	 * that ended the machine, so the order here is the other one -- the PHY's own out first, a wait, then this.
	 */
	put(0, "pcie_phy_com_bcr (release)", GCC_PCIE_PHY_COM_BCR, 0);
	sleep_ms(10);
	for (unsigned i = 0; i < sizeof(phy_sequence) / sizeof(phy_sequence[0]); i++) {
		wr(reg(2, phy_sequence[i].reg), phy_sequence[i].value);
	}
	sleep_ms(10);
	{
		uint32_t status = 0;

		for (uint32_t waited = 0; waited < 20; waited++) {
			status = rd(reg(2, PHY_STATUS));
			if (status & (1u << 6)) {
				break;
			}
			sleep_ms(10);
		}
		line("the PHY %s (status %#x, bit 6 is its ready)", (status & (1u << 6)) ? "IS READY" : "DID NOT BECOME READY",
		    status);
	}
	return true;
}

static bool
step_clocks_dump(void)
{
	/* the whole PCIe0 corner of the clock controller: the two RCGs' own registers and everything around them */
	static const struct {
		uint32_t offset;
		const char *name;
	} area[] = {
		{ 0x6b000, "pcie_0_bcr" },
		{ 0x6b004, "pcie_0_gdscr" },
		{ 0x6b010, "q2a_axi_halt" },
		{ 0x6b014, "slv_axi_halt" },
		{ 0x6b01c, "mstr_axi_halt" },
		{ 0x6b024, "cfg_ahb_halt" },
		{ 0x6b028, "aux_halt" },
		{ 0x6b02c, "pipe_halt" },
		{ 0x6b038, "aux_clk_src rcgr" },
		{ 0x6b03c, "aux_clk_src cfg" },
		{ 0x6b040, "aux_clk_src m" },
		{ 0x6b044, "aux_clk_src n" },
		{ 0x6b048, "aux_clk_src d" },
		{ 0x6c000, "pcie_0_link_down area" },
		{ 0x6c01c, "pcie_0_phy_bcr" },
		{ 0x6f000, "pcie_phy_bcr" },
		{ 0x6f004, "phy_aux" },
		{ 0x6f00c, "pcie_phy_cfg_ahb_bcr" },
		{ 0x6f010, "pcie_phy_com_bcr" },
		{ 0x6f014, "phy_refgen_src rcgr" },
		{ 0x6f018, "phy_refgen_src cfg" },
		{ 0x6f02c, "pcie0_phy_refgen" },
		{ 0x52008, "pcie_0_axi_misc" },
		{ 0x5200c, "pcie_0_axi_misc2" },
		{ 0x9000c, "aggre_noc_pcie_tbu" },
	};

	announce("read the whole PCIe0 corner of the clock controller: the two source generators' own registers (the aux "
	    "clock's and the PHY reference generator's), every branch and every reset around them");
	for (unsigned i = 0; i < sizeof(area) / sizeof(area[0]); i++) {
		get(0, area[i].name, area[i].offset);
	}
	return true;
}

static bool
step_aux_src(void)
{
	/*
	 * The aux clock's source generator (gcc_sm8250.c: gcc_pcie_0_aux_clk_src, 0x6b038) was found all zeros, and
	 * its RCGR register is a command register: bit 0 updates the configuration, bit 1 switches the root on. Its
	 * table (ftbl_gcc_pcie_0_aux_clk_src) is the crystal at 19.2 MHz through a divider of one -- source 0, and
	 * the divider field holds the divider minus one.
	 */
	announce("switch the aux clock's own source on: the crystal through a divider of one, the root enable bit in its "
	    "command register, then see that its branches are no longer halted");
	put(0, "aux_clk_src cfg (source 0, divide by 1)", 0x6b03c, 0);
	put(0, "aux_clk_src m", 0x6b040, 0);
	put(0, "aux_clk_src n", 0x6b044, 0);
	put(0, "aux_clk_src d", 0x6b048, 0);
	put(0, "aux_clk_src root enable", 0x6b038, 0x2);
	sleep_ms(2);
	get(0, "aux_clk_src rcgr after", 0x6b038);
	get(0, "aux_halt after", GCC_PCIE_0_AUX_HALT);
	get(0, "cfg_ahb_halt after", GCC_PCIE_0_CFG_AHB_HALT);
	get(0, "mstr_axi_halt after", GCC_PCIE_0_MSTR_HALT);
	get(0, "slv_axi_halt after", GCC_PCIE_0_SLV_AXI_HALT);
	return true;
}

static bool
step_parf(void)
{
	announce("with power, clocks and the PHY awake, read the controller's own registers (before, a read here was a bus "
	    "that never returned)");
	get(1, "parf_sys_ctrl", 0x00);
	get(1, "parf_phy_ctrl", 0x40);
	get(1, "parf_phy_refclk", 0x4c);
	get(1, "parf_dbi_base", 0x168);
	get(1, "parf_slv_addr_space_size (2.3.2, 0x16c)", 0x16c);
	get(1, "parf_slv_addr_space_size (2.7.0, 0x358)", 0x358);
	get(1, "parf_q2a_flush", 0x1ac);
	get(1, "parf_ltssm", 0x1b0);
	get(1, "parf_bdf_translate", 0x24c);
	return true;
}

/*
 * What the vendor driver does to these registers between switching the clocks on and turning the link on
 * (qcom_pcie_init_2_3_2): the PHY's reset line out, the DBI base set to zero, the MAC power-down mux off, the
 * MHI clock reset, and the master's write-address halt. And the PHY's reference generator switched on first:
 * gcc_pcie0_phy_refgen_clk (0x6f02c) hangs off gcc_pcie_phy_refgen_clk_src (0x6f014), which was found with its
 * root switched off -- a branch with a stopped source produces no clock, and the PHY cannot lock without one.
 */
static bool
step_parf_init(void)
{
	uint32_t value;

	announce("the vendor driver's own register writes, and the PHY reference generator switched on");
	/*
	 * The core's own reset pulsed the way qcom_pcie_init_2_7_0 does it (that is the variant for this SoC in
	 * mainline), and then the word that tells the controller it is a root complex rather than an endpoint:
	 * PARF_DEVICE_TYPE, 0x1000 in the block, 4 for RC.
	 */
	put(0, "pcie_0_bcr (assert, the core's own reset)", GCC_PCIE_0_BCR, 1);
	sleep_ms(1);
	put(0, "pcie_0_bcr (release)", GCC_PCIE_0_BCR, 0);
	sleep_ms(2);
	get(0, "pcie_0_bcr after", GCC_PCIE_0_BCR);
	get(1, "parf_device_type before", 0x1000);
	put(1, "parf_device_type (root complex)", 0x1000, 4);
	get(1, "parf_device_type after", 0x1000);

	/*
	 * The PHY node in the device tree asks for this clock at 100 MHz (assigned-clock-rates with
	 * GCC_PCIE0_PHY_REFGEN_CLK): source 1 is GPLL0's main output at 600 MHz through a divider of six,
	 * the divider field holding the divider minus one (gcc-sm8250.c's ftbl_gcc_pcie_phy_refgen_clk_src).
	 */
	put(0, "phy_refgen_src cfg (source 1, divide by 6: 100 MHz)", 0x6f018, 0x501);
	put(0, "phy_refgen_src root enable", 0x6f014, 0x2);
	sleep_ms(2);
	get(0, "phy_refgen_src rcgr after", 0x6f014);
	get(0, "phy_refgen_src cfg after", 0x6f018);
	get(0, "pcie0_phy_refgen after", GCC_PCIE_0_PHY_REFGEN);

	value = get(1, "parf_phy_ctrl", 0x40);
	put(1, "parf_phy_ctrl (bit 0 out)", 0x40, value & ~1u);
	put(1, "parf_dbi_base (zero)", 0x168, 0);
	/*
	 * The controller's system control word as a later vendor variant of this init writes it: the master's and
	 * the slave's AXI clock gating off, and -- this one matters here -- the core clock's gating off too
	 * (CORE_CLK_CGC_DIS, bit 6), together with the L1 clock removal disable. The word was found with bit 6
	 * clear, and the core's own registers then never answered.
	 */
	value = get(1, "parf_sys_ctrl", 0x00);
	put(1, "parf_sys_ctrl (bit 29 out, core clock gating off)", 0x00,
	    (value & ~(1u << 29)) | (1u << 6) | (1u << 1));
	/*
	 * The device tree's qcom,slv-addr-space-size is 0x4000000 (64 MiB), not the
	 * 0x10000000 the reference wrote: 0x358 is the register's own offset here, and
	 * the value has to be the size the DT names.
	 */
	put(1, "parf_slv_addr_space_size", 0x358, 0x4000000);
	get(1, "parf_slv_addr_space_size after", 0x358);
	/*
	 * The two NoC-side clocks the device tree lists besides the six in the vote (clock-names "tbu" and
	 * "ddrss_sf_tbu"): the first was found voted already, the second not.
	 */
	get(0, "aggre_noc_pcie_tbu", GCC_PCIE_TBU);
	put(0, "ddrss_pcie_sf_tbu (bit 0 in)", 0x8d058, rd(GCC(0x8d058)) | 1u);
	get(0, "ddrss_pcie_sf_tbu after", 0x8d058);

	value = get(1, "parf_mhi_clock_reset_ctrl", 0x174);
	put(1, "parf_mhi_clock_reset_ctrl (bit 4 in)", 0x174, value | (1u << 4));
	value = get(1, "parf_axi_mstr_wr_addr_halt_v2", 0x1a8);
	put(1, "parf_axi_mstr_wr_addr_halt_v2 (bit 31 in)", 0x1a8, value | (1u << 31));
	/* Read the clock vote register once more now that the PHY reference generator is running. Step 3
	 * votes CLOCK_VOTES and reads it back; bit 4 (pipe) is deliberately left out there because the pipe
	 * clock comes from the PHY. If it is still clear at the end of step 9, then the controller's AXI
	 * branches are not clocked when step 10 touches the IATU window, and a bus that never returns is
	 * what that predicts. Read only: nothing here changes a vote. */
	get(0, "pcie_0_axi_misc after the PHY reference generator", GCC_PCIE_0_AXI_MISC);
	return true;
}

/*
 * The controller's own configuration space (the DBI window at 0x60000000) and the link control block at 0x60000f20.
 * The ELBI read is last: in the run before this one the log ended on exactly that access, so whatever comes before
 * it is what this step is for.
 */
static bool
step_dbi(void)
{
	announce("ask the controller's own three windows in the order that tells them apart: the address translator, the "
	    "configuration space the buses are seen through, the core's own 4 KB, and the link control block");
	/* One access only: the first one is the one that takes the board down, and knowing
	 * which of the six it is decides whether the mapping or the access rule is wrong. */
	get(4, "iatu_viewport", 0x00);
	return true;
}

/* ---- what runs ------------------------------------------------------------------------------ */

static bool (*const steps[])(void) = {
	step_look,
	step_gdsc,
	step_clocks,
	step_phy,
	step_phy_seq,
	step_clocks_dump,
	step_aux_src,
	step_parf,
	step_parf_init,
	step_dbi,
};

void
alioth_pcie_init(void)
{
	uint32_t steps_wanted = 0, skip = 0;

	PE_parse_boot_argn("pcie", &steps_wanted, sizeof(steps_wanted));
	if (steps_wanted == 0) {
		return;
	}
	risky();                /* everything the boot wrote before this is still in its ring: put it on the medium first */
	PE_parse_boot_argn("pcie_pause", &pcie_pause_ms, sizeof(pcie_pause_ms));
	PE_parse_boot_argn("pcie_skip", &skip, sizeof(skip));
	pcie_skip_steps = skip;
	if (steps_wanted > sizeof(steps) / sizeof(steps[0])) {
		steps_wanted = sizeof(steps) / sizeof(steps[0]);
	}
	line("the Wi-Fi's controller at 0x1c00000, steps up to %u, without those in mask %#x", steps_wanted, skip);
	/*
	 * A pause here, so that the loader (which writes the end of this log to storage every quarter of
	 * a second) has this line before any of the controller is touched: a step that hangs the machine
	 * takes whatever was not written down with it.
	 */
	sleep_ms(300);
	if (!map_all()) {
		return;
	}
	line("the blocks are mapped");
	pcie_last_step = steps_wanted;
	{
		uint32_t delay_ms = 0;

		/*
		 * pcie_delay=MS: wait first. The loader writes the end of this log down every quarter second for its
		 * first 24 seconds (then it hands the interrupt controller to the system), so a step that takes its time
		 * is best started well inside that window rather than in the first millisecond of the boot.
		 */
		PE_parse_boot_argn("pcie_delay", &delay_ms, sizeof(delay_ms));
		if (delay_ms) {
			printf("pcie: waiting %u ms before the steps\n", delay_ms);
			sleep_ms(delay_ms);
		}
	}
	{
		uint32_t threaded = 0;
		thread_t thread;

		/*
		 * pcie_thread=1: the steps run on a thread of their own instead of the one the system is booting
		 * on. An access to the controller that never returns then only costs that thread -- the boot goes
		 * on, the log is written out, and what it printed before the access is in it. On the boot's own
		 * processor the same access takes the whole log with it.
		 */
		PE_parse_boot_argn("pcie_thread", &threaded, sizeof(threaded));
		if (threaded) {
			kern_return_t started = kernel_thread_start(pcie_steps_thread, NULL, &thread);

			printf("pcie: pcie_thread=1: the thread to run the steps on %s (%d)\n",
			    started == KERN_SUCCESS ? "started" : "DID NOT START", started);
			if (started == KERN_SUCCESS) {
				uint32_t wait_ms = 3000;

				thread_deallocate(thread);
				/*
				 * The boot goes on without the steps, but not instantly: it reaches launchd, and a kernel with
				 * no system image panics there, some 300 ms after this -- which would cut the thread off after
				 * its first two accesses. So give it the time it needs first (pcie_wait=MS).
				 */
				PE_parse_boot_argn("pcie_wait", &wait_ms, sizeof(wait_ms));
				printf("pcie: letting the thread have %u ms before the boot goes on\n", wait_ms);
				sleep_ms(wait_ms);
				return;
			}
		}
	}
	run_steps();
}

static void
pcie_steps_thread(void *argument, wait_result_t wait_result)
{
	(void)argument;
	(void)wait_result;
	if (!map_all()) {
		thread_terminate(current_thread());
	}

	run_steps();
	printf("pcie: the thread that ran the steps is done\n");
	thread_terminate(current_thread());
}

static void
run_steps(void)
{
	for (pcie_step = 1; pcie_step <= pcie_last_step; pcie_step++) {
		if (pcie_skip_steps & 1u << pcie_step) {
			continue;
		}
		if (!steps[pcie_step - 1]()) {
			line("step %u: FAILED; the steps after it are not attempted", pcie_step);
			return;
		}
		line("step %u: done", pcie_step);
	}
	line("steps done; the controller is left as it was found");
}
#endif /* ARM64_BOARD_CONFIG_SWEET */

/* ---- board shim ---------------------------------------------------------------------------- */
/* The driver asks the ios-sweet log to reach the medium before it touches the controller, so a
 * step that hangs the machine does not take the preceding lines with it. This board has no such
 * log; the persistent log writer owns that job here, and it flushes on its own cadence. */
int
sweet_log_flush(void)
{
	return 0;
}
