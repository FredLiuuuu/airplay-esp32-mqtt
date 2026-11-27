// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * MQTT now-playing publisher — see mqtt_nowplaying.h.
 *
 * Threading: RTSP event callbacks only copy data under a mutex and set a
 * dirty flag.  A single low-priority worker task does all MQTT publishing,
 * so the RTSP/audio path never blocks on the network.  No NVS writes happen
 * during playback (only when the user saves the MQTT form).
 */

#include "mqtt_nowplaying.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "dacp_client.h"
#include "playback_control.h"
#include "playback_events.h"
#include "settings.h"
#include "spiram_task.h"
#include "wifi.h"

static const char *TAG = "mqtt_np";

#define NP_NVS_NS        "mqtt_np"
#define NP_HOST_MAX      64
#define NP_USER_MAX      64
#define NP_PASS_MAX      96
#define NP_BASE_MAX      48
#define NP_PREFIX_MAX    32
#define NP_TOPIC_MAX     128
#define NP_PROGRESS_SECS 5 // republish position this often while playing

typedef struct {
  bool enabled;
  char host[NP_HOST_MAX];
  uint16_t port;
  char user[NP_USER_MAX];
  char pass[NP_PASS_MAX];
  char base[NP_BASE_MAX];     // base topic, e.g. "airplay/a1b2c3" (last 3 MAC bytes)
  char prefix[NP_PREFIX_MAX]; // discovery prefix, e.g. "homeassistant"
} np_config_t;

typedef enum {
  NP_STATE_IDLE = 0,
  NP_STATE_PLAYING,
  NP_STATE_PAUSED,
} np_play_state_t;

typedef struct {
  np_play_state_t state;
  bool connected; // an AirPlay client holds a session
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  char genre[METADATA_STRING_MAX];
  uint32_t duration;
  uint32_t position;        // seconds at position_at_us
  int64_t position_at_us;   // esp_timer time when position was set
} np_state_t;

static np_config_t s_cfg;
static np_state_t s_np;
static SemaphoreHandle_t s_lock;
static volatile bool s_dirty;
static volatile playback_source_t s_last_source = PLAYBACK_SOURCE_NONE;
static volatile bool s_restart;
static esp_mqtt_client_handle_t s_client;
static volatile bool s_mqtt_connected;
static TaskHandle_t s_task;
static char s_dev_id[32]; // "airplay_esp32_<12 hex digits of the STA MAC>"

// ---------------------------------------------------------------------------
// Config (NVS)
// ---------------------------------------------------------------------------

static void cfg_defaults(np_config_t *c) {
  memset(c, 0, sizeof(*c));
  c->enabled = false;
  c->port = 1883;
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(c->base, sizeof(c->base), "airplay/%02x%02x%02x", mac[3], mac[4],
           mac[5]);
  strlcpy(c->prefix, "homeassistant", sizeof(c->prefix));
}

static void nvs_get_str_opt(nvs_handle_t h, const char *key, char *out,
                            size_t len) {
  size_t l = len;
  if (nvs_get_str(h, key, out, &l) != ESP_OK) {
    // keep default
  }
}

static void cfg_load(np_config_t *c) {
  cfg_defaults(c);
  nvs_handle_t h;
  if (nvs_open(NP_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
    return;
  }
  uint8_t u8;
  if (nvs_get_u8(h, "en", &u8) == ESP_OK) {
    c->enabled = u8 != 0;
  }
  uint16_t u16;
  if (nvs_get_u16(h, "port", &u16) == ESP_OK && u16 != 0) {
    c->port = u16;
  }
  nvs_get_str_opt(h, "host", c->host, sizeof(c->host));
  nvs_get_str_opt(h, "user", c->user, sizeof(c->user));
  nvs_get_str_opt(h, "pass", c->pass, sizeof(c->pass));
  nvs_get_str_opt(h, "base", c->base, sizeof(c->base));
  nvs_get_str_opt(h, "prefix", c->prefix, sizeof(c->prefix));
  nvs_close(h);
}

static esp_err_t cfg_save(const np_config_t *c) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NP_NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_u8(h, "en", c->enabled ? 1 : 0);
  if (err == ESP_OK) {
    err = nvs_set_u16(h, "port", c->port);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(h, "host", c->host);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(h, "user", c->user);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(h, "pass", c->pass);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(h, "base", c->base);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(h, "prefix", c->prefix);
  }
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return err;
}

