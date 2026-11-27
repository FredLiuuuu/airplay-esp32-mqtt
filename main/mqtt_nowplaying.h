// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * MQTT now-playing publisher (airplay-esp32-mqtt addition).
 *
 * Publishes AirPlay now-playing state (title/artist/album/genre, play state,
 * volume, progress) to an MQTT broker, with Home Assistant MQTT
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

/** Register /api/mqtt (GET/POST), /api/nowplaying, /mqtt. */
void mqtt_nowplaying_register_http(httpd_handle_t server);

/** Number of URI handlers mqtt_nowplaying_register_http() adds. */
#define MQTT_NOWPLAYING_URI_HANDLERS 4
