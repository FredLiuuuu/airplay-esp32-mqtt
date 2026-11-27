// SPDX-License-Identifier: GPL-3.0-or-later
// airplay-esp32-mqtt: AirPlay 2 event channel remote control. See event_ctrl.h.
//
// Message shape follows shairport-sync 5.4-dev (development branch,
// commit 6eb119a, 2026-11-25):
//   remote/remote.c  ap2_event_send_simple_modern_media_remote_command()
//                    paramsPlist(), prepareNSKeyedArchiver()
//   ap2_event_receiver.c  ap2_event_port_post_command_stream() (headers),
//                    ap2_event_port_send_message() (reads one reply per send)
//   pair_ap/pair_homekit.c  pair_keys_map[] "Events-Salt" entries, and
//                    cipher_new() channel 4 (server: write = Events-Write,
//                    read = Events-Read)

#include "event_ctrl.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "plist.h"
#include "sodium.h"

static const char *TAG = "event_ctrl";

#define EV_BLOCK_MAX 0x400
#define EV_RX_MAX 2048
#define EV_REPLY_MS 2000
#define EV_FORMS 3
#define EV_HDR_ROOM 160

static SemaphoreHandle_t s_mtx; /* guards fd, keys, tx nonce, swap */
static QueueHandle_t s_reply_q; /* int status codes */
static QueueHandle_t s_cmd_q;
static int s_fd = -1;
static uint8_t s_wkey[32], s_rkey[32];
static bool s_keys;
static uint64_t s_tx_nonce, s_rx_nonce;
static bool s_rx_checked; /* first frame decrypted, direction known */
static char s_gid[40];
static uint8_t *s_rx;     /* plaintext reassembly */
static size_t s_rx_len;
static volatile int s_good_form = -1; /* form whose last reply was 2xx */

/* pending session keys from SETUP, applied at the next attach */
static uint8_t s_pw[32], s_pr[32];
static bool s_pkeys;
static char s_pgid[40];

typedef struct {
  int8_t cmd;
  int8_t form; /* -1 auto */
  bool experiment;
} ev_req_t;

/* ------------------------------------------------------------------ */
/* Tiny binary plist writer                                           */
/* ------------------------------------------------------------------ */

#define BP_MAX_OBJ 64
#define BP_MAX_CHILD 8
typedef enum { BP_DICT, BP_ARRAY, BP_STR, BP_INT, BP_BOOL, BP_DATA } bp_type_t;
typedef struct {
  bp_type_t t;
  uint8_t n;
  uint8_t k[BP_MAX_CHILD]; /* dict keys */
  uint8_t v[BP_MAX_CHILD]; /* dict values / array items */
  const void *p;
  size_t len;
  uint64_t i;
} bp_obj_t;
typedef struct {
  bp_obj_t o[BP_MAX_OBJ];
  int n;
  bool err; /* an object or child did not fit: bp_write() fails */
} bp_t;

static int bp_new(bp_t *b, bp_type_t t) {
  if (b->n >= BP_MAX_OBJ) {
    b->err = true;
    return -1;
  }
  memset(&b->o[b->n], 0, sizeof(bp_obj_t));
  b->o[b->n].t = t;
  return b->n++;
}
static int bp_str(bp_t *b, const char *s) {
  int i = bp_new(b, BP_STR);
  if (i >= 0) {
    b->o[i].p = s;
    b->o[i].len = strlen(s);
  }
  return i;
}
static int bp_int(bp_t *b, uint64_t v) {
  int i = bp_new(b, BP_INT);
  if (i >= 0) {
    b->o[i].i = v;
  }
  return i;
}
static int bp_bool(bp_t *b, bool v) {
  int i = bp_new(b, BP_BOOL);
  if (i >= 0) {
    b->o[i].i = v;
  }
  return i;
}
static int bp_data(bp_t *b, const void *p, size_t len) {
  int i = bp_new(b, BP_DATA);
  if (i >= 0) {
    b->o[i].p = p;
    b->o[i].len = len;
  }
  return i;
}
static bool bp_set(bp_t *b, int dict, const char *key, int val) {
  if (dict < 0 || val < 0) {
    b->err = true;
    return false;
  }
  bp_obj_t *d = &b->o[dict];
  int k = bp_str(b, key);
  if (k < 0 || d->n >= BP_MAX_CHILD) {
    b->err = true;
    return false;
  }
  d->k[d->n] = (uint8_t)k;
  d->v[d->n++] = (uint8_t)val;
  return true;
}
static bool bp_push(bp_t *b, int arr, int val) {
  if (arr < 0 || val < 0 || b->o[arr].n >= BP_MAX_CHILD) {
    b->err = true;
    return false;
  }
  b->o[arr].v[b->o[arr].n++] = (uint8_t)val;
  return true;
}

