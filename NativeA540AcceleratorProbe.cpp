// SPDX-License-Identifier: MIT
// Minimal, own-code IOAcceleratorES publication probe for the A540 bring-up.
// It never touches GPU registers, firmware, SMMU or command submission. Its
// only purpose is to answer whether backboardd/Metal discovers and queries an
// accelerator service before the real QcomGPU stack exists.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

class NativeA540AcceleratorProbe : public IOService {
    OSDeclareDefaultStructors(NativeA540AcceleratorProbe)
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        // H103fs: property-only stub. Do not derive from the prelinked
        // IOAcceleratorES: the absolute base-class bindings corrupt early
        // OSMetaClass bookkeeping. Publish the class name as a property so
        // discovery can be tested without the accelerator ABI.
        setProperty("IOClass", "IOAcceleratorES");
        setProperty("A540Probe", "minimal-no-hardware");
        return true;
    }
};
OSDefineMetaClassAndStructors(NativeA540AcceleratorProbe, IOService)

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
