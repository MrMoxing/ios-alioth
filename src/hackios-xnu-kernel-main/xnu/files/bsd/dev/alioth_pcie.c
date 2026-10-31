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
 *   conf   0x60100000 + 0x100000 (the configuration space, bus 0 onwards)
 *   memory 0x60300000 + 0x3d00000, i/o 0x60200000 + 0x100000 (both the same on the bus side)
 *   perst-gpio is TLMM 0x4f (79), wake-gpio 0x51 (81); root complex 0
 *
 * From the clock controller (gcc-sm8250.c; all of these are offsets from its 0x100000):
 *   pcie_0_gdscr 0x6b004 (the controller's own power switch), the five clock votes in
 *   gcc_pcie_0_axi_clk_misc 0x52008 (bit 0 slv_axi, 1 mstr_axi, 2 cfg_ahb, 3 aux, 4 pipe, 5
 *   slv_q2a_axi), the phy's own two clocks at 0x6f004 and 0x6f02c, the bus clock at 0x9000c,
 *   and the two block resets 0x6b000 (core) and 0x6c01c (phy).
 *
 * The steps are how this is to be found out, in the order the vendor driver takes them, and one that
 * fails says so and stops the sequence (the table of them is at the bottom of this file):
 *   1  reads   what the boot loader and Android left: the power switch, the clock votes and their halt
 *              state, the resets, the PHY's status word, and the endpoint's reset line (PERST)
 *   2-3  assert PERST (the endpoint held in reset), switch the controller's own power on and vote for
 *              its six clocks
 *   4  switches the aux clock's source generator on (the wrapper's own register access depends on it)
 *   5  the vendor's register writes: the core reset, root-complex mode, the reference generator, the DBI
 *              base (0x350: this IP revision's offset), the system and power-management words, the NoC
 *              clocks, the MHI clock reset
 *   6-7  the PHY: its auxiliary clock, its power-down word, its 99 pairs, its ready bit
 *   8-9  read the whole clock corner, then the wrapper's registers
 *   10 reads   the core's own windows: configuration space, address translator, DBI, ELBI
 *   11 release PERST, turn the link training state machine on and see whether the core moves its state
 *
 * Boot-arg pcie=N performs steps 1 to N and stops (pcie_skip=MASK leaves out steps by 1<<N);
 * pcie_pause=MS is how long a step waits after announcing a group of accesses (default 2200).
 */
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
extern void loader_log_line(const char *line);
extern void loader_log_flush(void);

static uint32_t pcie_last_step, pcie_step, pcie_pause_ms = 2200;
static uint32_t pcie_skip_steps;

static void run_steps(void);
static void pcie_steps_thread(void *argument, wait_result_t wait_result);
static void line(const char *fmt, ...);

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
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

#define BLOCK_NAMES 8
static struct {
	const char *name;
	uint64_t base;
	uint32_t size;
	vm_offset_t va;
} blocks[BLOCK_NAMES] = {
	{ "gcc", 0x00100000, 0x00200000 },
	{ "parf", 0x01c00000, 0x3000 },
	{ "phy", 0x01c06000, 0x1000 },
	{ "elbi", 0x60000f20, 0xa8 },
	{ "iatu", 0x60001000, 0x1000 },
	{ "conf", 0x60100000, 0x100000 },
	{ "dbi", 0x60000000, 0x1000 },   /* the core's own 4 KB: DBI at its start, ELBI at 0xf20 */
	{ "tlmm-north", 0x0f900000, 0x90000 },  /* gpio79 (PERST) is in the north tile: 0x1000 * 79 */
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

/*
 * PERST: the endpoint's reset line is TLMM gpio 79, in the north tile (pinctrl-kona.c: north is
 * 0x0f900000 and a pin's registers are at tile + 0x1000 * pin). Its registers are the msm layout:
 * CTL holds pull [1:0], function [5:2], drive [8:6] and the output-enable bit 9; IO holds the input
 * (bit 0) and the output level (bit 1). The device tree asks for the line active high in the pin flags,
 * but the vendor driver's table has perst on = 0 -- asserted is a low level, released a high one.
 */
#define PERST_GPIO              79
#define PERST_CFG               (reg(7, 0x1000UL * PERST_GPIO))
#define PERST_IO                (reg(7, 0x1000UL * PERST_GPIO + 4))
#define PERST_CTL_MASK          ((0xfu << 2) | 3u | (7u << 6) | (1u << 9))

static void
perst_set(bool high)
{
	wr(PERST_IO, high ? 2 : 0);     /* the level first; the pin must never drive the wrong one */
}

/* Hold the endpoint in reset: drive it low, then make the pin an output (gpio mode, 4 mA). */
static void
perst_assert(void)
{
	uint32_t cfg = rd(PERST_CFG), io = rd(PERST_IO);

	line("PERST (gpio79, north) before: cfg %#x, io %#x", cfg, io);
	perst_set(false);
	wr(PERST_CFG, (cfg & ~PERST_CTL_MASK) | (1u << 6) | (1u << 9));
	line("PERST asserted (low): cfg %#x, io %#x", rd(PERST_CFG), rd(PERST_IO));
}

/* Bring the endpoint out of reset: drive it high. */
static void
perst_release(void)
{
	perst_set(true);
	line("PERST released (high): io %#x", rd(PERST_IO));
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
 * Every line of this driver goes into the loader's own log (loader_return.c's loader_log_line, the channel
 * the touch driver's "tl:" lines use), and is written down to memory at once: the loader reads its end from
 * another processor every quarter of a second, and a processor that stops on an access that never returns
 * takes whatever it printed last with it unless it has been cleaned out of its cache.
 */
static void
line(const char *fmt, ...)
{
	static char buffer[224];
	__builtin_va_list ap;
	int n, m;

	n = snprintf(buffer, sizeof(buffer), "pcie: %u: ", pcie_step);
	if (n < 0) {
		return;
	}
	if ((unsigned)n > sizeof(buffer) - 2) {
		n = sizeof(buffer) - 2;
	}
	__builtin_va_start(ap, fmt);
	m = vsnprintf(buffer + n, sizeof(buffer) - n - 1, fmt, ap);
	__builtin_va_end(ap);
	if (m > 0) {
		n += m;
		if (n > (int)sizeof(buffer) - 2) {
			n = sizeof(buffer) - 2;
		}
	}
	buffer[n++] = '\n';
	buffer[n] = 0;
	loader_log_line(buffer);
	loader_log_flush();
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
	value = rd(address);
	line("%s %s (%#llx) = %#x", blocks[which].name, name, address, value);
	return value;
}

static void
put(int which, const char *name, uint32_t offset, uint32_t value)
{
	uint64_t address = reg(which, offset);

	line("about to write %s %s at %#llx: %#x", blocks[which].name, name, address, value);
	wr(address, value);
}

/* Where the clock controller's registers are, as an address (its block base plus the offset). */
#define GCC(off)                reg(0, (off))

/*
 * The SoC's own clock measure (the vendor's clk-debug.c; the GPU driver here uses the same registers):
 * one of the clock controller's clocks is put on a debug output, the global counter counts it against
 * the crystal divided by four, and the count over a known number of those ticks is the rate. The
 * numbers that name each clock are from the vendor's debugcc-kona.c.
 */
#define GCC_DEBUG_MUX           0x62000         /* [9:0] which clock */
#define GCC_DEBUG_DIV           0x62004         /* [3:0] divider - 1: this path divides by two */
#define GCC_DEBUG_CBCR          0x62008
#define GCC_MEASURE_CTL         0x62038
#define GCC_MEASURE_STATUS      0x6203c
#define GCC_XO_DIV4_CBCR        0x4300c
#define MEASURE_RUN             (1u << 20)
#define MEASURE_CLEAR           (1u << 21)
#define MEASURE_DONE            (1u << 25)
#define MEASURE_FULL            0x20000u
#define MEASURE_FAILED          0xffffffffu

#define CLK_CPUSS_AHB           0x0e0           /* a clock that is certainly running: the control */
#define CLK_PCIE0_PIPE          0x0f7
#define CLK_PCIE0_AUX           0x0f6
#define CLK_PCIE0_CFG_AHB       0x0f5
#define CLK_PCIE0_MSTR_AXI      0x0f4
#define CLK_PCIE0_SLV_AXI       0x0f3
#define CLK_PCIE_PHY_AUX        0x102

static uint32_t
count_ticks(uint32_t ticks)
{
	uint32_t control = rd(GCC(GCC_MEASURE_CTL)) & ~(MEASURE_RUN | MEASURE_CLEAR | 0xfffffu);
	uint32_t status = 0;

	wr(GCC(GCC_MEASURE_CTL), control | MEASURE_CLEAR);
	sleep_us(10);
	wr(GCC(GCC_MEASURE_CTL), control | ticks);
	wr(GCC(GCC_MEASURE_CTL), control | ticks | MEASURE_RUN);
	for (int i = 0; i < 2000 && !(status & MEASURE_DONE); i++) {
		sleep_us(100);
		status = rd(GCC(GCC_MEASURE_STATUS));
	}
	wr(GCC(GCC_MEASURE_CTL), control);
	return (status & MEASURE_DONE) ? (status & 0x1ffffff) : MEASURE_FAILED;
}

/* The rate in Hz of one of the clock controller's clocks; 0 if it does not run. Everything is put back. */
static uint32_t
measure_gcc_clock(uint32_t select)
{
	uint32_t mux = rd(GCC(GCC_DEBUG_MUX)), div = rd(GCC(GCC_DEBUG_DIV)), cbcr = rd(GCC(GCC_DEBUG_CBCR));
	uint32_t base = rd(GCC(GCC_XO_DIV4_CBCR)), brief, full;

	wr(GCC(GCC_DEBUG_MUX), (mux & ~0x3ffu) | select);
	wr(GCC(GCC_DEBUG_DIV), (div & ~0xfu) | 1u);
	wr(GCC(GCC_DEBUG_CBCR), cbcr | 1u);
	wr(GCC(GCC_XO_DIV4_CBCR), base | 1u);
	/* a counter whose input stands still keeps its last count: the two counts differ only if it runs */
	brief = count_ticks(0x1000);
	full = brief == MEASURE_FAILED ? MEASURE_FAILED : count_ticks(MEASURE_FULL);
	wr(GCC(GCC_XO_DIV4_CBCR), base);
	wr(GCC(GCC_DEBUG_CBCR), cbcr);
	wr(GCC(GCC_DEBUG_DIV), div);
	wr(GCC(GCC_DEBUG_MUX), mux);
	if (full == MEASURE_FAILED) {
		return MEASURE_FAILED;
	}
	if (full == brief) {
		return 0;
	}
	/* the crystal over four is the time base; the divider on the way to the counter is two */
	return (uint32_t)(((uint64_t)full * 4800000 * 2) / MEASURE_FULL);
}

static bool
map_all(void)
{
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
/*
 * Every branch in gcc_pcie_0_axi_clk_misc (gcc-sm8250.c): bit 0 slv_axi, 1 mstr_axi, 2 cfg_ahb, 3 aux,
 * 4 pipe, 5 slv_q2a. Bit 4 is the one the core itself runs on -- it is the PHY's pipe clock coming back
 * through the controller -- and with it out the core never answered a single register: reads of the DBI,
 * the address translator and the link block sat there until the machine was reset, while every read of
 * the wrapper's own PARF block came back.
 */
#define CLOCK_VOTES             0x3fu

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
	get(1, "phy_status", PHY_STATUS);
	get(1, "phy_power_down", PHY_POWER_DOWN);
	get(2, "parf_sys_ctrl", 0x00);
	get(2, "parf_phy_ctrl", 0x40);
	get(2, "parf_phy_refclk", 0x4c);
	get(2, "parf_dbi_base_v2 (0x350: this IP revision)", 0x350);
	get(2, "parf_dbi_base_v1 (0x168: what the driver used before)", 0x168);
	get(2, "parf_slv_addr_space_size_v2", 0x358);
	get(2, "parf_ltssm", 0x1b0);
	get(2, "parf_q2a_flush", 0x1ac);
	get(2, "parf_bdf_translate", 0x24c);
	get(3, "elbi_sys_ctrl", 0x04);
	get(7, "perst_gpio79 cfg", 0x1000 * PERST_GPIO);
	get(7, "perst_gpio79 io", 0x1000 * PERST_GPIO + 4);
	return true;
}

static bool
step_gdsc(void)
{
	uint32_t gdscr, waited;

	announce("hold the endpoint in reset (PERST low), switch the controller's own power on: the software collapse bit "
	    "out of its power switch, then wait for its power-on bit");
	perst_assert();
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
	announce("vote for its clocks (slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a) and read the halt state of each");
	put(0, "pcie_0_axi_misc (vote for its clocks)", GCC_PCIE_0_AXI_MISC, rd(GCC(GCC_PCIE_0_AXI_MISC)) | CLOCK_VOTES);
	sleep_ms(1);
	get(0, "pcie_0_axi_misc after", GCC_PCIE_0_AXI_MISC);
	get(0, "pcie_0_slv_axi_halt", GCC_PCIE_0_SLV_AXI_HALT);
	get(0, "pcie_0_mstr_axi_halt", GCC_PCIE_0_MSTR_HALT);
	get(0, "pcie_0_cfg_ahb_halt", GCC_PCIE_0_CFG_AHB_HALT);
	get(0, "pcie_0_aux_halt", GCC_PCIE_0_AUX_HALT);
	get(0, "pcie_0_pipe_halt", GCC_PCIE_0_PIPE_HALT);
	get(0, "pcie_0_slv_q2a_halt", GCC_PCIE_0_Q2A_AXI_HALT);
	/*
	 * The halt registers cannot say whether a clock runs; the SoC can. cpuss_ahb is the control: it is
	 * certainly running, so if it does not read as a rate the measuring itself is wrong. A zero among the
	 * PCIe clocks means that clock is not running at all -- for the two AXI clocks that would be the
	 * NoC behind them, not this controller.
	 */
	line("measured: cpuss_ahb %u Hz, pcie slv_axi %u, mstr_axi %u, cfg_ahb %u, aux %u, pipe %u",
	    measure_gcc_clock(CLK_CPUSS_AHB), measure_gcc_clock(CLK_PCIE0_SLV_AXI),
	    measure_gcc_clock(CLK_PCIE0_MSTR_AXI), measure_gcc_clock(CLK_PCIE0_CFG_AHB),
	    measure_gcc_clock(CLK_PCIE0_AUX), measure_gcc_clock(CLK_PCIE0_PIPE));
	return true;
}

static bool
step_phy(void)
{
	uint32_t status;

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
	/*
	 * And which reference the PHY takes: PARF_PHY_REFCLK's bit 12 is USE_PAD in mainline's header -- the PHY
	 * looking for its reference on a pad. This board clocks the PHY from its own reference generator, so the
	 * bit is cleared before the sequence is written.
	 */
	{
		uint32_t value = get(1, "parf_phy_refclk", 0x4c);

		put(1, "parf_phy_refclk (pad reference off)", 0x4c, value & ~(1u << 12));
		get(1, "parf_phy_refclk after", 0x4c);
	}
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
	/* the last two pairs are the start control (0x844) and the word before it: what the PLL was told to do */
	get(2, "phy_start_control", 0x844);
	get(2, "phy_800", 0x800);
	get(2, "phy_9a4", 0x9a4);
	get(2, "phy_8dc", 0x8dc);
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
	/* now that the PHY is up as well, the same measurement: the pipe clock comes from it */
	line("measured after the PHY: cpuss_ahb %u Hz, slv_axi %u, mstr_axi %u, cfg_ahb %u, aux %u, pipe %u, phy_aux %u",
	    measure_gcc_clock(CLK_CPUSS_AHB), measure_gcc_clock(CLK_PCIE0_SLV_AXI),
	    measure_gcc_clock(CLK_PCIE0_MSTR_AXI), measure_gcc_clock(CLK_PCIE0_CFG_AHB),
	    measure_gcc_clock(CLK_PCIE0_AUX), measure_gcc_clock(CLK_PCIE0_PIPE),
	    measure_gcc_clock(CLK_PCIE_PHY_AUX));
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
	get(1, "parf_dbi_base_v2 (0x350)", 0x350);
	get(1, "parf_slv_addr_space_size_v2", 0x358);
	get(1, "parf_q2a_flush", 0x1ac);
	get(1, "parf_ltssm", 0x1b0);
	get(1, "parf_bdf_translate", 0x24c);
	get(7, "perst_gpio79 io (still asserted)", 0x1000 * PERST_GPIO + 4);
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
	value = get(1, "parf_phy_refclk", 0x4c);
	put(1, "parf_phy_refclk (pad reference off)", 0x4c, value & ~(1u << 12));
	/*
	 * The DBI base register's offset depends on the controller's IP revision: 0x168 in the older one,
	 * 0x350 in this SoC's (the vendor driver's PCIE20_PARF_DBI_BASE_ADDR, and pcie-qcom.c's 2.7.0
	 * variant). The run before this one wrote the older offset, which on this core lands in another
	 * register; the vendor sets it to zero at this point in the sequence.
	 */
	put(1, "parf_dbi_base_v2 (zero, at 0x350)", 0x350, 0);
	get(1, "parf_dbi_base_v1 (legacy offset, read only)", 0x168);
	/*
	 * Exactly what qcom_pcie_init_2_7_0 -- the variant this SoC uses in mainline -- does with these two:
	 * bit 29 of the system control word (the MAC's PHY power-down mux) out, and the power management word's
	 * REQ_NOT_ENTR_L1 (bit 5) out, which allows L1 entry.
	 */
	value = get(1, "parf_sys_ctrl", 0x00);
	put(1, "parf_sys_ctrl (bit 29 out)", 0x00, value & ~(1u << 29));
	value = get(1, "parf_pm_ctrl", 0x20);
	put(1, "parf_pm_ctrl (L1 entry allowed)", 0x20, value & ~(1u << 5));
	/*
	 * The inbound address space: this phone's device tree gives 0x4000000, and this IP revision keeps
	 * the size at 0x358 (the older core had it at 0x16c, read only here).
	 */
	get(1, "parf_slv_addr_space_size_v1 (legacy offset, read only)", 0x16c);
	put(1, "parf_slv_addr_space_size_v2", 0x358, 0x4000000);
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
	/*
	 * The configuration space first: it is the one the system would enumerate through, and it is a window of
	 * its own (0x60100000) rather than part of the core's 4 KB. The address translator and the core's own 4 KB
	 * follow; the link control block is last, since that is the access the run before this one ended on.
	 */
	announce("ask the controller's windows in the order that tells them apart: the configuration space the buses are "
	    "seen through, the address translator, the core's own 4 KB, and the link control block");
	get(5, "conf_vendor_device (bus 0, device 0, function 0)", 0x00);
	get(5, "conf_header_type", 0x0c);
	get(5, "conf_capability", 0x34);
	get(4, "iatu_viewport", 0x00);
	get(6, "dbi_vendor_device", 0x00);
	get(6, "dbi_capability", 0x70);
	get(3, "elbi_sys_ctrl", 0x04);
	return true;
}

/*
 * Turn the link training state machine on and look at what the core writes back into the wrapper's own copy
 * of its state (PARF_LTSSM): if the low bits move, the core is running even though the window it serves never
 * answered; if they stay where they were, the core itself has no clock or no power.
 */
static bool
step_ltssm(void)
{
	uint32_t before, after;

	announce("bring the endpoint out of reset (PERST high), then turn the link training state machine on and see "
	    "whether the core moves its state");
	perst_release();
	sleep_us(1000);         /* the vendor waits a millisecond after the reset line goes up */
	before = get(1, "parf_ltssm", 0x1b0);
	put(1, "parf_ltssm (training on)", 0x1b0, before | (1u << 8));
	sleep_ms(50);
	after = get(1, "parf_ltssm after", 0x1b0);
	line("the core's link state %s (before %#x, after %#x)",
	    ((after ^ before) & 0x1fu) ? "MOVED" : "DID NOT MOVE", before, after);
	return true;
}

/* ---- what runs ------------------------------------------------------------------------------ */

/*
 * In the order the vendor driver's host init takes them: the controller's clocks and its own registers first
 * (qcom_pcie_init_2_7_0), then the PHY's power-on and its sequence (phy_power_on), then the pipe clock and the
 * rest -- which is why the PHY steps come after the registers now.
 */
static bool (*const steps[])(void) = {
	step_look,              /* 1  read what the boot loader and Android left */
	step_gdsc,              /* 2  the controller's own power switch */
	step_clocks,            /* 3  the six clock branches in 0x52008 */
	step_aux_src,           /* 4  the aux clock's source generator (the wrapper's APB clock) */
	step_parf_init,         /* 5  the vendor's register writes, the core reset, the reference generator */
	step_phy,               /* 6  the PHY's auxiliary clock and its power-down word */
	step_phy_seq,           /* 7  the PHY's own 99 pairs and its ready bit */
	step_clocks_dump,       /* 8  the whole PCIe0 corner of the clock controller */
	step_parf,              /* 9  the wrapper's registers, once everything above is on */
	step_dbi,               /* 10 the core's own windows: conf, iATU, DBI, ELBI */
	step_ltssm,             /* 11 turn the link training state machine on and see if the core moves */
};

void
alioth_pcie_init(void)
{
	uint32_t steps_wanted = 0, skip = 0;

	PE_parse_boot_argn("pcie", &steps_wanted, sizeof(steps_wanted));
	if (steps_wanted == 0) {
		return;
	}
	loader_log_flush();     /* everything the boot wrote before this is still in its cache: put it in memory first */
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
	line("the six blocks are mapped");
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
	map_all();
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
