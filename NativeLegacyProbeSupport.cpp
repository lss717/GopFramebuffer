// SPDX-License-Identifier: BSD-2-Clause-Patent
// OP5 legacy-display bring-up support. Read-only OSM observation only.
// The early CPU/OSM fix lives in sheng_pmgr_run(), before vfs_mountroot.
#include <stdint.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>

volatile uint32_t native_cpu_freq_state;

namespace {
constexpr uint64_t kOsmBase = 0x179c0000ULL;
constexpr uint64_t kOsmBytes = 0x4000;
constexpr uint64_t kCpuClockBases[2] = {0x17910000ULL, 0x17810000ULL};

static uint32_t osmRead(volatile uint32_t *base, uint32_t offset) {
    uint32_t value = base[offset / 4];
    __asm__ volatile ("dmb oshld" ::: "memory");
    return value;
}

// H210: the OSM window is mapped once and resampled by the 1 Hz display
// heartbeat, so the status cell shows the current state instead of the single
// boot-time sample. Reading through an uncached mapping needs no locks and is
// safe from the timer callback.
static IOMemoryMap *gOsmMap;
static volatile uint32_t *gOsm;

// 1 = an enabled cluster is below 1 GHz, 2 = enabled and at least 1 GHz,
// 3 = OSM not readable. log != 0 prints the per-cluster decode once.
static uint32_t osmSample(int log) {
    if (!gOsm)
        return 3;
    bool low = false, any = false;
    for (unsigned cluster = 0; cluster < 2; ++cluster) {
        uint32_t off = cluster * 0x2000;
        uint32_t enable = osmRead(gOsm, off + 0x1004);
        uint32_t desired = osmRead(gOsm, off + 0x1f10);
        uint32_t frequency = 0;
        if (desired < 40)
            frequency = osmRead(gOsm, off + 0x1150 + desired * 32 + 4);
        uint32_t source = (frequency >> 26) & 3;
        uint32_t multiplier = frequency & 0xff;
        uint32_t hz = source ? 19200000U * multiplier : 300000000U;
        if (log) {
            IOLog("NATIVE_CPUFREQ: cluster=%u enable=%u desired=%u raw=%08x source=%u mult=%u hz=%u\n",
                  cluster, enable, desired, frequency, source, multiplier, hz);
        }
        if (enable & 1) {
            any = true;
            if (hz < 1000000000U)
                low = true;
        }
    }
    /* H213: no cluster reports enabled means the OSM state is not readable here at all
     * (H211 read enable=0, desired=0, raw=0 for both clusters), not that the CPU runs
     * slowly. Report it as unknown so the cell does not sit on a misleading red. */
    return any ? (low ? 1U : 2U) : 6U;
}
}

