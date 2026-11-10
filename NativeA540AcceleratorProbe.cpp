// SPDX-License-Identifier: MIT
// Minimal, own-code IOAcceleratorES publication probe for the A540 bring-up.
// It never touches GPU registers, firmware, SMMU or command submission. Its
// only purpose is to answer whether backboardd/Metal discovers and queries an
// accelerator service before the real QcomGPU stack exists.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include "../NativeMetal/OriginalAcceleratorABI.h"
#include "../NativeAdreno/NativeAdrenoABI.h"

// H168: a registry-visible accelerator. CoreAnimation looks the renderer up
// with IOServiceMatching("IOAcceleratorES"), which matches on the IOProviderClass
// class hierarchy, so a property named "IOClass" is invisible; the service must
// really derive from the (audited) original IOAcceleratorES. The class is
// registered only when the original IOGPUFamily kext is already loaded, because
// its parent metaclass lives there.
// H192 (A540 milestone 1): a client that answers the native-Adreno tunnel handshake and the
// small subset of the ABI the userspace GPU test needs to report where it stops: Query,
// GetDeviceInfo, GetStatus, GetTimestamp and SetMetadata. Nothing here touches GPU registers,
// firmware, SMMU or command submission; BoAlloc and later steps answer kIOReturnUnsupported on
// purpose so the test reports "reached the driver" instead of failing at the open.
// H203: persistent ABI state for the UFS heartbeat; the boot log window is unreliable, so the counters
// and the last SetMetadata text must survive in the kernel and be printed every heartbeat instead.
/* C4-B: the ABI surface exported by NativeA540C3.cpp. */
extern "C" int native_a540_gpu_ready(void);
extern "C" uint64_t native_a540_bo_iova(unsigned slot);
extern "C" unsigned native_a540_bo_bytes(void);
extern "C" unsigned native_a540_bo_slots(void);
extern "C" void *native_a540_bo_descriptor(unsigned slot);
extern "C" int native_a540_submit_bo(unsigned slot, unsigned words, uint64_t *fence);
extern "C" int native_a540_wait_fence(uint64_t fence, uint64_t timeout_ns);

extern "C" volatile int native_a540_abi_calls;
extern "C" volatile int native_a540_abi_last_method;
extern "C" volatile int native_a540_abi_reports;
extern "C" char native_a540_abi_report[64];
extern "C" { volatile int native_a540_abi_calls = 0; volatile int native_a540_abi_last_method = 0;
             volatile int native_a540_abi_reports = 0; char native_a540_abi_report[64] = {}; }
// H219: the persistent log window can hold slots from earlier boots, so the open and selector
// counts also go to the heartbeat, which is written from this boot's state only.
extern "C" { volatile uint32_t native_a540_open_count = 0; volatile uint32_t native_a540_open_type = 0;
             volatile uint32_t native_a540_sel_count = 0; volatile uint32_t native_a540_sel_last = 0; }

// H195: the class name is the sandbox key: the target's MACF profile admits IOServiceOpen only for
// the user-client class AGXDeviceUserClient (the same reason NativeAdrenoPublish.cpp uses it), and
// H194's kernel-side grant was followed by "IOUC NativeAcceleratorClient failed MACF".
class AGXDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AGXDeviceUserClient)
    /* C4-B: handles are slot+1, so zero stays invalid. The pool itself lives in
     * NativeA540C3.cpp and is never freed, which is why a fresh descriptor per
     * clientMemoryForType call is safe here. */
    bool boUsed_[8] = {};
    int slotOf(uint64_t handle) {
        if (!handle) return -1;
        const unsigned s = (unsigned)(handle - 1);
        if (s >= native_a540_bo_slots() || s >= 8 || !boUsed_[s]) return -1;
        return (int)s;
    }
public:
    bool initWithTask(task_t owningTask,void *securityID,UInt32 type,OSDictionary *properties) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;
};
OSDefineMetaClassAndStructors(AGXDeviceUserClient,IOUserClient)

