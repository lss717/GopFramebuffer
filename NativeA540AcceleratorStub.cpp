// SPDX-License-Identifier: BSD-2-Clause-Patent
// H236bv B-line stub: the A540 C3 (NativeA540C3.cpp) is the bring-up path.
// The A-line AGXAccelerator probe pulls in IOAcceleratorES from the original
// IOAcceleratorFamily2 kext, which is not part of this kernel link. Keep the
// display/UFS ABI symbols defined so the C3 and Legacy display can link; the
// accelerator probe reports "not present" until the original kext is linked.
#include <stdint.h>

extern "C" {
volatile uint32_t native_a540_accel_state = 0;
volatile int native_a540_abi_reports = 0;
char native_a540_abi_report[64] = {};
volatile uint32_t native_a540_abi_calls = 0;
volatile uint32_t native_a540_abi_last_method = 0;
volatile uint32_t native_a540_open_count = 0;
volatile uint32_t native_a540_open_type = 0;
volatile uint32_t native_a540_sel_count = 0;
volatile uint32_t native_a540_sel_last = 0;
}

extern "C" int native_a540_accelerator_probe(void) {
    return 0;
}

extern "C" int native_iogpu_accel_probe(void) {
    return 0;
}

/* LegacyDisplayProbe.cpp calls the AGX probe's global constructor by name.
 * The A540 C3 path does not need the AGXAccelerator/IOAcceleratorES object. */
extern "C" void native_a540_accelerator_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeA540AcceleratorProbe.cpp");
extern "C" void native_a540_accelerator_static_init(void) {}