typedef struct {
  uint8_t *p;
  size_t cap, len;
  bool err;
} wbuf_t;
static void wb(wbuf_t *w, const void *d, size_t n) {
  if (w->len + n > w->cap) {
    w->err = true;
    return;
  }
  memcpy(w->p + w->len, d, n);
  w->len += n;
}
static void wb1(wbuf_t *w, uint8_t c) { wb(w, &c, 1); }
static void wb_marker(wbuf_t *w, uint8_t hi, size_t n) {
  if (n < 15) {
    wb1(w, (uint8_t)(hi | n));
  } else {
    wb1(w, (uint8_t)(hi | 0x0f));
    if (n < 256) {
      wb1(w, 0x10);
      wb1(w, (uint8_t)n);
    } else {
      wb1(w, 0x11);
      wb1(w, (uint8_t)(n >> 8));
      wb1(w, (uint8_t)n);
    }
  }
}

/* Objects in index order; refs are one byte, offsets two bytes. Root = 0. */
static size_t bp_write(const bp_t *b, uint8_t *out, size_t cap) {
  wbuf_t w = {out, cap, 0, b->err};
  uint16_t off[BP_MAX_OBJ];
  wb(&w, "bplist00", 8);
  for (int i = 0; i < b->n; i++) {
    const bp_obj_t *o = &b->o[i];
    off[i] = (uint16_t)w.len;
    switch (o->t) {
    case BP_BOOL:
      wb1(&w, o->i ? 0x09 : 0x08);
      break;
    case BP_INT:
      if (o->i < 0x100) {
        wb1(&w, 0x10);
        wb1(&w, (uint8_t)o->i);
      } else if (o->i < 0x10000) {
        wb1(&w, 0x11);
        wb1(&w, (uint8_t)(o->i >> 8));
        wb1(&w, (uint8_t)o->i);
      } else {
        wb1(&w, 0x13);
        for (int s = 56; s >= 0; s -= 8) {
          wb1(&w, (uint8_t)(o->i >> s));
        }
      }
      break;
    case BP_STR:
      wb_marker(&w, 0x50, o->len);
      wb(&w, o->p, o->len);
      break;
    case BP_DATA:
      wb_marker(&w, 0x40, o->len);
      wb(&w, o->p, o->len);
      break;
    case BP_ARRAY:
      wb_marker(&w, 0xa0, o->n);
      wb(&w, o->v, o->n);
      break;
    case BP_DICT:
      wb_marker(&w, 0xd0, o->n);
      wb(&w, o->k, o->n);
      wb(&w, o->v, o->n);
      break;
    }
  }
  size_t table = w.len;
  for (int i = 0; i < b->n; i++) {
    wb1(&w, (uint8_t)(off[i] >> 8));
    wb1(&w, (uint8_t)off[i]);
  }
  uint8_t tr[32] = {0};
  tr[6] = 2; /* offset int size */
  tr[7] = 1; /* object ref size */
  tr[15] = (uint8_t)b->n;
  tr[23] = 0; /* top object */
  tr[30] = (uint8_t)(table >> 8);
  tr[31] = (uint8_t)table;
  wb(&w, tr, sizeof(tr));
  return (w.err || table > 0xffff) ? 0 : w.len;
}

static void random_uuid(char out[37]) {
  uint8_t r[16];
  esp_fill_random(r, sizeof(r));
  r[6] = (r[6] & 0x0f) | 0x40;
  r[8] = (r[8] & 0x3f) | 0x80;
  snprintf(out, 37,
           "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
           r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10],
           r[11], r[12], r[13], r[14], r[15]);
}

/* NSKeyedArchiver of an NSMutableArray holding one string, laid out as
 * shairport-sync's prepareNSKeyedArchiver() does (CF$UID as integers). */
