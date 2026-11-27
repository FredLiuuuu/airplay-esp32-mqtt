// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * MQTT now-playing publisher (airplay-esp32-mqtt addition).
 *
 * Publishes AirPlay now-playing state (title/artist/album/genre, play state,
 * volume, progress, artwork URL) to an MQTT broker, with Home Assistant MQTT
 * discovery, and accepts play/pause/next/prev/volume commands that are
 * forwarded to the sender through the existing DACP client.
 *
 * Broker host/port/user/password live in NVS (namespace "mqtt_np") and are
 * set at runtime through GET/POST /api/mqtt.  Nothing is compiled in.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>

/** Load config from NVS and register RTSP event listeners. Call once, early. */
esp_err_t mqtt_nowplaying_init(void);

/** Start (or restart) the MQTT client if enabled. Safe to call repeatedly. */
void mqtt_nowplaying_start(void);

/** Register /api/mqtt (GET/POST), /api/nowplaying, /api/nowplaying/artwork, /mqtt. */
void mqtt_nowplaying_register_http(httpd_handle_t server);

/** Number of URI handlers mqtt_nowplaying_register_http() adds. */
#define MQTT_NOWPLAYING_URI_HANDLERS 5

/**
 * Cover-art reception: true when CONFIG_ENABLE_AIRPLAY_ARTWORK is set or the
 * MQTT "artwork" setting is on (and MQTT is enabled).  Read at boot; changing
 * it via /api/mqtt takes effect after a restart (mDNS "md" record).
 */
bool mqtt_nowplaying_artwork_enabled(void);

/**
 * True when MQTT artwork is on: also advertise AirPlay feature bit 15
 * (artwork metadata), which AirPlay 2 senders need before they send cover art.
 */
bool mqtt_nowplaying_artwork_advertise(void);

/** Store a cover-art image received over RTSP SET_PARAMETER (copied). */
void mqtt_nowplaying_set_artwork(const void *data, size_t len,
                                 const char *content_type);