extern "C" {
void native_hid_input_probe(void) {}
void native_keystore_probe(void) {}
void native_touch_probe(void) {}
void sheng_cpufreq_probe(void) {
    if (!gOsm) {
        IODeviceMemory *memory = IODeviceMemory::withRange(kOsmBase, kOsmBytes);
        IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        if (!map) {
            native_cpu_freq_state = 3;
            IOLog("NATIVE_CPUFREQ: OSM map failed\n");
            return;
        }
        volatile uint32_t *base = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
        if (!base) {
            OSSafeReleaseNULL(map);
            native_cpu_freq_state = 3;
            IOLog("NATIVE_CPUFREQ: OSM virtual map null\n");
            return;
        }
        // Retained for sheng_cpufreq_tick(): one mapping for the life of the boot.
        gOsmMap = map;
        gOsm = base;
    }
    native_cpu_freq_state = osmSample(1);
    /* H213: one-shot register map, to find where this SoC keeps the current CPU state.
     * The OSM window reads all zero, so the APCS PLL/divider is printed as well. */
    IOLog("NATIVE_CPUFREQ_OSM: c0 0x0=%08x 0x4=%08x 0x1000=%08x 0x1004=%08x 0x1f10=%08x 0x1f14=%08x\n",
          osmRead(gOsm, 0x0), osmRead(gOsm, 0x4), osmRead(gOsm, 0x1000), osmRead(gOsm, 0x1004),
          osmRead(gOsm, 0x1f10), osmRead(gOsm, 0x1f14));
    IOLog("NATIVE_CPUFREQ_OSM: c1 0x0=%08x 0x4=%08x 0x1000=%08x 0x1004=%08x 0x1f10=%08x 0x1f14=%08x\n",
          osmRead(gOsm, 0x2000), osmRead(gOsm, 0x2004), osmRead(gOsm, 0x3000), osmRead(gOsm, 0x3004),
          osmRead(gOsm, 0x3f10), osmRead(gOsm, 0x3f14));
    for (unsigned cluster = 0; cluster < 2; ++cluster) {
        IODeviceMemory *apcsMemory = IODeviceMemory::withRange(cluster ? 0x17810000ULL : 0x17910000ULL, 0x10000);
        IOMemoryMap *apcsMap = apcsMemory ?
            apcsMemory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
        OSSafeReleaseNULL(apcsMemory);
        if (!apcsMap) {
            IOLog("NATIVE_CPUFREQ_APCS: cluster=%u map failed\n", cluster);
            continue;
        }
        volatile uint32_t *apcs = reinterpret_cast<volatile uint32_t *>(apcsMap->getVirtualAddress());
        if (apcs)
            IOLog("NATIVE_CPUFREQ_APCS: cluster=%u pll_l=%08x rcgr=%08x\n", cluster,
                  osmRead(apcs, 0x6004), osmRead(apcs, 0x1054));
        OSSafeReleaseNULL(apcsMap);
    }
    for (unsigned cluster = 0; cluster < 2; ++cluster) {
        IODeviceMemory *clockMemory = IODeviceMemory::withRange(kCpuClockBases[cluster], 0x8000);
        IOMemoryMap *clockMap = clockMemory ?
            clockMemory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
        OSSafeReleaseNULL(clockMemory);
        if (!clockMap) {
            IOLog("NATIVE_CPUFREQ: cluster=%u clock map failed\n", cluster);
            continue;
        }
        volatile uint32_t *clock = reinterpret_cast<volatile uint32_t *>(clockMap->getVirtualAddress());
        if (clock)
            IOLog("NATIVE_CPUFREQ_CLOCK: cluster=%u cmd=%08x cfg=%08x status=%08x\n",
                  cluster, osmRead(clock, 0x0), osmRead(clock, 0x4), osmRead(clock, 0x8));
        OSSafeReleaseNULL(clockMap);
    }
}
// H210: called from the display heartbeat (timer context, 1 Hz). Updates the
// state only; no logging and no allocation.
void sheng_cpufreq_tick(void) {
    if (gOsm)
        native_cpu_freq_state = osmSample(0);
}
// C1 baseline: targeted GPUCC state read (msm8998 GPUCC, same block as the A540 vendor DT).
// H221 scanned 0x0000.. and the bus hung after 0x0078, so this reads only the registers the
// msm8998 GPUCC driver actually programs (mainline drivers/clk/qcom/gpucc-msm8998.c):
// fabia PLL gpupll0 at 0x0, the GX clamp at 0x130, and the CX/GX GDSC + branch clocks at
// 0x1000..0x1124. Those must stay accessible or nothing could ever power the domain up.
void sheng_gpu_power_probe(void) {
    static const struct { uint32_t off; const char *name; } regs[] = {
        {0x0000, "gpupll0_lval"},   {0x0004, "gpupll0_config"},
        {0x0008, "gpupll0_config_ctl"}, {0x000c, "gpupll0_user_ctl"},
        {0x0010, "gpupll0_user_ctl_u"}, {0x0014, "gpupll0_state"},
        {0x0020, "gpupll0_status"},
        {0x0130, "gx_clamp_io"},
        {0x1000, "cx_bcr"},        {0x1004, "cx_gdscr"},      {0x1008, "cx_gds_hw_ctrl"},
        {0x1020, "cxo_branch"},
        {0x1030, "rbcpr_rcg"},     {0x1050, "rbcpr_bcr"},     {0x1054, "rbcpr_branch"},
        {0x1070, "gfx3d_rcg"},     {0x1090, "gx_bcr"},        {0x1094, "gx_gdscr"},
        {0x1098, "gfx3d_branch"},  {0x10b0, "rbbmtimer_rcg"}, {0x10d0, "rbbmtimer_branch"},
        {0x1100, "isense_rcg"},    {0x1120, "isense_bcr"},    {0x1124, "isense_branch"},
    };
    IODeviceMemory *memory = IODeviceMemory::withRange(0x5065000ULL, 0x9000);
    IOMemoryMap *map = memory ?
        memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!map) {
        IOLog("NATIVE_GPUCC: map failed\n");
        return;
    }
    volatile uint32_t *base = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
    if (!base) {
        IOLog("NATIVE_GPUCC: virtual map null\n");
        OSSafeReleaseNULL(map);
        return;
    }
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i)
        IOLog("NATIVE_GPUCC: %s[%04x]=%08x\n", regs[i].name, regs[i].off, osmRead(base, regs[i].off));
    IOLog("NATIVE_GPUCC: state done\n");
    OSSafeReleaseNULL(map);
}
void sheng_wifi_probe(void) {}
void sheng_adsp_probe(void) {}
void sheng_battery_probe(void) {}
void sheng_audio_probe(void) {}
void sheng_buttons_probe(void) {}
void sheng_backlight_probe(void) {}
void sheng_dpu_probe(void) {}
int boot_progress_prepare_scanout(void) { return 1; }
void boot_progress_cancel_scanout(void) {}
void boot_progress_stage(const char *) {}
int boot_progress_pulse_anchor(uintptr_t,uint64_t,unsigned,unsigned,unsigned) { return 0; }
}