static size_t build_dest_archive(const char *uid, uint8_t *out, size_t cap) {
  bp_t *b = calloc(1, sizeof(bp_t));
  if (!b) {
    return 0;
  }
  int root = bp_new(b, BP_DICT);
  bp_set(b, root, "$version", bp_int(b, 100000));
  bp_set(b, root, "$archiver", bp_str(b, "NSKeyedArchiver"));
  int top = bp_new(b, BP_DICT);
  int ru = bp_new(b, BP_DICT);
  bp_set(b, ru, "CF$UID", bp_int(b, 1));
  bp_set(b, top, "root", ru);
  bp_set(b, root, "$top", top);
  int objs = bp_new(b, BP_ARRAY);
  bp_push(b, objs, bp_str(b, "$null"));
  int ma = bp_new(b, BP_DICT);
  int nso = bp_new(b, BP_ARRAY);
  int eu = bp_new(b, BP_DICT);
  bp_set(b, eu, "CF$UID", bp_int(b, 2));
  bp_push(b, nso, eu);
  bp_set(b, ma, "NS.objects", nso);
  int cu = bp_new(b, BP_DICT);
  bp_set(b, cu, "CF$UID", bp_int(b, 3));
  bp_set(b, ma, "$class", cu);
  bp_push(b, objs, ma);
  bp_push(b, objs, bp_str(b, uid));
  int cd = bp_new(b, BP_DICT);
  bp_set(b, cd, "$classname", bp_str(b, "NSMutableArray"));
  int cls = bp_new(b, BP_ARRAY);
  bp_push(b, cls, bp_str(b, "NSMutableArray"));
  bp_push(b, cls, bp_str(b, "NSArray"));
  bp_push(b, cls, bp_str(b, "NSObject"));
  bp_set(b, cd, "$classes", cls);
  bp_push(b, objs, cd);
  bp_set(b, root, "$objects", objs);
  size_t n = bp_write(b, out, cap);
  free(b);
  return n;
}

static const char *const LEGACY[] = {"play", "paus", NULL,
                                      "stop", "nitm", "pitm"};

/* form 0: shairport-sync exact (string number + params)
 * form 1: bare {type, modernMediaRemoteCommand: <int>}
 * form 2: legacy {type, value: "<four-char code>"} */
static size_t build_command(int cmd, int form, uint8_t *out, size_t cap) {
  bp_t *b = calloc(1, sizeof(bp_t));
  uint8_t *arch = NULL;
  if (!b) {
    return 0;
  }
  char num[8], uuid[37];
  size_t n = 0;
  snprintf(num, sizeof(num), "%d", cmd);
  int root = bp_new(b, BP_DICT);
  if (form == 2) {
    if (cmd < 0 || cmd > 5 || !LEGACY[cmd]) {
      goto done;
    }
    bp_set(b, root, "type", bp_str(b, "sendMediaRemoteCommand"));
    bp_set(b, root, "value", bp_str(b, LEGACY[cmd]));
  } else if (form == 1) {
    bp_set(b, root, "type", bp_str(b, "sendMediaRemoteCommand"));
    bp_set(b, root, "modernMediaRemoteCommand", bp_int(b, (uint64_t)cmd));
  } else {
    bp_set(b, root, "modernMediaRemoteCommand", bp_str(b, num));
    bp_set(b, root, "type", bp_str(b, "sendMediaRemoteCommand"));
    int params = bp_new(b, BP_DICT);
    bp_set(b, params, "kMRMediaRemoteOptionSendOptionsNumber", bp_int(b, 0));
    random_uuid(uuid);
    bp_set(b, params, "kMRMediaRemoteOptionCommandID", bp_str(b, uuid));
    bp_set(b, params, "kMRMediaRemoteOptionIsRedirectingCommand",
           bp_bool(b, true));
    if (s_gid[0]) {
      arch = malloc(512);
      size_t al = arch ? build_dest_archive(s_gid, arch, 512) : 0;
      if (al) {
        bp_set(b, params, "kMRMediaRemoteOptionDestinationDeviceUIDs",
               bp_data(b, arch, al));
      }
    }
    bp_set(b, root, "params", params);
  }
  n = bp_write(b, out, cap);
done:
  free(arch);
  free(b);
  return n;
}

/* ------------------------------------------------------------------ */
/* Framing                                                            */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const uint8_t *d, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t r = send(fd, d + sent, len - sent, 0);
    if (r <= 0) {
      return -1;
    }
    sent += (size_t)r;
  }
  return 0;
}

