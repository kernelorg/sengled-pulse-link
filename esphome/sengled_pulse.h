// Sengled Pulse Link (C01-BR30) protocol helpers for ESPHome.
// Frame format: see ../README.md.
#pragma once

#include "esphome.h"
#include <deque>
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

static std::deque<std::vector<uint8_t>> tx_queue;
static std::vector<uint8_t> rx_buf;
static bool rx_in_frame = false;
static bool rx_escape = false;
static uint32_t last_tx_ms = 0;

// Build a frame of total length `len` and put it into the TX queue.
// `data` = {offset, value} pairs for the payload bytes.
inline void queue(size_t len, uint32_t dst, uint8_t sub, uint8_t cmd,
                  std::initializer_list<std::pair<uint8_t, uint8_t>> data = {}) {
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
  tx_queue.push_back(std::move(out));
}

// ---- commands -------------------------------------------------------------
inline void handshake(uint32_t dst) { queue(16, dst, 0, 1); }
inline void identify(uint32_t dst) { queue(16, dst, 0, 7); }  // lamp blinks
inline void set_config(uint32_t dst, uint8_t item, uint8_t v) { queue(20, dst, 0, 11, {{13, item}, {15, v}}); }
inline void query_config(uint8_t item) { queue(18, MASTER, 0, 12, {{13, item}}); }
inline void lamp_onoff(uint32_t dst, bool on) { queue(20, dst, 1, 0, {{15, uint8_t(on ? 0 : 1)}}); }
// group / own brightness 0..100, 0xFF = keep
inline void brightness(uint32_t dst, uint8_t group, uint8_t own) { queue(20, dst, 1, 1, {{15, group}, {16, own}}); }
inline void lamp_state(uint32_t dst) { queue(18, dst, 1, 3); }
inline void channel(uint32_t dst, uint8_t v) { queue(19, dst, 2, 0, {{15, v}}); }  // 0 R, 1 L, 2 L+R
inline void eq(uint32_t dst, uint8_t v) { queue(19, dst, 2, 1, {{15, v}}); }       // 0..5
inline void mute(uint32_t dst, bool m) { queue(19, dst, 2, 2, {{15, uint8_t(m ? 1 : 0)}}); }
inline void volume(uint32_t dst, uint8_t v) { queue(20, dst, 2, 3, {{15, v}, {16, 0xFF}}); }
inline void speaker_state(uint32_t dst) { queue(18, dst, 2, 4); }
inline void audio_source(uint8_t v) { queue(19, ALL, 4, 6, {{15, v}}); }  // 0 3.5mm, 1 BT
inline void get_audio_source() { queue(16, MASTER, 4, 7); }

// Same sequences as the phone app.
inline void all_lamps(float level) {  // 0.0 .. 1.0
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
  int v = (int) (level * 100.0f + 0.5f);
  if (v <= 0) {
    lamp_onoff(addr, false);
    return;
  }
  if (v > 100) v = 100;
  brightness(addr, 0xFF, (uint8_t) v);
  lamp_onoff(addr, true);
}

// Call often (every ~20 ms): sends the next queued frame when the gap has
// passed and returns true each time a valid frame was received into `out`.
inline bool poll(esphome::uart::UARTComponent *uart, Frame &out) {
  uint32_t now = esphome::millis();
  if (!tx_queue.empty() && now - last_tx_ms >= TX_GAP_MS) {
    const auto &f = tx_queue.front();
    uart->write_array(f.data(), f.size());
    tx_queue.pop_front();
    last_tx_ms = now;
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
          rx_buf.clear();
          rx_in_frame = false;  // this 7E was the end flag
          return true;
        }
      }
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

}  // namespace sengled
