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
extern "C" volatile int native_a540_abi_calls;
extern "C" volatile int native_a540_abi_last_method;
extern "C" volatile int native_a540_abi_reports;
extern "C" char native_a540_abi_report[64];
extern "C" { volatile int native_a540_abi_calls = 0; volatile int native_a540_abi_last_method = 0;
             volatile int native_a540_abi_reports = 0; char native_a540_abi_report[64] = {}; }

// H195: the class name is the sandbox key: the target's MACF profile admits IOServiceOpen only for
// the user-client class AGXDeviceUserClient (the same reason NativeAdrenoPublish.cpp uses it), and
// H194's kernel-side grant was followed by "IOUC NativeAcceleratorClient failed MACF".
class AGXDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AGXDeviceUserClient)
public:
    bool initWithTask(task_t owningTask,void *securityID,UInt32 type,OSDictionary *properties) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
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

IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                                 IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    (void) selector; (void) dispatch; (void) target; (void) reference;
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
            info.va_start = 0x10000000;
            info.va_size = 0x10000000;
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
static void notePropertyRead(const char *key) {
    unsigned seen = gA540PropertyReads;
    if (seen >= 40) return;
    gA540PropertyReads = seen + 1;
    IOLog("NATIVE_A540_ACCEL: read key=%s\n", key ? key : "(null)");
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