/* Caller holds s_mtx. */
static int write_encrypted_locked(const uint8_t *data, size_t len) {
  if (s_fd < 0 || !s_keys) {
    return -1;
  }
  uint8_t *enc = malloc(EV_BLOCK_MAX + 2 + 16);
  if (!enc) {
    return -1;
  }
  int rc = 0;
  size_t off = 0;
  while (off < len && rc == 0) {
    size_t bl = len - off > EV_BLOCK_MAX ? EV_BLOCK_MAX : len - off;
    enc[0] = (uint8_t)bl;
    enc[1] = (uint8_t)(bl >> 8);
    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, &s_tx_nonce, 8);
    unsigned long long cl = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(enc + 2, &cl, data + off, bl,
                                              enc, 2, NULL, nonce, s_wkey);
    if (send_all(s_fd, enc, 2 + (size_t)cl) != 0) {
      rc = -1;
      break;
    }
    s_tx_nonce++;
    off += bl;
  }
  free(enc);
  return rc;
}

static int recv_full(int fd, uint8_t *b, size_t n) {
  size_t got = 0;
  int idle = 0;
  while (got < n) {
    ssize_t r = recv(fd, b + got, n - got, 0);
    if (r > 0) {
      got += (size_t)r;
      idle = 0;
      continue;
    }
    if (r == 0) {
      return -1;
    }
    if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
        ++idle < 5) {
      continue; /* SO_RCVTIMEO is 1 s; give a split frame 5 s */
    }
    return -1;
  }
  return 0;
}

static bool decrypt_with(const uint8_t *key, uint64_t nonce_ctr,
                         const uint8_t *hdr, const uint8_t *ct, size_t ct_len,
                         uint8_t *out, unsigned long long *out_len) {
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &nonce_ctr, 8);
  return crypto_aead_chacha20poly1305_ietf_decrypt(out, out_len, NULL, ct,
                                                   ct_len, hdr, 2, nonce,
                                                   key) == 0;
}

/* ------------------------------------------------------------------ */
/* Incoming messages                                                  */
/* ------------------------------------------------------------------ */

static size_t content_length(const char *hdr, size_t hlen) {
  const char *p = hdr;
  while (p < hdr + hlen) {
    const char *eol = memchr(p, '\n', (size_t)(hdr + hlen - p));
    if (!eol) {
      break;
    }
    if (strncasecmp(p, "Content-Length:", 15) == 0) {
      return (size_t)strtoul(p + 15, NULL, 10);
    }
    p = eol + 1;
  }
  return 0;
}

static void log_first_line(const char *what, const char *m, size_t len) {
  char line[96];
  size_t n = 0;
  while (n < len && n < sizeof(line) - 1 && m[n] != '\r' && m[n] != '\n') {
    line[n] = m[n];
    n++;
  }
  line[n] = 0;
  ESP_LOGI(TAG, "%s: %s", what, line);
}

static void handle_message(const char *m, size_t hlen, size_t blen) {
  if (strncmp(m, "RTSP/1.", 7) == 0 && hlen > 12) {
    int code = atoi(m + 9);
    log_first_line("reply", m, hlen);
    xQueueOverwrite(s_reply_q, &code);
    return;
  }
  /* A request from the sender (e.g. POST /command updateMRSupportedCommands)
   */
  log_first_line("sender request", m, hlen);
  const uint8_t *body = (const uint8_t *)m + hlen;
  char type[48] = "";
  if (blen > 8 && memcmp(body, "bplist00", 8) == 0 &&
      bplist_find_string(body, blen, "type", type, sizeof(type))) {
    ESP_LOGI(TAG, "sender request type=%s (%u B)", type, (unsigned)blen);
  } else if (blen) {
    ESP_LOGI(TAG, "sender request body %u B", (unsigned)blen);
  }
  char cseq[16] = "";
  const char *c = strcasestr(m, "CSeq:");
  if (c && (size_t)(c - m) < hlen) {
    c += 5;
    while (*c == ' ') {
      c++;
    }
    size_t k = 0;
    while (k < sizeof(cseq) - 1 && c[k] >= '0' && c[k] <= '9') {
      cseq[k] = c[k];
      k++;
    }
    cseq[k] = 0;
  }
  char resp[96];
  int rl = snprintf(resp, sizeof(resp),
                    "RTSP/1.0 200 OK\r\n%s%s%sContent-Length: 0\r\n\r\n",
                    cseq[0] ? "CSeq: " : "", cseq, cseq[0] ? "\r\n" : "");
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  write_encrypted_locked((const uint8_t *)resp, (size_t)rl);
  xSemaphoreGive(s_mtx);
}

