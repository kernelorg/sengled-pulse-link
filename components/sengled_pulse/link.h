// Sengled Pulse Link (C01-BR30) frame layer: build, queue, send, receive.
// Frame format: see README.md.
#pragma once

#include <cstdint>
#include <deque>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "esphome/components/uart/uart.h"

namespace esphome::sengled_pulse {

static constexpr uint32_t CLIENT = 0xFFFFFFFEu;  // us (same address the phone app uses)
static constexpr uint32_t MASTER = 0x00000000u;  // the gateway itself
static constexpr uint32_t ALL = 0xFFFFFFFFu;     // broadcast
static constexpr uint32_t UNBOUND_CODE = 0x00FFFFFFu;  // code the app writes into a removed lamp

struct Frame {
  uint32_t src{0}, dst{0};
  uint8_t sub{0}, cmd{0};
  std::vector<uint8_t> payload;  // frame bytes [13 .. n-3)
  bool is(uint8_t s, uint8_t c) const { return sub == s && cmd == c; }
  uint8_t at(size_t i) const { return i < payload.size() ? payload[i] : 0; }
  uint32_t u32(size_t i) const { return at(i) | (at(i + 1) << 8) | (at(i + 2) << 16) | ((uint32_t) at(i + 3) << 24); }
};

std::string hex8(uint32_t v);
// "00020056", "0x00020056" or the QR/label text "...;AD:00020056" -> address
bool parse_addr(const std::string &in, uint32_t &out);

// TX queue and RX parser.
//
// Queries are answered by the addressed node; a lamp answers over the radio
// after ~0.7-1 s. The gateway handles one request at a time and drops frames
// that arrive while it waits for a lamp, so (like the app's TransportService)
// after a query nothing is sent until its reply or a timeout. Commands
// (on/off, brightness, volume, ...) go before queries, so a change from Home
// Assistant does not wait for a polling round.
class Link {
 public:
  void set_uart(uart::UARTDevice *uart) { uart_ = uart; }

  // Build a frame of total length `len` and queue it. `data` = {offset, value}
  // pairs for the payload bytes; `delay_ms` holds the frame back that long.
  void queue(size_t len, uint32_t dst, uint8_t sub, uint8_t cmd,
             std::initializer_list<std::pair<uint8_t, uint8_t>> data = {}, uint32_t delay_ms = 0);

  // Call often: sends the next queued frame when the gap has passed and
  // returns true each time a valid frame was received into `out`.
  bool poll(Frame &out);

  // ---- commands ----
  void generic_info() { queue(16, MASTER, 0, 0); }  // name, productID, versions
  void handshake(uint32_t dst) { queue(16, dst, 0, 1); }
  void enum_slaves() { queue(16, MASTER, 0, 2); }
  void identify(uint32_t dst) { queue(16, dst, 0, 7); }  // lamp blinks
  void set_config(uint32_t dst, uint8_t item, uint8_t v) { queue(20, dst, 0, 11, {{13, item}, {15, v}}); }
  void query_config(uint8_t item) { queue(18, MASTER, 0, 12, {{13, item}}); }
  // The radio link to the lamps loses frames; like the app, send on/off
  // twice, the second time 200 ms later.
  void lamp_onoff(uint32_t dst, bool on) {
    queue(20, dst, 1, 0, {{15, uint8_t(on ? 0 : 1)}});
    queue(20, dst, 1, 0, {{15, uint8_t(on ? 0 : 1)}}, 200);
  }
  // group / own brightness 0..100, 0xFF = keep
  void brightness(uint32_t dst, uint8_t group, uint8_t own) { queue(20, dst, 1, 1, {{15, group}, {16, own}}); }
  void lamp_state(uint32_t dst, uint32_t delay_ms = 0) { queue(18, dst, 1, 3, {}, delay_ms); }
  void channel(uint32_t dst, uint8_t v) { queue(19, dst, 2, 0, {{15, v}}); }  // 0 R, 1 L, 2 L+R
  void eq(uint32_t dst, uint8_t v) { queue(19, dst, 2, 1, {{15, v}}); }       // 0..5
  void mute(uint32_t dst, bool m, uint32_t delay_ms = 0) { queue(19, dst, 2, 2, {{15, uint8_t(m ? 1 : 0)}}, delay_ms); }
  void volume(uint32_t dst, uint8_t v) { queue(20, dst, 2, 3, {{15, v}, {16, 0xFF}}); }
  void speaker_state(uint32_t dst, uint32_t delay_ms = 0) { queue(18, dst, 2, 4, {}, delay_ms); }
  void query_connect_code() { queue(16, MASTER, 4, 0); }
  void set_connect_code(uint32_t dst, uint32_t code) {
    queue(20, dst, 4, 1,
          {{13, uint8_t(code)}, {14, uint8_t(code >> 8)}, {15, uint8_t(code >> 16)}, {16, uint8_t(code >> 24)}});
  }
  // gateway node table: add (del=false) or delete a lamp address
  void set_id_address(uint32_t addr, bool del) {
    queue(24, MASTER, 4, 2,
          {{13, uint8_t(del ? 1 : 0)},
           {17, uint8_t(addr)},
           {18, uint8_t(addr >> 8)},
           {19, uint8_t(addr >> 16)},
           {20, uint8_t(addr >> 24)}});
  }
  void audio_source(uint8_t v) { queue(19, ALL, 4, 6, {{15, v}}); }  // 0 3.5 mm, 1 Bluetooth
  void get_audio_source() { queue(16, MASTER, 4, 7); }

 protected:
  struct TxFrame {
    std::vector<uint8_t> data;
    uint32_t due;  // millis() before which the frame is not sent
    uint32_t dst;
    uint8_t sub, cmd;
  };
  static bool is_query(uint8_t sub, uint8_t cmd);
  void send_next_(uint32_t now);
  bool receive_(Frame &out);

  uart::UARTDevice *uart_{nullptr};
  std::deque<TxFrame> tx_cmd_, tx_query_;
  bool waiting_{false};  // a query was sent, its reply is pending
  uint32_t wait_dst_{0}, wait_until_{0};
  uint8_t wait_sub_{0}, wait_cmd_{0};
  uint32_t last_tx_ms_{0}, tx_gap_ms_{0};
  std::vector<uint8_t> rx_buf_;
  bool rx_in_frame_{false}, rx_escape_{false};
};

}  // namespace esphome::sengled_pulse