// ---------------------------------------------------------------------------
// Now-playing state (fed by RTSP events)
// ---------------------------------------------------------------------------

static uint32_t current_position(const np_state_t *st) {
  if (st->state != NP_STATE_PLAYING || st->position_at_us == 0) {
    return st->position;
  }
  int64_t dt = (esp_timer_get_time() - st->position_at_us) / 1000000;
  uint64_t p = (uint64_t)st->position + (dt > 0 ? (uint64_t)dt : 0);
  if (st->duration > 0 && p > st->duration) {
    p = st->duration;
  }
  return (uint32_t)p;
}

static void freeze_position(np_state_t *st) {
  st->position = current_position(st);
  st->position_at_us = esp_timer_get_time();
}

static void on_rtsp_event(playback_source_t source, playback_event_t event,
                          const playback_event_data_t *data,
                          void *user_data) {
  (void)user_data;
  s_last_source = source;
  if (!s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  switch (event) {
  case PLAYBACK_EVENT_CONNECTED:
    s_np.connected = true;
    break;
  case PLAYBACK_EVENT_PLAYING:
    if (s_np.state != NP_STATE_PLAYING) {
      s_np.position_at_us = esp_timer_get_time();
    }
    s_np.connected = true;
    s_np.state = NP_STATE_PLAYING;
    break;
  case PLAYBACK_EVENT_PAUSED:
    freeze_position(&s_np);
    s_np.state = NP_STATE_PAUSED;
    break;
  case PLAYBACK_EVENT_DISCONNECTED:
    memset(&s_np, 0, sizeof(s_np));
    break;
  case PLAYBACK_EVENT_METADATA:
    if (data) {
      const playback_metadata_t *m = &data->metadata;
      // A text set (DMAP or bplist) replaces the whole text block; a
      // progress-only or artwork-only update leaves it alone.
      if (m->title[0] || m->artist[0] || m->album[0]) {
        bool new_track = strcmp(m->title, s_np.title) != 0;
        strlcpy(s_np.title, m->title, sizeof(s_np.title));
        strlcpy(s_np.artist, m->artist, sizeof(s_np.artist));
        strlcpy(s_np.album, m->album, sizeof(s_np.album));
        strlcpy(s_np.genre, m->genre, sizeof(s_np.genre));
        if (new_track && m->duration_secs == 0) {
          s_np.position = 0;
          s_np.position_at_us = esp_timer_get_time();
        }
      }
      if (m->duration_secs > 0) {
        s_np.duration = m->duration_secs;
        s_np.position = m->position_secs;
        s_np.position_at_us = esp_timer_get_time();
      }
    }
    break;
  }
  xSemaphoreGive(s_lock);
  s_dirty = true;
}

static const char *state_str(np_play_state_t s) {
  switch (s) {
  case NP_STATE_PLAYING:
    return "playing";
  case NP_STATE_PAUSED:
    return "paused";
  default:
    return "idle";
  }
}

// Build the JSON state document. Caller frees the string.
static char *build_state_json(void) {
  np_state_t st;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  st = s_np;
  xSemaphoreGive(s_lock);

  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "state", state_str(st.state));
  static const char *const SRC[] = {"none", "airplay", "bluetooth", "usb",
                                     "sendspin"};
  playback_source_t ls = s_last_source;
  cJSON_AddStringToObject(j, "source",
                          (unsigned)ls < sizeof(SRC) / sizeof(SRC[0])
                              ? SRC[ls]
                              : "other");
  cJSON_AddBoolToObject(j, "connected", st.connected);
  cJSON_AddStringToObject(j, "title", st.title);
  cJSON_AddStringToObject(j, "artist", st.artist);
  cJSON_AddStringToObject(j, "album", st.album);
  cJSON_AddStringToObject(j, "genre", st.genre);
  cJSON_AddNumberToObject(j, "duration", st.duration);
  uint32_t pos = current_position(&st);
  cJSON_AddNumberToObject(j, "position", pos);
  // position_ts / position_updated_at: the wall-clock moment at which the
  // published `position` was true (the anchor while paused; while playing,
  // the anchor plus the whole seconds counted since). Pairs with `position`
  // the way HA's media_position_updated_at expects. Null until SNTP synced.
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (tv.tv_sec > 1700000000) { // wall clock valid (SNTP)
    int64_t now_us = esp_timer_get_time();
    int64_t at_us = st.position_at_us ? st.position_at_us : now_us;
    if (st.state == NP_STATE_PLAYING && st.position_at_us && pos >= st.position) {
      at_us += (int64_t)(pos - st.position) * 1000000LL;
      if (at_us > now_us) {
        at_us = now_us; // clamped at duration
      }
    }
    double wall = (double)tv.tv_sec + tv.tv_usec / 1e6 - (now_us - at_us) / 1e6;
    cJSON_AddNumberToObject(j, "position_ts", (double)(int64_t)(wall * 1000) / 1000.0);
    time_t ws = (time_t)wall;
    struct tm tmv;
    gmtime_r(&ws, &tmv);
    char iso[24];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    cJSON_AddStringToObject(j, "position_updated_at", iso);
  } else {
    cJSON_AddNullToObject(j, "position_ts");
    cJSON_AddNullToObject(j, "position_updated_at");
  }
  int vol = playback_control_get_volume_percent();
  cJSON_AddNumberToObject(j, "volume", vol);
  cJSON_AddNumberToObject(j, "volume_level", vol / 100.0);
  cJSON_AddBoolToObject(j, "muted", playback_control_is_muted());
  cJSON_AddBoolToObject(j, "dacp", dacp_is_active());
  // Diagnostic: did the sender send DACP-ID/Active-Remote at all.
  cJSON_AddBoolToObject(j, "dacp_id_seen", dacp_has_id());
  char *out = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  return out;
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------

static void topic(char *out, size_t len, const char *suffix) {
  snprintf(out, len, "%s/%s", s_cfg.base, suffix);
}

static void publish(const char *suffix, const char *payload, bool retain) {
  if (!s_client || !s_mqtt_connected) {
    return;
  }
  char t[NP_TOPIC_MAX];
  topic(t, sizeof(t), suffix);
  esp_mqtt_client_publish(s_client, t, payload, 0, 1, retain ? 1 : 0);
}

static cJSON *device_block(void) {
  char name[65] = "AirPlay ESP32";
  settings_get_device_name(name, sizeof(name));
  const esp_app_desc_t *app = esp_app_get_description();
  cJSON *d = cJSON_CreateObject();
  cJSON *ids = cJSON_CreateArray();
  cJSON_AddItemToArray(ids, cJSON_CreateString(s_dev_id));
  cJSON_AddItemToObject(d, "identifiers", ids);
  cJSON_AddStringToObject(d, "name", name);
  cJSON_AddStringToObject(d, "manufacturer", "airplay-esp32 (custom MQTT)");
  cJSON_AddStringToObject(d, "model", "ESP32-S3 AirPlay 2 receiver");
  cJSON_AddStringToObject(d, "sw_version", app->version);
  char ip[20] = {0};
  if (wifi_get_ip_str(ip, sizeof(ip)) == ESP_OK && ip[0]) {
    char url[40];
    snprintf(url, sizeof(url), "http://%s/", ip);
    cJSON_AddStringToObject(d, "configuration_url", url);
  }
  return d;
}

// Publish one discovery config. `extra` is consumed.
static void discovery(const char *component, const char *object_id,
                      const char *name, cJSON *extra) {
  char t[NP_TOPIC_MAX];
  snprintf(t, sizeof(t), "%s/%s/%s/%s/config", s_cfg.prefix, component,
           s_dev_id, object_id);
  cJSON *c = extra ? extra : cJSON_CreateObject();
  char uid[64];
  snprintf(uid, sizeof(uid), "%s_%s", s_dev_id, object_id);
  cJSON_AddStringToObject(c, "unique_id", uid);
  cJSON_AddStringToObject(c, "object_id", uid);
  cJSON_AddStringToObject(c, "name", name);
  char avail[NP_TOPIC_MAX];
  topic(avail, sizeof(avail), "status");
  cJSON_AddStringToObject(c, "availability_topic", avail);
  cJSON_AddItemToObject(c, "device", device_block());
  char *p = cJSON_PrintUnformatted(c);
  cJSON_Delete(c);
  if (p) {
    esp_mqtt_client_publish(s_client, t, p, 0, 1, 1);
    free(p);
  }
}

static cJSON *state_sensor(const char *tmpl, const char *icon) {
  char st[NP_TOPIC_MAX];
  topic(st, sizeof(st), "state");
  cJSON *c = cJSON_CreateObject();
  cJSON_AddStringToObject(c, "state_topic", st);
  cJSON_AddStringToObject(c, "value_template", tmpl);
  if (icon) {
    cJSON_AddStringToObject(c, "icon", icon);
  }
  return c;
}

static cJSON *button(const char *payload, const char *icon) {
  char cmd[NP_TOPIC_MAX];
  topic(cmd, sizeof(cmd), "cmd");
  cJSON *c = cJSON_CreateObject();
  cJSON_AddStringToObject(c, "command_topic", cmd);
  cJSON_AddStringToObject(c, "payload_press", payload);
  cJSON_AddStringToObject(c, "icon", icon);
  return c;
}

static void publish_discovery(void) {
  cJSON *c;

  c = state_sensor("{{ value_json.state }}", "mdi:play-pause");
  {
    char st[NP_TOPIC_MAX];
    topic(st, sizeof(st), "state");
    cJSON_AddStringToObject(c, "json_attributes_topic", st);
  }
  discovery("sensor", "state", "Playback state", c);
  discovery("sensor", "title", "Title",
            state_sensor("{{ value_json.title }}", "mdi:music-note"));
  discovery("sensor", "artist", "Artist",
            state_sensor("{{ value_json.artist }}", "mdi:account-music"));
  discovery("sensor", "album", "Album",
            state_sensor("{{ value_json.album }}", "mdi:album"));

  c = state_sensor("{{ value_json.position }}", "mdi:timer-outline");
  cJSON_AddStringToObject(c, "unit_of_measurement", "s");
  cJSON_AddStringToObject(c, "device_class", "duration");
  discovery("sensor", "position", "Position", c);

  c = state_sensor("{{ value_json.duration }}", "mdi:timer");
  cJSON_AddStringToObject(c, "unit_of_measurement", "s");
  cJSON_AddStringToObject(c, "device_class", "duration");
  discovery("sensor", "duration", "Duration", c);

  // Volume as a number entity: state from JSON, set via volume/set (0..100).
  {
    char st[NP_TOPIC_MAX], cmd[NP_TOPIC_MAX];
    topic(st, sizeof(st), "state");
    topic(cmd, sizeof(cmd), "volume/set");
    c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "state_topic", st);
    cJSON_AddStringToObject(c, "value_template", "{{ value_json.volume }}");
    cJSON_AddStringToObject(c, "command_topic", cmd);
    cJSON_AddNumberToObject(c, "min", 0);
    cJSON_AddNumberToObject(c, "max", 100);
    cJSON_AddNumberToObject(c, "step", 1);
    cJSON_AddStringToObject(c, "mode", "slider");
    cJSON_AddStringToObject(c, "unit_of_measurement", "%");
    cJSON_AddStringToObject(c, "icon", "mdi:volume-high");
    discovery("number", "volume", "Volume", c);
  }

  discovery("button", "play_pause", "Play/Pause",
            button("play_pause", "mdi:play-pause"));
  discovery("button", "next", "Next", button("next", "mdi:skip-next"));
  discovery("button", "prev", "Previous",
            button("prev", "mdi:skip-previous"));
}