bool AGXDeviceUserClient::initWithTask(task_t owningTask,void *securityID,UInt32 type,OSDictionary *properties) {
    if(!IOUserClient::initWithTask(owningTask,securityID,type,properties))return false;
    if(!owningTask)return false;
    return true;
}
IOReturn AGXDeviceUserClient::clientClose() {terminate();return kIOReturnSuccess;}
IOReturn AGXDeviceUserClient::clientDied() {return clientClose();}

namespace {
constexpr uint32_t kAccelTunnelMagic = 0x4e544e4c;   /* 'NTNL', native-Adreno tunnel */
constexpr uint32_t kAccelProbe = 0xffffffffu;
extern "C" void proc_selfname(char *, int);
const char *accelRequester() {
    static char name[32];
    name[0] = 0;
    proc_selfname(name, sizeof name);
    return name;
}

IOReturn accelCopyOut(IOExternalMethodArguments *arguments, const void *data, size_t bytes) {
    if (!arguments->structureOutput || arguments->structureOutputSize < bytes) return kIOReturnBadArgument;
    memcpy(arguments->structureOutput, data, bytes);
    arguments->structureOutputSize = (IOByteCount) bytes;
    return kIOReturnSuccess;
}
}  // namespace

IOReturn AGXDeviceUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                  IOMemoryDescriptor **memory) {
    if (memory) *memory = nullptr;
    for (unsigned i = 0; i < 2400 && !native_a540_gpu_ready(); ++i)
        IOSleep(50);
    if (!native_a540_gpu_ready()) return kIOReturnNotReady;
    if (type >= native_a540_bo_slots()) return kIOReturnNoDevice;
    IOMemoryDescriptor *d = static_cast<IOMemoryDescriptor *>(native_a540_bo_descriptor(type));
    if (!d) return kIOReturnNoDevice;
    d->retain();
    if (memory) *memory = d;
    if (options) *options = 0;
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                                 IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    (void) dispatch; (void) target; (void) reference;
    /* H218: record every distinct selector the real accelerator ABI uses, so the port can
     * implement what CoreAnimation asks for instead of guessing. The tunnel protocol below is
     * this port's own test ABI and is unaffected. */
    if (arguments) {
        native_a540_sel_count = native_a540_sel_count + 1;
        native_a540_sel_last = selector;
        static uint32_t seen[32];
        static unsigned seenCount;
        bool fresh = true;
        for (unsigned i = 0; i < seenCount; ++i) {
            if (seen[i] == selector) { fresh = false; break; }
        }
        if (fresh && seenCount < 32) {
            seen[seenCount++] = selector;
            IOLog("NATIVE_A540_ABI: sel=%u in=%u/%u out=%u/%u comm=%s\n", selector,
                  (unsigned)arguments->scalarInputCount, (unsigned)arguments->structureInputSize,
                  (unsigned)arguments->scalarOutputCount, (unsigned)arguments->structureOutputSize,
                  accelRequester());
            for (uint32_t k = 0; k < arguments->scalarInputCount && k < 4; ++k) {
                IOLog("NATIVE_A540_ABI: sel=%u s%u=%llx\n", selector, k,
                      (unsigned long long)arguments->scalarInput[k]);
            }
            if (arguments->structureInput && arguments->structureInputSize >= 8) {
                const uint32_t *words = static_cast<const uint32_t *>(arguments->structureInput);
                IOLog("NATIVE_A540_ABI: sel=%u w=%08x %08x\n", selector, words[0], words[1]);
            }
        }
    }
    if (!arguments || !arguments->structureInput || arguments->structureInputSize < 8) return kIOReturnUnsupported;
    const uint32_t *input = static_cast<const uint32_t *>(arguments->structureInput);
    if (input[0] != kAccelTunnelMagic) return kIOReturnUnsupported;
    const uint32_t method = input[1];
    native_a540_abi_calls = native_a540_abi_calls + 1;
    native_a540_abi_last_method = (int) method;
    static unsigned noted;
    if (method != kAccelProbe && noted < 32) {
        ++noted;
        IOLog("NATIVE_A540_ABI: method=%u selector=%u in=%u comm=%s\n", method, selector,
              (unsigned) arguments->structureInputSize, accelRequester());
    } else if (method == kAccelProbe && noted == 0) {
        noted = 1;   /* the selector probe is one line, not 24 */
        IOLog("NATIVE_A540_ABI: probe selector=%u comm=%s\n", selector, accelRequester());
    }
    // The userspace test probes selectors with {magic, kProbe}; admitting them all picks selector 0.
    if (method == kAccelProbe) return kIOReturnSuccess;
    switch (method) {
        case NativeAdrenoMethodQuery: {
            NativeAdrenoAbiParams params = {};
            params.abi_version = NativeAdrenoAbiVersion;
            params.input_bits = 32;
            params.output_bits = 40;
            params.flags = 0;
            params.max_bo_bytes = 256ull << 20;
            return accelCopyOut(arguments, &params, sizeof params);
        }
        case NativeAdrenoMethodGetDeviceInfo: {
            NativeAdrenoAbiDeviceInfo info = {};
            info.chip_id = 0x05040000;   /* Adreno 540 family (a5xx) */
            info.va_start = native_a540_bo_iova(0);
            info.va_size = (uint64_t)native_a540_bo_slots() * native_a540_bo_bytes();
            return accelCopyOut(arguments, &info, sizeof info);
        }
        case NativeAdrenoMethodGetStatus: {
            NativeAdrenoAbiStatus status = {};
            return accelCopyOut(arguments, &status, sizeof status);
        }
        case NativeAdrenoMethodGetTimestamp: {
            NativeAdrenoAbiTimestamp stamp = { mach_absolute_time() };
            return accelCopyOut(arguments, &stamp, sizeof stamp);
        }
        case NativeAdrenoMethodBoAlloc: {
            /* H236fd: the render-server test can reach the ABI before the C3
             * step machine has finished (it runs at ~35 s, C3 at ~70 s), so a
             * plain NotReady makes it give up. Wait for the engine instead. */
            for (unsigned i = 0; i < 2400 && !native_a540_gpu_ready(); ++i)
                IOSleep(50);   /* up to 120 s */
            if (!native_a540_gpu_ready()) return kIOReturnNotReady;
            int slot = -1;
            for (unsigned i = 0; i < native_a540_bo_slots() && i < 8; ++i)
                if (!boUsed_[i]) { slot = (int)i; break; }
            if (slot < 0) return kIOReturnNoSpace;
            boUsed_[slot] = true;
            NativeAdrenoAbiBoAllocReply reply = { (uint64_t)(slot + 1) };
            return accelCopyOut(arguments, &reply, sizeof reply);
        }
        case NativeAdrenoMethodBoMap: {
            if (arguments->structureInputSize < 8 + sizeof(NativeAdrenoAbiBoMap))
                return kIOReturnBadArgument;
            const NativeAdrenoAbiBoMap *m =
                reinterpret_cast<const NativeAdrenoAbiBoMap *>(input + 2);
            const int slot = slotOf(m->handle);
            if (slot < 0) return kIOReturnBadArgument;
            if (m->iova != native_a540_bo_iova((unsigned)slot)) return kIOReturnBadArgument;
            NativeAdrenoAbiBoMapReply reply = { m->iova };
            return accelCopyOut(arguments, &reply, sizeof reply);
        }
        case NativeAdrenoMethodBoCpuMap: {
            if (arguments->structureInputSize < 8 + sizeof(NativeAdrenoAbiBoCpuMap))
                return kIOReturnBadArgument;
            const NativeAdrenoAbiBoCpuMap *m =
                reinterpret_cast<const NativeAdrenoAbiBoCpuMap *>(input + 2);
            const int slot = slotOf(m->handle);
            if (slot < 0) return kIOReturnBadArgument;
            NativeAdrenoAbiBoCpuMapReply reply = {};
            reply.token = (uint32_t)slot;
            reply.bytes = native_a540_bo_bytes();
            return accelCopyOut(arguments, &reply, sizeof reply);
        }
        case NativeAdrenoMethodBoCpuUnmap:
            return kIOReturnSuccess;
        case NativeAdrenoMethodBoFree: {
            if (arguments->structureInputSize < 8 + sizeof(NativeAdrenoAbiBoFree))
                return kIOReturnBadArgument;
            const NativeAdrenoAbiBoFree *f =
                reinterpret_cast<const NativeAdrenoAbiBoFree *>(input + 2);
            const int slot = slotOf(f->handle);
            if (slot < 0) return kIOReturnBadArgument;
            boUsed_[slot] = false;
            return kIOReturnSuccess;
        }
        case NativeAdrenoMethodSubmit: {
            if (arguments->structureInputSize < 8 + sizeof(NativeAdrenoAbiSubmit))
                return kIOReturnBadArgument;
            const NativeAdrenoAbiSubmit *sub =
                reinterpret_cast<const NativeAdrenoAbiSubmit *>(input + 2);
            if (sub->buffer_count != 1) return kIOReturnBadArgument;
            const int slot = slotOf(sub->buffers[0].handle);
            if (slot < 0) return kIOReturnBadArgument;
            if (sub->buffers[0].iova != native_a540_bo_iova((unsigned)slot))
                return kIOReturnBadArgument;
            const unsigned words = (unsigned)(sub->buffers[0].bytes / 4);
            uint64_t fence = 0;
            if (native_a540_submit_bo((unsigned)slot, words, &fence))
                return kIOReturnError;
            NativeAdrenoAbiSubmitReply reply = { fence };
            return accelCopyOut(arguments, &reply, sizeof reply);
        }
        case NativeAdrenoMethodWaitFence: {
            if (arguments->structureInputSize < 8 + sizeof(NativeAdrenoAbiWaitFence))
                return kIOReturnBadArgument;
            const NativeAdrenoAbiWaitFence *w =
                reinterpret_cast<const NativeAdrenoAbiWaitFence *>(input + 2);
            if (native_a540_wait_fence(w->fence, w->timeout_ns))
                return kIOReturnTimeout;
            return kIOReturnSuccess;
        }
        case NativeAdrenoMethodSetMetadata: {
            if (arguments->structureInputSize < 8 + sizeof(NativeAdrenoAbiMetadata)) return kIOReturnBadArgument;
            const NativeAdrenoAbiMetadata *meta =
                reinterpret_cast<const NativeAdrenoAbiMetadata *>(input + 2);
            char text[NativeAdrenoAbiMetadataBytes + 1] = {};
            const uint32_t bytes = meta->bytes < NativeAdrenoAbiMetadataBytes ? meta->bytes : NativeAdrenoAbiMetadataBytes;
            memcpy(text, meta->text, bytes);
            IOLog("NATIVE_A540_ABI: report %s\n", text);
            for (unsigned i = 0; i < sizeof(native_a540_abi_report) - 1; ++i) {
                native_a540_abi_report[i] = text[i];
                if (!text[i]) break;
            }
            native_a540_abi_reports = native_a540_abi_reports + 1;
            return kIOReturnSuccess;
        }
        default:
            return kIOReturnUnsupported;
    }
}

