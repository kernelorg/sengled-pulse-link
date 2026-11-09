#include "link.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::sengled_pulse {

static const char *const TAG = "sengled_pulse";

// Minimum gap between frames: the gateway CPU is slow.
static constexpr uint32_t TX_GAP_MS = 80;
// A command to a lamp (or to all lamps) keeps the gateway's radio busy for a
// while; frames arriving meanwhile are lost (e.g. "on" right after
// "brightness"), so wait this long after such a command.
static constexpr uint32_t LAMP_CMD_GAP_MS = 500;
static constexpr uint32_t REPLY_TIMEOUT_GATEWAY_MS = 600;
static constexpr uint32_t REPLY_TIMEOUT_LAMP_MS = 2000;  // replies take ~0.5 s; the app waits up to 5 s

std::string hex8(uint32_t v) {
  char b[9];
  snprintf(b, sizeof(b), "%08X", (unsigned) v);
  return b;
}

bool parse_addr(const std::string &in, uint32_t &out) {
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

bool Link::is_query(uint8_t sub, uint8_t cmd) {
  return (sub == 0 && (cmd == 0 || cmd == 1 || cmd == 2 || cmd == 5 || cmd == 12)) || (sub == 1 && cmd == 3) ||
         (sub == 2 && cmd == 4) || (sub == 4 && (cmd == 0 || cmd == 7));
}

void Link::queue(size_t len, uint32_t dst, uint8_t sub, uint8_t cmd,
                 std::initializer_list<std::pair<uint8_t, uint8_t>> data, uint32_t delay_ms) {
  std::vector<uint8_t> f(len, 0);
  f[0] = f[len - 1] = 0x7E;
  for (int i = 0; i < 4; i++) {
    f[1 + i] = (CLIENT >> (8 * i)) & 0xFF;
    f[5 + i] = (dst >> (8 * i)) & 0xFF;
  }
  f[9] = sub;
  f[11] = cmd;
  for (const auto &kv : data)
    if (kv.first < len - 3) f[kv.first] = kv.second;
  uint8_t sum = 0;
  for (size_t i = 1; i < len - 3; i++) sum += f[i];
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
  TxFrame tf{std::move(out), millis() + delay_ms, dst, sub, cmd};
  (is_query(sub, cmd) ? tx_query_ : tx_cmd_).push_back(std::move(tf));
}

bool Link::poll(Frame &out) {
  uint32_t now = millis();
  if (waiting_ && (int32_t) (now - wait_until_) >= 0) {
    ESP_LOGD(TAG, "no reply from %08X to %u/%u", (unsigned) wait_dst_, wait_sub_, wait_cmd_);
    waiting_ = false;
  }
  if (!waiting_ && now - last_tx_ms_ >= tx_gap_ms_) send_next_(now);
  return receive_(out);
}

void Link::send_next_(uint32_t now) {
  auto ready = [now](std::deque<TxFrame> &q) { return !q.empty() && (int32_t) (now - q.front().due) >= 0; };
  std::deque<TxFrame> *q = ready(tx_cmd_) ? &tx_cmd_ : ready(tx_query_) ? &tx_query_ : nullptr;
  if (q == nullptr || uart_ == nullptr) return;
  const TxFrame &f = q->front();
  uart_->write_array(f.data.data(), f.data.size());
  bool query = is_query(f.sub, f.cmd);
  if (query) {
    waiting_ = true;
    wait_dst_ = f.dst;
    wait_sub_ = f.sub;
    wait_cmd_ = f.cmd;
    wait_until_ = now + (f.dst == MASTER ? REPLY_TIMEOUT_GATEWAY_MS : REPLY_TIMEOUT_LAMP_MS);
  }
  tx_gap_ms_ = (f.dst != MASTER && !query) ? LAMP_CMD_GAP_MS : TX_GAP_MS;
  q->pop_front();
  last_tx_ms_ = now;
}

bool Link::receive_(Frame &out) {
  if (uart_ == nullptr) return false;
  uint8_t b;
  while (uart_->available() > 0 && uart_->read_byte(&b)) {
    if (b == 0x7E) {
      // rx_buf_ holds frame bytes [1 .. n-2]
      if (rx_in_frame_ && rx_buf_.size() >= 14) {
        size_t m = rx_buf_.size();
        uint8_t sum = 0;
        for (size_t i = 0; i + 2 < m; i++) sum += rx_buf_[i];
        if (sum == rx_buf_[m - 2]) {
          out.src = rx_buf_[0] | (rx_buf_[1] << 8) | (rx_buf_[2] << 16) | ((uint32_t) rx_buf_[3] << 24);
          out.dst = rx_buf_[4] | (rx_buf_[5] << 8) | (rx_buf_[6] << 16) | ((uint32_t) rx_buf_[7] << 24);
          out.sub = rx_buf_[8];
          out.cmd = rx_buf_[10];
          out.payload.assign(rx_buf_.begin() + 12, rx_buf_.end() - 2);
          ESP_LOGV(TAG, "rx %08X -> %08X  %u/%u  payload %u", (unsigned) out.src, (unsigned) out.dst, out.sub, out.cmd,
                   (unsigned) out.payload.size());
          // the reply we wait for: same sub/cmd, from the queried node (the
          // gateway answers some queries from FFFFFFFE)
          if (waiting_ && out.sub == wait_sub_ && out.cmd == wait_cmd_ &&
              (out.src == wait_dst_ || out.src == CLIENT || wait_dst_ == MASTER))
            waiting_ = false;
          rx_buf_.clear();
          rx_in_frame_ = false;  // this 7E was the end flag
          return true;
        }
        ESP_LOGW(TAG, "rx frame dropped: bad checksum, %u bytes", (unsigned) m);
      }
      // start flag (or resync after garbage / bad checksum)
      rx_buf_.clear();
      rx_in_frame_ = true;
      rx_escape_ = false;
      continue;
    }
    if (!rx_in_frame_) continue;  // text between frames, e.g. "SS=01,..." from the BT module
    if (rx_escape_) {
      rx_buf_.push_back(b ^ 0x20);
      rx_escape_ = false;
    } else if (b == 0x7D) {
      rx_escape_ = true;
    } else {
      rx_buf_.push_back(b);
    }
    if (rx_buf_.size() > 512) {
      rx_buf_.clear();
      rx_in_frame_ = false;
    }
  }
  return false;
}

}  // namespace esphome::sengled_pulse