static void handle_command(const char *t, int tlen, const char *d, int dlen) {
  char suffix[24] = {0};
  size_t blen = strlen(s_cfg.base);
  if (tlen <= (int)blen + 1 || strncmp(t, s_cfg.base, blen) != 0) {
    return;
  }
  int slen = tlen - (int)blen - 1;
  if (slen >= (int)sizeof(suffix)) {
    return;
  }
  memcpy(suffix, t + blen + 1, slen);
  char payload[24] = {0};
  if (dlen >= (int)sizeof(payload)) {
    dlen = sizeof(payload) - 1;
  }
  memcpy(payload, d, dlen);

  if (strcmp(suffix, "cmd") == 0) {
    ESP_LOGI(TAG, "command: %s", payload);
    if (strcmp(payload, "play_pause") == 0 || strcmp(payload, "play") == 0 ||
        strcmp(payload, "pause") == 0) {
      // DACP only exposes a toggle; play/pause act as toggle unless we are
      // already in the requested state.
      np_play_state_t cur = s_np.state;
      if ((strcmp(payload, "play") == 0 && cur == NP_STATE_PLAYING) ||
          (strcmp(payload, "pause") == 0 && cur != NP_STATE_PLAYING)) {
        return;
      }
      playback_control_play_pause();
    } else if (strcmp(payload, "next") == 0) {
      playback_control_next();
    } else if (strcmp(payload, "prev") == 0) {
      playback_control_prev();
    } else if (strcmp(payload, "volume_up") == 0) {
      playback_control_volume_up();
    } else if (strcmp(payload, "volume_down") == 0) {
      playback_control_volume_down();
    }
  } else if (strcmp(suffix, "volume/set") == 0) {
    float v = strtof(payload, NULL);
    if (v < 0) {
      v = 0;
    } else if (v > 100) {
      v = 100;
    }
    ESP_LOGI(TAG, "volume set %.0f%%", v);
    if (dacp_is_active()) {
      // The sender owns the level and echoes it back; don't fight it.
      ESP_LOGI(TAG, "volume set via DACP");
      dacp_send_volume(v);
    } else {
      // No DACP (idle box, or a grouped stream without it): apply locally,
      // as the buttons do - settings level (DAC) plus the AirPlay session
      // level that scales samples on this PCM5102. Not persisted per step;
      // playback_control commits it at pause/session end.
      ESP_LOGI(TAG, "volume set locally (no DACP)");
      playback_control_set_volume_db(-30.0f + (v / 100.0f) * 30.0f);
    }
    s_dirty = true;
  }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *event_data) {
  (void)arg;
  (void)base;
  esp_mqtt_event_handle_t ev = event_data;
  switch ((esp_mqtt_event_id_t)id) {
  case MQTT_EVENT_CONNECTED: {
    ESP_LOGI(TAG, "connected to %s:%u", s_cfg.host, s_cfg.port);
    s_mqtt_connected = true;
    char t[NP_TOPIC_MAX];
    topic(t, sizeof(t), "cmd");
    esp_mqtt_client_subscribe(s_client, t, 0);
    topic(t, sizeof(t), "volume/set");
    esp_mqtt_client_subscribe(s_client, t, 0);
    publish("status", "online", true);
    publish_discovery();
    s_dirty = true;
    break;
  }
  case MQTT_EVENT_DISCONNECTED:
    s_mqtt_connected = false;
    break;
  case MQTT_EVENT_DATA:
    if (ev->current_data_offset == 0 && ev->total_data_len == ev->data_len) {
      handle_command(ev->topic, ev->topic_len, ev->data, ev->data_len);
    }
    break;
  default:
    break;
  }
}

