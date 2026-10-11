#pragma once

#include <stdbool.h>

/**
 * Brown-out / crash boot-loop breaker.
 *
 * Counts consecutive boots that fail to stay up, in NVS
 * (CONFIG_KEY_BOOT_FAIL). The complement of plug_reset: it counts boots
 * whose reset reason is NOT a clean power-on — a brown-out reset (on the
 * C5/C6 a BOD-triggered software reset reports as SW/"unknown", never
 * POWERON), a panic, or a watchdog. A user unplug/replug is POWERON and is
 * deliberately NOT counted, so normal power cycling can never trip this.
 *
 * A boot that stays up BOOT_GUARD_STABLE_MS clears the streak. On the
 * BOOT_GUARD_THRESHOLD-th un-stable boot in a row the lamp enters SAFE
 * BOOT: the caller skips lighting the panel (the usual boot-time current
 * surge) and comes up dark but reachable on the network, so an
 * underpowered lamp — e.g. a 256-LED panel on USB — always claws back to a
 * reachable state instead of looping forever. Safe boot is latched: it is
 * left tripped until the user takes control with an explicit power command
 * (boot_guard_user_ok), which clears the streak.
 *
 * Call order in app_main():
 *   boot_guard_begin()     — early, right after plug_reset_check() (needs
 *                            config_store up). Increments/evaluates the
 *                            streak before the risky wifi + panel bring-up.
 *   boot_guard_is_safe()   — gate panel-lighting (persisted-state restore,
 *                            AP-mode autoplay) and report in /api/health.
 *   boot_guard_arm_clear() — late, after the panel restore. Arms the
 *                            stable-uptime timer that clears the streak.
 *   boot_guard_user_ok()   — from POST /api/power: the user is driving the
 *                            lamp, so clear the streak and leave safe boot.
 */
void boot_guard_begin(void);
bool boot_guard_is_safe(void);
void boot_guard_arm_clear(void);
void boot_guard_user_ok(void);