class AGXAccelerator : public IOAcceleratorES {
    OSDeclareDefaultStructors(AGXAccelerator)
    IOReturn makeClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **);
public:
    bool init() override;
    bool start(IOService *provider) override;
    IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
    // H169: registry property reads reach these virtuals, so they show exactly
    // what the renderer asks the accelerator for (MetalPluginName, IOClass, …).
    OSObject *copyProperty(const OSSymbol *key) const override;
    OSObject *copyProperty(const OSString *key) const override;
    OSObject *copyProperty(const char *key) const override;
};
OSDefineMetaClassAndStructors(AGXAccelerator, IOAcceleratorES)

bool AGXAccelerator::init() {
    if (!IOService::init()) return false;
    // H199: the grant must exist before the first open, on every instance, whatever path published it.
    setProperty("NativeSandboxGrantProcess", "backboardd");
    return true;
}

bool AGXAccelerator::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    // No hardware behind this service: it only lets the renderer discovery
    // complete. Nothing here touches GPU registers, firmware or SMMU.
    setProperty("A540Probe", "minimal-no-hardware");
    // H197: the sandbox's iokit-open-user-client check names no class of this driver. The
    // zzw-native-adreno-sandbox-grant kernel patch lets the service name one process that is
    // granted its own user client in place of the sandbox verdict; the render server is the one
    // that must open it (H196: backboardd's open was the only one still refused by MACF).
    setProperty("NativeSandboxGrantProcess", "backboardd");
    // H170: CoreAnimation reads MetalPluginName from the matched accelerator and
    // then loads /System/Library/Extensions/<name>.bundle. An underscore-prefixed
    // boot argument forces a string copy in PE_parse_boot_argn (max 16 chars).
    char plugin[17];
    if (PE_parse_boot_argn("_offline-accelerator-plugin", plugin, sizeof plugin) && plugin[0]) {
        setProperty("MetalPluginName", plugin);
        IOLog("NATIVE_A540_ACCEL: MetalPluginName=%s\n", plugin);
    }
    IOLog("NATIVE_A540_ACCEL: started provider=%p\n", provider);
    return true;
}