static void parse_rx(void) {
  for (;;) {
    char *end = NULL;
    for (size_t i = 0; i + 3 < s_rx_len; i++) {
      if (memcmp(s_rx + i, "\r\n\r\n", 4) == 0) {
        end = (char *)s_rx + i + 4;
        break;
      }
    }
    if (!end) {
      if (s_rx_len >= EV_RX_MAX) {
        ESP_LOGW(TAG, "rx header too long, dropped");
        s_rx_len = 0;
      }
      return;
    }
    size_t hlen = (size_t)(end - (char *)s_rx);
    size_t blen = content_length((char *)s_rx, hlen);
    if (hlen + blen > EV_RX_MAX) {
      ESP_LOGW(TAG, "rx message too long (%u), dropped", (unsigned)(hlen + blen));
      s_rx_len = 0;
      return;
    }
    if (s_rx_len < hlen + blen) {
      return; /* wait for the rest */
    }
    char save = 0;
    if (hlen + blen < EV_RX_MAX) {
      save = (char)s_rx[hlen + blen];
      s_rx[hlen + blen] = 0;
    }
    handle_message((char *)s_rx, hlen, blen);
    if (hlen + blen < EV_RX_MAX) {
      s_rx[hlen + blen] = (uint8_t)save;
    }
    memmove(s_rx, s_rx + hlen + blen, s_rx_len - hlen - blen);
    s_rx_len -= hlen + blen;
  }
}

