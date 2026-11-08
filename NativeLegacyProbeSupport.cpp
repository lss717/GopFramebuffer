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
static unsigned sheng_spmi_moved;
static unsigned sheng_spmi2_moved;
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
// C1e-1: SPMI PMIC arbiter read-only identity words (gc=70..77).
static volatile uint32_t gSpmiWords[8];
static volatile int gSpmiDone;
// C1e-2: PM8005 S1 survey/sequence results, emitted through the gc= channel as soon as
// each one is produced (index 80 and up). Keeping them ahead of the one-per-beat GPUCC
// survey means a failed write at boot still reaches the pulled log in this boot.
static volatile uint32_t gSpmi2Words[40];
static volatile int gSpmi2Count;
// C1e-2: SPMI PMIC-arbiter transport state (see the C1e-2 block below).
static IOMemoryMap *gSpmiCoreMap, *gSpmiCnfgMap, *gSpmiChnlsMap, *gSpmiObsrvrMap;
static volatile uint32_t *gSpmiCore, *gSpmiCnfg, *gSpmiChnls, *gSpmiObsrvr;
static int gSpmiApid = -1;
static int gSpmiOwner = -1;
static uint32_t gSpmiVersion;

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
    // C1e-2: emit each PM8005 result on the very next beat, before the slower GPUCC survey.
    if (sheng_spmi2_moved < (unsigned)gSpmi2Count) {
        unsigned k = sheng_spmi2_moved++;
        if (index) *index = 80 + k;
        if (value) *value = gSpmi2Words[k];
        return 1;
    }
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
    if (gSpmiDone && sheng_gpucc_done >= total && sheng_spmi_moved < 8) {
        unsigned k = sheng_spmi_moved++;
        if (index) *index = 70 + k;
        if (value) *value = gSpmiWords[k];
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
// C1e-1: the SPMI PMIC arbiter (qcom,spmi@800f000) is the only remaining route to the GX rail
// (PM8005 S1, slave 5). Before writing anything, read two register windows: core at 0x800f000
// and cnfg at 0x800a000. A non-zero version/EE field proves the controller is present.
static void sheng_spmi_survey(void) {
    static const struct { uint64_t base; uint64_t bytes; unsigned slot; } regions[] = {
        { 0x0800f000ULL, 0x1000, 0 },
        { 0x0800a000ULL, 0x3000, 4 },
    };
    for (unsigned r = 0; r < 2; ++r) {
        IODeviceMemory *mem = IODeviceMemory::withRange(regions[r].base, regions[r].bytes);
        IOMemoryMap *map = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
        OSSafeReleaseNULL(mem);
        if (!map)
            continue;
        volatile uint32_t *base = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
        if (base) {
            for (unsigned k = 0; k < 4; ++k)
                gSpmiWords[regions[r].slot + k] = osmRead(base, k * 4);
        }
        OSSafeReleaseNULL(map);
    }
    gSpmiDone = 1;
}
// ---------------------------------------------------------------------------
// C1e-2: SPMI PMIC-arbiter transport and PM8005 S1.
//
// The arbiter is not memory-mapped to the PMIC: a transaction names a channel by APID,
// and the APID comes from the linear PPID map the arbiter keeps at core+0x800+4*apid
// (vendor drivers/spmi/spmi-pmic-arb.c, v2/v3 ops; msm8998 core reads 0x30000000, which
// selects the v3 op table and therefore the v2 data path). For a PMIC register:
//
//   PPID  = (sid << 8) | (addr >> 8)          e.g. sid=5, addr=0x1441 -> 0x514
//   page  = 0x8000 * apid                     (EE 0; EE n adds 0x1000 * n)
//   CMD   = (opcode << 27) | ((addr & 0xff) << 4) | (byte_count - 1)
//           opcode 14 = write, 15 = read      (PMIC_ARB_OP_WRITE / _READ)
//
// Reads go through the "obsrvr" window (CMD/STATUS/RDATA0 at 0x00/0x08/0x18), writes
// through "chnls" (WDATA0 at 0x10 before CMD). STATUS bit 0 is DONE; bits 1..3 are
// FAILURE / DENIED / DROPPED, and DENIED is what a write from a non-owning EE returns,
// so ownership is checked in the config window (cnfg+0x700+4*apid, low 3 bits) first.
// ---------------------------------------------------------------------------
static constexpr uint32_t kSpmiEe = 0;
static constexpr uint32_t kSpmiMaxApid = 0x200;   /* (core_size 0x1000 - 0x800) / 4 */
static constexpr uint8_t  kPm8005Sid = 5;
static constexpr uint16_t kPm8005S1Base = 0x1400;
// PM8005 S1 common QPNP regulator layout (vendor drivers/regulator/qpnp-regulator.c).
enum {
    kPm8005Rev     = 0x01,
    kPm8005Type    = 0x04,
    kPm8005Subtype = 0x05,
    kPm8005Range   = 0x40,   /* smps_ranges[0]: 375000 uV + 12500 * VSET */
    kPm8005Vset    = 0x41,
    kPm8005Mode    = 0x45,   /* bit7 = HPM */
    kPm8005Enable  = 0x46,   /* bit7 = enabled */
    kPm8005Step    = 0x61,
};
// 524000 uV is the board minimum; the 12.5 mV grid makes 525000 (VSET 12) the first
// settable point at or above it.
static constexpr uint32_t kGfxTargetUv = 525000;
static constexpr uint32_t kGfxTargetVset = (kGfxTargetUv - 375000) / 12500;

static volatile uint32_t *sheng_spmi_map(uint64_t base, uint64_t size,
                                         IOMemoryMap **slot, bool writable) {
    if (*slot)
        return reinterpret_cast<volatile uint32_t *>((*slot)->getVirtualAddress());
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache |
                                            (writable ? 0 : (int)kIOMapReadOnly)) : nullptr;
    OSSafeReleaseNULL(memory);
    volatile uint32_t *p = map ?
        reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
    if (!p) {
        if (map) OSSafeReleaseNULL(map);
        return nullptr;
    }
    *slot = map;
    return p;
}
static int sheng_spmi_bus_open(void) {
    if (!gSpmiCore)
        gSpmiCore = sheng_spmi_map(0x0800f000ULL, 0x1000, &gSpmiCoreMap, false);
    if (!gSpmiCnfg)
        gSpmiCnfg = sheng_spmi_map(0x0800a000ULL, 0x3000, &gSpmiCnfgMap, false);
    if (!gSpmiCore || !gSpmiCnfg)
        return -1;
    gSpmiVersion = osmRead(gSpmiCore, 0);
    return 0;
}
// Returns the first APID mapped to this PPID whose write owner is EE 0; if none is
// owned, returns the first mapping and reports its owner so the log can explain the
// DENIED that a write would otherwise produce.
static int sheng_spmi_find_apid(uint8_t sid, uint16_t addr, int *owner) {
    uint16_t ppid = (uint16_t)(((uint16_t)sid << 8) | (addr >> 8));
    int fallback = -1, fallbackOwner = -1;
    if (!gSpmiCore || !gSpmiCnfg) {
        *owner = -1;
        return -1;
    }
    for (uint32_t apid = 0; apid < kSpmiMaxApid; ++apid) {
        if (apid == 0x1ffu)                 /* qcom,reserved-chan = <0x1ff> */
            continue;
        uint32_t entry = osmRead(gSpmiCore, 0x800 + apid * 4);
        if (!entry)
            continue;
        if (((entry >> 8) & 0xfffu) != ppid)
            continue;
        int apidOwner = (int)(osmRead(gSpmiCnfg, 0x700 + apid * 4) & 0x7u);
        if (apidOwner == (int)kSpmiEe) {
            *owner = apidOwner;
            return (int)apid;
        }
        if (fallback < 0) {
            fallback = (int)apid;
            fallbackOwner = apidOwner;
        }
    }
    *owner = fallbackOwner;
    return fallback;
}
static int sheng_spmi_open_channel(int apid) {
    if (apid < 0)
        return -1;
    if (!gSpmiChnls)
        gSpmiChnls = sheng_spmi_map(0x08400000ULL + 0x8000ULL * (uint32_t)apid, 0x1000,
                                    &gSpmiChnlsMap, true);
    if (!gSpmiObsrvr)
        gSpmiObsrvr = sheng_spmi_map(0x09400000ULL + 0x8000ULL * (uint32_t)apid, 0x1000,
                                     &gSpmiObsrvrMap, true);
    return (gSpmiChnls && gSpmiObsrvr) ? 0 : -1;
}
static volatile uint32_t gSpmiLastStatus;
static int sheng_spmi_wait(volatile uint32_t *base) {
    for (unsigned i = 0; i < 4000; ++i) {
        uint32_t status = osmRead(base, 0x08);
        if (status & 1u) {
            gSpmiLastStatus = status;
            return (status & 0xeu) ? -1 : 0;
        }
        gpuDelay(16);
    }
    gSpmiLastStatus = osmRead(base, 0x08);
    return -2;
}
static int sheng_spmi_read8(uint16_t addr, uint32_t *out) {
    if (!gSpmiObsrvr)
        return -3;
    gSpmiObsrvr[0] = (15u << 27) | ((uint32_t)(addr & 0xffu) << 4);
    __asm__ volatile ("dmb oshst" ::: "memory");
    int rc = sheng_spmi_wait(gSpmiObsrvr);
    if (!rc)
        *out = osmRead(gSpmiObsrvr, 0x18) & 0xffu;
    return rc;
}
static int sheng_spmi_write(uint16_t addr, uint32_t data, unsigned len) {
    if (!gSpmiChnls || len == 0 || len > 4)
        return -3;
    gSpmiChnls[4] = data;                                    /* WDATA0 */
    __asm__ volatile ("dmb oshst" ::: "memory");
    gSpmiChnls[0] = (14u << 27) | ((uint32_t)(addr & 0xffu) << 4) |
                    (uint32_t)((len - 1) & 7u);
    __asm__ volatile ("dmb oshst" ::: "memory");
    return sheng_spmi_wait(gSpmiChnls);
}
static void sheng_spmi2_record(uint32_t value) {
    if (gSpmi2Count < (int)(sizeof(gSpmi2Words) / sizeof(gSpmi2Words[0])))
        gSpmi2Words[gSpmi2Count++] = value;
}
// Failure marker for the gc= channel: distinguishable from any real register byte.
static uint32_t sheng_spmi_err(int rc) {
    return 0xdea00000u | ((uint32_t)(-rc) & 0xffu);
}
static int pm8005Uv(uint32_t range, uint32_t vset) {
    if (range == 0)
        return 375000 + (int)(vset * 12500u);
    if (range == 1)
        return 1550000 + (int)(vset * 25000u);
    return -1;
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

// ---------------------------------------------------------------------------
// C1e-2 state machine, selected by sheng-gpu-power=2/3/4. One step per heartbeat:
//   2  read-only PM8005 S1 survey (APID/owner + identity + voltage registers)
//   3  the same, then write the ENABLE byte back unchanged (CMD/DATA path proof)
//   4  the same, then set 525 mV, enable S1, power GX and retry the bus/clock path
// Steps 0..2 prepare CX and the clamp exactly as the C1b/C1c sequence did; nothing is
// written to the PMIC unless mode >= 3 and all eight S1 reads succeeded and the APID is
// owned by EE 0.
// ---------------------------------------------------------------------------
static const struct { uint16_t off; const char *name; } kPm8005S1Regs[] = {
    { kPm8005Rev,     "rev" },
    { kPm8005Type,    "type" },
    { kPm8005Subtype, "subtype" },
    { kPm8005Range,   "range" },
    { kPm8005Vset,    "vset" },
    { kPm8005Mode,    "mode" },
    { kPm8005Enable,  "enable" },
    { kPm8005Step,    "step" },
};
static uint32_t gS1[8];
static unsigned gS1Ok;
static int gS1Uv = -1;

static int sheng_spmi_power_step(int mode) {
    static unsigned step;
    if (mode < 2 || step > 24)
        return 0;
    if (step == 0) {
        if (!gGpuccBase) {
            IODeviceMemory *cc = IODeviceMemory::withRange(0x5065000ULL, 0x9000);
            IOMemoryMap *ccMap = cc ? cc->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
            OSSafeReleaseNULL(cc);
            if (ccMap)
                gGpuccBase = reinterpret_cast<volatile uint32_t *>(ccMap->getVirtualAddress());
        }
        if (!gGpuccBase) {
            sheng_spmi2_record(sheng_spmi_err(-8));
            IOLog("NATIVE_GPU_PWR: c1e2 GPUCC map failed\n");
            step = 99;
            return 0;
        }
        int rc = sheng_spmi_bus_open();
        if (rc) {
            sheng_spmi2_record(sheng_spmi_err(rc));
            IOLog("NATIVE_GPU_PWR: c1e2 spmi core/cnfg map failed\n");
            step = 99;
            return 0;
        }
        uint16_t ppid = (uint16_t)(((uint16_t)kPm8005Sid << 8) | (kPm8005S1Base >> 8));
        gSpmiApid = sheng_spmi_find_apid(kPm8005Sid, kPm8005S1Base, &gSpmiOwner);
        sheng_spmi2_record(gSpmiVersion);
        sheng_spmi2_record(ppid);
        sheng_spmi2_record(gSpmiApid < 0 ? 0xffffffffu : (uint32_t)gSpmiApid);
        sheng_spmi2_record(gSpmiOwner < 0 ? 0xffffffffu : (uint32_t)gSpmiOwner);
        IOLog("NATIVE_GPU_PWR: c1e2 ver=%08x ppid=%03x apid=%d owner=%d\n",
              gSpmiVersion, ppid, gSpmiApid, gSpmiOwner);
        if (gSpmiApid < 0 || sheng_spmi_open_channel(gSpmiApid)) {
            sheng_spmi2_record(sheng_spmi_err(-4));
            IOLog("NATIVE_GPU_PWR: c1e2 apid/channel unavailable\n");
            step = 99;
            return 0;
        }
        ++step;
        return 1;
    }
    if (step == 1) {
        gpuccWr(0x0130, 0x0);
        gpuDelay(256);
        sheng_spmi2_record(gpuccRd(0x0130));
        IOLog("NATIVE_GPU_PWR: c1e2 clamp=%08x\n", gpuccRd(0x0130));
        ++step;
        return 1;
    }
    if (step == 2) {
        gpuGdscOn("cx", 0x1004, 0x1008);
        sheng_spmi2_record(gpuccRd(0x1004));
        ++step;
        return 1;
    }
    if (step >= 3 && step <= 10) {
        unsigned k = step - 3;
        uint32_t value = 0;
        int rc = sheng_spmi_read8((uint16_t)(kPm8005S1Base + kPm8005S1Regs[k].off), &value);
        if (!rc) {
            gS1[k] = value;
            gS1Ok |= 1u << k;
        }
        sheng_spmi2_record(rc ? sheng_spmi_err(rc) : value);
        IOLog("NATIVE_GPU_PWR: c1e2 s1 %-7s @%04x = %08x rc=%d\n", kPm8005S1Regs[k].name,
              kPm8005S1Base + kPm8005S1Regs[k].off, value, rc);
        ++step;
        return 1;
    }
    if (step == 11) {
        gS1Uv = (gS1Ok & 0xffu) == 0xffu ? pm8005Uv(gS1[3], gS1[4]) : -1;
        sheng_spmi2_record(gS1Uv < 0 ? 0xffffffffu : (uint32_t)gS1Uv);
        IOLog("NATIVE_GPU_PWR: c1e2 s1 range=%u vset=%u -> %d uV enable=%02x ok=%02x\n",
              gS1[3], gS1[4], gS1Uv, gS1[6], gS1Ok);
        if (mode == 2) {
            IOLog("NATIVE_GPU_PWR: c1e2 survey done (read-only)\n");
            step = 99;
            return 0;
        }
        ++step;
        return 1;
    }
    if (step == 12) {
        if ((gS1Ok & 0xffu) != 0xffu || gSpmiOwner != (int)kSpmiEe) {
            sheng_spmi2_record(sheng_spmi_err(-5));
            IOLog("NATIVE_GPU_PWR: c1e2 write gate: ok=%02x owner=%d -> skipped\n",
                  gS1Ok, gSpmiOwner);
            step = 99;
            return 0;
        }
        int rc = sheng_spmi_write((uint16_t)(kPm8005S1Base + kPm8005Enable), gS1[6], 1);
        sheng_spmi2_record(rc ? sheng_spmi_err(rc) : 0x600d0000u);
        IOLog("NATIVE_GPU_PWR: c1e2 same-value write enable=%02x rc=%d status=%08x\n",
              gS1[6], rc, gSpmiLastStatus);
        ++step;
        return 1;
    }
    if (step == 13) {
        uint32_t value = 0;
        int rc = sheng_spmi_read8((uint16_t)(kPm8005S1Base + kPm8005Enable), &value);
        sheng_spmi2_record(rc ? sheng_spmi_err(rc) : value);
        IOLog("NATIVE_GPU_PWR: c1e2 write path readback enable=%08x rc=%d\n", value, rc);
        if (mode == 3) {
            IOLog("NATIVE_GPU_PWR: c1e2 write proof done\n");
            step = 99;
            return 0;
        }
        ++step;
        return 1;
    }
    if (step == 14) {
        if (gS1[1] != 3 || (gS1[2] != 0x08 && gS1[2] != 0x09) ||
            (gpuccRd(0x1094) & 1u) == 0) {
            sheng_spmi2_record(sheng_spmi_err(-6));
            IOLog("NATIVE_GPU_PWR: c1e2 voltage write skipped type=%02x subtype=%02x gx=%08x\n",
                  gS1[1], gS1[2], gpuccRd(0x1094));
            step = 99;
            return 0;
        }
        // A rail the bootloader already left on keeps its voltage: lowering a live VDD_GFX
        // is the one write that could destabilise the SoC before the first GDSC retry.
        if ((gS1[6] & 0x80u) && gS1Uv >= 500000 && gS1Uv <= 1100000) {
            sheng_spmi2_record(0x600d0002u);
            IOLog("NATIVE_GPU_PWR: c1e2 rail already on at %d uV, keep\n", gS1Uv);
            ++step;
            return 1;
        }
        uint32_t both = (kGfxTargetVset << 8) | 0u;   /* range 0, VSET -> 525000 uV */
        int rc = sheng_spmi_write((uint16_t)(kPm8005S1Base + kPm8005Range), both, 2);
        sheng_spmi2_record(rc ? sheng_spmi_err(rc) : 0x600d0001u);
        IOLog("NATIVE_GPU_PWR: c1e2 set %u uV (range 0 vset %u) rc=%d status=%08x\n",
              kGfxTargetUv, kGfxTargetVset, rc, gSpmiLastStatus);
        ++step;
        return 1;
    }
    if (step == 15) {
        uint32_t range = 0, vset = 0;
        int r1 = sheng_spmi_read8((uint16_t)(kPm8005S1Base + kPm8005Range), &range);
        int r2 = sheng_spmi_read8((uint16_t)(kPm8005S1Base + kPm8005Vset), &vset);
        if (!r1) gS1[3] = range;
        if (!r2) gS1[4] = vset;
        if (!r1 && !r2)
            gS1Uv = pm8005Uv(gS1[3], gS1[4]);
        sheng_spmi2_record((r1 || r2) ? sheng_spmi_err(r1 ? r1 : r2)
                                      : ((range & 0xffu) | ((vset & 0xffu) << 8)));
        IOLog("NATIVE_GPU_PWR: c1e2 voltage readback range=%u vset=%u rc=%d/%d\n",
              range, vset, r1, r2);
        ++step;
        return 1;
    }
    if (step == 16) {
        uint32_t next = gS1[5] | 0x80u;
        int rc = sheng_spmi_write((uint16_t)(kPm8005S1Base + kPm8005Mode), next, 1);
        sheng_spmi2_record(rc ? sheng_spmi_err(rc) : next);
        IOLog("NATIVE_GPU_PWR: c1e2 mode hpm %02x->%02x rc=%d status=%08x\n",
              gS1[5], next, rc, gSpmiLastStatus);
        ++step;
        return 1;
    }
    if (step == 17) {
        uint32_t next = gS1[6] | 0x80u;
        int rc = sheng_spmi_write((uint16_t)(kPm8005S1Base + kPm8005Enable), next, 1);
        sheng_spmi2_record(rc ? sheng_spmi_err(rc) : next);
        IOLog("NATIVE_GPU_PWR: c1e2 enable %02x->%02x rc=%d status=%08x\n",
              gS1[6], next, rc, gSpmiLastStatus);
        ++step;
        return 1;
    }
    if (step == 18) {
        gpuDelay(2000);                              /* qcom,enable-time = 500 us */
        uint32_t enable = 0, vset = 0, m = 0;
        int r1 = sheng_spmi_read8((uint16_t)(kPm8005S1Base + kPm8005Enable), &enable);
        int r2 = sheng_spmi_read8((uint16_t)(kPm8005S1Base + kPm8005Vset), &vset);
        int r3 = sheng_spmi_read8((uint16_t)(kPm8005S1Base + kPm8005Mode), &m);
        if (!r1) gS1[6] = enable;
        if (!r2) gS1[4] = vset;
        if (!r2)
            gS1Uv = pm8005Uv(gS1[3], gS1[4]);
        sheng_spmi2_record(r1 ? sheng_spmi_err(r1) : enable);
        sheng_spmi2_record(r2 ? sheng_spmi_err(r2) : vset);
        sheng_spmi2_record(r3 ? sheng_spmi_err(r3) : m);
        IOLog("NATIVE_GPU_PWR: c1e2 s1 on: enable=%02x vset=%02x mode=%02x rc=%d/%d/%d\n",
              enable, vset, m, r1, r2, r3);
        ++step;
        return 1;
    }
    if (step == 19) {
        if ((gS1[6] & 0x80u) == 0 || gS1Uv < 500000 || gS1Uv > 1100000) {
            sheng_spmi2_record(sheng_spmi_err(-7));
            IOLog("NATIVE_GPU_PWR: c1e2 rail not confirmed enable=%02x uv=%d\n",
                  gS1[6], gS1Uv);
            step = 99;
            return 0;
        }
        gpuGdscOn("gx", 0x1094, 0x1094);
        sheng_spmi2_record(gpuccRd(0x1094));
        ++step;
        return 1;
    }
    if (step == 20) {
        gpuccWr(0x1074, 0x00000000u);
        gpuccWr(0x1070, (gpuccRd(0x1070) & ~(1u << 31)) | 1u);
        gpuDelay(256);
        sheng_spmi2_record(gpuccRd(0x1070));
        sheng_spmi2_record(gpuccRd(0x1074));
        IOLog("NATIVE_GPU_PWR: c1e2 gfx3d_rcg=%08x cfg=%08x\n",
              gpuccRd(0x1070), gpuccRd(0x1074));
        ++step;
        return 1;
    }
    if (step == 21) {
        uint32_t branch = gpuccRd(0x1098);
        gpuccWr(0x1098, (branch | (1u << 13)) & ~(1u << 31));
        gpuccWr(0x1098, (branch | (1u << 13) | 1u) & ~(1u << 31));
        gpuDelay(256);
        sheng_spmi2_record(gpuccRd(0x1098));
        IOLog("NATIVE_GPU_PWR: c1e2 gfx3d_branch=%08x\n", gpuccRd(0x1098));
        ++step;
        return 1;
    }
    if (step == 22) {
        gpuccWr(0x1090, 0x1);
        gpuDelay(256);
        gpuccWr(0x1090, 0x0);
        gpuDelay(256);
        sheng_spmi2_record(gpuccRd(0x1090));
        IODeviceMemory *rm = IODeviceMemory::withRange(0x5000000ULL, 0x40000);
        IOMemoryMap *rmMap = rm ? rm->map(kIOMapAnywhere | kIOMapInhibitCache |
                                          kIOMapReadOnly) : nullptr;
        OSSafeReleaseNULL(rm);
        if (!rmMap) {
            sheng_spmi2_record(0xdea00006u);
        } else {
            volatile uint32_t *base = reinterpret_cast<volatile uint32_t *>(rmMap->getVirtualAddress());
            if (base) {
                sheng_spmi2_record(osmRead(base, 0x0000));
                sheng_spmi2_record(osmRead(base, 0x0004));
                sheng_spmi2_record(osmRead(base, 0x0210));
                IOLog("NATIVE_GPU_PWR: c1e2 rbbm=%08x/%08x/%08x\n",
                      osmRead(base, 0x0000), osmRead(base, 0x0004), osmRead(base, 0x0210));
            } else {
                sheng_spmi2_record(0xdea00007u);
            }
            OSSafeReleaseNULL(rmMap);
        }
        ++step;
        return 1;
    }
    IOLog("NATIVE_GPU_PWR: c1e2 sequence done\n");
    step = 99;
    return 0;
}

// One step per call so each log line is separated by a heartbeat beat (a burst is dropped by
// the log writer's snapshot window, see the H223/H224 notes above).
int sheng_gpu_power_step(void) {
    static int mode = -1;
    static unsigned step;
    if (mode < 0) {
        uint32_t value = 0;
        mode = PE_parse_boot_argn("sheng-gpu-power", &value, sizeof value) ? (int)value : 0;
    }
    if (mode >= 2)
        return sheng_spmi_power_step(mode);
    if (mode != 1 || step > 6)
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
        sheng_spmi_survey();
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