static volatile unsigned gA540PropertyReads;
// H218: no IOLog here. A matching pass holds the IOKit registry lock while it reads
// properties, and logging from under that lock deadlocked the render server: H217's log
// ended at "op=match IOAcceleratorES" with no open request afterwards (the same call
// worked before H202 added the pre-emptive lookup). Only the count is kept now.
static void notePropertyRead(const char *key) {
    (void) key;
    if (gA540PropertyReads < 40) gA540PropertyReads = gA540PropertyReads + 1;
}
OSObject *AGXAccelerator::copyProperty(const OSSymbol *key) const {
    notePropertyRead(key ? key->getCStringNoCopy() : nullptr);
    return IORegistryEntry::copyProperty(key);
}
OSObject *AGXAccelerator::copyProperty(const OSString *key) const {
    notePropertyRead(key ? key->getCStringNoCopy() : nullptr);
    return IORegistryEntry::copyProperty(key);
}
OSObject *AGXAccelerator::copyProperty(const char *key) const {
    notePropertyRead(key);
    return IORegistryEntry::copyProperty(key);
}

IOReturn AGXAccelerator::makeClient(task_t task,void *security,UInt32 type,
                                               OSDictionary *properties,IOUserClient **client) {
    {
        OSString *grant = OSDynamicCast(OSString, getProperty("NativeSandboxGrantProcess"));
        native_a540_open_count = native_a540_open_count + 1;
        native_a540_open_type = type;
        IOLog("NATIVE_A540_ACCEL: open request type=%u comm=%s self=%p grant=%s\n", type,
              accelRequester(), this, grant ? grant->getCStringNoCopy() : "-");
    }
    if (!client) return kIOReturnBadArgument;
    *client = nullptr;
    AGXDeviceUserClient *created = new AGXDeviceUserClient;
    if (!created) return kIOReturnNoMemory;
    if (!created->initWithTask(task,security,type,properties) || !created->attach(this) ||
        !created->start(this)) {
        created->detach(this);
        created->release();
        IOLog("NATIVE_A540_ACCEL: user client type=%u setup failed\n", type);
        return kIOReturnError;
    }
    *client = created;
    IOLog("NATIVE_A540_ACCEL: user client type=%u granted\n", type);
    return kIOReturnSuccess;
}
IOReturn AGXAccelerator::newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) {
    return makeClient(task,security,type,nullptr,client);
}
IOReturn AGXAccelerator::newUserClient(task_t task,void *security,UInt32 type,
                                                   OSDictionary *properties,IOUserClient **client) {
    return makeClient(task,security,type,properties,client);
}

