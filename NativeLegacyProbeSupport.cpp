// SPDX-License-Identifier: BSD-2-Clause-Patent
// OP5 legacy-display bring-up support. Read-only OSM observation only.
// The early CPU/OSM fix lives in sheng_pmgr_run(), before vfs_mountroot.
#include <stdint.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

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
// H223: GPUCC survey state (see sheng_gpucc_report).
static IOMemoryMap *sheng_gpucc_map;
static volatile uint32_t *sheng_gpucc_base;
static unsigned sheng_gpucc_done;
static unsigned sheng_rbbm_moved;
static unsigned sheng_rpm_moved;
static unsigned sheng_mem_moved;
static unsigned sheng_vm_moved;
static unsigned sheng_fb_moved;
static unsigned sheng_scan_moved;
// H228: the three RBBM identity words once the power-up step read them.
static volatile uint32_t gRbbmValues[3];
static volatile int gRbbmValid;
// C1d: read-only SMEM/GLINK survey words, reported through the gc= channel (30..35).
static volatile uint32_t gRpmWords[6];
// C1d-2: the same probe applied to memory with known content (GPU addon magic) to prove the
// mapping path works before concluding that SMEM itself is empty.
static volatile uint32_t gMemWords[3];
// C1d-3: [gPhysBase, mem_size, avail_start, avail_end] from arm_vm_init.
extern "C" uint64_t native_vm_range[4];
static volatile int gMemValid;
// C1d-4: low-memory control read. The GOP framebuffer at 0x9d400000 always holds the boot
// splash, so it proves whether low physical memory is readable in this kernel at all.
static volatile uint32_t gFbWords[3];
static volatile int gFbValid;
// C1d-4b: bounded SMEM scan over 0x80000000..0xa0000000 in 2 MiB steps.
static volatile uint32_t gScanBase[4];
static volatile uint32_t gScanValue[4];
static volatile int gScanHits;
static volatile int gScanDone;

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
// H225: neither a burst of lines (H223: dropped by the writer's snapshot) nor a slow stream
// (H224: the writer reset the window at ~371 s and the middle was lost) reaches the pulled log
// reliably. The heartbeat line is the one thing that always survives, so the survey hands two
// register values per call to the caller, which prints them inside NATIVE_ACCEL_HB.
static const struct { uint32_t off; const char *name; } kGpuccRegs[] = {
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
// The late probe can no longer print the survey: it runs before the log ring starts capturing
// and a burst is dropped. The heartbeat prints sheng_gpucc_pair() instead.
void sheng_gpu_power_probe(void) {}
// The late probe can no longer print the survey: it runs before the log ring starts capturing
// and a burst is dropped. The heartbeat prints one indexed value per beat instead.
int sheng_gpucc_count(void) {
    return (int)(sizeof(kGpuccRegs) / sizeof(kGpuccRegs[0]));
}
static int sheng_gpucc_step(unsigned *index, uint32_t *value) {
    unsigned total = sizeof(kGpuccRegs) / sizeof(kGpuccRegs[0]);
    if (sheng_gpucc_done >= total)
        return 0;
    if (!sheng_gpucc_map) {
        IODeviceMemory *memory = IODeviceMemory::withRange(0x5065000ULL, 0x9000);
        IOMemoryMap *map = memory ?
            memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
        OSSafeReleaseNULL(memory);
        volatile uint32_t *base = map ?
            reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
        if (!base) {
            IOLog("NATIVE_GPUCC: map failed\n");
            sheng_gpucc_done = total;
            if (map) OSSafeReleaseNULL(map);
            return 0;
        }
        sheng_gpucc_map = map;
        sheng_gpucc_base = base;
    }
    unsigned at = sheng_gpucc_done;
    uint32_t v = osmRead(sheng_gpucc_base, kGpuccRegs[at].off);
    sheng_gpucc_done = at + 1;
    if (index) *index = at;
    if (value) *value = v;
    if (sheng_gpucc_done >= total) {
        IOLog("NATIVE_GPUCC: survey complete\n");
        OSSafeReleaseNULL(sheng_gpucc_map);
        sheng_gpucc_base = nullptr;
    }
    return 1;
}
int sheng_gpucc_pair(uint32_t *first, uint32_t *second) {
    unsigned index = 0;
    uint32_t value = 0;
    int got = 0;
    if (sheng_gpucc_step(&index, &value)) {
        if (first) *first = value;
        got = 1;
    }
    if (sheng_gpucc_step(&index, &value)) {
        if (second) *second = value;
        got = got ? 2 : 1;
    }
    return got;
}
// H228: indices 24..26 are the three RBBM words, available once the power-up read them.
int sheng_gpucc_next(unsigned *index, uint32_t *value) {
    unsigned at = 0;
    uint32_t v = 0;
    unsigned total = sizeof(kGpuccRegs) / sizeof(kGpuccRegs[0]);
    if (gRbbmValid && sheng_gpucc_done >= total && sheng_rbbm_moved < 3) {
        unsigned k = sheng_rbbm_moved++;
        if (index) *index = total + k;
        if (value) *value = gRbbmValues[k];
        return 1;
    }
    if (gRbbmValid && sheng_gpucc_done >= total && sheng_rpm_moved < 6) {
        unsigned k = sheng_rpm_moved++;
        if (index) *index = 30 + k;
        if (value) *value = gRpmWords[k];
        return 1;
    }
    if (gMemValid && sheng_gpucc_done >= total && sheng_mem_moved < 3) {
        unsigned k = sheng_mem_moved++;
        if (index) *index = 36 + k;
        if (value) *value = gMemWords[k];
        return 1;
    }
    if (gScanDone && sheng_gpucc_done >= total && sheng_scan_moved == 0) {
        sheng_scan_moved = 1;
        if (index) *index = 60;
        if (value) *value = (uint32_t)gScanHits;
        return 1;
    }
    if (gScanDone && sheng_gpucc_done >= total && sheng_scan_moved >= 1 && sheng_scan_moved <= 8) {
        unsigned k = (sheng_scan_moved - 1) / 2;
        unsigned half = (sheng_scan_moved - 1) % 2;
        sheng_scan_moved++;
        if (index) *index = 61 + (k * 2) + half;
        if (value) *value = half ? gScanValue[k] : gScanBase[k];
        return 1;
    }
    if (gFbValid && sheng_gpucc_done >= total && sheng_fb_moved < 3) {
        unsigned k = sheng_fb_moved++;
        if (index) *index = 48 + k;
        if (value) *value = gFbWords[k];
        return 1;
    }
    // C1d-3: indices 40..47 are the 64-bit VM range numbers, two 32-bit halves each.
    if (gMemValid && sheng_gpucc_done >= total && sheng_vm_moved < 8) {
        unsigned k = sheng_vm_moved++;
        uint64_t v = native_vm_range[k >> 1];
        if (index) *index = 40 + k;
        if (value) *value = (k & 1) ? (uint32_t)(v >> 32) : (uint32_t)v;
        return 1;
    }
    if (!sheng_gpucc_step(&at, &v))
        return 0;
    if (index) *index = at;
    if (value) *value = v;
    return 1;
}

// ---------------------------------------------------------------------------
// C1b: first GPU power-up attempt, gated by sheng-gpu-power=1.
//
// Order (each step logs before it writes, so a bus hang shows where it stopped):
//   1. release the GX clamp            GPUCC 0x0130
//   2. un-collapse the CX GDSC         GPUCC 0x1004 (clear SW_COLLAPSE, bit 0)
//   3. gfx3d_clk_src <- XO (19.2 MHz)  GPUCC 0x1074 cfg = 0, 0x1070 update
//   4. enable the gfx3d branch clock   GPUCC 0x1098 (bit 0, plus bit 13 per the msm8998 probe)
//   5. un-collapse the GX GDSC         GPUCC 0x1094
//   6. pulse the GX reset              GPUCC 0x1090
//   7. read RBBM (GPU id)              0x5000000
//
// Steps 5 and 7 touch the GX domain, which has no RPM/CX-GX rail vote in this port yet; if the
// box hangs there the log shows the last step reached.
// ---------------------------------------------------------------------------
static volatile uint32_t *gGpuccBase;
static volatile uint32_t *gRbbmBase;

static uint32_t gpuccRd(uint32_t off) { return gGpuccBase ? osmRead(gGpuccBase, off) : 0xffffffffu; }
static void gpuccWr(uint32_t off, uint32_t value) {
    if (!gGpuccBase) return;
    gGpuccBase[off / 4] = value;
    __asm__ volatile ("dmb oshst" ::: "memory");
}
static void gpuDelay(unsigned rounds) {
    for (unsigned i = 0; i < rounds; ++i)
        __asm__ volatile ("yield" ::: "memory");
}
// Mainline gdsc.c: status is PWR_ON = BIT(31) read from gds_hw_ctrl when the GDSC has one
// (gpu_cx_gdsc: 0x1008), otherwise from the GDSCR itself (gpu_gx_gdsc: 0x1094). SW_COLLAPSE
// is BIT(0); HW_CONTROL is BIT(1); SW_OVERRIDE is BIT(2); the wait fields are 23:20, 19:16,
// 15:12 (2, 8, 2 at reset).
// C1d: the RPM on msm8998 speaks GLINK over SMEM (qcom,smem@86000000, 2 MiB;
// qcom,rpm-glink / rpm_requests). Before writing any request, prove SMEM is readable and
// find the partition table: magic "COMM" (0x434f4d4d) in the first 4 KiB.
static void sheng_rpm_survey(void) {
    IODeviceMemory *mem = IODeviceMemory::withRange(0x86000000ULL, 0x200000);
    IOMemoryMap *map = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
    OSSafeReleaseNULL(mem);
    if (!map) {
        gRpmWords[0] = 0xffffffffu;
        return;
    }
    volatile uint32_t *base = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
    if (!base) {
        OSSafeReleaseNULL(map);
        gRpmWords[0] = 0xfffffffeu;
        return;
    }
    const uint32_t *w = const_cast<const uint32_t *>(base);
    /* same mechanism on RAM whose first bytes are known: "NGPUADD2" -> 0x5550474e, 0x32444441 */
    {
        IODeviceMemory *known = IODeviceMemory::withRange(0x185f58000ULL, 0x1000);
        IOMemoryMap *knownMap = known ? known->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
        OSSafeReleaseNULL(known);
        if (knownMap) {
            volatile uint32_t *kw = reinterpret_cast<volatile uint32_t *>(knownMap->getVirtualAddress());
            if (kw) {
                gMemWords[0] = kw[0];
                gMemWords[1] = kw[1];
                gMemWords[2] = kw[2];
                gMemValid = 1;
            }
            OSSafeReleaseNULL(knownMap);
        }
    }
    /* low-memory control: the framebuffer the display backend already maps and uses */
    {
        IODeviceMemory *fb = IODeviceMemory::withRange(0x9d400000ULL, 0x1000);
        IOMemoryMap *fbMap = fb ? fb->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
        OSSafeReleaseNULL(fb);
        if (fbMap) {
            volatile uint32_t *fw = reinterpret_cast<volatile uint32_t *>(fbMap->getVirtualAddress());
            if (fw) {
                gFbWords[0] = fw[0];
                gFbWords[1] = fw[1];
                gFbWords[2] = fw[2];
                gFbValid = 1;
            }
            OSSafeReleaseNULL(fbMap);
        }
    }
    gRpmWords[0] = w[0];
    gRpmWords[1] = w[1];
    gRpmWords[2] = w[2];
    unsigned magicAt = 0;
    for (unsigned off = 0; off < 0x1000; off += 4) {
        if (w[off / 4] == 0x434f4d4du) {
            magicAt = off;
            break;
        }
    }
    gRpmWords[3] = magicAt;
    if (magicAt) {
        gRpmWords[4] = w[magicAt / 4 + 1];   /* version */
        gRpmWords[5] = w[magicAt / 4 + 2];   /* num_entries */
    }
    OSSafeReleaseNULL(map);
}
// C1d-4b: the vendor DT puts SMEM at 0x86000000, but that region reads zero on this boot path.
// Scan the plausible low-DRAM window in 2 MiB steps and remember the first four non-zero heads:
// the SMEM header (or anything else that lives there) shows up as base/value pairs.
static void sheng_smem_scan(void) {
    int hits = 0;
    for (uint64_t window = 0x80000000ULL; window < 0xA0000000ULL && hits < 4; window += 0x2000000ULL) {
        IODeviceMemory *mem = IODeviceMemory::withRange(window, 0x2000000);
        IOMemoryMap *map = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
        OSSafeReleaseNULL(mem);
        if (!map)
            continue;
        volatile uint32_t *base = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
        if (base) {
            for (uint64_t off = 0; off < 0x2000000ULL && hits < 4; off += 0x200000ULL) {
                uint32_t value = osmRead(base, (uint32_t)off);
                if (value) {
                    gScanBase[hits] = (uint32_t)(window + off);
                    gScanValue[hits] = value;
                    ++hits;
                }
            }
        }
        OSSafeReleaseNULL(map);
    }
    gScanHits = hits;
    gScanDone = 1;
}
static int gpuGdscOn(const char *name, uint32_t gdscr, uint32_t statusReg) {
    uint32_t before = gpuccRd(gdscr);
    uint32_t value = before;
    value &= ~(1u << 1);                 /* HW_CONTROL off: software drives the sequencer */
    value &= ~(1u << 2);                 /* SW_OVERRIDE off */
    value &= ~(0xfu << 20); value |= (2u << 20);
    value &= ~(0xfu << 16); value |= (8u << 16);
    value &= ~(0xfu << 12); value |= (2u << 12);
    value &= ~1u;                        /* SW_COLLAPSE = 0 -> power on */
    gpuccWr(gdscr, value);
    gpuDelay(64);                        /* gdsc.c waits 1 us before polling */
    int on = 0;
    for (unsigned i = 0; i < 200; ++i) {
        if (gpuccRd(statusReg) & (1u << 31)) {
            on = 1;
            break;
        }
        gpuDelay(64);
    }
    IOLog("NATIVE_GPU_PWR: %s gdscr %08x->%08x status[%03x]=%08x on=%d\n", name, before,
          gpuccRd(gdscr), statusReg, gpuccRd(statusReg), on);
    return on;
}
static __attribute__((unused)) int gpuWaitBit(uint32_t off, uint32_t bit, int wantSet, unsigned rounds) {
    for (unsigned i = 0; i < rounds; ++i) {
        uint32_t v = gpuccRd(off);
        if (((v & bit) != 0) == (wantSet != 0))
            return 1;
        gpuDelay(64);
    }
    return 0;
}

// One step per call so each log line is separated by a heartbeat beat (a burst is dropped by
// the log writer's snapshot window, see the H223/H224 notes above).
int sheng_gpu_power_step(void) {
    static unsigned step;
    static int enabled = -1;
    if (enabled < 0) {
        uint32_t value = 0;
        enabled = PE_parse_boot_argn("sheng-gpu-power", &value, sizeof value) && value == 1;
    }
    if (!enabled || step > 6)
        return 0;
    if (step == 0) {
        IODeviceMemory *cc = IODeviceMemory::withRange(0x5065000ULL, 0x9000);
        IOMemoryMap *ccMap = cc ? cc->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(cc);
        if (!ccMap) {
            IOLog("NATIVE_GPU_PWR: GPUCC map failed\n");
            step = 99;
            return 0;
        }
        gGpuccBase = reinterpret_cast<volatile uint32_t *>(ccMap->getVirtualAddress());
        IOLog("NATIVE_GPU_PWR: start clamp=%08x cx=%08x gx=%08x gfx3d_rcg=%08x\n",
              gpuccRd(0x0130), gpuccRd(0x1004), gpuccRd(0x1094), gpuccRd(0x1070));
        ++step;
        return 1;
    }
    if (step == 1) {
        IOLog("NATIVE_GPU_PWR: step1 release gx clamp\n");
        gpuccWr(0x0130, 0x0);
        gpuDelay(256);
        IOLog("NATIVE_GPU_PWR: clamp now=%08x\n", gpuccRd(0x0130));
        ++step;
        return 1;
    }
    if (step == 2) {
        IOLog("NATIVE_GPU_PWR: step2 cx gdsc on\n");
        gpuGdscOn("cx", 0x1004, 0x1008);
        ++step;
        return 1;
    }
    if (step == 3) {
        IOLog("NATIVE_GPU_PWR: step3 gfx3d_clk_src <- xo\n");
        gpuccWr(0x1074, 0x00000000u);                    /* src = XO, div = 0 */
        gpuccWr(0x1070, (gpuccRd(0x1070) & ~(1u << 31)) | 1u); /* clear ROOT_OFF, RCG_UPDATE */
        gpuDelay(256);
        IOLog("NATIVE_GPU_PWR: gfx3d_rcg=%08x cfg=%08x\n", gpuccRd(0x1070), gpuccRd(0x1074));
        ++step;
        return 1;
    }
    if (step == 4) {
        IOLog("NATIVE_GPU_PWR: step4 gfx3d branch on\n");
        uint32_t branch = gpuccRd(0x1098);
        gpuccWr(0x1098, (branch | (1u << 13)) & ~(1u << 31));
        gpuccWr(0x1098, (branch | (1u << 13) | 1u) & ~(1u << 31));
        int running = 0;
        for (unsigned i = 0; i < 2000 && !running; ++i) {
            if (!(gpuccRd(0x1098) & (1u << 31)))
                running = 1;
            else
                gpuDelay(64);
        }
        IOLog("NATIVE_GPU_PWR: gfx3d_branch=%08x running=%d\n", gpuccRd(0x1098), running);
        ++step;
        return 1;
    }
    if (step == 5) {
        IOLog("NATIVE_GPU_PWR: step5 gx gdsc on\n");
        gpuGdscOn("gx", 0x1094, 0x1094);
        ++step;
        return 1;
    }
    if (step == 6) {
        IOLog("NATIVE_GPU_PWR: step6 gx reset pulse\n");
        gpuccWr(0x1090, 0x1);
        gpuDelay(256);
        gpuccWr(0x1090, 0x0);
        gpuDelay(256);
        IOLog("NATIVE_GPU_PWR: gx_bcr=%08x\n", gpuccRd(0x1090));

        IOLog("NATIVE_GPU_PWR: step7 rbbm read\n");
        IODeviceMemory *rm = IODeviceMemory::withRange(0x5000000ULL, 0x40000);
        IOMemoryMap *rmMap = rm ? rm->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
        OSSafeReleaseNULL(rm);
        if (!rmMap) {
            IOLog("NATIVE_GPU_PWR: RBBM map failed\n");
        } else {
            gRbbmBase = reinterpret_cast<volatile uint32_t *>(rmMap->getVirtualAddress());
            if (gRbbmBase) {
                gRbbmValues[0] = osmRead(gRbbmBase, 0x0000);
                gRbbmValues[1] = osmRead(gRbbmBase, 0x0004);
                gRbbmValues[2] = osmRead(gRbbmBase, 0x0210);
                gRbbmValid = 1;
                IOLog("NATIVE_GPU_PWR: rbbm_hw_version=%08x hw_release=%08x status=%08x\n",
                      gRbbmValues[0], gRbbmValues[1], gRbbmValues[2]);
            }
            OSSafeReleaseNULL(rmMap);
            gRbbmBase = nullptr;
        }
        sheng_rpm_survey();
        sheng_smem_scan();
        IOLog("NATIVE_GPU_PWR: done\n");
        ++step;
        return 0;
    }
    return 0;
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
