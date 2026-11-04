/*
 * The Wi-Fi's PCIe controller on the Redmi K40 (alioth), poked one step at a time.
 *
 * The K40's radio is a QCA6390 on PCIe0 (vendor 0x17cb, device 0x1101), behind the root complex
 * qcom,pcie@1c00000 -- not the AHB radio (WCN6750) the rest of this tree's Wi-Fi driver was
 * written for (redwood_wifi.c). Everything below is this phone's: the device tree's node
 * qcom,pcie@1c00000 (reg-names parf, phy, dm_core, elbi, iatu, conf), the clock controller's
 * PCIe0 entries (gcc-sm8250.c) and the PCIe block's offsets (the vendor 4.19 tree's pci-msm.c,
 * the driver this phone actually runs: msm_pcie_enable, msm_pcie_link_train).
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
 * The steps are how this is to be found out, in the order the vendor driver takes them (pci-msm.c's
 * msm_pcie_enable then msm_pcie_link_train), and one that fails says so and stops the sequence (the
 * table of them is at the bottom of this file):
 *   1  reads   what the boot loader and Android left: the power switch, the clock votes and their halt
 *              state, the resets, the PHY's status word, and the endpoint's reset line (PERST)
 *   2  votes   for the PHY's power: the three rails the device tree gives the controller's node
 *              (pm8150_l9 at 1.2 V, pm8150_l5 at 0.88 V, cx at nominal), through RPMh
 *   3-4  raise WLAN_EN (gpio 20, the radio's own enable line), assert PERST, switch the controller's own
 *              power on, vote for its six clocks and for the endpoint's clock reference (0x8c004)
 *   5  switches the aux clock's source generator on (the wrapper's own register access depends on it)
 *   6  the vendor's register writes: the core reset, root-complex mode, the reference generator, the DBI
 *              base (0x350: this IP revision's offset), the system and power-management words, the NoC
 *              clocks, the MHI clock reset
 *   7-8  the PHY: its auxiliary clock, its 99 pairs, its reset pulse, its ready bit
 *   9-10  read the whole clock corner, then the wrapper's registers
 *   11 brings  the link up in the vendor's order: PERST out, the training registers in, the state machine
 *              on, and the vendor's three checks polled together
 *   12-13 the endpoint: its config space (vendor/device, the BAR probed and assigned), the root
 *              complex's iatu region 0 pointed at it 1:1, and the MHI/BHI registers behind the BAR
 *   14 the  firmware: amss20.bin (the phone's own, from fw-gpu/ into the image's fw/) downloaded
 *              through BHI -- address, size, trigger, and the status machine waited for
 *   15-17 the MHI proper (mhi_pm.c's mhi_ready_state_transition, mhi_init.c's mhi_init_mmio): the
 *              firmware's boot watched to the AMSS execution environment and the READY bit, one piece
 *              of physical memory for the context arrays and the rings, the MMIO base registers
 *              programmed, the event rings' doorbells rung, and the move to M0 polled for
 *
 * Nothing touches the core's own windows (0x60000000) before step 11 has the endpoint out of reset: a
 * DBI access with PERST held took the machine down twice -- 2026-11-03 here, 2026-10-30 in the UniXNU
 * record of the same phone.
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
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
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
	/*
	 * A pause after EVERY step, not only the last one: the loader writes the log out every
	 * two seconds, so a machine that dies inside a step takes its own announcements with it.
	 * With the pause, the lines of each step reach the logdump before the next step runs,
	 * and the last flushed line names the access that killed the machine (2026-11-03, the
	 * deaths at four and a half seconds left no pcie line at all).
	 */
	if (pcie_pause_ms) {
		sleep_ms(pcie_pause_ms);
	}
}

#define BLOCK_NAMES 9
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
	/*
	 * The TLMM: the device tree gives it 0x0f000000 + 16 MB, and the vendor's pinctrl-kona.c puts a
	 * pin's registers at a tile base (north 0x900000, south 0x500000, west 0x100000) plus 0x1000 * pin.
	 * The tiles interleave -- gpio 0-3 and 24-27 south, 4-23 and 77-96 north -- so a pin that is not of
	 * a tile is a hole in it, and the first read of a hole took the machine down twice (2026-11-03: the
	 * south read of gpio 20 at 0x0f514000, then the flat 0x0f114000). Both of our pins are north:
	 * WLAN_EN gpio 20 at 0x0f914000 and PERST gpio 79 at 0x0f94f000.
	 */
	{ "tlmm-north", 0x0f900000, 0x90000 },
	/*
	 * The endpoint's BAR0 window: the iatu covers the whole MB the BAR decodes, but only the first
	 * 64 KB are mapped here -- the MHI registers (4 KB), the BHI block behind BHIOFF and the device's
	 * BHI_EXECENV at 0xe228 all live inside them. The io_submap is eight MB and the other eight blocks
	 * take 3.6 MB of it: mapping the full MB here panicked with "kmem_alloc(1048576) failed" before a
	 * single step ran (p26, 2026-11-04).
	 */
	{ "mhi", 0x60300000, 0x10000 },
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

/*
 * WLAN_EN: the radio's own enable line, TLMM gpio 20 -- which pinctrl-kona.c puts in the NORTH tile
 * (PINGROUP(20, NORTH, qup7, ...); the tiles interleave, 4-23 north), so its registers are at
 * 0x0f900000 + 0x1000 * 20 = 0x0f914000. The first version had it in the south tile (the SDM845-style
 * "gpio 0-76 south" reading), and the first read of that address took the machine down (p20, p21,
 * 2026-11-03). The cnss driver's cnss_wlan_en_active asks for gpio mode, 16 mA drive, pull-up and
 * output high, and raises the line before anything on the PCIe side happens: the QCA6390 does not
 * come out of its own reset until it. The UniXNU record of this phone measured the pair
 * PERST#/WLAN_EN never written by the ten steps it inherited, and no link ever trained.
 */
#define WLANEN_GPIO             20
#define WLANEN_CFG              (reg(7, 0x1000UL * WLANEN_GPIO))
#define WLANEN_IO               (reg(7, 0x1000UL * WLANEN_GPIO + 4))

static void
wlanen_raise(void)
{
	uint32_t cfg = rd(WLANEN_CFG), io = rd(WLANEN_IO);

	line("WLAN_EN (gpio20, north) before: cfg %#x, io %#x", cfg, io);
	wr(WLANEN_IO, 0);       /* the level first, as with PERST */
	wr(WLANEN_CFG, (cfg & ~PERST_CTL_MASK) | (7u << 6) | 3u | (1u << 9));   /* gpio mode, 16 mA, pull-up, out */
	wr(WLANEN_IO, 2);
	line("WLAN_EN raised (high): cfg %#x, io %#x", rd(WLANEN_CFG), rd(WLANEN_IO));
}