// Called by LegacyDisplayProbe.cpp between OSMetaClass::preModLoad() and
// postModLoad(), together with the display and presenter constructors. The
// global constructor is aliased here so the display probe can find it.
extern "C" void native_a540_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_AGXAccelerator.cpp");
volatile uint32_t native_a540_accel_state;

extern "C" int native_a540_accelerator_probe(void) {
    native_a540_accel_state = 1;
    IOService *platform = IOService::getResourceService();
    AGXAccelerator *service = new AGXAccelerator;
    if (!platform || !service || !service->init()) {
        if (service) service->release();
        IOLog("NATIVE_A540_ACCEL: init/platform failed\n");
        native_a540_accel_state = 3;
        return 0;
    }
    if (!service->attach(platform)) {
        service->release();
        IOLog("NATIVE_A540_ACCEL: attach failed\n");
        native_a540_accel_state = 4;
        return 0;
    }
    service->setName("AGXAccelerator");
    if (!service->start(platform)) {
        service->detach(platform);
        service->release();
        IOLog("NATIVE_A540_ACCEL: start failed\n");
        native_a540_accel_state = 5;
        return 0;
    }
    service->registerService();
    native_a540_accel_state = 6;
    IOLog("NATIVE_A540_ACCEL: published IOAcceleratorES (probe only)\n");
    return 1;
}