static void client_stop(void) {
  if (s_client) {
    if (s_mqtt_connected) {
      publish("status", "offline", true);
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_mqtt_connected = false;
  }
}

static void client_start(void) {
  client_stop();
  if (!s_cfg.enabled || !s_cfg.host[0]) {
    return;
  }
  static char lwt_topic[NP_TOPIC_MAX];
  topic(lwt_topic, sizeof(lwt_topic), "status");
  esp_mqtt_client_config_t mc = {
      .broker.address.hostname = s_cfg.host,
      .broker.address.port = s_cfg.port,
      .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
      .credentials.client_id = s_dev_id,
      .credentials.username = s_cfg.user[0] ? s_cfg.user : NULL,
      .credentials.authentication.password =
          s_cfg.pass[0] ? s_cfg.pass : NULL,
      .session.last_will.topic = lwt_topic,
      .session.last_will.msg = "offline",
      .session.last_will.qos = 1,
      .session.last_will.retain = 1,
      .session.keepalive = 30,
      .network.reconnect_timeout_ms = 10000,
      .buffer.size = 1024,
      .buffer.out_size = 1536,
      .task.stack_size = 4096,
      .task.priority = 3,
  };
  s_client = esp_mqtt_client_init(&mc);
  if (!s_client) {
    ESP_LOGE(TAG, "client init failed");
    return;
  }
  esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, NULL);
  esp_mqtt_client_start(s_client);
}