/*
 * The PHY's own power: it has no GDSC of its own, but three rails the device tree gives the controller's
 * node (vreg-1p8-supply, vreg-0p9-supply, vreg-cx-supply) and nobody has voted for: pm8150_l9 at 1.2 V,
 * pm8150_l5 at 0.88 V, both in the high-power mode the tree's init-mode 4 names, and the cx rail at level
 * 256, nominal. The votes go the way redwood_cpufreq.c's memory bandwidth vote goes: through the
 * application processor's RPMh sequencer (dt: rsc@18200000, the operating system's DRV 2), the resources
 * found by name in the command database the boot loader left at 0x80860000. A VRM resource is four words
 * at its address -- the voltage in millivolts, the enable, the mode, the headroom (the vendor's
 * rpmh-regulator.c) -- and a level resource is one: the level asked for.
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *pcie_cmd_db;

static uint32_t
cmd_db_address(const char *name)
{
	char id[8] = { 0 };

	if (pcie_cmd_db == NULL) {
		pcie_cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	if (pcie_cmd_db == NULL ||
	    pcie_cmd_db->magic[0] != 0xdb || pcie_cmd_db->magic[1] != 0x30 ||
	    pcie_cmd_db->magic[2] != 0x03 || pcie_cmd_db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &pcie_cmd_db->slave[s];
		unsigned long long base, room, entries;

		/* every read here stays inside the 128 KiB the mapping gives: a read past it takes the machine */
		if (slave->id == 0 || slave->count == 0 || slave->count > 512) {
			continue;
		}
		base = (unsigned long long)(uintptr_t)pcie_cmd_db->data + slave->header_offset;
		if (base < (unsigned long long)(uintptr_t)pcie_cmd_db->data ||
		    base >= (unsigned long long)(uintptr_t)pcie_cmd_db->data + CMD_DB_SIZE) {
			continue;
		}
		room = (unsigned long long)(uintptr_t)pcie_cmd_db->data + CMD_DB_SIZE - base;
		entries = room / sizeof(struct cmd_db_entry);
		if (entries > slave->count) {
			entries = slave->count;
		}
		{
			const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(uintptr_t)base;

			for (unsigned i = 0; i < entries; i++) {
				if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
					return entry[i].address;
				}
			}
		}
	}
	return 0;
}

#define RSC_PHYS                0x18220000UL    /* the application processor's RPMh sequencer, DRV 2 */
#define RSC_SIZE                0x10000UL
#define RSC_TCS(reg)            (0xd00 + (reg)) /* command slot set 0, an "active" one, as redwood_cpufreq.c */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* its flags; then its address, its value, its status */
#define RPMH_WRITE_WAITED       (0x10008u | 0x100)      /* a 4-byte write, with a response */

