// SPDX-License-Identifier: MIT
// Minimal, own-code IOAcceleratorES publication probe for the A540 bring-up.
// It never touches GPU registers, firmware, SMMU or command submission. Its
// only purpose is to answer whether backboardd/Metal discovers and queries an
// accelerator service before the real QcomGPU stack exists.
#include "../NativeMetal/OriginalAcceleratorABI.h"
#include <IOKit/IOLib.h>
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);

class NativeA540AcceleratorProbe : public IOAcceleratorES {
    OSDeclareDefaultStructors(NativeA540AcceleratorProbe)
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        setProperty("MetalPluginName", "NativeAdrenoMetal");
        setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
        setProperty("IOClass", "IOAcceleratorES");
        setProperty("A540Probe", "minimal-no-hardware");
        return true;
    }
};
OSDefineMetaClassAndStructors(NativeA540AcceleratorProbe, IOAcceleratorES)

extern "C" void native_a540_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeA540AcceleratorProbe.cpp");
volatile uint32_t native_a540_accel_state;

extern "C" int native_a540_accelerator_probe(void) {
    native_a540_accel_state = 1;
    if (!OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily")) {
        IOLog("NATIVE_A540_ACCEL: IOGPUFamily load failed\n");
        native_a540_accel_state = 2;
        return 0;
    }
    native_a540_accelerator_static_init();
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
