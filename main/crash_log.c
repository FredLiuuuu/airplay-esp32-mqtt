// SPDX-License-Identifier: GPL-3.0-or-later
// airplay-esp32-mqtt: crash capture that survives a reset.
//
// The partition table has no coredump slot and OTA cannot add one, so the
// last few KB of log text and a short panic record (reason, PC, backtrace of
// both cores, the task running on each) are kept in RTC slow memory, which a
// panic / watchdog / software reset leaves alone. On the next boot they are
// copied to the heap and served read-only at GET /api/system/crashlog.
//
// RTC slow memory is accessed in 32-bit words only.

#include "crash_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xtensa_context.h"

static const char *TAG = "crash_log";

#define CL_MAGIC 0x434c4f47u /* "CLOG" */
#define CL_RING_WORDS 768    /* 3 KB of text */
#define CL_RING_BYTES (CL_RING_WORDS * 4)
#define CL_BT_DEPTH 10

typedef struct {
  uint32_t magic;
  uint32_t check; /* ~magic ^ boot */
  uint32_t boot;  /* boots counted since the ring was created */
  uint32_t head;  /* bytes written in total (position = head % size) */
  uint32_t ring[CL_RING_WORDS];
  /* panic record */
  uint32_t p_magic;
  uint32_t p_core;
  uint32_t p_addr;
  uint32_t p_uptime_ms;
  uint32_t p_reason[12];        /* 48 chars */
  uint32_t p_task[2][4];        /* 16 chars each */
  uint32_t p_bt[2][CL_BT_DEPTH]; /* PCs per core */
} crash_rtc_t;

static RTC_NOINIT_ATTR crash_rtc_t s_rtc;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready;

/* previous boot, copied out at init */
static char *s_prev;   /* text, NUL terminated */
static size_t s_prev_len;
static bool s_prev_present;
static bool s_prev_crash;
static char s_prev_reason[64];

static inline void IRAM_ATTR put_byte(uint32_t pos, uint8_t b) {
  uint32_t w = pos >> 2, sh = (pos & 3) * 8;
  uint32_t v = s_rtc.ring[w];
  v = (v & ~(0xffu << sh)) | ((uint32_t)b << sh);
  s_rtc.ring[w] = v;
}

static inline uint8_t get_byte(uint32_t pos) {
  return (uint8_t)(s_rtc.ring[pos >> 2] >> ((pos & 3) * 8));
}

static void IRAM_ATTR put_words(uint32_t *dst, size_t nwords, const char *s) {
  for (size_t i = 0; i < nwords; i++) {
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
      char c = s ? *s : 0;
      if (c) {
        s++;
      }
      v |= (uint32_t)(uint8_t)c << (k * 8);
    }
    dst[i] = v;
  }
  /* always terminate */
  dst[nwords - 1] &= 0x00ffffffu;
}

static void get_words(const uint32_t *src, size_t nwords, char *out) {
  for (size_t i = 0; i < nwords * 4; i++) {
    out[i] = (char)(src[i >> 2] >> ((i & 3) * 8));
  }
  out[nwords * 4 - 1] = 0;
}

void crash_log_append(const char *s, size_t len) {
  if (!s_ready || !s || !len) {
    return;
  }
  taskENTER_CRITICAL(&s_mux);
  uint32_t h = s_rtc.head;
  for (size_t i = 0; i < len; i++) {
    put_byte((h + i) % CL_RING_BYTES, (uint8_t)s[i]);
  }
  s_rtc.head = h + (uint32_t)len;
  taskEXIT_CRITICAL(&s_mux);
}

