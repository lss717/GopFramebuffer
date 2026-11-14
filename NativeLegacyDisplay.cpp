#include "NativeLegacyRequest.h"
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned,void *,void *);
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *,uint32_t,uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t,uint32_t);
extern "C" void native_legacy_client_observed(IOReturn,IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned,IOReturn,unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *),native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool,unsigned,const void *,const void *);
// Color-status panel inputs. These are diagnostics only; the panel is drawn
// from the UFS heartbeat so it remains visible even when the console scrolls.
extern "C" volatile uint32_t native_legacy_stage;
extern "C" volatile uint32_t native_boot_flags;
extern "C" volatile uint32_t native_legacy_client_calls;
extern "C" volatile uint32_t native_legacy_swap_validation_calls;
static volatile unsigned gSwapTrace;   /* H236fn: bounded kick/guard trace */
extern "C" volatile uint32_t native_legacy_transfer_calls;
extern "C" volatile int native_legacy_client_status;
extern "C" char native_legacy_client_comm[64];
extern "C" volatile uint32_t native_cpu_freq_state;
// H210: resample the OSM state from the 1 Hz heartbeat before repainting.
extern "C" void sheng_cpufreq_tick(void);
extern "C" volatile uint32_t native_h103_slot_state;
extern "C" volatile uint32_t native_a540_accel_state;
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn,const void *,uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
extern "C" unsigned native_legacy_prepare_report(char *out,unsigned cap);
extern "C" void native_legacy_retirement_observed(IOService *);
// GPU hand-off (boot-arg sheng-fb-gpu=1): the frame is offered to the privileged GPU client instead of being copied; the swap retires when the
// client reports the GPU copy done (native_legacy_gpu_complete). A refused offer or a failed copy uses the CPU copy below and is counted.
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
extern "C" void native_legacy_gpu_fallback_observed(int status);
// Display refresh tick (boot-arg sheng-vsync=<Hz>, 0 or absent: off). The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. The GOP scanout has no vblank interrupt, so a
// work-loop timer stands in for it while at least one client has vsync notifications enabled. Events: 0 armed, 1 sent, 2 idle (no client),
// 3 late (deadline passed, cadence restarted), 4 started, 5 refresh properties reported at that rate.
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
extern "C" uint64_t mach_continuous_time(void);
// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spelling ('&BGA', one plane): the GPU that renders the surface here writes plain rows
// either way (no compression is implemented), so the bytes are the same.
static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1) ||
           (format==0x62336138 && planes==2);   // H274: 'b3a8', two-plane wide colour
}
class NativeLegacyDisplay;
static NativeLegacyDisplay *gTestDisplay;
// H315: set once the framebuffer is started, so the panic hook can paint without the registry.
static NativeLegacyDisplay *gPanicDisplay;

class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
    uint32_t scanBudget=0;   /* H272w: the scans read the drawable's device memory at 0.24 MB/s (68 s/frame); opt in with sheng-fb-scan=N */
    void *shadow=nullptr;uint64_t shadowBytes=0;bool shadowValid=false;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    const OriginalLegacySwapRequest *validated[8]={};
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {for(auto &entry: validated)if(entry==request)entry=nullptr;}
    void addValidated(const OriginalLegacySwapRequest *request) {
        if(isValidated(request))return;
        for(auto &entry: validated)if(!entry){entry=request;return;}
        validated[0]=request;
    }
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
    uint64_t gpuLength=0;
    bool gpuBypass=false;
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue at +5592 never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer=nullptr;
    uint64_t vsyncInterval=0,vsyncNext=0;
    unsigned vsyncHz=0;
    bool vsyncArmed=false;
    bool vsyncWanted() const {
        const void *head=nullptr;memcpy(&head,reinterpret_cast<const unsigned char *>(this)+5592,sizeof(head));return head;
    }
    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync() {
        if(!vsyncTimer || vsyncArmed || !vsyncWanted())return;
        vsyncArmed=true;vsyncNext=mach_absolute_time()+vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);
    }
    static void vsyncFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();}
    // H345: the panel progress bar is driven by the display's own timer, not by the UFS watchdog:
    // it keeps repainting once a second whether or not the log path can write anything.
    IOTimerEventSource *progressTimer=nullptr;
    unsigned progressSeconds=0;
    static void progressFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->progressTick();}
    void progressTick() {
        if(!progressTimer)return;
        progressSeconds++;
        progress(progressSeconds,900);
        progressTimer->wakeAtTime(mach_absolute_time()+19200000ULL);
    }
    void startProgress() {
        if(progressTimer||!bootMapping||!primary)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::progressFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        progressTimer=timer;progressSeconds=0;
        timer->wakeAtTime(mach_absolute_time()+19200000ULL);
    }
    void vsyncTick() {
        vsyncArmed=false;
        if(!vsyncTimer)return;
        if(!vsyncWanted()){native_legacy_vsync_observed(2,vsyncHz);return;}
        const uint64_t now=mach_absolute_time();
        native_legacy_vsync_notify(this,now,mach_continuous_time(),vsyncInterval);
        native_legacy_vsync_observed(1,vsyncHz);
        vsyncNext+=vsyncInterval;
        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!hz || hz>240 || !primary || !bootMapping || vsyncTimer)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        nanoseconds_to_absolutetime(1000000000ull/hz,&vsyncInterval);
        vsyncHz=hz;vsyncTimer=timer;native_legacy_vsync_observed(4,hz);
        // A client may already have registered: arm under the gate, like the registration does.
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(!vsyncTimer)return;
        vsyncTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
