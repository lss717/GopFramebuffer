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

// H168: a registry-visible accelerator. CoreAnimation looks the renderer up
// with IOServiceMatching("IOAcceleratorES"), which matches on the IOProviderClass
// class hierarchy, so a property named "IOClass" is invisible; the service must
// really derive from the (audited) original IOAcceleratorES. The class is
// registered only when the original IOGPUFamily kext is already loaded, because
// its parent metaclass lives there.
class NativeAcceleratorClient : public IOUserClient {
    OSDeclareDefaultStructors(NativeAcceleratorClient)
public:
    bool initWithTask(task_t owningTask,void *securityID,UInt32 type,OSDictionary *properties) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
};
OSDefineMetaClassAndStructors(NativeAcceleratorClient,IOUserClient)

bool NativeAcceleratorClient::initWithTask(task_t owningTask,void *securityID,UInt32 type,OSDictionary *properties) {
    if(!IOUserClient::initWithTask(owningTask,securityID,type,properties))return false;
    if(!owningTask)return false;
    return true;
}
IOReturn NativeAcceleratorClient::clientClose() {terminate();return kIOReturnSuccess;}
IOReturn NativeAcceleratorClient::clientDied() {return clientClose();}

class NativeA540AcceleratorProbe : public IOAcceleratorES {
    OSDeclareDefaultStructors(NativeA540AcceleratorProbe)
    IOReturn makeClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **);
public:
    bool start(IOService *provider) override;
    IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
    // H169: registry property reads reach these virtuals, so they show exactly
    // what the renderer asks the accelerator for (MetalPluginName, IOClass, …).
    OSObject *copyProperty(const OSSymbol *key) const override;
    OSObject *copyProperty(const OSString *key) const override;
    OSObject *copyProperty(const char *key) const override;
};
OSDefineMetaClassAndStructors(NativeA540AcceleratorProbe, IOAcceleratorES)

bool NativeA540AcceleratorProbe::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    // No hardware behind this service: it only lets the renderer discovery
    // complete. Nothing here touches GPU registers, firmware or SMMU.
    setProperty("A540Probe", "minimal-no-hardware");
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
OSObject *NativeA540AcceleratorProbe::copyProperty(const OSSymbol *key) const {
    notePropertyRead(key ? key->getCStringNoCopy() : nullptr);
    return IORegistryEntry::copyProperty(key);
}
OSObject *NativeA540AcceleratorProbe::copyProperty(const OSString *key) const {
    notePropertyRead(key ? key->getCStringNoCopy() : nullptr);
    return IORegistryEntry::copyProperty(key);
}
OSObject *NativeA540AcceleratorProbe::copyProperty(const char *key) const {
    notePropertyRead(key);
    return IORegistryEntry::copyProperty(key);
}

IOReturn NativeA540AcceleratorProbe::makeClient(task_t task,void *security,UInt32 type,
                                               OSDictionary *properties,IOUserClient **client) {
    IOLog("NATIVE_A540_ACCEL: open request type=%u\n", type);
    if (!client) return kIOReturnBadArgument;
    *client = nullptr;
    NativeAcceleratorClient *created = new NativeAcceleratorClient;
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
IOReturn NativeA540AcceleratorProbe::newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) {
    return makeClient(task,security,type,nullptr,client);
}
IOReturn NativeA540AcceleratorProbe::newUserClient(task_t task,void *security,UInt32 type,
                                                   OSDictionary *properties,IOUserClient **client) {
    return makeClient(task,security,type,properties,client);
}

// Called by LegacyDisplayProbe.cpp between OSMetaClass::preModLoad() and
// postModLoad(), together with the display and presenter constructors. The
// global constructor is aliased here so the display probe can find it.
extern "C" void native_a540_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeA540AcceleratorProbe.cpp");
volatile uint32_t native_a540_accel_state;

extern "C" int native_a540_accelerator_probe(void) {
    native_a540_accel_state = 1;
    IOService *platform = IOService::getResourceService();
    NativeA540AcceleratorProbe *service = new NativeA540AcceleratorProbe;
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
    service->setName("NativeA540Accelerator");
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
    return NativeA540AcceleratorProbe::gMetaClass.getInstanceCount();
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
        "IOGPU", "IOGPUDevice", "AGXAccelerator", "IOAcceleratorES", "IOGPUFamily"
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