static bool is_crash(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
         r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

static const char *rr_str(esp_reset_reason_t r) {
  switch (r) {
  case ESP_RST_POWERON: return "power_on";
  case ESP_RST_SW: return "software";
  case ESP_RST_PANIC: return "panic";
  case ESP_RST_INT_WDT: return "int_wdt";
  case ESP_RST_TASK_WDT: return "task_wdt";
  case ESP_RST_WDT: return "wdt";
  case ESP_RST_BROWNOUT: return "brownout";
  case ESP_RST_DEEPSLEEP: return "deep_sleep";
  default: return "other";
  }
}

void crash_log_init(void) {
  esp_reset_reason_t rr = esp_reset_reason();
  bool valid = s_rtc.magic == CL_MAGIC &&
               s_rtc.check == (~CL_MAGIC ^ s_rtc.boot) &&
               rr != ESP_RST_POWERON && rr != ESP_RST_BROWNOUT;
  if (valid) {
    uint32_t h = s_rtc.head;
    uint32_t n = h < CL_RING_BYTES ? h : CL_RING_BYTES;
    uint32_t start = h - n;
    size_t cap = n + 1024;
    s_prev = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prev) {
      s_prev = malloc(cap);
    }
    if (s_prev) {
      size_t o = 0;
      o += snprintf(s_prev + o, cap - o,
                    "# previous boot #%" PRIu32 ", ended by reset reason %s\n",
                    s_rtc.boot, rr_str(rr));
      if (s_rtc.p_magic == CL_MAGIC) {
        char reason[48], t0[16], t1[16];
        get_words(s_rtc.p_reason, 12, reason);
        get_words(s_rtc.p_task[0], 4, t0);
        get_words(s_rtc.p_task[1], 4, t1);
        o += snprintf(s_prev + o, cap - o,
                      "# panic: %s (core %" PRIu32 ", addr 0x%08" PRIx32
                      ", uptime %" PRIu32 " ms)\n# core0 task %s:",
                      reason, s_rtc.p_core, s_rtc.p_addr, s_rtc.p_uptime_ms,
                      t0);
        for (int i = 0; i < CL_BT_DEPTH && s_rtc.p_bt[0][i]; i++) {
          o += snprintf(s_prev + o, cap - o, " 0x%08" PRIx32, s_rtc.p_bt[0][i]);
        }
        o += snprintf(s_prev + o, cap - o, "\n# core1 task %s:", t1);
        for (int i = 0; i < CL_BT_DEPTH && s_rtc.p_bt[1][i]; i++) {
          o += snprintf(s_prev + o, cap - o, " 0x%08" PRIx32, s_rtc.p_bt[1][i]);
        }
        o += snprintf(s_prev + o, cap - o, "\n");
        snprintf(s_prev_reason, sizeof(s_prev_reason), "%s", reason);
      } else {
        snprintf(s_prev_reason, sizeof(s_prev_reason), "%s", rr_str(rr));
      }
      o += snprintf(s_prev + o, cap - o, "# last %" PRIu32 " bytes of log:\n",
                    n);
      for (uint32_t i = 0; i < n && o < cap - 1; i++) {
        char c = (char)get_byte((start + i) % CL_RING_BYTES);
        s_prev[o++] = c ? c : '?';
      }
      s_prev[o] = 0;
      s_prev_len = o;
      s_prev_present = true;
      s_prev_crash = is_crash(rr);
    }
  }
  /* start this boot's ring */
  uint32_t boot = valid ? s_rtc.boot + 1 : 1;
  s_rtc.p_magic = 0;
  s_rtc.head = 0;
  s_rtc.boot = boot;
  s_rtc.check = ~CL_MAGIC ^ boot;
  s_rtc.magic = CL_MAGIC;
  s_ready = true;
  ESP_LOGI(TAG, "boot #%" PRIu32 "; previous log %s%s", boot,
           s_prev_present ? "kept" : "none",
           s_prev_crash ? " (crash)" : "");
}

bool crash_log_present(bool *crash, const char **reason) {
  if (crash) {
    *crash = s_prev_crash;
  }
  if (reason) {
    *reason = s_prev_reason;
  }
  return s_prev_present;
}

esp_err_t crash_log_http_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  if (!s_prev_present || !s_prev) {
    return httpd_resp_sendstr(req, "# no log from a previous boot\n");
  }
  return httpd_resp_send(req, s_prev, (ssize_t)s_prev_len);
}

/* ---- panic hook (linked with -Wl,--wrap=esp_panic_handler) ---- */

extern void __real_esp_panic_handler(panic_info_t *info);

static void IRAM_ATTR record_bt(int core) {
  for (int i = 0; i < CL_BT_DEPTH; i++) {
    s_rtc.p_bt[core][i] = 0;
  }
  XtExcFrame *f = (XtExcFrame *)g_exc_frames[core];
  if (!f) {
    return;
  }
  esp_backtrace_frame_t fr = {
      .pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = f};
  s_rtc.p_bt[core][0] = esp_cpu_process_stack_pc(fr.pc);
  for (int i = 1; i < CL_BT_DEPTH; i++) {
    if (fr.next_pc == 0 || !esp_stack_ptr_is_sane(fr.sp)) {
      break;
    }
    if (!esp_backtrace_get_next_frame(&fr)) {
      break;
    }
    s_rtc.p_bt[core][i] = esp_cpu_process_stack_pc(fr.pc);
  }
}

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info) {
  if (s_ready && info) {
    s_rtc.p_core = (uint32_t)info->core;
    s_rtc.p_addr = (uint32_t)info->addr;
    s_rtc.p_uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const char *r = info->reason;
    if (r && !esp_ptr_byte_accessible(r) && !esp_ptr_in_drom(r)) {
      r = "?";
    }
    put_words(s_rtc.p_reason, 12, r);
    for (int c = 0; c < portNUM_PROCESSORS && c < 2; c++) {
      TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(c);
      put_words(s_rtc.p_task[c], 4, t ? pcTaskGetName(t) : "-");
      record_bt(c);
    }
    s_rtc.p_magic = CL_MAGIC;
  }
  __real_esp_panic_handler(info);
}