static void worker_task(void *arg) {
  (void)arg;
  int last_vol = -1;
  int64_t last_pub = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (s_restart) {
      s_restart = false;
      client_start();
    }
    if (!s_mqtt_connected) {
      continue;
    }
    int vol = playback_control_get_volume_percent();
    bool playing = s_np.state == NP_STATE_PLAYING;
    int64_t now = esp_timer_get_time();
    bool periodic = playing && (now - last_pub) > NP_PROGRESS_SECS * 1000000LL;
    if (!s_dirty && vol == last_vol && !periodic) {
      continue;
    }
    s_dirty = false;
    last_vol = vol;
    last_pub = now;
    char *js = build_state_json();
    if (js) {
      publish("state", js, true);
      free(js);
    }
  }
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, cJSON *j) {
  char *s = cJSON_PrintUnformatted(j);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, s ? s : "{}", HTTPD_RESP_USE_STRLEN);
  free(s);
  cJSON_Delete(j);
  return ESP_OK;
}

static esp_err_t mqtt_get_handler(httpd_req_t *req) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "enabled", s_cfg.enabled);
  cJSON_AddStringToObject(j, "host", s_cfg.host);
  cJSON_AddNumberToObject(j, "port", s_cfg.port);
  cJSON_AddStringToObject(j, "username", s_cfg.user);
  cJSON_AddBoolToObject(j, "has_password", s_cfg.pass[0] != 0);
  cJSON_AddStringToObject(j, "base_topic", s_cfg.base);
  cJSON_AddStringToObject(j, "discovery_prefix", s_cfg.prefix);
  cJSON_AddBoolToObject(j, "connected", s_mqtt_connected);
  return send_json(req, j);
}

