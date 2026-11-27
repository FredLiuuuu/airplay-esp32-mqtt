// SPDX-License-Identifier: GPL-3.0-or-later
// airplay-esp32-mqtt: app-level crash-loop guard.
#pragma once

#include <stdint.h>

/**
 * Call first in app_main(). Counts consecutive boots that followed a panic or
 * watchdog reset (kept in RTC no-init RAM). After BOOT_GUARD_MAX_CRASHES such
 * boots in a row it points otadata at the other OTA slot (the previous
 * firmware) and restarts.
 */
void boot_guard_check(void);

/**
 * Starts a small task that clears the counter once the app has run for
 * BOOT_GUARD_STABLE_S seconds with Wi-Fi up. Call after Wi-Fi init.
 */
void boot_guard_start_stable_timer(void);

/** Consecutive crash boots seen at this boot (0 once the app is stable). */
uint32_t boot_guard_crash_count(void);