bool event_ctrl_on_readable(int fd) {
  uint8_t hdr[2];
  if (recv_full(fd, hdr, 2) != 0) {
    return false;
  }
  size_t bl = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
  if (bl == 0 || bl > EV_BLOCK_MAX) {
    ESP_LOGW(TAG, "bad frame length %u", (unsigned)bl);
    return false;
  }
  uint8_t *ct = malloc(bl + 16);
  uint8_t *pt = malloc(bl);
  bool ok = false;
  if (!ct || !pt || !s_rx || recv_full(fd, ct, bl + 16) != 0) {
    goto out;
  }
  unsigned long long pl = 0;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  bool dec = s_keys &&
             decrypt_with(s_rkey, s_rx_nonce, hdr, ct, bl + 16, pt, &pl);
  if (!dec && s_keys && !s_rx_checked &&
      decrypt_with(s_wkey, s_rx_nonce, hdr, ct, bl + 16, pt, &pl)) {
    /* The sender used the other key: the two directions are the other way
     * round. Swap for the rest of this connection. */
    uint8_t t[32];
    memcpy(t, s_wkey, 32);
    memcpy(s_wkey, s_rkey, 32);
    memcpy(s_rkey, t, 32);
    sodium_memzero(t, sizeof(t));
    ESP_LOGW(TAG, "event keys were the other way round; swapped");
    dec = true;
  }
  if (dec) {
    s_rx_nonce++;
    s_rx_checked = true;
  }
  xSemaphoreGive(s_mtx);
  if (!dec) {
    ESP_LOGW(TAG, "frame of %u B did not decrypt (nonce %" PRIu64 ")",
             (unsigned)bl, s_rx_nonce);
    goto out;
  }
  if (s_rx_len + pl > EV_RX_MAX) {
    s_rx_len = 0;
  }
  memcpy(s_rx + s_rx_len, pt, (size_t)pl);
  s_rx_len += (size_t)pl;
  parse_rx();
  ok = true;
out:
  free(ct);
  free(pt);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Session state                                                      */
/* ------------------------------------------------------------------ */

void event_ctrl_set_session(const uint8_t write_key[32],
                            const uint8_t read_key[32],
                            const char *group_uuid) {
  if (!s_mtx) {
    return;
  }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  memcpy(s_pw, write_key, 32);
  memcpy(s_pr, read_key, 32);
  s_pkeys = true;
  snprintf(s_pgid, sizeof(s_pgid), "%s", group_uuid ? group_uuid : "");
  xSemaphoreGive(s_mtx);
}

void event_ctrl_attach(int fd) {
  if (!s_mtx) {
    return;
  }
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_fd = fd;
  s_keys = s_pkeys;
  memcpy(s_wkey, s_pw, 32);
  memcpy(s_rkey, s_pr, 32);
  memcpy(s_gid, s_pgid, sizeof(s_gid));
  s_tx_nonce = s_rx_nonce = 0;
  s_rx_checked = false;
  s_rx_len = 0;
  xSemaphoreGive(s_mtx);
  ESP_LOGI(TAG, "event client attached (keys %s, group %s)",
           s_keys ? "yes" : "NO", s_gid[0] ? s_gid : "-");
}

void event_ctrl_detach(void) {
  if (!s_mtx) {
    return;
  }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  bool was = s_fd >= 0;
  s_fd = -1;
  s_keys = false;
  sodium_memzero(s_wkey, sizeof(s_wkey));
  sodium_memzero(s_rkey, sizeof(s_rkey));
  xSemaphoreGive(s_mtx);
  if (was) {
    ESP_LOGI(TAG, "event client detached");
  }
}

bool event_ctrl_available(void) {
  return s_fd >= 0 && s_keys;
}

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

/* Returns the status code, -1 no reply, -2 not sent. */
static int send_one(int cmd, int form) {
  uint8_t *msg = malloc(1536);
  if (!msg) {
    return -2;
  }
  size_t pl = build_command(cmd, form, msg + EV_HDR_ROOM, 1536 - EV_HDR_ROOM);
  if (!pl) {
    free(msg);
    return -2;
  }
  char hdr[EV_HDR_ROOM];
  int hl = snprintf(hdr, sizeof(hdr),
                    "POST /command RTSP/1.0\r\nContent-Length: %u\r\n"
                    "Content-Type: application/x-apple-binary-plist\r\n\r\n",
                    (unsigned)pl);
  uint8_t *start = msg + EV_HDR_ROOM - hl;
  memcpy(start, hdr, (size_t)hl);
  xQueueReset(s_reply_q);
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  int w = write_encrypted_locked(start, (size_t)hl + pl);
  xSemaphoreGive(s_mtx);
  free(msg);
  if (w != 0) {
    return -2;
  }
  int code = -1;
  if (xQueueReceive(s_reply_q, &code, pdMS_TO_TICKS(EV_REPLY_MS)) != pdTRUE) {
    code = -1;
  }
  return code;
}

static void cmd_task(void *arg) {
  (void)arg;
  ev_req_t r;
  for (;;) {
    if (xQueueReceive(s_cmd_q, &r, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (!event_ctrl_available()) {
      ESP_LOGW(TAG, "command %d: no event client", r.cmd);
      continue;
    }
    if (r.form >= 0) {
      int st = send_one(r.cmd, r.form);
      ESP_LOGI(TAG, "command %d form %d -> status %d", r.cmd, r.form, st);
      if (st >= 200 && st < 300) {
        s_good_form = r.form;
      }
      continue;
    }
    int first = r.experiment || s_good_form < 0 ? 0 : s_good_form;
    for (int k = 0; k < EV_FORMS; k++) {
      int f = (first + k) % EV_FORMS;
      int st = send_one(r.cmd, f);
      ESP_LOGI(TAG, "command %d form %d -> status %d", r.cmd, f, st);
      if (st >= 200 && st < 300) {
        s_good_form = f;
        break;
      }
      if (st == -2 && !event_ctrl_available()) {
        break;
      }
    }
  }
}

bool event_ctrl_queue(int cmd, bool experiment) {
  if (!s_cmd_q || cmd < 0 || cmd > 5) {
    return false;
  }
  ev_req_t r = {.cmd = (int8_t)cmd, .form = -1, .experiment = experiment};
  return xQueueSend(s_cmd_q, &r, 0) == pdTRUE;
}

bool event_ctrl_queue_form(int cmd, int form) {
  if (!s_cmd_q || cmd < 0 || cmd > 5 || form < 0 || form >= EV_FORMS) {
    return false;
  }
  ev_req_t r = {.cmd = (int8_t)cmd, .form = (int8_t)form, .experiment = true};
  return xQueueSend(s_cmd_q, &r, 0) == pdTRUE;
}

void event_ctrl_init(void) {
  if (s_mtx) {
    return;
  }
  s_mtx = xSemaphoreCreateMutex();
  s_reply_q = xQueueCreate(1, sizeof(int));
  s_cmd_q = xQueueCreate(4, sizeof(ev_req_t));
  s_rx = heap_caps_malloc(EV_RX_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_rx) {
    s_rx = malloc(EV_RX_MAX + 1);
  }
  /* internal-RAM stack (flash ops disable the cache) */
  xTaskCreate(cmd_task, "ev_cmd", 6144, NULL, 4, NULL);
}
