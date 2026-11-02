// SPDX-License-Identifier: BSD-2-Clause-Patent
// OP5 legacy-display bring-up: the sheng probe's unrelated device probes are
// intentionally absent.  These are C-linkage no-ops so the unchanged display
// lifecycle probe can be linked without pulling in touch/WiFi/audio drivers.
#include <stdint.h>
extern "C" {
void native_hid_input_probe(void) {}
void native_keystore_probe(void) {}
void native_touch_probe(void) {}
void sheng_cpufreq_probe(void) {}
void sheng_gpu_power_probe(void) {}
void sheng_wifi_probe(void) {}
void sheng_adsp_probe(void) {}
void sheng_battery_probe(void) {}
void sheng_audio_probe(void) {}
void sheng_buttons_probe(void) {}
void sheng_backlight_probe(void) {}
void sheng_dpu_probe(void) {}
// The boot-progress archive is disabled in the H103 display build.  The
// display probe only uses these as optional scanout coordination hooks.
int boot_progress_prepare_scanout(void) { return 1; }
void boot_progress_cancel_scanout(void) {}
void boot_progress_stage(const char *) {}
int boot_progress_pulse_anchor(uintptr_t,uint64_t,unsigned,unsigned,unsigned) { return 0; }
}
