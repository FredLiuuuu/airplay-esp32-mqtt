// SPDX-License-Identifier: GPL-3.0-or-later
// airplay-esp32-mqtt: crash capture that survives a reset (no coredump partition).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

/** Call first thing in app_main: snapshots the previous boot's ring. */
void crash_log_init(void);

/** Append formatted log text (called from the log_stream vprintf hook). */
void crash_log_append(const char *s, size_t len);

/** True if the previous boot left a log; *crash says if it ended in a crash
 * (panic / watchdog). reason is a short string, "" if none. */
bool crash_log_present(bool *crash, const char **reason);

/** GET /api/system/crashlog (text/plain, read-only). */
esp_err_t crash_log_http_handler(httpd_req_t *req);
