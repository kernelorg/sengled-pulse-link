// Sengled Pulse Link (C01-BR30) protocol helpers for ESPHome.
// Frame format: see ../README.md.
#pragma once

#include "esphome.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <initializer_list>
#include <utility>
#include <vector>

namespace sengled {

static const uint32_t CLIENT = 0xFFFFFFFEu;  // us (same address the phone app uses)
static const uint32_t MASTER = 0x00000000u;  // the gateway itself
static const uint32_t ALL = 0xFFFFFFFFu;     // broadcast

// Minimum gap between frames. The app waits for each reply; the gateway CPU
// is slow, so don't send frames back to back.
static const uint32_t TX_GAP_MS = 80;

struct Frame {
  uint32_t src{0}, dst{0};
  uint8_t sub{0}, cmd{0};
  std::vector<uint8_t> payload;  // frame bytes [13 .. n-3)
  bool is(uint8_t s, uint8_t c) const { return sub == s && cmd == c; }
  uint8_t at(size_t i) const { return i < payload.size() ? payload[i] : 0; }
};

struct TxFrame {
  std::vector<uint8_t> data;
  uint32_t due;  // millis() before which the frame is not sent
  uint32_t dst;
  uint8_t sub, cmd;
};

// Queries are answered by the addressed node; a lamp answers over the radio
// after ~0.7-1 s. The gateway handles one request at a time and drops frames
// that arrive while it waits for a lamp, so (like the app's TransportService)
// after a query nothing is sent until its reply or a timeout.
inline bool is_query(uint8_t sub, uint8_t cmd) {
  return (sub == 0 && (cmd == 0 || cmd == 1 || cmd == 2 || cmd == 5 || cmd == 12)) ||
         (sub == 1 && cmd == 3) || (sub == 2 && cmd == 4) || (sub == 4 && (cmd == 0 || cmd == 7));
}
static const uint32_t REPLY_TIMEOUT_GATEWAY_MS = 600;
// a command to a lamp (or to all lamps) keeps the gateway's radio busy for a
// while too; frames arriving meanwhile are lost (e.g. "on" right after
// "brightness"), so wait this long after such a command
static const uint32_t LAMP_CMD_GAP_MS = 500;
static uint32_t tx_gap_ms = 0;  // gap required after the last sent frame
static const uint32_t REPLY_TIMEOUT_LAMP_MS = 2000;  // replies take ~0.5 s; the app waits up to 5 s

// Commands (on/off, brightness, volume, ...) go before queries, so a change
// from Home Assistant does not wait for a polling round.
static std::deque<TxFrame> tx_cmd, tx_query;
static bool waiting = false;  // a query was sent, its reply is pending
static uint32_t wait_dst = 0, wait_until = 0;
static uint8_t wait_sub = 0, wait_cmd = 0;
static std::vector<uint8_t> rx_buf;
static bool rx_in_frame = false;
static bool rx_escape = false;
static uint32_t last_tx_ms = 0;

// Build a frame of total length `len` and put it into the TX queue.
// `data` = {offset, value} pairs for the payload bytes; `delay_ms` holds the
// frame (and everything queued after it) back for that long.
inline void queue(size_t len, uint32_t dst, uint8_t sub, uint8_t cmd,
                  std::initializer_list<std::pair<uint8_t, uint8_t>> data = {}, uint32_t delay_ms = 0) {
  std::vector<uint8_t> f(len, 0);
  f[0] = f[len - 1] = 0x7E;
  for (int i = 0; i < 4; i++) {
    f[1 + i] = (CLIENT >> (8 * i)) & 0xFF;
    f[5 + i] = (dst >> (8 * i)) & 0xFF;
  }
  f[9] = sub;
  f[11] = cmd;
  for (const auto &kv : data)
    if (kv.first < len - 3)
      f[kv.first] = kv.second;
  uint8_t sum = 0;
  for (size_t i = 1; i < len - 3; i++)
    sum += f[i];
  f[len - 3] = sum;

  std::vector<uint8_t> out{0x7E};
  for (size_t i = 1; i < len - 1; i++) {
    if (f[i] == 0x7D || f[i] == 0x7E) {
      out.push_back(0x7D);
      out.push_back(f[i] ^ 0x20);
    } else {
      out.push_back(f[i]);
    }
  }
  out.push_back(0x7E);
  TxFrame tf{std::move(out), esphome::millis() + delay_ms, dst, sub, cmd};
  (is_query(sub, cmd) ? tx_query : tx_cmd).push_back(std::move(tf));
}

// ---- commands -------------------------------------------------------------
inline void generic_info() { queue(16, MASTER, 0, 0); }  // name, productID, versions
inline void handshake(uint32_t dst) { queue(16, dst, 0, 1); }
inline void identify(uint32_t dst) { queue(16, dst, 0, 7); }  // lamp blinks
inline void set_config(uint32_t dst, uint8_t item, uint8_t v) { queue(20, dst, 0, 11, {{13, item}, {15, v}}); }
inline void query_config(uint8_t item) { queue(18, MASTER, 0, 12, {{13, item}}); }
// The radio link to the lamps loses frames; like the app, send on/off twice,
// the second time 200 ms later (SettingLampActivity / LightControlPannelActivity).
inline void lamp_onoff(uint32_t dst, bool on) {
  queue(20, dst, 1, 0, {{15, uint8_t(on ? 0 : 1)}});
  queue(20, dst, 1, 0, {{15, uint8_t(on ? 0 : 1)}}, 200);
}
// group / own brightness 0..100, 0xFF = keep
inline void brightness(uint32_t dst, uint8_t group, uint8_t own) { queue(20, dst, 1, 1, {{15, group}, {16, own}}); }
inline void lamp_state(uint32_t dst, uint32_t delay_ms = 0) { queue(18, dst, 1, 3, {}, delay_ms); }
inline void channel(uint32_t dst, uint8_t v) { queue(19, dst, 2, 0, {{15, v}}); }  // 0 R, 1 L, 2 L+R
inline void eq(uint32_t dst, uint8_t v) { queue(19, dst, 2, 1, {{15, v}}); }       // 0..5
inline void mute(uint32_t dst, bool m) { queue(19, dst, 2, 2, {{15, uint8_t(m ? 1 : 0)}}); }
inline void volume(uint32_t dst, uint8_t v) { queue(20, dst, 2, 3, {{15, v}, {16, 0xFF}}); }
inline void speaker_state(uint32_t dst) { queue(18, dst, 2, 4); }
inline void audio_source(uint8_t v) { queue(19, ALL, 4, 6, {{15, v}}); }  // 0 3.5mm, 1 BT
inline void get_audio_source() { queue(16, MASTER, 4, 7); }

// Light outputs are ignored until `ready` is set (a few seconds after boot):
// on boot ESPHome restores every light and writes it to its output, which
// would otherwise switch all lamps off whenever the ESP restarts.
static bool ready = false;

// Same sequences as the phone app.
inline void all_lamps(float level) {  // 0.0 .. 1.0
  if (!ready) return;
  int v = (int) (level * 100.0f + 0.5f);
  if (v <= 0) {
    lamp_onoff(ALL, false);
    return;
  }
  if (v > 100) v = 100;
  brightness(ALL, (uint8_t) v, 0xFF);
  brightness(ALL, 0xFF, 100);  // "apply"
  lamp_onoff(ALL, true);
}

inline void one_lamp(uint32_t addr, float level) {
  if (!ready) return;
  int v = (int) (level * 100.0f + 0.5f);
  if (v <= 0) {
    lamp_onoff(addr, false);
    return;
  }
  if (v > 100) v = 100;
  brightness(addr, 0xFF, (uint8_t) v);
  lamp_onoff(addr, true);
}

// ---- pairing commands -----------------------------------------------------
static const uint32_t UNBOUND_CODE = 0x00FFFFFFu;  // code the app writes into a removed lamp

inline void enum_slaves() { queue(16, MASTER, 0, 2); }
inline void query_connect_code() { queue(16, MASTER, 4, 0); }
inline void set_connect_code(uint32_t dst, uint32_t code) {
  queue(20, dst, 4, 1,
        {{13, uint8_t(code)}, {14, uint8_t(code >> 8)}, {15, uint8_t(code >> 16)}, {16, uint8_t(code >> 24)}});
}
// gateway node table: add (del=false) or delete a lamp address
inline void set_id_address(uint32_t addr, bool del) {
  queue(24, MASTER, 4, 2,
        {{13, uint8_t(del ? 1 : 0)},
         {17, uint8_t(addr)},
         {18, uint8_t(addr >> 8)},
         {19, uint8_t(addr >> 16)},
         {20, uint8_t(addr >> 24)}});
}

// "00020056", "0x00020056" or the QR/label text "...;AD:00020056" -> address
inline bool parse_addr(const std::string &in, uint32_t &out) {
  std::string s = in;
  size_t p = s.find("AD:");
  if (p != std::string::npos) s = s.substr(p + 3);
  if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s = s.substr(2);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
  if (s.empty() || s.size() > 8) return false;
  for (char c : s)
    if (!isxdigit((unsigned char) c)) return false;
  out = (uint32_t) strtoul(s.c_str(), nullptr, 16);
  return out != MASTER && out != ALL && out != CLIENT;
}

// Call often (every ~20 ms): sends the next queued frame when the gap has
// passed and returns true each time a valid frame was received into `out`.
inline bool poll(esphome::uart::UARTComponent *uart, Frame &out) {
  uint32_t now = esphome::millis();
  if (waiting && (int32_t) (now - wait_until) >= 0) {
    ESP_LOGD("sengled", "no reply from %08X to %u/%u", (unsigned) wait_dst, wait_sub, wait_cmd);
    waiting = false;
  }
  if (!waiting && now - last_tx_ms >= tx_gap_ms) {
    auto ready_q = [now](std::deque<TxFrame> &q) { return !q.empty() && (int32_t) (now - q.front().due) >= 0; };
    std::deque<TxFrame> *q = ready_q(tx_cmd) ? &tx_cmd : ready_q(tx_query) ? &tx_query : nullptr;
    if (q != nullptr) {
      const TxFrame &f = q->front();
      uart->write_array(f.data.data(), f.data.size());
      if (is_query(f.sub, f.cmd)) {
        waiting = true;
        wait_dst = f.dst;
        wait_sub = f.sub;
        wait_cmd = f.cmd;
        wait_until = now + (f.dst == MASTER ? REPLY_TIMEOUT_GATEWAY_MS : REPLY_TIMEOUT_LAMP_MS);
      }
      tx_gap_ms = (f.dst != MASTER && !is_query(f.sub, f.cmd)) ? LAMP_CMD_GAP_MS : TX_GAP_MS;
      q->pop_front();
      last_tx_ms = now;
    }
  }

  uint8_t b;
  while (uart->available() > 0 && uart->read_byte(&b)) {
    if (b == 0x7E) {
      // rx_buf holds frame bytes [1 .. n-2]
      if (rx_in_frame && rx_buf.size() >= 14) {
        size_t m = rx_buf.size();
        uint8_t sum = 0;
        for (size_t i = 0; i + 2 < m; i++)
          sum += rx_buf[i];
        if (sum == rx_buf[m - 2]) {
          out.src = rx_buf[0] | (rx_buf[1] << 8) | (rx_buf[2] << 16) | ((uint32_t) rx_buf[3] << 24);
          out.dst = rx_buf[4] | (rx_buf[5] << 8) | (rx_buf[6] << 16) | ((uint32_t) rx_buf[7] << 24);
          out.sub = rx_buf[8];
          out.cmd = rx_buf[10];
          out.payload.assign(rx_buf.begin() + 12, rx_buf.end() - 2);
          ESP_LOGV("sengled", "rx %08X -> %08X  %u/%u  payload %u", (unsigned) out.src, (unsigned) out.dst, out.sub,
                   out.cmd, (unsigned) out.payload.size());
          // the reply we wait for: same sub/cmd, from the queried node (the
          // gateway answers some queries from FFFFFFFE)
          if (waiting && out.sub == wait_sub && out.cmd == wait_cmd &&
              (out.src == wait_dst || out.src == CLIENT || wait_dst == MASTER))
            waiting = false;
          rx_buf.clear();
          rx_in_frame = false;  // this 7E was the end flag
          return true;
        }
      }
      if (rx_in_frame && rx_buf.size() >= 14)
        ESP_LOGW("sengled", "rx frame dropped: bad checksum, %u bytes", (unsigned) rx_buf.size());
      // start flag (or resync after garbage / bad checksum)
      rx_buf.clear();
      rx_in_frame = true;
      rx_escape = false;
      continue;
    }
    if (!rx_in_frame)
      continue;  // text between frames, e.g. "SS=01,..." from the BT module
    if (rx_escape) {
      rx_buf.push_back(b ^ 0x20);
      rx_escape = false;
    } else if (b == 0x7D) {
      rx_escape = true;
    } else {
      rx_buf.push_back(b);
    }
    if (rx_buf.size() > 512) {
      rx_buf.clear();
      rx_in_frame = false;
    }
  }
  return false;
}


inline std::string hex8(uint32_t v) {
  char b[9];
  snprintf(b, sizeof(b), "%08X", (unsigned) v);
  return b;
}

// ---- lamp slots -------------------------------------------------------------
// ESPHome entities are fixed at build time, so the config has N lamp slots.
// A slot is bound to a lamp address at run time from the gateway's node list
// (enum_slaves): known lamps keep their slot, new ones take the first free
// slot, slots of removed lamps are freed. The table lives in a `globals` with
// restore_value, so the binding survives reboots.
namespace slots {

static const size_t N = 8;  // the gateway supports up to 8 lamps
using Table = std::array<uint32_t, N>;
static Table *table = nullptr;
static bool changed = false;

// last speaker_state reply per slot
struct Sound {
  bool known = false;
  bool changed = false;
  uint8_t channel = 0, eq = 0, mute = 0, volume = 0;
};
static std::array<Sound, N> sound{};

// last lamp_state reply per slot
struct Light {
  bool changed = false;
  uint32_t ignore_until = 0;  // replies before this belong to the state before the last command
  bool on = false;
  uint8_t level = 0;  // 0..100, the lamp's own brightness (the one a slot sets)
};
static std::array<Light, N> light{};

inline void bind(Table *t) { table = t; changed = true; }
inline uint32_t addr(size_t i) { return (table && i < N) ? (*table)[i] : 0; }
inline std::string text(size_t i) { return addr(i) ? hex8(addr(i)) : std::string("empty"); }

inline void sync(const std::vector<uint32_t> &nodes) {
  if (!table) return;
  Table t = *table;
  for (auto &a : t)
    if (a && std::find(nodes.begin(), nodes.end(), a) == nodes.end()) a = 0;
  for (uint32_t a : nodes) {
    if (a == MASTER || a == ALL || a == CLIENT) continue;
    if (std::find(t.begin(), t.end(), a) != t.end()) continue;
    for (auto &slot : t)
      if (slot == 0) { slot = a; break; }
  }
  if (t != *table) {
    for (size_t i = 0; i < N; i++)
      if (t[i] != (*table)[i]) {
        sound[i] = Sound{};
        light[i] = Light{};
      }
    *table = t;
    changed = true;
  }
}

inline int index_of(uint32_t a) {
  if (!table || a == MASTER) return -1;
  for (size_t i = 0; i < N; i++)
    if ((*table)[i] == a) return (int) i;
  return -1;
}

// feed every received frame here
inline void on_frame(const Frame &f) {
  if (f.dst != CLIENT) return;
  int i = index_of(f.src);
  if (i < 0) return;
  if (f.is(1, 3) && f.payload.size() >= 5) {  // lamp_state: [2] on/off, [3] group, [4] own brightness
    ESP_LOGD("sengled", "lamp %08X: %s, group %u, own %u", (unsigned) f.src, f.at(2) == 0 ? "on" : "off",
             f.at(3), f.at(4));
    if (f.at(3) == 255 && f.at(4) == 255) return;  // dongle, no lamp
    Light &l = light[i];
    if ((int32_t) (esphome::millis() - l.ignore_until) < 0) return;  // stale
    l.on = f.at(2) == 0;
    if (f.at(4) <= 100) l.level = f.at(4);
    l.changed = true;
    return;
  }
  if (!f.is(2, 4) || f.payload.size() < 6) return;
  ESP_LOGD("sengled", "lamp %08X: channel %u, eq %u, mute %u, volume %u", (unsigned) f.src, f.at(2), f.at(3), f.at(4),
           f.at(5));
  Sound &s = sound[i];
  s.channel = f.at(2);
  s.eq = f.at(3);
  s.mute = f.at(4);
  s.volume = f.at(5);
  s.known = true;
  s.changed = true;
}

}  // namespace slots

inline void slot_lamp(size_t i, float level) {
  if (!ready || !slots::addr(i)) return;
  // drop a read-back that is pending or already on its way: it shows the
  // state before this command (the confirming lamp_state is sent 1 s later)
  slots::light[i].changed = false;
  slots::light[i].ignore_until = esphome::millis() + 900;
  one_lamp(slots::addr(i), level);
  lamp_state(slots::addr(i), 1000);  // read back what the lamp really did
}
inline void slot_identify(size_t i) {
  if (slots::addr(i)) identify(slots::addr(i));
}
inline void slot_mute(size_t i, bool m) {
  if (slots::addr(i)) mute(slots::addr(i), m);
}
inline void slot_channel(size_t i, uint8_t c) {
  if (slots::addr(i)) channel(slots::addr(i), c);
}
// like the app's volume slider: set the volume, then unmute
inline void slot_volume(size_t i, uint8_t v) {
  if (!slots::addr(i)) return;
  volume(slots::addr(i), v);
  mute(slots::addr(i), false);
}
// ask every bound lamp for its light and sound state
inline void poll_slots() {
  for (size_t i = 0; i < slots::N; i++)
    if (slots::addr(i)) {
      lamp_state(slots::addr(i));
      speaker_state(slots::addr(i));
    }
}
// after an "all lamps" change: read every lamp back a second later
inline void poll_lamps_later() {
  for (size_t i = 0; i < slots::N; i++)
    if (slots::addr(i)) lamp_state(slots::addr(i), i == 0 ? 1000 : 0);
}

// ---- add / remove lamps ----------------------------------------------------
// Same procedure as the phone app (SLSpeakerFunction.addNewDevice /
// deleteDeviceItem in com.sengled.stspeaker 4.2.1):
//
// add:    read gateway code CC -> set gateway code to 0 ("universal") ->
//         wait 8.5 s, check it is 0 -> write CC into the lamp and back into the
//         gateway -> wait 8.5 s, check it is CC -> handshake the lamp ->
//         add the lamp to the gateway table -> wait 3 s -> enum_slaves
// remove: handshake the lamp (2 tries) -> if it answers, write 00FFFFFF into
//         it -> delete it from the gateway table -> enum_slaves
//
// On any failure while adding, the gateway code is restored to CC.

namespace pairing {

enum Step : uint8_t {
  IDLE,
  A_QUERY_CC, A_WAIT_CC,
  A_SET_ZERO, A_WAIT_ZERO, A_CHECK_ZERO,
  A_SET_CC, A_WAIT_SET_CC, A_CHECK_CC,
  A_HANDSHAKE, A_WAIT_HANDSHAKE,
  A_WAIT_ADDED,
  D_HANDSHAKE, D_WAIT_HANDSHAKE,
  D_WAIT_DONE,
};

static const uint32_t SETTLE_MS = 8500;  // the app waits 8.5 s after a code change
static const uint32_t REPLY_MS = 3000;   // the app's reply timeout
static const uint8_t TRIES = 3;

static Step step = IDLE;
static uint32_t target = 0;   // lamp address
static uint32_t cc = 0;       // gateway network code
static uint32_t t0 = 0;
static uint8_t tries = 0;     // retries of the current stage
static uint8_t hs_tries = 0;  // handshake rounds while adding
static bool got_reply = false;
static uint32_t reply_code = 0;
static std::string status = "idle";
static bool status_changed = true;
static std::string nodes;
static bool nodes_changed = false;

inline void set_status(const std::string &s) {
  status = s;
  status_changed = true;
  ESP_LOGI("sengled", "pairing: %s", s.c_str());
}

inline void go(Step s) {
  step = s;
  t0 = esphome::millis();
  got_reply = false;
}

inline void fail(const std::string &why, bool restore_cc) {
  if (restore_cc && cc != 0) set_connect_code(MASTER, cc);
  set_status("failed: " + why + " (" + hex8(target) + ")");
  step = IDLE;
  enum_slaves();
}

inline bool busy() { return step != IDLE; }

inline void start_add(const std::string &addr_text) {
  if (busy()) { set_status("busy"); return; }
  if (!parse_addr(addr_text, target)) { set_status("bad address: " + addr_text); return; }
  cc = 0;
  tries = 0;
  hs_tries = 0;
  set_status("adding " + hex8(target) + ": reading gateway code");
  go(A_QUERY_CC);
}

inline void start_remove(const std::string &addr_text) {
  if (busy()) { set_status("busy"); return; }
  if (!parse_addr(addr_text, target)) { set_status("bad address: " + addr_text); return; }
  tries = 0;
  set_status("removing " + hex8(target));
  go(D_HANDSHAKE);
}

// feed every received frame here
inline void on_frame(const Frame &f) {
  if (f.dst != CLIENT) return;
  if (f.is(4, 0) && f.src == MASTER && f.payload.size() >= 4) {
    reply_code = f.payload[0] | (f.payload[1] << 8) | (f.payload[2] << 16) | ((uint32_t) f.payload[3] << 24);
    got_reply = true;
  } else if (f.is(0, 1) && f.src == target && step != IDLE) {
    got_reply = true;  // the app treats any handshake reply as success
  } else if (f.is(0, 2) && f.src == MASTER && f.payload.size() >= 4) {
    uint16_t count = f.payload[2] | (f.payload[3] << 8);
    std::string s;
    std::vector<uint32_t> list;
    for (uint16_t i = 0; i < count && size_t(4 + 4 * i + 3) < f.payload.size(); i++) {
      const uint8_t *p = &f.payload[4 + 4 * i];
      uint32_t a = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
      list.push_back(a);
      if (!s.empty()) s += ", ";
      s += a == MASTER ? std::string("gateway") : hex8(a);
    }
    nodes = s.empty() ? "none" : s;
    nodes_changed = true;
    slots::sync(list);
  }
}

// call every tick
inline void loop() {
  uint32_t el = esphome::millis() - t0;
  switch (step) {
    case IDLE:
      break;

    // --- add ---
    case A_QUERY_CC:
      query_connect_code();
      go(A_WAIT_CC);
      break;
    case A_WAIT_CC:
      if (got_reply) {
        if (reply_code == 0 || reply_code == UNBOUND_CODE) {
          // the app would invent a new code and drop all lamps here; don't
          fail("gateway code is " + hex8(reply_code) + ", fix it in the app first", false);
          break;
        }
        cc = reply_code;
        tries = 0;
        set_status("adding " + hex8(target) + ": gateway code " + hex8(cc) + ", opening network");
        go(A_SET_ZERO);
      } else if (el > REPLY_MS) {
        fail("no reply from gateway", false);
      }
      break;
    case A_SET_ZERO:
      set_connect_code(MASTER, 0);
      go(A_WAIT_ZERO);
      break;
    case A_WAIT_ZERO:
      if (el > SETTLE_MS) {
        query_connect_code();
        go(A_CHECK_ZERO);
      }
      break;
    case A_CHECK_ZERO:
      if (got_reply && reply_code == 0) {
        tries = 0;
        set_status("adding " + hex8(target) + ": writing code to lamp");
        go(A_SET_CC);
      } else if (got_reply || el > REPLY_MS) {
        if (++tries >= TRIES) fail("gateway did not accept code 0", true);
        else go(A_SET_ZERO);
      }
      break;
    case A_SET_CC:
      set_connect_code(target, cc);
      set_connect_code(MASTER, cc);
      go(A_WAIT_SET_CC);
      break;
    case A_WAIT_SET_CC:
      if (el > SETTLE_MS) {
        query_connect_code();
        go(A_CHECK_CC);
      }
      break;
    case A_CHECK_CC:
      if (got_reply && reply_code == cc) {
        tries = 0;
        set_status("adding " + hex8(target) + ": handshake");
        go(A_HANDSHAKE);
      } else if (got_reply || el > REPLY_MS) {
        if (++tries >= TRIES) fail("gateway code not restored", true);
        else go(A_SET_CC);
      }
      break;
    case A_HANDSHAKE:
      handshake(target);
      go(A_WAIT_HANDSHAKE);
      break;
    case A_WAIT_HANDSHAKE:
      if (got_reply) {
        set_id_address(target, false);
        set_status("adding " + hex8(target) + ": adding to gateway");
        go(A_WAIT_ADDED);
      } else if (el > REPLY_MS) {
        if (++hs_tries >= TRIES) {
          fail("lamp does not answer (power it on, close to the gateway)", true);
        } else {
          tries = 0;
          set_status("adding " + hex8(target) + ": no handshake, retry " + std::to_string(hs_tries));
          go(A_SET_ZERO);  // the app restarts from "open network"
        }
      }
      break;
    case A_WAIT_ADDED:
      if (el > REPLY_MS) {
        enum_slaves();
        set_status("added " + hex8(target));
        step = IDLE;
      }
      break;

    // --- remove ---
    case D_HANDSHAKE:
      handshake(target);
      go(D_WAIT_HANDSHAKE);
      break;
    case D_WAIT_HANDSHAKE:
      if (got_reply) {
        set_connect_code(target, UNBOUND_CODE);
        set_id_address(target, true);
        go(D_WAIT_DONE);
      } else if (el > REPLY_MS) {
        if (++tries >= 2) {
          // lamp is gone: just drop it from the gateway table
          set_id_address(target, true);
          go(D_WAIT_DONE);
        } else {
          go(D_HANDSHAKE);
        }
      }
      break;
    case D_WAIT_DONE:
      if (el > 1000) {
        enum_slaves();
        set_status("removed " + hex8(target) + (tries >= 2 ? " (lamp did not answer, only removed from gateway)" : ""));
        step = IDLE;
      }
      break;
  }
}

}  // namespace pairing
}  // namespace sengled