static void copy_json_str(cJSON *j, const char *key, char *out, size_t len) {
  cJSON *v = cJSON_GetObjectItem(j, key);
  if (v && cJSON_IsString(v)) {
    strlcpy(out, v->valuestring, len);
  }
}

// POST /api/mqtt  {"enabled":true,"host":"mqtt.example.lan","port":1883,
//                  "username":"...","password":"...","base_topic":"...",
//                  "discovery_prefix":"homeassistant"}
// Omitted keys keep their value. "password" is write-only.
static esp_err_t mqtt_post_handler(httpd_req_t *req) {
  char body[512];
  if (req->content_len >= sizeof(body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large");
    return ESP_FAIL;
  }
  int got = 0;
  while (got < (int)req->content_len) {
    int r = httpd_req_recv(req, body + got, req->content_len - got);
    if (r <= 0) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    got += r;
  }
  body[got] = '\0';
  cJSON *j = cJSON_Parse(body);
  if (!j) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  np_config_t c = s_cfg;
  cJSON *v = cJSON_GetObjectItem(j, "enabled");
  if (v && cJSON_IsBool(v)) {
    c.enabled = cJSON_IsTrue(v);
  }
  v = cJSON_GetObjectItem(j, "port");
  if (v && cJSON_IsNumber(v) && v->valueint > 0 && v->valueint < 65536) {
    c.port = (uint16_t)v->valueint;
  }
  copy_json_str(j, "host", c.host, sizeof(c.host));
  copy_json_str(j, "username", c.user, sizeof(c.user));
  copy_json_str(j, "password", c.pass, sizeof(c.pass));
  copy_json_str(j, "base_topic", c.base, sizeof(c.base));
  copy_json_str(j, "discovery_prefix", c.prefix, sizeof(c.prefix));
  cJSON_Delete(j);
  if (!c.base[0] || !c.prefix[0]) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty topic");
    return ESP_FAIL;
  }

  esp_err_t err = cfg_save(&c);
  cJSON *r = cJSON_CreateObject();
  if (err == ESP_OK) {
    s_cfg = c;
    s_restart = true; // worker task reconnects with the new settings
    cJSON_AddBoolToObject(r, "success", true);
  } else {
    cJSON_AddBoolToObject(r, "success", false);
    cJSON_AddStringToObject(r, "error", esp_err_to_name(err));
  }
  return send_json(req, r);
}

static esp_err_t nowplaying_handler(httpd_req_t *req) {
  char *js = build_state_json();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, js ? js : "{}", HTTPD_RESP_USE_STRLEN);
  free(js);
  return ESP_OK;
}