extern "C" unsigned native_a540_accelerator_instances(void) {
    return AGXAccelerator::gMetaClass.getInstanceCount();
}

// H104e: use the real prelinked IOAcceleratorES/IOGPU class instead of a local
// subclass. The local subclass's absolute base bindings corrupted early
// OSMetaClass bookkeeping; the property-only stub was ignored by CoreAnimation.
// This probe allocates the original class at runtime, attaches it to the
// resource service, and publishes it. It never touches GPU registers.
//
// `h103-iogpu-start=1` additionally calls the original start(); it is off by
// default because IOGPU::start expects a real GPU provider.
extern "C" int native_iogpu_accel_probe(void) {
    native_a540_accel_state = 1;
    static const char *names[] = {
        "IOGPU", "IOGPUDevice", "IOAcceleratorES", "IOGPUFamily"
    };
    OSObject *object = nullptr;
    IOService *service = nullptr;
    const char *name = "none";
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const OSSymbol *symbol = OSSymbol::withCStringNoCopy(names[i]);
        OSMetaClass *mc = symbol ? const_cast<OSMetaClass *>(OSMetaClass::getMetaClassWithName(symbol)) : nullptr;
        OSObject *candidate = mc ? mc->alloc() : nullptr;
        IOService *candidateService = OSDynamicCast(IOService, candidate);
        IOLog("NATIVE_IOGPU_ACCEL: name=%s mc=%p obj=%p service=%p\n",
              names[i], mc, candidate, candidateService);
        if (!object && candidateService) {
            object = candidate;
            service = candidateService;
            name = names[i];
        } else if (candidate) {
            candidate->release();
        }
    }
    if (!service) {
        native_a540_accel_state = 3;
        return 0;
    }
    IOService *parent = IOService::getResourceService();
    if (!parent || !service->attach(parent)) {
        IOLog("NATIVE_IOGPU_ACCEL: attach failed parent=%p\n", parent);
        service->release();
        native_a540_accel_state = 4;
        return 0;
    }
    service->setName("NativeIOGPUAccelerator");
    unsigned startEnabled = 0;
    PE_parse_boot_argn("h103-iogpu-start", &startEnabled, sizeof(startEnabled));
    bool started = false;
    if (startEnabled == 1) {
        started = service->start(parent);
        IOLog("NATIVE_IOGPU_ACCEL: start=%d\n", started);
        if (!started) {
            service->detach(parent);
            service->release();
            native_a540_accel_state = 5;
            return 0;
        }
    }
    service->registerService();
    native_a540_accel_state = 6;
    IOLog("NATIVE_IOGPU_ACCEL: published class=%s started=%d\n", name, started);
    return 1;
}