public:
    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop.
    void gpuComplete(int status) {
        struct Args {NativeLegacyDisplay *display;int status;} args={this,status};
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=static_cast<Args *>(a0);a->display->finishGpuSwap(a->status);return kIOReturnSuccess;
        };
        if(loop)loop->runAction(action,this,&args);else finishGpuSwap(status);
    }
    void free() override {stopVsync();if(logicalPM){PMstop();logicalPM=false;}primary=false;OSSafeReleaseNULL(bootMapping);IOMobileFramebufferLegacy::free();}
    void stop(IOService *provider) override {
        stopVsync();
        transferred=false;for(auto &entry: validated)entry=nullptr;gpuRequest=nullptr;
        if(logicalPM){PMstop();logicalPM=false;}
        primary=false;IOMobileFramebufferLegacy::stop(provider);OSSafeReleaseNULL(bootMapping);
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840,
    // the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part of this
    // port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is the
    // original state setter (virtual 354, the one the original code calls with this same request shape) with only the powered word named.
    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this,&request,false);
    }
    bool start(IOService *provider) override {
        // H143: proves the KernelConfigTable personality matched this nub
        // and that IOKit instantiated and started the framebuffer.
        native_boot_flags |= 0x4000000U;
        if(!startDisplay(provider))return false;
        gPanicDisplay=this;   // H315: the panic hook reaches the panel through this
        startVsync();return true;
    }
    // H343: a progress bar drawn straight into the scanout, independent of the log path.  The
    // watchdog calls this once a second, so how far the bar got is exactly how long the machine
    // lived, even when neither the ring nor the console can tell.
    void progress(unsigned step, unsigned total) {
        if(!bootMapping || !total) return;
        uint32_t w=0,h=0;getDisplayDimensions(&w,&h);
        if(!w||!h)return;
        PE_Video video={};
        if(!PE_current_console(&video))return;
        if(video.v_baseAddr!=bootMapping->getPhysicalAddress()||video.v_depth!=32||video.v_rotate)return;
        uintptr_t base=(uintptr_t)bootMapping->getVirtualAddress();
        if(!base)return;
        volatile uint32_t *row=reinterpret_cast<volatile uint32_t *>(base);
        uint32_t words=video.v_rowBytes/4;
        if(words<w)return;
        uint32_t filled=(uint32_t)(((uint64_t)w*step)/total);
        if(filled>w)filled=w;
        for(unsigned y=0;y<6;y++) {
            volatile uint32_t *line=row+(uint64_t)y*words;
            for(uint32_t x=0;x<w;x++) line[x]=(x<filled)?0xffffffffU:0xff000000U;
        }
        __asm__ volatile("dsb sy":::"memory");
    }
    // H315: a dying machine gets one full-screen fill of the real scanout.  Nothing here takes a
    // lock, waits or retries: either the panel turns solid or the fill is skipped and the marker
    // block remains the only record.
    void panicPaint(uint32_t color) {
        if(!bootMapping) return;
        uint32_t w=0,h=0;getDisplayDimensions(&w,&h);
        if(!w||!h)return;
        PE_Video video={};
        if(!PE_current_console(&video))return;
        if(video.v_baseAddr!=bootMapping->getPhysicalAddress()||video.v_depth!=32||video.v_rotate)return;
        if(video.v_width!=w||video.v_height!=h)return;
        if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,w,h,color))return;
        uintptr_t base=static_cast<uintptr_t>(bootMapping->getVirtualAddress());
        uintptr_t end=base+static_cast<uintptr_t>(video.v_rowBytes)*h;
        for(uintptr_t a=base&~static_cast<uintptr_t>(63);a<end;a+=64)
            __asm__ volatile("dc civac, %0"::"r"(a):"memory");
        __asm__ volatile("dsb sy":::"memory");
    }
    int drawTestPattern() {
        if(!bootMapping) return 0;
        uint32_t w=0,h=0;getDisplayDimensions(&w,&h);
        uint64_t bytes=bootMapping->getLength();
        if(!w || !h || !bytes || bytes<h*4)return 0;
        uint64_t stride=bytes/h;if(stride<w*4 || (stride&3))return 0;
        volatile uint32_t *base=reinterpret_cast<volatile uint32_t *>(bootMapping->getVirtualAddress());
        if(!base)return 0;
        // Top-right 2x2 color panel. The console can overwrite it, so the UFS
        // heartbeat redraws it every ten seconds.
        uint32_t panel=128;
        if(panel*2>w)panel=w/2;
        if(panel*2>h)panel=h/2;
        if(!panel)return 0;
        // No diagnostic cell uses pure black or cyan. The neutral cell is a
        // visible dark gray; backboardd uses a deep blue distinct from green.
        const uint32_t black=0xff404040U,red=0xffff0000U,green=0xff00ff00U,
                       blue=0xff0000ffU,deepblue=0xff0080ffU,yellow=0xffffff00U,
                       magenta=0xffff00ffU,orange=0xffff8000U,purple=0xff8000ffU,
                       white=0xffffffffU;
        auto fill=[&](uint32_t x0,uint32_t y0,uint32_t cw,uint32_t ch,uint32_t color) {
            uint64_t words=stride/4;
            for(uint32_t y=0;y<ch && y0+y<h;++y) {
                volatile uint32_t *row=base+(uint64_t)(y0+y)*words;
                for(uint32_t x=0;x<cw && x0+x<w;++x)row[x0+x]=color;
            }
        };
        // Top-left is the userland frontier, not just the display stage:
        // red none, yellow logd_helper, orange driverkitd, deep blue backboardd,
        // green SpringBoard. The panel's presence already proves the GOP
        // display backend is running.
        uint32_t stageColor=red;
        if(native_boot_flags&32)stageColor=purple;       // SpringBoard exited
        else if(native_boot_flags&64)stageColor=magenta; // backboardd exited
        else if((native_boot_flags&8) && (native_boot_flags&4))stageColor=white;
        else if(native_boot_flags&8)stageColor=green;
        else if(native_boot_flags&4)stageColor=deepblue;
        else if(native_boot_flags&2)stageColor=orange;
        else if(native_boot_flags&1)stageColor=yellow;
        uint32_t clientColor=black;
        if(native_legacy_client_calls) {
            const char *comm=native_legacy_client_comm;
            auto starts=[](const char *s,const char *p) {
                while(*p)if(*s++!=*p++)return false;
                return true;
            };
            if(starts(comm,"IOMFB_FDR"))clientColor=yellow;
            else if(starts(comm,"backboard"))clientColor=deepblue;
            else clientColor=magenta;
        } else if(native_boot_flags&1024) {
            clientColor=purple;      // backboardd queried IOMobile*, no match
        } else if(native_boot_flags&512) {
            clientColor=blue;        // backboardd queried IOMobile*, matched
        } else if(native_boot_flags&128) {
            clientColor=orange;     // backboardd queried some other IOKit class
        }
        uint32_t calls=native_legacy_client_calls;
        uint32_t callsColor=!calls?black:(calls<10?blue:(calls<100?green:orange));
        uint32_t progressColor=native_legacy_swap_validation_calls?purple:
                                (native_legacy_transfer_calls?white:black);
        uint32_t x0=w-2*panel,y0=0;
        fill(x0,y0,panel,panel,stageColor);
        fill(x0+panel,y0,panel,panel,clientColor);
        fill(x0,y0+panel,panel,panel,callsColor);
        // Right-bottom is split: top half = swap/transfer, bottom half =
        // backboardd's IOKit matching state (not hidden by the FDR client).
        uint32_t ikColor=black;
        if(native_boot_flags&1024)ikColor=purple;
        else if(native_boot_flags&512)ikColor=blue;
        else if(native_boot_flags&128)ikColor=orange;
        fill(x0+panel,y0+panel,panel,panel/2,progressColor);
        fill(x0+panel,y0+panel+panel/2,panel,panel/2,ikColor);
        static uint32_t blink;
        // The three 16x16 status cells sit at the top-left corner.
        // Left to right: CPU frequency, A540 accelerator, heartbeat.
        fill(40,0,16,16,(blink++&1)?white:orange);
        // A540 accelerator probe state: black not attempted, green published,
        // red IOGPUFamily/start/attach failure.
        uint32_t accelColor=black;
        if(native_a540_accel_state==6)accelColor=green;
        else if(native_a540_accel_state)accelColor=red;
        fill(20,0,16,16,accelColor);
        // CPU frequency observation: gray not run, red low/disabled,
        // green normal (>=1 GHz), purple OSM map failure.
        uint32_t cpuColor=black;
        if(native_cpu_freq_state==2)cpuColor=green;
        else if(native_cpu_freq_state==1)cpuColor=red;
        else if(native_cpu_freq_state==3)cpuColor=purple;
        else if(native_cpu_freq_state==4)cpuColor=orange;
        else if(native_cpu_freq_state==5)cpuColor=blue;
        else if(native_cpu_freq_state==6)cpuColor=0xff808080U;   // H213: OSM not readable
        fill(0,0,16,16,cpuColor);
        // H103 slot writer: green/white pulse while writes succeed, yellow
        // when writes stop or fail, blue while the window is not ready.
        uint32_t slotColor=red;
        if(native_h103_slot_state==3)slotColor=blue;
        else if(native_h103_slot_state==1)slotColor=yellow;
        else if(native_h103_slot_state==2)slotColor=green;
        else if(native_h103_slot_state==4)slotColor=white;
        fill(60,0,16,16,slotColor);
        __asm__ volatile("dsb oshst" ::: "memory");
        return 1;
    }
    void drawSlotCell() {
        if(!bootMapping)return;
        uint32_t w=0,h=0;getDisplayDimensions(&w,&h);
        uint64_t bytes=bootMapping->getLength();
        if(!w || !h || bytes<h*4)return;
        uint64_t stride=bytes/h;if(stride<w*4 || (stride&3))return;
        volatile uint32_t *base=reinterpret_cast<volatile uint32_t *>(bootMapping->getVirtualAddress());
        if(!base)return;
        uint32_t color=0xffff0000U;
        if(native_h103_slot_state==3)color=0xff0000ffU;
        else if(native_h103_slot_state==1)color=0xffffff00U;
        else if(native_h103_slot_state==2)color=0xff00ff00U;
        else if(native_h103_slot_state==4)color=0xffffffffU;
        uint64_t words=stride/4;
        for(uint32_t y=0;y<16 && y<h;++y) {
            volatile uint32_t *row=base+(uint64_t)y*words;
            for(uint32_t x=60;x<76 && x<w;++x)row[x]=color;
        }
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    // H108: independent heartbeat. Paint only the 16x16 cell at (40,0) so a
    // 1 Hz driver can prove the kernel is alive without a full 2x2 redraw and
    // without touching any lock the display family may hold during a client
    // swap. The framebuffer mapping is uncached device memory.
    void drawHeartbeatCell() {
        if(!bootMapping)return;
        uint32_t w=0,h=0;getDisplayDimensions(&w,&h);
        uint64_t bytes=bootMapping->getLength();
        if(!w || !h || bytes<h*4)return;
        uint64_t stride=bytes/h;if(stride<w*4 || (stride&3))return;
        volatile uint32_t *base=reinterpret_cast<volatile uint32_t *>(bootMapping->getVirtualAddress());
        if(!base)return;
        static uint32_t on;
        uint32_t color=(on++&1)?0xffffffffU:0xffff8000U;
        uint64_t words=stride/4;
        for(uint32_t y=0;y<16 && y<h;++y) {
            volatile uint32_t *row=base+(uint64_t)y*words;
            for(uint32_t x=40;x<56 && x<w;++x)row[x]=color;
        }
        // H210: repaint the CPU frequency cell from the state sheng_cpufreq_tick()
        // just refreshed. Same palette as drawStatusCells(): red below 1 GHz or
        // no enabled cluster, green at or above 1 GHz.
        uint32_t cpuColor=0xff404040U;
        if(native_cpu_freq_state==2)cpuColor=0xff00ff00U;
        else if(native_cpu_freq_state==1)cpuColor=0xffff0000U;
        else if(native_cpu_freq_state==3)cpuColor=0xff8000ffU;
        else if(native_cpu_freq_state==4)cpuColor=0xffff8000U;
        else if(native_cpu_freq_state==5)cpuColor=0xff0000ffU;
        // H213: OSM not readable/uninitialised (no enabled cluster) is unknown, not slow.
        else if(native_cpu_freq_state==6)cpuColor=0xff808080U;
        for(uint32_t y=0;y<16 && y<h;++y) {
            volatile uint32_t *row=base+(uint64_t)y*words;
            for(uint32_t x=0;x<16 && x<w;++x)row[x]=cpuColor;
        }
        drawSlotCell();
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    bool startDisplay(IOService *provider) {
        IOLog("NATIVE_LEGACY: startDisplay provider=%p\n",provider);
        if(!IOMobileFramebufferLegacy::start(provider))return false;
        gTestDisplay=this;
        IOLog("NATIVE_LEGACY: base start ok primary=%u mapping=%p\n",primary,bootMapping);
        startProgress();
        if(primary && bootMapping)announcePoweredByFirmware();
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
        if(enabled!=1 || phase!=4 || !provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        PMinit();logicalPM=initialized;
        if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;}
        provider->joinPMtree(this);
        IOReturn status=registerPowerDriver(this,states,2);native_legacy_pm_observed(1,status,0);
        if(status==kIOReturnSuccess){status=changePowerStateToPriv(1);native_legacy_pm_observed(2,status,1);}
        if(status!=kIOReturnSuccess){PMstop();logicalPM=false;IOMobileFramebufferLegacy::stop(provider);return false;}
        return true;
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        if(!logicalPM)return IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(ordinal>1 || !bootMapping){native_legacy_pm_observed(3,kIOReturnUnsupported,ordinal);return kIOReturnUnsupported;}
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        IOReturn status=IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        native_legacy_pm_observed(4,status,ordinal);return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        // Nothing in the family publishes IOMFBDisplayRefresh (its platform subclass does), and without it the render server keeps its own
        // 1/60 s heartbeat (int-210: refresh=0). Publish it before any client can find the display, only when sheng-vsync asks for more.
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(primary && hz>60 && hz<=240)native_legacy_set_refresh_properties(this);
        // H140: backboardd registers an IOPropertyMatch notification on
        // "backlight-control" and only then brings up the display path.
        // Publish it before the service becomes visible.
        setProperty("backlight-control",kOSBooleanTrue);
        IOService::registerService(options);native_legacy_registration_observed(this);
        native_boot_flags |= 0x8000000U;
        // H141: the UFS probe cannot find this service by class name even though
        // it registers, so prove the local view: state, IOClass, provider and a
        // self lookup under the three names backboardd or the family may use.
        {
            IOService *prov=getProvider();
            OSString *ioClass=OSDynamicCast(OSString,getProperty("IOClass"));
            IOLog("NATIVE_LEGACY: self inactive=%d ioclass=%s provider=%s\n",
                  isInactive()?1:0,
                  ioClass?ioClass->getCStringNoCopy():"-",
                  prov?prov->getMetaClass()->getClassName():"-");
            // H340: this loop called IOService::copyMatchingService() from inside the driver's
            // own registration path.  That waits for the registry lock the matcher already holds,
            // so the boot stopped after the first two names printed.  The service is registered
            // and published; the self lookup was only an H141 diagnostic and is gone.
        }
    }
    // H142: the KernelConfigTable personality asks the driver whether it
    // takes this nub.  Accept only a provider that carries our GOP device
    // memory, so the inherited Apple checks cannot veto the service.
    IOService *probe(IOService *provider, SInt32 *score) override {
        if(!provider || !provider->getDeviceMemoryCount())return nullptr;
        if(score)*score=1000;
        return this;
    }
    bool isMainDisplay() override {return primary;}
    // The render server paces its display timer by the minimum refresh interval in IOMFBDisplayRefresh (int-209: no vsync client ever
    // registered, frames came every 16.7 ms), and the generic slot 301 fixes it at 1/60 s. With sheng-vsync=<Hz> it reports the panel's
    // own rate instead; the maximum interval and the step keep the original values. Words in 32.32 seconds, then their mach times.
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(hz<=60 || hz>240)return true;
        const uint64_t interval=(1ull<<32)/hz;
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        if(fixed){words[2]=interval;words[3]=machTime;}
        native_legacy_vsync_observed(5,hz);
        return true;
    }
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking=false,rekick=false;
    void kickSwapQueue() override {
        if(gSwapTrace<64){gSwapTrace=gSwapTrace+1;IOLog("NATIVE_LEGACY_SWAP: kick transferred=%u pending=%u\n",transferred?1u:0u,pendingSwapRequest()?1u:0u);}
        armVsync();
        if(kicking){rekick=true;return;}
        kicking=true;
        do {
            rekick=false;
            kickOnce();
            gpuBypass=false;
        } while(rekick && !transferred);
        kicking=false;
    }
    void kickOnce() {
        uint64_t tEntry,tDescriptor,tMap;uint64_t tLayout,tScan;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(tEntry));
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary) {
            if(gSwapTrace<64){gSwapTrace=gSwapTrace+1;IOLog("NATIVE_LEGACY_SWAP: skip en=%u tr=%u pm=%u map=%u pri=%u\n",
                enabled,transferred?1u:0u,logicalPM?1u:0u,bootMapping?1u:0u,primary?1u:0u);}
            return;
        }
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request)) {
            if(gSwapTrace<64){gSwapTrace=gSwapTrace+1;IOLog("NATIVE_LEGACY_SWAP: skip req=%u valid=%u\n",
                request?1u:0u,(request && isValidated(request))?1u:0u);}
            return;
        }
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
        OSObject *surface=nullptr;
        memcpy(&surface,reinterpret_cast<const unsigned char *>(request)+2624,sizeof(surface));
        if(!surface) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank={};
            uint32_t bw=0,bh=0;getDisplayDimensions(&bw,&bh);
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),blank.v_rowBytes,bw,bh,0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            shadowValid=false;
            transferred=true;
            native_legacy_frame_observed(1,0,0,0,0,0);
            {const uint32_t colors[4]={0xff000000U,0,0,0};const uint64_t counts[4]={(uint64_t)bw*bh,0,0,0};native_legacy_frame_colors_observed(colors,counts,0);}
            native_legacy_transfer_observed(kIOReturnSuccess,request,blankLength);
            finishTransfer(request);
            return;
        }
        uint64_t kickStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(kickStart));
        if(!surface->metaCast("IOSurface"))return;
        IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(tDescriptor));
        PE_Video video={};
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint32_t sourceFormat=native_legacy_surface_format(surface);
        uint64_t sourceStride=native_legacy_surface_bytestride(width,sourceFormat,
            native_legacy_surface_stride(surface));
        uint64_t allocation=native_legacy_surface_allocation(surface);
        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||
           PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress() ||
           video.v_width!=width || video.v_height!=height || video.v_depth!=32 || video.v_rotate ||
           !native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
        }
        if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height)) {
            // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            shadowValid=false;gpuRequest=request;gpuLength=length;transferred=true;return;
        }
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(tMap));
        if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(tLayout));
        // The two content scans read every pixel again; they only describe the first frames (the probe keeps 32).
        {static bool scanArgRead;if(!scanArgRead){scanArgRead=true;unsigned v=0;PE_parse_boot_argn("sheng-fb-scan",&v,sizeof v);if(v)scanBudget=v;}}
        if(scanBudget) {
            --scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
            uint32_t colors[4];uint64_t counts[4],other=0;
            if(native_legacy_frame_colors(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,colors,counts,&other))
                native_legacy_frame_colors_observed(colors,counts,other);
        }
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(tScan));
        if(!native_legacy_prepare_scanout()) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
        uint64_t stored=UINT64_MAX;
        {
            // Store only what changed since the previous frame (the scanout memory is slow device memory).
            uint64_t need=(uint64_t)width*height*4;
            // Differential copy is opt-in (sheng-fb-diff=1): on hardware it reads the whole source every frame and was not faster
            // than the plain copy (143-285 ms per swap either way), and the first boots with it were followed by an unresponsive
            // lock screen that disappeared with the plain copy (int-005).
            static int diffOff=-1;
            if(diffOff<0){unsigned v=0;PE_parse_boot_argn("sheng-fb-diff",&v,sizeof v);diffOff=(v!=1);}
            if(!diffOff && !shadow && need<=(64ull<<20)){shadow=IOMallocAligned((vm_size_t)need,16384);if(shadow){shadowBytes=need;shadowValid=false;}}
            if(!diffOff && shadow && shadowBytes>=need)
                stored=native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                    source->getLength(),sourceStride,width,height,shadow,shadowValid);
            if(stored!=UINT64_MAX)shadowValid=true;
        }
        if(stored==UINT64_MAX) {
            shadowValid=false;
            if(!native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height)) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            stored=length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));
            native_legacy_copy_timing_observed(copyEnd-copyStart,stored);
            /* H272r: the total time from kick entry to copy end, so the 99 s can be
             * attributed to a segment instead of the whole swap. */
            uint64_t nowTick;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(nowTick));
            IOLog("NATIVE_LEGACY_COPY: ticks=%llu us=%llu bytes=%llu kick_us=%llu\n",
                  (unsigned long long)(copyEnd-copyStart),
                  (unsigned long long)((copyEnd-copyStart)/24),
                  (unsigned long long)stored,
                  (unsigned long long)((nowTick-kickStart)/24));
        }
        /* H272r: read back what is actually in the scanout right now - numeric proof of
         * who wrote last, independent of whatever the console decided to draw. */
        {
            volatile uint32_t *scan=(volatile uint32_t *)bootMapping->getVirtualAddress();
            const uint64_t stride4=video.v_rowBytes/4;
            IOLog("NATIVE_LEGACY_RAW: entry=%llu desc=%llu map=%llu kick=%llu\\n",(unsigned long long)tEntry,(unsigned long long)tDescriptor,(unsigned long long)tMap,(unsigned long long)kickStart);
            IOLog("NATIVE_LEGACY_RAW2: layout=%llu scan=%llu copy=%llu\\n",(unsigned long long)tLayout,(unsigned long long)tScan,(unsigned long long)copyStart);
            IOLog("NATIVE_LEGACY_STAMPS: entry_us=%llu descriptor_us=%llu map_us=%llu\n",(unsigned long long)((tDescriptor-tEntry)/24),(unsigned long long)((tMap-tDescriptor)/24),(unsigned long long)((kickStart-tMap)/24));
            {static bool prepShown;if(!prepShown){prepShown=true;char pb[120];if(native_legacy_prepare_report(pb,sizeof pb))IOLog("NATIVE_LEGACY_PREPARE: %s\n",pb);}}
            IOLog("NATIVE_LEGACY_READBACK: tl=%08x c=%08x br=%08x q=%08x\n",
                  scan[0],
                  scan[stride4*(height/2)+width/2],
                  scan[stride4*(height-1)+width-1],
                  scan[stride4*(height/4)+width/4]);
        }
        OSSafeReleaseNULL(source);transferred=true;
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        finishTransfer(request);
    }
    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy; anything else uses the CPU copy.
    void finishGpuSwap(int status) {
        const OriginalLegacySwapRequest *request=gpuRequest;
        if(!request)return;
        gpuRequest=nullptr;
        if(status==0) {
            native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            finishTransfer(request,true);return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred=false;gpuBypass=true;kickSwapQueue();
    }
    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    void finishTransfer(const OriginalLegacySwapRequest *request,bool force=false) {
        unsigned retire=0,cpuPresent=0;
        PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
        PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
        if(retire==1 || cpuPresent==1 || force) {
            // Only after the real copy and store barrier: select the actual
            // pending head and let the unchanged family release it and wake
            // waiters. This reports memory scanout completion, not a panel
            // vblank or hardware fence. Producer completion must precede
            // submission; this does not implement asynchronous GPU sync.
            if(!activateOnlyPendingSwap(request))return;
            removeValidated(request);
            IOMobileFramebufferLegacy::completeCurrentSwap();
            native_legacy_retirement_observed(this);
            // The unchanged family has retired this generation and released
            // its current reference. Permit the next independently validated
            // single pending frame. Keep the latch set while completion can
            // reenter this method, and whenever retirement remains disabled.
            transferred=false;
            kickSwapQueue();
        }
    }
    IOReturn collectCompletionReports(uint32_t,void *,uint32_t *count,uint32_t) override {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if(count)*count=0;
        return kIOReturnUnsupported;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override {
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes=reinterpret_cast<const unsigned char *>(request);
        auto word=[&](unsigned offset){uint32_t value=0;memcpy(&value,bytes+offset,4);return value;};
        OSObject *surface=nullptr;unsigned reason=1;bool accepted=false;
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                for(unsigned offset: rectOffsets) {
                    uint32_t rect[4]={};memcpy(rect,bytes+offset,sizeof(rect));
                    rectangles=rectangles && rect[0]==0 && rect[1]==0 && rect[2]==width && rect[3]==height;
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                if(width && height && rectangles && options && word(816)==width && word(820)==height) {
                    reason=4;
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied.
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,
                           native_legacy_surface_bytestride(width,native_legacy_surface_format(surface),
                               native_legacy_surface_stride(surface)),
                           native_legacy_surface_allocation(surface),memory->getLength())) {
                        reason=0;accepted=true;
                    } else {
                        native_legacy_surface_rejected(native_legacy_surface_width(surface),native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface),native_legacy_surface_planes(surface),native_legacy_surface_stride(surface),
                            native_legacy_surface_allocation(surface),memory?memory->getLength():0);
                    }
                }
            }
        }
        if(!accepted && reason==2 && !surface && enabled==1) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4]={};memcpy(none,bytes+2624,sizeof(none));
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
            if(!none[0] && !none[1] && !none[2] && !none[3] && width && height && options &&
               word(816)==width && word(820)==height && native_legacy_blank_state(word(792),word(796),word(800))) {
                reason=5;accepted=true;
            }
        }
        if(accepted)addValidated(request);else removeValidated(request);
        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override {
        if(!pitch)return kIOReturnBadArgument;
        *pitch=0;IOService *provider=getProvider();if(!provider)return kIOReturnNotReady;
        OSObject *property=provider->getProperty("dot-pitch");
        if(!property)return kIOReturnNotFound;
        OSData *data=OSDynamicCast(OSData,property);
        if(!data || data->getLength()!=sizeof(*pitch))return kIOReturnBadArgument;
        memcpy(pitch,data->getBytesNoCopy(),sizeof(*pitch));return kIOReturnSuccess;
    }
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    IOReturn supportedFrameInfo(uint32_t,uint32_t *count,char *key,uint32_t *mask) override {
        if(!count || !key || !mask)return kIOReturnBadArgument;
        *count=0;*mask=0;bzero(key,64);return kIOReturnUnsupported;
    }
    IOReturn newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) override {
        native_legacy_client_enter(task,type);
        // DinnerABC parity: the FDR loader is allowed to create the real user
        // client. It is the original family's client, not a synthetic shadow.
        IOReturn status=IOMobileFramebufferLegacy::newUserClient(task,security,type,client);
        native_legacy_client_observed(status,status==kIOReturnSuccess && client?*client:nullptr);return status;
    }
    bool startHardware() override {
        unsigned enabled=0;
        PE_parse_boot_argn("offline-legacy-gop",&enabled,sizeof(enabled));
        IOLog("NATIVE_LEGACY: startHardware gop=%u\n",enabled);
        if(enabled!=1)return false;
        PE_Video video={};IOService *provider=getProvider();
        IOLog("NATIVE_LEGACY: startHardware provider=%p\n",provider);
        if(!provider || PE_current_console(&video) || !video.v_baseAddr ||
           !video.v_width || !video.v_height || video.v_depth!=32 || video.v_rotate ||
           video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
           video.v_rowBytes>UINT64_MAX/video.v_height)return false;
        IOLog("NATIVE_LEGACY: startHardware video=%llx %lux%lu row=%lu display=%lu\n",(unsigned long long)video.v_baseAddr,(unsigned long)video.v_width,(unsigned long)video.v_height,(unsigned long)video.v_rowBytes,(unsigned long)video.v_display);
        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapDefaultCache);
        IOLog("NATIVE_LEGACY: startHardware mapping=%p\n",mapping);
        if(!mapping)return false;
        bool valid=mapping->getVirtualAddress() && mapping->getPhysicalAddress()==video.v_baseAddr &&
                   mapping->getLength()==video.v_rowBytes*video.v_height;
        if(valid) {
            installBootDimensions((uint32_t)video.v_width,(uint32_t)video.v_height);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            // H163: what the framework can actually read from this provider.
            // The display list / mode build reads canvas-width/canvas-height
            // from the IODeviceTree parent, and MobileGestaltHelper asks for
            // the dot pitch (selector 28); print whether either exists.
            auto providerWord=[&](const char *key,uint32_t &value)->bool {
                OSObject *property=provider->getProperty(key);
                OSData *data=OSDynamicCast(OSData,property);
                if(!data || data->getLength()!=sizeof(value))return false;
                memcpy(&value,data->getBytesNoCopy(),sizeof(value));return true;
            };
            uint32_t canvasW=0,canvasH=0,pitch=0;
            bool hasCanvasW=providerWord("canvas-width",canvasW);
            bool hasCanvasH=providerWord("canvas-height",canvasH);
            bool hasPitch=providerWord("dot-pitch",pitch);
            IOLog("NATIVE_LEGACY: dims %ux%u canvas=%u:%u,%u:%u dotpitch=%u:%u\n",
                  width,height,hasCanvasW?1:0,canvasW,hasCanvasH?1:0,canvasH,
                  hasPitch?1:0,pitch);
            valid=width==video.v_width && height==video.v_height;
            if(valid) {
                OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;primary=video.v_display==1;
                valid=native_legacy_gop_mapping_observed(this,bootMapping,width,height);
            }
        }
        IOLog("NATIVE_LEGACY: startHardware valid=%d phys=%llx len=%llu\n",valid,mapping?(unsigned long long)mapping->getPhysicalAddress():0,mapping?(unsigned long long)mapping->getLength():0);
        OSSafeReleaseNULL(mapping);return valid;
    }
#define LEAF(n) __attribute__((noreturn,noinline)) void opaque##n() override { native_legacy_backend_guard(n,this,__builtin_return_address(0)); }
    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
    LEAF(345)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
extern "C" int native_legacy_test_pattern(void) {return gTestDisplay?gTestDisplay->drawTestPattern():0;}
// H315: panic paint entry.  Absent (weak_import) in builds without the display archive, which is
// why the caller checks the pointer before using it.
extern "C" void native_legacy_panic_paint(uint32_t color) {if(gPanicDisplay)gPanicDisplay->panicPaint(color);}
extern "C" void native_legacy_progress(unsigned step,unsigned total) {if(gPanicDisplay)gPanicDisplay->progress(step,total);}
extern "C" void native_legacy_heartbeat_cell(void) {sheng_cpufreq_tick();if(gTestDisplay)gTestDisplay->drawHeartbeatCell();}
extern "C" void native_legacy_slot_cell(void) {if(gTestDisplay)gTestDisplay->drawSlotCell();}
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