/* Up to `count` writes through command slot set 0, and the wait until RPMh has done them. */
static bool
rpmh_write(vm_offset_t rsc, const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(rsc + RSC_TCS(TCS_IRQ_ENABLE));
	bool done = false;

	for (int i = 0; rd(rsc + RSC_TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			line("the RPMh command slots stay busy");
			return false;
		}
		sleep_us(100);
	}
	wr(rsc + RSC_TCS(TCS_CONTROL), rd(rsc + RSC_TCS(TCS_CONTROL)) & ~((1u << 24) | (1u << 16)));
	wr(rsc + RSC_TCS(0x08), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(rsc + RSC_TCS(TCS_COMMAND(i)), RPMH_WRITE_WAITED);
		wr(rsc + RSC_TCS(TCS_COMMAND(i) + 4), address[i]);
		wr(rsc + RSC_TCS(TCS_COMMAND(i) + 8), data[i]);
	}
	wr(rsc + RSC_TCS(0x10), (1u << count) - 1);
	wr(rsc + RSC_TCS(TCS_ENABLED), (1u << count) - 1);
	wr(rsc + RSC_TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(rsc + RSC_TCS(TCS_CONTROL), 1u << 16);
	wr(rsc + RSC_TCS(TCS_CONTROL), (1u << 16) | (1u << 24));
	for (int i = 0; i < 2000 && !done; i++) {
		done = (rd(rsc + RSC_TCS(TCS_IRQ_STATUS)) & 1) != 0;
		if (!done) {
			sleep_us(100);
		}
	}
	wr(rsc + RSC_TCS(TCS_CONTROL), rd(rsc + RSC_TCS(TCS_CONTROL)) & ~((1u << 24) | (1u << 16)));
	wr(rsc + RSC_TCS(TCS_ENABLED), 0);
	wr(rsc + RSC_TCS(0x10), 0);
	wr(rsc + RSC_TCS(0x08), 1);
	wr(rsc + RSC_TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

static bool
step_power(void)
{
	static const struct {
		const char *name, *what;
		bool vrm;               /* a VRM resource takes the voltage, the enable and the mode; a level one the level */
		uint32_t voltage_mv, level;
	} rails[] = {
		{ "ldoa9", "pm8150_l9, the PHY's 1.2 V (dt: vreg-1p8)", true, 1200, 0 },
		{ "ldoa5", "pm8150_l5, the PHY's 0.88 V (dt: vreg-0p9)", true, 880, 0 },
		{ "cx.lvl", "the cx rail, the PHY's logic (dt: vreg-cx)", false, 0, 256 },
	};
	uint32_t address[3 * 3], data[3 * 3], count = 0;
	vm_offset_t rsc;

	announce("vote for the PHY's power: the three rails the device tree gives the controller's node, through the RPMh "
	    "command database (0x80860000) and its sequencer (0x18220000)");
	for (unsigned r = 0; r < sizeof(rails) / sizeof(rails[0]); r++) {
		uint32_t base = cmd_db_address(rails[r].name);

		line("%s (%s) at %#x", rails[r].name, rails[r].what, base);
		if (base == 0) {
			printf("pcie: %u: THE COMMAND DATABASE HAS NO %s\n", pcie_step, rails[r].name);
			return false;
		}
		if (rails[r].vrm) {
			address[count] = base + 0;      /* the voltage, in millivolts (the vendor's rpmh-regulator.c) */
			data[count++] = rails[r].voltage_mv;
			address[count] = base + 4;      /* the enable */
			data[count++] = 1;
			address[count] = base + 8;      /* the mode: 7 is pmic5-ldo high power (the tree's init-mode 4) */
			data[count++] = 7;
		} else {
			address[count] = base;          /* the level: 256 is nominal (the tree's vreg-cx-voltage-level) */
			data[count++] = rails[r].level;
		}
	}
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	if (rsc == 0) {
		printf("pcie: %u: NO MEMORY FOR THE RPMh SEQUENCER\n", pcie_step);
		return false;
	}
	line("the sequencer: control %#x, status %#x, enabled %#x", rd(rsc + RSC_TCS(TCS_CONTROL)),
	    rd(rsc + RSC_TCS(TCS_STATUS)), rd(rsc + RSC_TCS(TCS_ENABLED)));
	if (!rpmh_write(rsc, address, data, count)) {
		printf("pcie: %u: THE POWER VOTE GOT NO ANSWER FROM RPMh\n", pcie_step);
		return false;
	}
	line("the power vote is in (%u commands: ldoa9 %u mV, ldoa5 %u mV, both mode 7, cx level 256)", count,
	    rails[0].voltage_mv, rails[1].voltage_mv);
	sleep_ms(10);           /* a moment for the regulators to settle */
	return true;
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
#define GCC_PCIE_WIFI_CLKREF    0x8c004         /* GCC_PCIE_WIFI_CLKREF_EN: the endpoint's clock reference (the tree's pcie_0_ldo) */

#define GDSC_POWER_ON           (1u << 31)
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_RETAIN_FF_ENABLE   (1u << 11)

#define PHY_STATUS              0x814           /* dt: qcom,phy-status-offset; ready is bit 6 at 0 (the vendor's pcie_phy_is_ready) */
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
	get(2, "phy_status", PHY_STATUS);
	get(2, "phy_power_down", PHY_POWER_DOWN);
	get(1, "parf_sys_ctrl", 0x00);
	get(1, "parf_phy_ctrl", 0x40);
	get(1, "parf_phy_refclk", 0x4c);
	get(1, "parf_dbi_base_v2 (0x350: this IP revision)", 0x350);
	get(1, "parf_dbi_base_v1 (0x168: what the driver used before)", 0x168);
	get(1, "parf_slv_addr_space_size_v2", 0x358);
	get(1, "parf_ltssm", 0x1b0);
	get(1, "parf_q2a_flush", 0x1ac);
	get(1, "parf_bdf_translate", 0x24c);
	get(3, "elbi_sys_ctrl", 0x04);
	get(7, "perst_gpio79 cfg", 0x1000 * PERST_GPIO);
	get(7, "perst_gpio79 io", 0x1000 * PERST_GPIO + 4);
	return true;
}

static bool
step_gdsc(void)
{
	uint32_t gdscr, waited;

	announce("raise the radio's own enable line, hold the endpoint in reset (PERST low), switch the controller's own "
	    "power on: the software collapse bit out of its power switch, then wait for its power-on bit");
	pause_group();          /* the announce goes out before anything of the step is touched */
	wlanen_raise();         /* the cnss driver raises wlan-en before anything on the PCIe side */
	sleep_ms(1);            /* a moment for the radio's own reset to come out */
	/*
	 * A pause after each of the step's three groups: the run of 2026-11-03 (p19) died in this step
	 * without a single one of its lines in the log -- the loader only writes the log out every quarter
	 * of a second, and the first access took the machine before any of them was written down. With the
	 * pauses, the last written line names the access that kills (p20).
	 */
	pause_group();
	perst_assert();
	pause_group();
	gdscr = get(0, "pcie_0_gdscr", GCC_PCIE_0_GDSCR);
	put(0, "pcie_0_gdscr", GCC_PCIE_0_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
	pause_group();
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
	announce("vote for its clocks (slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a), the endpoint's own clock "
	    "reference, and read the halt state of each");
	put(0, "pcie_0_axi_misc (vote for its clocks)", GCC_PCIE_0_AXI_MISC, rd(GCC(GCC_PCIE_0_AXI_MISC)) | CLOCK_VOTES);
	/* the reference the radio's side is clocked with: the UniXNU record of this phone measured it
	 * off (0x80000000) until its bit was voted, and it is what the vendor's pcie_0_ldo asks for */
	put(0, "pcie_wifi_clkref (the endpoint's reference)", GCC_PCIE_WIFI_CLKREF, rd(GCC(GCC_PCIE_WIFI_CLKREF)) | 1u);
	sleep_ms(1);
	get(0, "pcie_0_axi_misc after", GCC_PCIE_0_AXI_MISC);
	get(0, "pcie_wifi_clkref after", GCC_PCIE_WIFI_CLKREF);
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

	announce("wake the PHY: its own auxiliary clock on, and a look at its power-down word and status");
	put(0, "pcie_phy_aux (the PHY's clock)", GCC_PCIE_PHY_AUX, rd(GCC(GCC_PCIE_PHY_AUX)) | 1u);
	sleep_ms(1);
	get(0, "pcie_phy_aux after", GCC_PCIE_PHY_AUX);
	pause_group();
	/*
	 * Only reads here: the vendor never writes the power-down word outside the dt's own sequence
	 * (whose first pair, 0x840 = 3, is what switches the PHY on) and outside its teardown path
	 * (pci-msm.c writes 0 there). The first version cleared bit 0 by hand -- a step the vendor
	 * does not take.
	 */
	status = get(2, "phy_status (before the sequence)", PHY_STATUS);
	line("the PHY reads %#x (its ready is bit 6 at 0)", status);
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
	    "them in the PHY's own 4 KB), its reset pulsed before them the way the vendor does, and its ready bit waited for");
	/*
	 * The vendor's order (pci-msm.c: msm_pcie_clk_init, then pcie_phy_init): the PHY's reset -- 0x6c01c,
	 * the device tree's "pcie_0_phy_reset" -- is pulsed with the clocks on and BEFORE the sequence is
	 * written, and nothing resets anything after it. (The vendor's post-sequence pulse wants
	 * "pcie_0_phy_pipe_reset", which this phone's tree does not have -- and the run of 2026-11-03 that
	 * pulsed 0x6c01c after the sequence wiped the PHY's registers: start control read 0, the software
	 * reset bit back at 1, the status stuck at 0x68. The core's own reset belongs before the PARF writes
	 * and step 6 already pulses it there: pulsing it here would wipe those writes with it.)
	 */
	put(0, "pcie_0_phy_bcr (assert)", GCC_PCIE_0_PHY_BCR, 1);
	sleep_ms(1);
	put(0, "pcie_0_phy_bcr (release)", GCC_PCIE_0_PHY_BCR, 0);
	sleep_ms(10);
	/* a block reset takes the clock branch with it: vote the PHY's own clocks again */
	put(0, "pcie_phy_aux again", GCC_PCIE_PHY_AUX, rd(GCC(GCC_PCIE_PHY_AUX)) | 1u);
	put(0, "pcie0_phy_refgen again", GCC_PCIE_0_PHY_REFGEN, rd(GCC(GCC_PCIE_0_PHY_REFGEN)) | 1u);
	for (unsigned i = 0; i < sizeof(phy_sequence) / sizeof(phy_sequence[0]); i++) {
		wr(reg(2, phy_sequence[i].reg), phy_sequence[i].value);
	}
	sleep_ms(1);            /* the vendor's PHY_STABILIZATION_DELAY, then the ready poll */
	/* the last pairs are the start control (0x844) and the word before it: what the PLL was told to do */
	get(2, "phy_start_control", 0x844);
	get(2, "phy_800", 0x800);
	get(2, "phy_9a4", 0x9a4);
	get(2, "phy_8dc", 0x8dc);
	{
		uint32_t status = 0;

		for (uint32_t waited = 0; waited < 20; waited++) {
			status = rd(reg(2, PHY_STATUS));
			if ((status & (1u << 6)) == 0) {
				break;
			}
			sleep_ms(10);
		}
		/* the vendor's pcie_phy_is_ready wants bit 6 at 0: set, the PHY is still coming up */
		line("the PHY %s (status %#x, the vendor's ready is bit 6 at 0)",
		    (status & (1u << 6)) ? "DID NOT BECOME READY" : "IS READY", status);
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
	 * GCC_PCIE0_PHY_REFGEN_CLK): the vendor's table (gcc-kona.c's ftbl_gcc_pcie_phy_refgen_clk_src) asks
	 * for GPLL0's main output at 600 MHz through a divider of six, so source 1 and the divider field
	 * holding the divider minus one -- 0x105, and not 0x501, which the first version wrote (a source the
	 * parent map does not even have; found against the vendor clock table, 2026-11-03).
	 */
	put(0, "phy_refgen_src cfg (source 1, divide by 6: 100 MHz)", 0x6f018, 0x105);
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
 * The DBI registers msm_pcie_link_train touches before it turns the state machine on (pci-msm.c offsets).
 */
#define DBI_GEN2_CTRL           0x80c   /* [12:8] the fast training sequences: the vendor asks for one */
#define DBI_GEN3_RELATED        0x890   /* bit 0 clear: the Gen3 equalization is not skipped */
#define DBI_GEN3_MISC_CONTROL   0x8bc   /* bit 0: the preset's gate */
#define DBI_SPCIE_CAP           0x154   /* the Gen3 preset the vendor equalizes with */
#define DBI_DEVICE_CONTROL      0x78    /* [14:12] the largest read request: the vendor asks for 5 */

/*
 * The link, in the vendor's order (msm_pcie_enable then msm_pcie_link_train): the device tree's ep-latency
 * wait, the reset line out, the training registers, the state machine on, and the vendor's three checks
 * polled together. Nothing may touch the core's own windows before the endpoint is out of reset: a DBI
 * access with PERST held took the machine down twice -- 2026-11-03 here, 2026-10-30 in the UniXNU record.
 */
static bool
step_ltssm(void)
{
	uint32_t value;

	announce("the vendor's order: the endpoint out of reset, its training words in, the state machine on, and "
	    "its three checks for the link");
	sleep_ms(10);           /* the vendor's ep-latency, before the reset line goes up */
	perst_release();
	sleep_us(1000);         /* the vendor waits a millisecond after the reset line goes up */

	value = get(6, "dbi gen2_ctrl", DBI_GEN2_CTRL);
	put(6, "dbi gen2_ctrl (one fast training sequence)", DBI_GEN2_CTRL, (value & ~0x1f00u) | (1u << 8));
	value = get(6, "dbi gen3_related", DBI_GEN3_RELATED);
	put(6, "dbi gen3_related (equalization not skipped)", DBI_GEN3_RELATED, value & ~1u);
	value = get(6, "dbi gen3_misc_control", DBI_GEN3_MISC_CONTROL) | 1u;
	put(6, "dbi gen3_misc_control (the preset's gate open)", DBI_GEN3_MISC_CONTROL, value);
	put(6, "dbi spcie_cap (the Gen3 preset)", DBI_SPCIE_CAP, 0x55555555u);
	put(6, "dbi gen3_misc_control (the preset's gate shut)", DBI_GEN3_MISC_CONTROL, value & ~1u);
	value = get(6, "dbi device_control", DBI_DEVICE_CONTROL);
	put(6, "dbi device_control (the largest read, 5)", DBI_DEVICE_CONTROL, (value & ~0x7000u) | (5u << 12));

	put(1, "parf_ltssm (training on)", 0x1b0, get(1, "parf_ltssm", 0x1b0) | (1u << 8));

	/* the vendor's three checks together: the wrapper's link-state word (XMLH_LINK_UP, bit 10 of
	 * ELBI_SYS_STTS), the data-link layer's active bit (DLLLA, bit 29 of DBI 0x80), and the core's
	 * own vendor and device answering in DBI 0x00 -- 20 rounds, 5 ms apart, as the vendor polls */
	for (uint32_t waited = 0; waited < 20; waited++) {
		uint32_t stts = rd(reg(3, 0x08));
		uint32_t dll = rd(reg(6, 0x80));
		uint32_t id = rd(reg(6, 0x00));

		line("the link after %u ms: elbi %#x, dll %#x, id %#x", waited * 5, stts, dll, id);
		if ((stts & 0x400u) && (dll & (1u << 29)) && id != 0xffffffffu) {
			line("THE LINK IS UP after %u ms (endpoint %#04x:%#04x)", waited * 5, id & 0xffff, id >> 16);
			get(1, "parf_ltssm after", 0x1b0);
			return true;
		}
		sleep_ms(5);
	}

	printf("pcie: %u: THE LINK DID NOT COME UP in 100 ms\n", pcie_step);
	return false;
}

/* ---- the endpoint beyond the link: its config space, its BAR, its MHI ---------------------- */

/*
 * The endpoint's configuration space is reached the way the vendor reaches it (pci-msm.c's
 * msm_pcie_oper_conf and msm_pcie_cfg_bdf): the conf window (0x60100000) is not an ECAM -- it only
 * answers once the root complex's iatu region 0 has been pointed at the device's BDF as a type 0
 * configuration region. The iatu sits in its own window here (0x60001000, the dt's "iatu"; region n
 * starts at n * 0x200 and there is no viewport register), region 0 is kept for configuration, and the
 * type is 0x04 (CFG0) for the secondary bus, the target carrying bus in bits 31:24 and devfn in
 * 23:16. Memory needs no iatu at all: the AXI "memory" window (0x60300000, the dt's 0x3d00000) is
 * identity-mapped onto the bus, so the BAR is assigned an address inside it and the TLPs carry the
 * same one. The first version here programmed region 0 as memory through the standard DWC offsets
 * -- wrong table, wrong idea, and every read of the endpoint answered all ones (p27, 2026-11-04).
 *
 * The MHI and BHI registers are the standard MHI 1.0 map (mhi_internal.h): MHIREGLEN 0x0, MHICFG
 * 0x10, CHDBOFF 0x18, ERDBOFF 0x20, BHIOFF 0x28, MHICTRL 0x38 (ready is MHISTATUS 0x48 bit 0), and
 * the BHI block at BHIOFF -- EXECENV 0x28, STATUS 0x2C (its state machine in bits 31:30, SUCCESS 2),
 * the image address and size at 0x08/0x10, the trigger at 0x18. The cnss2 driver also reaches the
 * device's BHI_EXECENV at the device-internal 0x01e0e228, which sits inside what BAR0 maps (the
 * device's own 0x01e00000 region): both are read here, side by side.
 */
#define EP_CFG_VENDOR_DEVICE    0x00
#define EP_CFG_COMMAND          0x04
#define EP_CFG_BAR0             0x10
#define EP_CFG_CMD_MEM_EN       (1u << 1)
#define EP_CFG_CMD_BUS_MASTER   (1u << 2)

/* the iatu's own window (dt "iatu", block 4): region 0, the one kept for configuration */
#define IATU_CTRL1              0x000
#define IATU_CTRL2              0x004
#define IATU_LOWER_BASE         0x008
#define IATU_UPPER_BASE         0x00c
#define IATU_LIMIT              0x010
#define IATU_LOWER_TARGET       0x014
#define IATU_UPPER_TARGET       0x018
#define IATU_TYPE_CFG0          0x04u   /* a type 0 configuration TLP: the secondary bus */
#define IATU_REGION_ENABLE      (1u << 31)

#define EP_BDF                  0x01000000ul    /* bus 1, device 0, function 0, as the iatu wants it */
#define MHI_WINDOW_BASE         0x60300000ul
#define MHI_WINDOW_SIZE         0x100000ul      /* what the BAR window decodes (the dt's "memory") */
#define MHI_MAP_SIZE            0x10000ul       /* what is actually mapped: the first 64 KB */

static bool
step_ep_cfg(void)
{
	uint32_t bar0, probe;

	announce("the endpoint's configuration space: the iatu's region 0 pointed at its BDF as a "
	    "configuration region, who it is, how big its first BAR is, an address assigned to it");
	/* region 0: from the conf window to the endpoint's type 0 configuration space */
	put(4, "iatu ctrl2 (off before changing)", IATU_CTRL2, 0);
	put(4, "iatu ctrl1 (cfg0)", IATU_CTRL1, IATU_TYPE_CFG0);
	put(4, "iatu lower base (the conf window)", IATU_LOWER_BASE, 0x60100000);
	put(4, "iatu upper base", IATU_UPPER_BASE, 0);
	put(4, "iatu limit (4 KB)", IATU_LIMIT, 0x60100000 + 0x1000 - 1);
	put(4, "iatu lower target (bus 1, device 0, function 0)", IATU_LOWER_TARGET, EP_BDF);
	put(4, "iatu upper target", IATU_UPPER_TARGET, 0);
	put(4, "iatu ctrl2 (enable)", IATU_CTRL2, IATU_REGION_ENABLE);
	get(4, "iatu ctrl2 read back", IATU_CTRL2);
	pause_group();
	get(5, "ep vendor:device", EP_CFG_VENDOR_DEVICE);
	get(5, "ep command/status", EP_CFG_COMMAND);
	bar0 = get(5, "ep bar0 before", EP_CFG_BAR0);
	wr(reg(5, EP_CFG_BAR0), 0xffffffffu);
	probe = get(5, "ep bar0 probed (the ones written)", EP_CFG_BAR0);
	wr(reg(5, EP_CFG_BAR0), bar0);
	line("the bar restored to %#x; the probe read %#x", bar0, probe);
	pause_group();
	{
		uint32_t size = (~(probe & ~0xfu)) + 1;
		bool bar64 = (probe & 0x6u) == 0x4u;    /* bits 2:1: 10 is a 64-bit BAR, 00 a 32-bit one */

		line("the MHI BAR is %#x bytes at the low bits %#x%s", size, probe & 0xf,
		    bar64 ? " (64-bit)" : "");
		if (size < 0x100u || size > MHI_WINDOW_SIZE || (probe & 0xf & ~0x4u) != 0) {
			say("the BAR does not look like plain memory; not assigning it\n");
			return false;
		}
		put(5, "ep bar0 (assigned)", EP_CFG_BAR0, MHI_WINDOW_BASE);
		if (bar64) {
			put(5, "ep bar0 upper (zero)", EP_CFG_BAR0 + 4, 0);
		}
	}
	put(5, "ep command (mem space, bus master)", EP_CFG_COMMAND,
	    (get(5, "ep command", EP_CFG_COMMAND) | EP_CFG_CMD_MEM_EN | EP_CFG_CMD_BUS_MASTER));
	/*
	 * The root port's own command word sits in its dbi (0x60000000, block 6): the same two bits, so
	 * its windows answer memory TLPs.
	 */
	put(6, "rc command (mem space, bus master)", EP_CFG_COMMAND,
	    (get(6, "rc command", EP_CFG_COMMAND) | EP_CFG_CMD_MEM_EN | EP_CFG_CMD_BUS_MASTER));
	line("both command words carry memory space and bus master now");
	/*
	 * The root port's bridge windows (type 1, dbi 0x1c-0x2c) filter the TLPs that travel upstream --
	 * and the firmware download in step 14 is the first upstream traffic there is: the device reads
	 * its image out of our RAM with a DMA of its own, and an address outside the memory window is
	 * answered with an unsupported request (the PBL of p33 hung on it, its error register pointing at
	 * its own 0x426890). The kernel and the images live at 0xf7000000-0xfdffffff; the window covers
	 * that (the PCIe windows themselves sit at 0x60000000-0x64ffffff, well below).
	 */
	get(6, "rc memory base/limit before", 0x20);
	get(6, "rc prefetch base/limit before", 0x24);
	put(6, "rc memory base/limit (0xf0000000-0xfdffffff)", 0x20, (0xfdfu << 20) | (0xf00u << 4));
	get(6, "rc memory base/limit read back", 0x20);
	line("the bridge's memory window covers our RAM now");
	pause_group();
	line("the first read of the MHI registers (the BAR's own memory) comes now");
	get(8, "mhi reglen", 0x0);
	line("the MHI registers answer: the read did not hang");
	get(8, "mhi version", 0x8);
	get(8, "mhi status", 0x48);
	return true;
}

static bool
step_mhi_look(void)
{
	uint32_t bhioff, execenv;

	announce("the MHI's own registers and, behind BHIOFF, the BHI block the boot comes through");
	get(8, "mhi cfg (channels)", 0x10);
	get(8, "mhi chdb off", 0x18);
	get(8, "mhi erdb off", 0x20);
	bhioff = get(8, "mhi bhi off", 0x28);
	get(8, "mhi bhie off", 0x2c);
	get(8, "mhi ctrl", 0x38);
	get(8, "mhi ctrl base lower", 0x80);
	get(8, "mhi status", 0x48);
	pause_group();
	if (bhioff == 0 || bhioff >= MHI_MAP_SIZE) {
		say("the BHI offset does not look right (%#x); going no further here\n", bhioff);
		return false;
	}
	get(8, "bhi version minor", bhioff + 0x00);
	get(8, "bhi version major", bhioff + 0x04);
	get(8, "bhi imgaddr low", bhioff + 0x08);
	get(8, "bhi imgaddr high", bhioff + 0x0c);
	get(8, "bhi imgsize", bhioff + 0x10);
	get(8, "bhi txdb", bhioff + 0x18);
	get(8, "bhi intvec", bhioff + 0x20);
	execenv = get(8, "bhi execenv", bhioff + 0x28);
	get(8, "bhi status", bhioff + 0x2c);
	get(8, "bhi serial", bhioff + 0x40);
	/* the same word the cnss2 driver reaches at the device's own 0x01e0e228 */
	get(8, "bhi execenv (the device's own 0x01e0e228)", 0xe228);
	line("the boot environment (EE) is %u: 0 PBL, 1 SBL, 2 AMSS, 3 RDDM", execenv);
	return true;
}

/* ---- step 14: the SBL, through BHI ----------------------------------------------------------- */

/*
 * The PBL that the endpoint came out of reset with (EE 0) knows one trick: BHI. The host points
 * BHI_IMGADDR at a piece of contiguous memory holding an image, writes its size to BHI_IMGSIZE, and
 * rings BHI_IMGTXDB; the device fetches it with a DMA of its own and runs it.
 *
 * What goes in first is NOT the 4.3 MB AMSS. It is the **SBL**, the 512 KiB stub that the AMSS image
 * starts with: the PBL hands control to it and the SBL is what later fetches the rest through BHIE.
 * This is the split qwx makes in if_qwx_pci.c (SBL by BHI, then AMSS in 512 KiB vectors through BHIE)
 * and the one Klipboard2022/k40-ios-experiments got a working Wi-Fi with on this same phone
 * (overlays/hakios/.../hakios.pcie.c, step 30: "requested signed SBL transfer (512 KiB) through
 * BHI"). p36 offered the whole 4,352,144 bytes to the PBL instead: BHI_STATUS stayed 0 for 200 ms
 * and then every BAR0 register read all ones -- the endpoint had gone, which is what a PBL given an
 * image it cannot take does.
 *
 * The image is the phone's own /vendor/firmware_mnt/image/qca6390/amss20.bin, which
 * userland/build.sh puts into the image's fw/ directory; redwood_image_file() finds it in the
 * memory of the RAM disk. A successful transfer leaves BHI_STATUS's top two bits at 2 and moves
 * BHI_EXECENV off 0 (1 = SBL); the error registers say more when it does not.
 */
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern uint64_t kvtophys(vm_offset_t va);       /* (pmap.h) the physical address of a kernel pointer */

#define SBL_BYTES       0x80000u        /* 512 KiB: what the PBL's BHI takes, and what qwx sends */

static bool
step_bhi_amss(void)
{
	const uint8_t *amss;
	uint64_t size = 0, phys = 0, image_phys, expected;
	uint32_t bhioff, status = 0, waited, attempt, ee;
	int disk = -1;

	announce("the SBL through BHI: amss20.bin found in the image, its first 512 KiB checked as one "
	    "piece of physical memory, the address and size written, the trigger rung, the status waited for");
	amss = redwood_image_file("fw", "amss20.bin", &size, &disk);
	if (amss == NULL || size == 0) {
		say("no amss20.bin in the image: put the phone's /vendor/firmware_mnt/image/qca6390/amss20.bin "
		    "into fw-gpu/ and rebuild the userland\n");
		return false;
	}
	line("amss20.bin: %llu bytes in the image of /dev/md%d at %#llx", size, disk, (uint64_t)(uintptr_t)amss);
	if (size < SBL_BYTES || memcmp(amss, "\177ELF", 4) != 0) {
		say("the image is %llu bytes and does not begin with an ELF header; the SBL is its first "
		    "512 KiB and this is not it\n", size);
		return false;
	}
	image_phys = kvtophys((vm_offset_t)amss);
	expected = image_phys;
	for (uint64_t page = 0; page < size; page += 0x1000) {
		uint64_t here = kvtophys((vm_offset_t)amss + page);

		if (here != expected) {
			say("the image is not one piece of physical memory (page %llu at %#llx, wanted %#llx); "
			    "BHI takes a contiguous region\n", page / 0x1000, here, expected);
			return false;
		}
		expected = here + 0x1000;
	}
	line("the image is one piece of physical memory at %#llx", image_phys);
	/*
	 * The image is not what the device is pointed at: a buffer of our own is, the way the reference
	 * does it (kmem_alloc_contig, then the image copied in). Two reasons -- the address is then one
	 * we chose and can report, and it is allocated where the kernel sees fit rather than wherever
	 * the RAM disk happened to land, which is the variable p37 could not separate from the rest.
	 */
	{
		vm_offset_t sbl = 0;
		kern_return_t kr = kmem_alloc_contig(kernel_map, &sbl, SBL_BYTES, 0, 0, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);

		if (kr != KERN_SUCCESS) {
			say("no contiguous %u bytes for the SBL\n", SBL_BYTES);
			return false;
		}
		memcpy((void *)sbl, amss, SBL_BYTES);
		phys = kvtophys(sbl);
		line("the SBL is copied into %u bytes of contiguous memory at physical %#llx",
		    SBL_BYTES, phys);
	}
	bhioff = get(8, "mhi bhi off", 0x28);
	ee = get(8, "bhi execenv before", bhioff + 0x28);
	if (bhioff != 0x100u) {
		say("the BHI block is at %#x, not the 0x100 every write below assumes; going no further\n", bhioff);
		return false;
	}
	if (ee != 0) {
		say("the execution environment is %u, not 0 (PBL): the endpoint has already been booted once "
		    "in this run, and BHI only answers a PBL\n", ee);
		return false;
	}
	/*
	 * Polled BHI, no MSI: the transfer's progress is read out of BHI_STATUS, so nothing has to be
	 * routed, acknowledged or left pending when the wait below ends.
	 */
	put(8, "bhi polled (no MSI)", bhioff + 0x20, 0);
	/*
	 * The DMA the download is done with is the first traffic that travels upstream, from the device
	 * into our RAM. The bridge window (step 12) lets it through the root port; what sits behind that
	 * is the applications SMMU (dt: apps-smmu@15000000, the endpoint's stream 0x1c00) -- but its
	 * registers are the hypervisor's to answer for, and the one probe this driver made of CR0 hung
	 * the calling thread without an answer (p35, 2026-11-04). The UFS, whose DMA has worked since
	 * the first boot without anybody programming the SMMU for it, says the streams go through; so
	 * the SMMU is left alone here. The reference implementation does programme it and hands the
	 * device an IOVA instead (qca_dma_prepare, SID 0x1c01) -- if the address is what fails next,
	 * that is the piece to port, not this one.
	 */
	sleep_ms(2000);         /* the PBL has only just seen the link: a moment before asking it for DMA */
	for (attempt = 1; attempt <= 1; attempt++) {       /* one trigger: a wedged endpoint is not retried */
		put(8, "bhi status (cleared first, as the vendor does)", bhioff + 0x2c, 0);
		put(8, "bhi imgaddr low", bhioff + 0x08, (uint32_t)phys);
		put(8, "bhi imgaddr high", bhioff + 0x0c, (uint32_t)(phys >> 32));
		put(8, "bhi imgsize (the SBL)", bhioff + 0x10, SBL_BYTES);
		/* what the endpoint kept of that: a write that does not stick makes the trigger meaningless,
		 * and p37 could not tell the two apart after the fact */
		get(8, "bhi imgaddr low read back", bhioff + 0x08);
		get(8, "bhi imgaddr high read back", bhioff + 0x0c);
		get(8, "bhi imgsize read back", bhioff + 0x10);
		pause_group();
		put(8, "bhi imgtxdb (the trigger)", bhioff + 0x18, attempt);
		status = 0;
		for (waited = 0; waited < 300; waited++) {      /* up to half a minute an attempt */
			status = rd(reg(8, bhioff + 0x2c));
			ee = rd(reg(8, bhioff + 0x28));
			if ((status >> 30) == 2 || (status >> 30) == 3 || ee != 0) {
				break;
			}
			if ((waited % 10) == 0) {
				/* a line a second: the loader only mirrors the log for its first 24 s, so the
				 * download is best watched inside that window (pcie_pause=100 pulls the steps early) */
				line("BHI still going after %u ms (status %#x, execenv %u)", waited * 100, status, ee);
			}
			sleep_ms(100);
		}
		line("BHI status %#x after %u ms (attempt %u): the top two bits %u (%s), execenv %u", status,
		    waited * 100, attempt, status >> 30,
		    (status >> 30) == 2 ? "SUCCESS" : (status >> 30) == 3 ? "ERROR" : "still going", ee);
		if ((status >> 30) == 2 || ee != 0) {
			break;
		}
		if (status == 0xffffffffu) {
			line("the BHI registers read all ones: the device or the link went away");
			get(1, "parf ltssm after the failure", 0x1b0);
			get(3, "elbi sys status after the failure", 0x08);
			/* the config space answers even when the BAR does not: a device that only reset its
			 * function still has its vendor id, one that left the bus has not */
			get(5, "ep vendor:device after the failure", 0);
			get(8, "mhi ctrl after the failure", 0x38);
			get(8, "mhi status after the failure", 0x48);
		} else {
			get(8, "bhi errcode", bhioff + 0x30);
			get(8, "bhi errdbg1", bhioff + 0x34);
			get(8, "bhi errdbg2", bhioff + 0x38);
			get(8, "bhi errdbg3", bhioff + 0x3c);
		}
		sleep_ms(2000);         /* a device that reset itself needs a moment before the next attempt */
	}
	line("the SBL is in: execution environment %u (1 = SBL, 2 = AMSS), %llu of the image's %llu bytes "
	    "sent", ee, (uint64_t)SBL_BYTES, size);
	get(8, "mhi status after the SBL", 0x48);
	return ee != 0 || (status >> 30) == 2;
}

/* ---- steps 15-17: the MHI proper ------------------------------------------------------------- */

/*
 * The register map the firmware speaks from here on (BAR0 offsets, mhi_internal.h): MHICFG 0x10
 * (the device's counts: channels in bits 7:0, event rings in 23:16), the doorbell array offsets
 * CHDBOFF 0x18 and ERDBOFF 0x20, MHICTRL 0x38 (bit 1 RESET, the state requested in bits 15:8),
 * MHISTATUS 0x48 (bit 0 READY, bits 15:8 the current state), the context base pointers CCABAP
 * 0x58, ECABAP 0x60, CRCBAP 0x68, the command ring doorbell CRDB 0x70, and the four window words
 * MHICTRLBASE/LIMIT 0x80-0x8f and MHIDATABASE/LIMIT 0x98-0xa7 that bound the host memory the
 * device may touch. A context element is 44 bytes: three words, then the four unaligned qwords
 * rbase, rlen, rp, wp (struct mhi_chan_ctxt/mhi_event_ctxt/mhi_cmd_ctxt, all __packed). A ring
 * element (struct mhi_tre) is 16 bytes.
 */
#define MHI_MHICFG      0x10
#define MHI_CHDBOFF     0x18
#define MHI_ERDBOFF     0x20
#define MHI_BHIOFF      0x28
#define MHI_MHICTRL     0x38
#define MHI_MHISTATUS   0x48
#define MHI_CCABAP      0x58
#define MHI_ECABAP      0x60
#define MHI_CRCBAP      0x68
#define MHI_CTRLBASE    0x80
#define MHI_CTRLLIMIT   0x88
#define MHI_DATABASE    0x98
#define MHI_DATALIMIT   0xa4

#define MHI_CTRL_RESET          (1u << 1)
#define MHI_STATUS_READY        (1u << 0)
#define MHI_STATUS_SYSERR       (1u << 2)
#define MHI_STATE_M0            2               /* MHICTRL's state field, and MHISTATUS's answer */
#define MHI_EE_AMSS             2               /* BHI_EXECENV: the firmware fully booted */

#define MHI_CTXT_EL     44      /* one context element: 3 words + 4 qwords, packed */
#define MHI_RING_EL     16      /* one ring element (struct mhi_tre) */
#define MHI_RING_ELS    32      /* elements a ring is given: 512 bytes, its own alignment */

#define MHI_ER_TYPE_VALID       1
#define MHI_DEV_WAKE_DB         127     /* the doorbell past the last channel's, zeroed like the vendor's */

static vm_offset_t mhi_ctxt_va;         /* contexts and rings in one piece of physical memory */
static uint64_t mhi_ctxt_phys, mhi_ctxt_size;
static uint32_t mhi_nch, mhi_ner;       /* what MHICFG said the device has */
static vm_offset_t mhi_er_ctxt_va;      /* the event contexts inside the piece */
static uint64_t mhi_er_ring_phys;       /* event ring 0: the device writes its events there */
static uint64_t mhi_cmd_ctxt_phys, mhi_chan_ctxt_phys, mhi_er_ctxt_phys;
static uint32_t mhi_chdboff, mhi_erdboff;

static void
ctxt64(vm_offset_t ctxt, uint32_t off, uint64_t value)
{
	/* the qword fields sit at 4-byte alignment in a packed context: two words each */
	*(volatile uint32_t *)(ctxt + off) = (uint32_t)value;
	*(volatile uint32_t *)(ctxt + off + 4) = (uint32_t)(value >> 32);
}

static void
mhi_put64(int block, uint32_t off, uint64_t value, const char *name)
{
	put(block, name, off + 4, (uint32_t)(value >> 32));
	put(block, name, off, (uint32_t)value);
}

static bool
step_mhi_ready(void)
{
	uint32_t bhioff, ee = 0, ctrl = 0, status = 0, waited, cfg;

	announce("the SBL running: the MHI's READY bit watched for, then its own counts read back");
	bhioff = get(8, "mhi bhi off", MHI_BHIOFF);
	/*
	 * After the BHI transfer the endpoint is in SBL (execenv 1), not in AMSS: the AMSS is what the
	 * SBL fetches through BHIE once MHI is ready, so 2 is not what this wait is for. qwx waits the
	 * same way (ctrl's RESET bit 1 clear, status's READY bit 0 set) and so does mhi_pm.c's
	 * mhi_ready_state_transition; the execution environment is only reported.
	 */
	for (waited = 0; waited < 100; waited++) {      /* up to ten seconds */
		ctrl = rd(reg(8, MHI_MHICTRL));
		status = rd(reg(8, MHI_MHISTATUS));
		ee = rd(reg(8, bhioff + 0x28));
		if (!(ctrl & MHI_CTRL_RESET) && (status & MHI_STATUS_READY)) {
			break;
		}
		if ((waited % 10) == 0) {
			line("mhi ctrl %#x, status %#x, execution environment %u after %u ms", ctrl, status, ee,
			    waited * 100);
		}
		if (ctrl == 0xffffffffu || status == 0xffffffffu) {
			say("the MHI registers read all ones: the endpoint is gone\n");
			return false;
		}
		sleep_ms(100);
	}
	line("mhi ctrl %#x, mhi status %#x, execution environment %u after %u ms", ctrl, status, ee,
	    waited * 100);
	if ((ctrl & MHI_CTRL_RESET) || !(status & MHI_STATUS_READY)) {
		say("no READY: the SBL did not raise it (ctrl %#x, status %#x, execenv %u)\n", ctrl, status, ee);
		return false;
	}
	pause_group();
	cfg = get(8, "mhi cfg (the device's counts)", MHI_MHICFG);
	mhi_nch = cfg & 0xff;
	mhi_ner = (cfg >> 16) & 0xff;
	line("the device has %u channels, %u event rings (%u of them hardware, %u hardware channels)",
	    mhi_nch, mhi_ner, cfg >> 24, (cfg >> 8) & 0xff);
	if (mhi_nch == 0 || mhi_nch > 128 || mhi_ner == 0 || mhi_ner > 32) {
		say("the counts do not look right (cfg %#x); going no further\n", cfg);
		return false;
	}
	mhi_chdboff = get(8, "mhi chdb off", MHI_CHDBOFF);
	mhi_erdboff = get(8, "mhi erdb off", MHI_ERDBOFF);
	if (mhi_chdboff + 8 * (MHI_DEV_WAKE_DB + 1) >= MHI_MAP_SIZE ||
	    mhi_erdboff + 8 * mhi_ner >= MHI_MAP_SIZE) {
		say("the doorbell arrays reach past the mapped window; going no further\n");
		return false;
	}
	return true;
}

/* step 16: the memory the device's DMA is given */

static bool
step_mhi_ctxt(void)
{
	uint64_t cmd_ring, er_ring, off;
	vm_offset_t cmd_ctxt, chan_ctxt, er_ctxt;
	kern_return_t kr;

	announce("the MHI's memory: one contiguous piece for the context arrays, the command ring and "
	    "the event rings, zeroed and filled in");
	/*
	 * The layout: the command ring first (page-aligned base is ring-aligned, 512 dividing 4096),
	 * then the event rings, then, 128-aligned, the command context, the channel contexts (all zero:
	 * DISABLED, as the vendor's freshly allocated array leaves them until a channel is started --
	 * mhi_init_chan_ctxt runs per channel, after M0) and the event contexts.
	 */
	off = 0;
	cmd_ring = off;
	off += MHI_RING_ELS * MHI_RING_EL;
	er_ring = off;
	off += mhi_ner * MHI_RING_ELS * MHI_RING_EL;
	off = (off + 127) & ~127ull;
	cmd_ctxt = (vm_offset_t)off;
	off += MHI_CTXT_EL;
	chan_ctxt = (vm_offset_t)off;
	off += mhi_nch * MHI_CTXT_EL;
	er_ctxt = (vm_offset_t)off;
	off += mhi_ner * MHI_CTXT_EL;
	mhi_ctxt_size = round_page(off);
	kr = kmem_alloc_contig(kernel_map, &mhi_ctxt_va, mhi_ctxt_size, 0, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
	if (kr != KERN_SUCCESS) {
		say("no contiguous memory for the MHI contexts (%llu bytes)\n", mhi_ctxt_size);
		return false;
	}
	mhi_ctxt_phys = kvtophys(mhi_ctxt_va);
	line("%llu bytes at physical %#llx: the rings first, the contexts from %#llx", mhi_ctxt_size,
	    mhi_ctxt_phys, mhi_ctxt_phys + (uint64_t)cmd_ctxt);
	mhi_er_ctxt_va = mhi_ctxt_va + er_ctxt;
	mhi_er_ring_phys = mhi_ctxt_phys + er_ring;
	mhi_cmd_ctxt_phys = mhi_ctxt_phys + (uint64_t)cmd_ctxt;
	mhi_chan_ctxt_phys = mhi_ctxt_phys + (uint64_t)chan_ctxt;
	mhi_er_ctxt_phys = mhi_ctxt_phys + (uint64_t)er_ctxt;
	/* the command context: its ring, empty (rp = wp = base) */
	ctxt64(mhi_ctxt_va + cmd_ctxt, 0x0c, mhi_ctxt_phys + cmd_ring);
	ctxt64(mhi_ctxt_va + cmd_ctxt, 0x14, MHI_RING_ELS * MHI_RING_EL);
	ctxt64(mhi_ctxt_va + cmd_ctxt, 0x1c, mhi_ctxt_phys + cmd_ring);
	ctxt64(mhi_ctxt_va + cmd_ctxt, 0x24, mhi_ctxt_phys + cmd_ring);
	for (uint32_t i = 0; i < mhi_ner; i++) {
		vm_offset_t c = mhi_ctxt_va + er_ctxt + i * MHI_CTXT_EL;
		uint64_t base = mhi_er_ring_phys + i * MHI_RING_ELS * MHI_RING_EL;

		*(volatile uint32_t *)(c + 0x04) = MHI_ER_TYPE_VALID;   /* ertype; msivec 0: no MSI here */
		ctxt64(c, 0x0c, base);
		ctxt64(c, 0x14, MHI_RING_ELS * MHI_RING_EL);
		ctxt64(c, 0x1c, base);
		ctxt64(c, 0x24, base);
	}
	line("the command context and %u event contexts are filled; %u channel contexts left zero "
	    "(disabled, as the vendor leaves them until a channel is started)", mhi_ner, mhi_nch);
	pause_group();
	return true;
}

/* step 17: the MMIO programming and the move to M0 */

static bool
step_mhi_m0(void)
{
	uint32_t cfg, status = 0, state = 0, waited;

	announce("the MHI's MMIO: the context and window base registers, the event rings' doorbells rung, "
	    "then the move to M0");
	/*
	 * mhi_init.c's mhi_init_mmio, with the physical addresses standing in for the iova (no SMMU
	 * is programmed: the windows below say the device may touch exactly this piece).
	 */
	mhi_put64(8, MHI_CCABAP, mhi_chan_ctxt_phys, "ccabap (the channel contexts)");
	mhi_put64(8, MHI_ECABAP, mhi_er_ctxt_phys, "ecabap (the event contexts)");
	mhi_put64(8, MHI_CRCBAP, mhi_cmd_ctxt_phys, "crcbap (the command context)");
	cfg = rd(reg(8, MHI_MHICFG));
	put(8, "mhi cfg (the event ring count the host provides)", MHI_MHICFG,
	    (cfg & ~0xff0000u) | (mhi_ner << 16));
	mhi_put64(8, MHI_CTRLBASE, mhi_ctxt_phys, "ctrlbase");
	mhi_put64(8, MHI_CTRLLIMIT, mhi_ctxt_phys + mhi_ctxt_size, "ctrllimit");
	mhi_put64(8, MHI_DATABASE, mhi_ctxt_phys, "database");
	mhi_put64(8, MHI_DATALIMIT, mhi_ctxt_phys + mhi_ctxt_size, "datalimit");
	/* the wake doorbell, zeroed as the vendor zeroes it */
	wr(reg(8, mhi_chdboff + 8 * MHI_DEV_WAKE_DB) + 4, 0);
	wr(reg(8, mhi_chdboff + 8 * MHI_DEV_WAKE_DB), 0);
	pause_group();
	/*
	 * mhi_ready_state_transition's last acts before M0: every event ring is given all of its
	 * elements (wp = the last one), and its doorbell is rung with that pointer -- the high word
	 * first, as mhi_write_db does.
	 */
	for (uint32_t i = 0; i < mhi_ner; i++) {
		uint64_t wp = mhi_er_ring_phys + (i + 1) * MHI_RING_ELS * MHI_RING_EL - MHI_RING_EL;

		ctxt64(mhi_er_ctxt_va + i * MHI_CTXT_EL, 0x24, wp);
		wr(reg(8, mhi_erdboff + 8 * i) + 4, (uint32_t)(wp >> 32));
		wr(reg(8, mhi_erdboff + 8 * i), (uint32_t)wp);
	}
	line("%u event rings rung", mhi_ner);
	pause_group();
	put(8, "mhi ctrl (M0)", MHI_MHICTRL, MHI_STATE_M0 << 8);
	for (waited = 0; waited < 50; waited++) {       /* five seconds for the state to answer */
		status = rd(reg(8, MHI_MHISTATUS));
		state = (status >> 8) & 0xff;
		if (state == MHI_STATE_M0 || (status & MHI_STATUS_SYSERR)) {
			break;
		}
		if ((waited % 5) == 0) {
			line("the MHI state is %u after %u ms (status %#x)", state, waited * 100, status);
		}
		sleep_ms(100);
	}
	line("the MHI state is %u after %u ms (status %#x)%s", state, waited * 100, status,
	    (status & MHI_STATUS_SYSERR) ? ": SYSERR" : "");
	if (state != MHI_STATE_M0) {
		say("no M0\n");
		return false;
	}
	/* what the device said while moving: event ring 0's elements up to the context's read pointer */
	{
		uint32_t rp_lo = *(volatile uint32_t *)(mhi_er_ctxt_va + 0x1c);
		uint64_t done = (uint64_t)rp_lo - mhi_er_ring_phys;

		line("event ring 0: the device read %llu bytes of events", done);
		for (uint64_t e = 0; e < done && e < 4 * MHI_RING_EL; e += MHI_RING_EL) {
			volatile uint32_t *tre = (volatile uint32_t *)(mhi_ctxt_va + e);

			line("  event: %#x %#x %#x %#x", tre[0], tre[1], tre[2], tre[3]);
		}
	}
	return true;
}

/* ---- what runs ------------------------------------------------------------------------------ */

/*
 * In the order the vendor driver takes them (pci-msm.c's msm_pcie_enable and msm_pcie_link_train): the
 * controller's clocks and its own registers first, then the PHY's power-on and its sequence, then the
 * endpoint out of reset and its training registers before the state machine goes on.
 */
static bool (*const steps[])(void) = {
	step_look,              /* 1  read what the boot loader and Android left */
	step_power,             /* 2  the PHY's three rails (1.2 V, 0.88 V, cx nominal), voted through RPMh */
	step_gdsc,              /* 3  the radio's own enable line, PERST low, the controller's own power switch */
	step_clocks,            /* 4  the six clock branches in 0x52008 and the endpoint's reference in 0x8c004 */
	step_aux_src,           /* 5  the aux clock's source generator (the wrapper's APB clock) */
	step_parf_init,         /* 6  the vendor's register writes, the core reset, the reference generator */
	step_phy,               /* 7  the PHY's auxiliary clock and a look at its status */
	step_phy_seq,           /* 8  the PHY's own 99 pairs and its ready bit */
	step_clocks_dump,       /* 9  the whole PCIe0 corner of the clock controller */
	step_parf,              /* 10 the wrapper's registers, once everything above is on */
	step_ltssm,             /* 11 PERST out, the vendor's training words, the state machine on, the vendor's checks */
	step_ep_cfg,            /* 12 the endpoint's config space, its BAR assigned, the iatu pointed at it */
	step_mhi_look,          /* 13 the MHI and BHI registers behind the BAR */
	step_bhi_amss,          /* 14 the WLAN firmware (amss20.bin) downloaded through BHI */
	step_mhi_ready,         /* 15 the firmware's boot to AMSS, then the READY bit */
	step_mhi_ctxt,          /* 16 the context arrays and rings, in one piece of physical memory */
	step_mhi_m0,            /* 17 the MMIO bases programmed, the event rings rung, the move to M0 */
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
	line("the %u blocks are mapped", BLOCK_NAMES);
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
			sleep_ms(delay_ms);
			/*
			 * Announced through line() and not printf: the loader only writes what went through
			 * loader_log_line into its log, and the run of 2026-11-03 died in the (18 s, 20 s)
			 * window with the thread not due until 20.5 s -- this line is what tells whether the
			 * sleep even ended (p16).
			 */
			line("the %u ms delay is over; the thread comes next", delay_ms);
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
	line("the steps thread is on the processor");
	/* the blocks were mapped before this thread was started: alioth_pcie_init() ran map_all() first */
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
