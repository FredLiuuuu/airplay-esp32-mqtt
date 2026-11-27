// SPDX-License-Identifier: GPL-3.0-or-later
// airplay-esp32-mqtt: AirPlay 2 event channel (receiver -> sender remote control).
//
// The sender connects to the eventPort given in SETUP. On that connection the
// receiver sends RTSP-style requests ("POST /command", bplist body) and the
// sender answers "RTSP/1.0 200 OK". Each direction is ChaCha20-Poly1305 framed
// like the control channel (2-byte LE length as AAD, ciphertext, 16-byte tag,
// 64-bit LE counter nonce), keyed from the pair-setup/verify secret with
// HKDF-SHA512 "Events-Salt" / "Events-{Write,Read}-Encryption-Key".
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MediaRemote command numbers */
enum {
  EVENT_CMD_PLAY = 0,
  EVENT_CMD_PAUSE = 1,
  EVENT_CMD_TOGGLE = 2,
  EVENT_CMD_STOP = 3,
  EVENT_CMD_NEXT = 4,
  EVENT_CMD_PREV = 5,
};

void event_ctrl_init(void);

/** From SETUP: keys of the session that owns the next event connection, and
 * the groupUUID (may be NULL). */
void event_ctrl_set_session(const uint8_t write_key[32],
                            const uint8_t read_key[32], const char *group_uuid);

/** Event port task: a sender connected / is about to be closed. */
void event_ctrl_attach(int fd);
void event_ctrl_detach(void);

/** Event port task: fd is readable. Reads and handles one frame. Returns
 * false when the connection is finished (EOF, error, bad frame). */
bool event_ctrl_on_readable(int fd);

/** True while an event client is attached with keys. */
bool event_ctrl_available(void);

/** Queue a command (non-blocking). experiment = try every message form and
 * log each status; otherwise use the form that last worked. */
bool event_ctrl_queue(int cmd, bool experiment);

/** Test only: send one command in one message form (0..2). */
bool event_ctrl_queue_form(int cmd, int form);
