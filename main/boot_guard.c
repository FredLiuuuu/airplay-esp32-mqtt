// SPDX-License-Identifier: GPL-3.0-or-later
// airplay-esp32-mqtt: app-level crash-loop guard.
//
// Many boards keep a vendor bootloader whose app-rollback support is unknown
// (and web OTA cannot replace it), so this app protects itself: if it keeps
// crashing (panic or watchdog) right after boot, it switches the boot
// partition back to the other OTA slot, which holds the firmware it was
// updated from.

#include "boot_guard.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi.h"

#define BOOT_GUARD_MAGIC 0x57304255u /* "W0BU" */
#define BOOT_GUARD_MAX_CRASHES 3
#define BOOT_GUARD_STABLE_S 60

static const char *TAG = "boot_guard";

static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_crash_boots;
static RTC_NOINIT_ATTR uint32_t s_check; /* ~s_crash_boots */

static uint32_t s_seen;

static bool is_crash(esp_reset_reason_t r) {
  switch (r) {
  case ESP_RST_PANIC:
  case ESP_RST_INT_WDT:
  case ESP_RST_TASK_WDT:
  case ESP_RST_WDT:
    return true;
  default:
    return false;
  }
}

static void store(uint32_t n) {
  s_crash_boots = n;
  s_check = ~n;
  s_magic = BOOT_GUARD_MAGIC;
}

void boot_guard_check(void) {
  esp_reset_reason_t reason = esp_reset_reason();
  bool valid = (s_magic == BOOT_GUARD_MAGIC) && (s_check == ~s_crash_boots);
  uint32_t n = valid ? s_crash_boots : 0;

  if (is_crash(reason)) {
    n++;
  } else {
    n = 0; // power-on, software restart, OTA reboot, brown-out...
  }
  store(n);
  s_seen = n;

  if (n == 0) {
    return;
  }
  ESP_LOGW(TAG, "crash boot %lu of %d in a row (reset reason %d)",
           (unsigned long)n, BOOT_GUARD_MAX_CRASHES, (int)reason);
  if (n < BOOT_GUARD_MAX_CRASHES) {
    return;
  }

  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
  if (!other || other == running) {
    ESP_LOGE(TAG, "no other OTA slot to fall back to");
    store(0);
    return;
  }
  ESP_LOGE(TAG, "crash loop: switching boot slot %s -> %s and restarting",
           running ? running->label : "?", other->label);
  store(0);
  esp_err_t err = esp_ota_set_boot_partition(other);
  if (err != ESP_OK) {
    // The other slot does not hold a valid image; keep running this one.
    ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s) failed: %s", other->label,
             esp_err_to_name(err));
    return;
  }
  esp_restart();
}

static void stable_task(void *arg) {
  (void)arg;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    if (esp_timer_get_time() >= (int64_t)BOOT_GUARD_STABLE_S * 1000000 &&
        wifi_is_connected()) {
      if (s_crash_boots != 0) {
        ESP_LOGI(TAG, "stable for %ds with Wi-Fi up: crash counter cleared",
                 BOOT_GUARD_STABLE_S);
      }
      store(0);
      s_seen = 0;
      break;
    }
  }
  vTaskDelete(NULL);
}

void boot_guard_start_stable_timer(void) {
  xTaskCreate(stable_task, "boot_guard", 2560, NULL, 1, NULL);
}

uint32_t boot_guard_crash_count(void) { return s_seen; }