// Minimal built-in settings page (the SPIFFS web UI has no MQTT form).
// The password field is write-only: left blank, the stored one is kept.
static const char MQTT_PAGE[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MQTT</title><style>body{font-family:sans-serif;max-width:420px;"
    "margin:20px auto;padding:0 12px}label{display:block;margin:10px 0 4px}"
    "input[type=text],input[type=password],input[type=number]{width:100%;"
    "padding:6px;box-sizing:border-box}button{margin-top:14px;padding:8px "
    "16px}</style></head><body><h2>MQTT now-playing</h2>"
    "<label><input type=checkbox id=enabled> Enabled</label>"
    "<label>Broker host</label><input type=text id=host>"
    "<label>Port</label><input type=number id=port>"
    "<label>Username</label><input type=text id=username>"
    "<label>Password <small id=pw></small></label>"
    "<input type=password id=password placeholder='(unchanged)'>"
    "<label>Base topic</label><input type=text id=base_topic>"
    "<label>Discovery prefix</label><input type=text id=discovery_prefix>"
    "<button onclick='save()'>Save</button> <span id=msg></span>"
    "<p><a href='/api/nowplaying'>/api/nowplaying</a></p><script>"
    "const F=['host','port','username','base_topic','discovery_prefix'];"
    "fetch('/api/mqtt').then(r=>r.json()).then(c=>{F.forEach(k=>"
    "document.getElementById(k).value=c[k]);enabled.checked=c.enabled;"
    "pw.textContent=c.has_password?'(set)':'(none)';"
    "msg.textContent=c.connected?'connected':'not connected'});"
    "function save(){const b={enabled:enabled.checked};"
    "F.forEach(k=>b[k]=document.getElementById(k).value);b.port=+b.port;"
    "if(password.value)b.password=password.value;"
    "fetch('/api/mqtt',{method:'POST',headers:{'Content-Type':"
    "'application/json'},body:JSON.stringify(b)}).then(r=>r.json()).then(r=>"
    "{msg.textContent=r.success?'saved':'error: '+r.error;password.value=''})}"
    "</script></body></html>";

static esp_err_t mqtt_page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, MQTT_PAGE, HTTPD_RESP_USE_STRLEN);
}

void mqtt_nowplaying_register_http(httpd_handle_t server) {
  if (!server) {
    return;
  }
  httpd_uri_t u1 = {
      .uri = "/api/mqtt", .method = HTTP_GET, .handler = mqtt_get_handler};
  httpd_uri_t u2 = {
      .uri = "/api/mqtt", .method = HTTP_POST, .handler = mqtt_post_handler};
  httpd_uri_t u3 = {.uri = "/api/nowplaying",
                    .method = HTTP_GET,
                    .handler = nowplaying_handler};
  httpd_register_uri_handler(server, &u1);
  httpd_register_uri_handler(server, &u2);
  httpd_register_uri_handler(server, &u3);
  httpd_uri_t u5 = {
      .uri = "/mqtt", .method = HTTP_GET, .handler = mqtt_page_handler};
  httpd_register_uri_handler(server, &u5);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

esp_err_t mqtt_nowplaying_init(void) {
  if (s_lock) {
    return ESP_OK;
  }
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) {
    return ESP_ERR_NO_MEM;
  }
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_dev_id, sizeof(s_dev_id), "airplay_esp32_%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  cfg_load(&s_cfg);
  playback_events_register(on_rtsp_event, NULL);
  ESP_LOGI(TAG, "init: mqtt %s, host '%s', base '%s'",
           s_cfg.enabled ? "on" : "off", s_cfg.host, s_cfg.base);
  return ESP_OK;
}

// SNTP wall clock: only gettimeofday() users see it. AirPlay's PTP
// and NTP clocks run on esp_timer and are not affected.
static void sntp_start_once(void) {
  static bool started;
  if (started) {
    return;
  }
  started = true;
  setenv("TZ", "UTC0", 1);
  tzset();
  esp_sntp_config_t cfg =
      ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  // Slot 0 takes the NTP server from DHCP when the router offers one; slot 1
  // stays pool.ntp.org.
  cfg.server_from_dhcp = true;
  cfg.renew_servers_after_new_IP = true;
  cfg.index_of_first_server = 1;
  cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
  cfg.wait_for_sync = false;
  esp_err_t err = esp_netif_sntp_init(&cfg);
  ESP_LOGI(TAG, "SNTP start: %s", esp_err_to_name(err));
}

void mqtt_nowplaying_start(void) {
  if (!s_lock) {
    return;
  }
  sntp_start_once();
  if (!s_task) {
    task_create_spiram(worker_task, "mqtt_np", 4096, NULL, 2, &s_task, NULL);
  }
  if (!s_client) {
    s_restart = true;
  }
}
