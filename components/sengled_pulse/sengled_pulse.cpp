#include "sengled_pulse.h"

#include <algorithm>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::sengled_pulse {

static const char *const TAG = "sengled_pulse";

// A command to all lamps shows the expected state at once; replies already on
// their way show the old state and are dropped this long.
static constexpr uint32_t STALE_MS = 900;
static constexpr uint32_t BOOT_READY_MS = 5000;  // lights restore their state without sending anything
static constexpr uint32_t BOOT_DONE_MS = 10000;  // lamps' mute state is known: automatic mute may act

static bool before(uint32_t t) { return (int32_t) (millis() - t) < 0; }
static uint8_t percent(float level) {
  int v = (int) (level * 100.0f + 0.5f);
  return (uint8_t) std::clamp(v, 0, 100);
}

// ============================================================================
// entities

void SengledLight::write_state(light::LightState *state) {
  float level;
  state->current_values_as_brightness(&level);
  parent_->on_light(slot_, level);
}

void SengledLight::show(bool on, uint8_t level) {
  if (state_ == nullptr) return;
  state_->remote_values.set_state(on);
  if (on && level > 0) state_->remote_values.set_brightness(level / 100.0f);
  state_->current_values = state_->remote_values;
  state_->publish_state();
}

void SengledNumber::control(float value) {
  publish_state(value);
  parent_->on_number(kind_, slot_, value);
}

void SengledSwitch::write_state(bool state) {
  publish_state(state);
  parent_->on_switch(kind_, slot_, state);
}

void SengledSelect::control(size_t index) {
  publish_state(index);
  parent_->on_select(kind_, slot_, index);
}

void SengledButton::press_action() { parent_->on_button(kind_, slot_); }

using TextStore = std::array<char, 66>;  // max_length 64 + NUL, rounded up

void SengledText::setup() {
  pref_ = make_entity_preference<TextStore>();
  TextStore buf{};
  if (pref_.load(&buf)) {
    buf.back() = 0;
    publish_state(std::string(buf.data()));
  } else {
    publish_state("");
  }
}

void SengledText::control(const std::string &value) {
  TextStore buf{};
  strncpy(buf.data(), value.c_str(), buf.size() - 1);
  pref_.save(&buf);
  publish_state(value);
}

// ============================================================================
// hub: boot, loop, frames

void SengledPulse::setup() {
  link_.set_uart(this);

  // lamp address per slot, kept over reboots: known lamps keep their slot
  pref_ = global_preferences->make_preference<Table>(fnv1_hash("sengled_pulse_lamp_slots"));
  Table t{};
  if (pref_.load(&t))
    for (size_t i = 0; i < n_slots_; i++) slots_[i].addr = t[i];
  for (size_t i = 0; i < n_slots_; i++)
    if (slots_[i].address != nullptr) slots_[i].address->publish_state(slots_[i].addr ? hex8(slots_[i].addr) : "empty");
  set_status_("idle");

  link_.handshake(MASTER);
  link_.generic_info();
  link_.enum_slaves();
  link_.speaker_state(MASTER);
  link_.get_audio_source();
  link_.query_config(0);

  set_timeout(BOOT_READY_MS, [this]() {
    ready_ = true;
    poll_slots_();  // slots are bound by now
  });
  set_timeout(BOOT_DONE_MS, [this]() {
    boot_done_ = true;
    check_playing_();
  });
}

void SengledPulse::update() {
  if (!ready_) return;  // the boot sequence reads everything
  link_.speaker_state(MASTER);
  link_.get_audio_source();
  poll_slots_();
}

void SengledPulse::loop() {
  Frame f;
  while (link_.poll(f)) on_frame_(f);
  pairing_loop_();
  publish_();
}

void SengledPulse::dump_config() {
  ESP_LOGCONFIG(TAG, "Sengled Pulse Link:\n  Lamp slots: %u\n  Update interval: %u ms", n_slots_,
                (unsigned) get_update_interval());
  for (size_t i = 0; i < n_slots_; i++)
    ESP_LOGCONFIG(TAG, "  Lamp %u: %s", (unsigned) (i + 1), slots_[i].addr ? hex8(slots_[i].addr).c_str() : "empty");
#ifdef USE_MEDIA_PLAYER
  if (player_ != nullptr) ESP_LOGCONFIG(TAG, "  Auto mute: after %u ms of silence", (unsigned) auto_mute_delay_ms_);
#endif
}

void SengledPulse::on_frame_(const Frame &f) {
  if (f.dst != CLIENT) return;

  // pairing: connect code, handshake, node list
  if (f.is(4, 0) && f.src == MASTER && f.payload.size() >= 4) {
    reply_code_ = f.u32(0);
    got_reply_ = true;
  } else if (f.is(0, 1) && f.src == target_ && step_ != 0) {
    got_reply_ = true;  // the app treats any handshake reply as success
  } else if (f.is(0, 2) && f.src == MASTER && f.payload.size() >= 4) {
    uint16_t count = f.at(2) | (f.at(3) << 8);
    std::vector<uint32_t> nodes;
    for (uint16_t i = 0; i < count && size_t(4 + 4 * i + 3) < f.payload.size(); i++) nodes.push_back(f.u32(4 + 4 * i));
    on_nodes_(nodes);
    return;
  }

  // the gateway answers some queries (e.g. generic_info) from FFFFFFFE
  if (f.src != MASTER && f.src != CLIENT) {
    on_slot_frame_(f);
    return;
  }
  if (f.is(2, 4)) {  // gateway speaker_state: [2] channel, [3] eq, [4] mute
    if (gw_channel_ != nullptr && f.at(2) < gw_channel_->size()) gw_channel_->publish_state((size_t) f.at(2));
    // one equalizer for the whole system (the app sends it to all and reads
    // it from the gateway)
    if (equalizer_ != nullptr && f.at(3) < equalizer_->size()) equalizer_->publish_state((size_t) f.at(3));
    if (!before(gw_mute_ignore_until_)) {  // else stale
      gw_mute_v_ = f.at(4) != 0;
      gw_mute_dirty_ = all_sound_dirty_ = true;
    }
  } else if (f.is(0, 0) && f.payload.size() >= 36) {  // generic_info
    uint32_t pid = f.u32(32);
    std::string name(f.payload.begin(), f.payload.begin() + 32);
    name = name.c_str();  // cut at the first NUL
    const char *kind = pid == 1 ? "single light" : pid == 2 ? "audio box, no lamp" : "lamp";
    if (gateway_type_ != nullptr)
      gateway_type_->publish_state(name + " (productID " + std::to_string(pid) + ", " + kind + ")");
  } else if (f.is(4, 7)) {  // audio source
    if (audio_source_ != nullptr && f.at(0) < audio_source_->size()) audio_source_->publish_state((size_t) f.at(0));
  } else if (f.is(0, 12) && f.at(0) == 0) {  // config item 0: light memory
    if (light_memory_ != nullptr) light_memory_->publish_state(f.at(2) != 0);
  }
}

// replies of a lamp bound to a slot
void SengledPulse::on_slot_frame_(const Frame &f) {
  int i = -1;
  for (size_t k = 0; k < n_slots_; k++)
    if (slots_[k].addr == f.src) i = (int) k;
  if (i < 0) return;
  Slot &s = slots_[i];

  if (f.is(1, 3) && f.payload.size() >= 5) {  // lamp_state: [2] on/off, [3] group, [4] own brightness
    ESP_LOGD(TAG, "lamp %08X: %s, group %u, own %u", (unsigned) f.src, f.at(2) == 0 ? "on" : "off", f.at(3), f.at(4));
    if (f.at(3) == 255 && f.at(4) == 255) return;  // dongle, no lamp
    if (before(s.light_ignore_until)) return;      // stale
    s.on = f.at(2) == 0;
    if (f.at(4) <= 100) s.level = f.at(4);
    s.light_known = s.light_dirty = all_light_dirty_ = true;
  } else if (f.is(2, 4) && f.payload.size() >= 6) {  // speaker_state: [2] channel, [3] eq, [4] mute, [5] volume
    ESP_LOGD(TAG, "lamp %08X: channel %u, eq %u, mute %u, volume %u", (unsigned) f.src, f.at(2), f.at(3), f.at(4),
             f.at(5));
    if (before(s.sound_ignore_until)) return;  // stale
    s.channel_v = f.at(2);
    s.mute_v = f.at(4);
    s.volume_v = f.at(5);
    s.sound_known = s.sound_dirty = all_sound_dirty_ = true;
  }
}

// Bind lamps to slots from the gateway's node list: known lamps keep their
// slot, new ones take the first free slot, slots of removed lamps are freed.
void SengledPulse::on_nodes_(const std::vector<uint32_t> &nodes) {
  std::string text;
  for (uint32_t a : nodes) {
    if (!text.empty()) text += ", ";
    text += a == MASTER ? std::string("gateway") : hex8(a);
  }
  if (gateway_nodes_ != nullptr) gateway_nodes_->publish_state(text.empty() ? "none" : text);

  Table t{};
  for (size_t i = 0; i < n_slots_; i++) t[i] = slots_[i].addr;
  for (size_t i = 0; i < n_slots_; i++)
    if (t[i] && std::find(nodes.begin(), nodes.end(), t[i]) == nodes.end()) t[i] = 0;
  for (uint32_t a : nodes) {
    if (a == MASTER || a == ALL || a == CLIENT) continue;
    if (std::find(t.begin(), t.begin() + n_slots_, a) != t.begin() + n_slots_) continue;
    size_t i = 0;
    while (i < n_slots_ && t[i]) i++;
    if (i < n_slots_)
      t[i] = a;
    else
      ESP_LOGW(TAG, "lamp %08X: no free slot (%u slots), raise lamp_slots", (unsigned) a, n_slots_);
  }
  bool changed = false;
  for (size_t i = 0; i < n_slots_; i++) {
    if (t[i] == slots_[i].addr) continue;
    Slot &s = slots_[i];
    Slot fresh;
    fresh.light = s.light;
    fresh.volume = s.volume;
    fresh.mute = s.mute;
    fresh.channel = s.channel;
    fresh.address = s.address;
    fresh.addr = t[i];
    s = fresh;
    if (s.address != nullptr) s.address->publish_state(s.addr ? hex8(s.addr) : "empty");
    ESP_LOGI(TAG, "lamp %u: %s", (unsigned) (i + 1), s.addr ? hex8(s.addr).c_str() : "empty");
    if (s.addr) {
      link_.lamp_state(s.addr);
      link_.speaker_state(s.addr);
    }
    changed = true;
  }
  if (changed) {
    Table save{};
    for (size_t i = 0; i < n_slots_; i++) save[i] = slots_[i].addr;
    pref_.save(&save);
    all_light_dirty_ = all_sound_dirty_ = true;
  }
}

// ============================================================================
// state shown in Home Assistant

void SengledPulse::expect_light_all_(bool on, uint8_t level) {
  for (size_t i = 0; i < n_slots_; i++) {
    Slot &s = slots_[i];
    if (!s.addr) continue;
    s.on = on;
    if (on) s.level = level;
    s.light_known = s.light_dirty = true;
    s.light_ignore_until = millis() + STALE_MS;
  }
  all_light_dirty_ = true;
}

void SengledPulse::expect_sound_(int i, int volume, int mute, int channel) {
  Slot &s = slots_[i];
  if (!s.addr) return;
  if (!s.sound_known) {
    // the other fields are unknown: publish only what was set
    if (volume >= 0 && s.volume != nullptr) s.volume->publish_state(volume);
    if (mute >= 0 && s.mute != nullptr) s.mute->publish_state(mute != 0);
    if (channel >= 0 && s.channel != nullptr && (size_t) channel < s.channel->size())
      s.channel->publish_state((size_t) channel);
    return;
  }
  if (volume >= 0) s.volume_v = volume;
  if (mute >= 0) s.mute_v = mute;
  if (channel >= 0) s.channel_v = channel;
  s.sound_ignore_until = millis() + STALE_MS;
  s.sound_dirty = all_sound_dirty_ = true;
}

void SengledPulse::expect_sound_all_(int volume, int mute, int channel) {
  for (size_t i = 0; i < n_slots_; i++) expect_sound_(i, volume, mute, channel);
}

void SengledPulse::expect_gateway_mute_(bool mute) {
  gw_mute_v_ = mute;
  gw_mute_ignore_until_ = millis() + STALE_MS;
  gw_mute_dirty_ = all_sound_dirty_ = true;
}

void SengledPulse::publish_() {
  for (size_t i = 0; i < n_slots_; i++) {
    Slot &s = slots_[i];
    if (s.light_dirty) {
      s.light_dirty = false;
      if (s.light != nullptr) s.light->show(s.on, s.level);
    }
    if (s.sound_dirty) {
      s.sound_dirty = false;
      if (s.volume != nullptr) s.volume->publish_state(s.volume_v);
      if (s.mute != nullptr) s.mute->publish_state(s.mute_v != 0);
      if (s.channel != nullptr && s.channel_v < s.channel->size()) s.channel->publish_state((size_t) s.channel_v);
    }
  }

  // "All lamps", like a Home Assistant light group: on when any lamp is on,
  // brightness = mean of the lamps that are on
  if (all_light_dirty_) {
    all_light_dirty_ = false;
    int known = 0, n_on = 0, sum = 0;
    for (size_t i = 0; i < n_slots_; i++) {
      const Slot &s = slots_[i];
      if (!s.addr || !s.light_known) continue;
      known++;
      if (s.on) {
        n_on++;
        sum += s.level;
      }
    }
    if (known && all_lamps_ != nullptr)
      all_lamps_->show(n_on > 0, n_on ? (uint8_t) ((sum + n_on / 2) / n_on) : 0);
  }

  if (gw_mute_dirty_) {
    gw_mute_dirty_ = false;
    if (gw_mute_ != nullptr && gw_mute_v_ >= 0) gw_mute_->publish_state(gw_mute_v_ != 0);
  }

  // Volume = mean of the lamps, Mute = every lamp and the gateway muted
  if (all_sound_dirty_) {
    all_sound_dirty_ = false;
    int n = 0, sum = 0, speakers = 0, muted = 0;
    for (size_t i = 0; i < n_slots_; i++) {
      const Slot &s = slots_[i];
      if (!s.addr || !s.sound_known) continue;
      n++;
      sum += s.volume_v;
      speakers++;
      if (s.mute_v) muted++;
    }
    if (gw_mute_v_ >= 0) {
      speakers++;
      if (gw_mute_v_) muted++;
    }
    if (n && volume_ != nullptr) volume_->publish_state((sum + n / 2) / n);
    if (speakers && mute_ != nullptr) mute_->publish_state(muted == speakers);
  }
}

// ============================================================================
// commands from the entities

void SengledPulse::poll_slots_() {
  for (size_t i = 0; i < n_slots_; i++)
    if (slots_[i].addr) {
      link_.lamp_state(slots_[i].addr);
      link_.speaker_state(slots_[i].addr);
    }
}

// same sequences as the phone app
void SengledPulse::send_all_lamps_(float level) {
  uint8_t v = percent(level);
  if (v == 0) {
    link_.lamp_onoff(ALL, false);
    return;
  }
  link_.brightness(ALL, v, 0xFF);
  link_.brightness(ALL, 0xFF, 100);  // "apply"
  link_.lamp_onoff(ALL, true);
}

void SengledPulse::one_lamp_(uint32_t addr, float level) {
  uint8_t v = percent(level);
  if (v == 0) {
    link_.lamp_onoff(addr, false);
    return;
  }
  link_.brightness(addr, 0xFF, v);
  link_.lamp_onoff(addr, true);
}

void SengledPulse::on_light(int slot, float level) {
  // on boot every light restores its state and writes it: ignore that, it
  // would switch all lamps off whenever the ESP restarts
  if (!ready_) return;
  if (slot < 0) {
    send_all_lamps_(level);
    uint8_t v = percent(level);
    expect_light_all_(v > 0, v);
    for (size_t i = 0; i < n_slots_; i++)  // read every lamp back a second later
      if (slots_[i].addr) link_.lamp_state(slots_[i].addr, 1000);
    return;
  }
  if (!bound_(slot)) return;
  Slot &s = slots_[slot];
  // drop a read-back already on its way: it shows the state before this
  // command (the confirming lamp_state is sent 1 s later)
  s.light_dirty = false;
  s.light_ignore_until = millis() + STALE_MS;
  s.on = level > 0;
  if (s.on) s.level = percent(level);
  s.light_known = all_light_dirty_ = true;
  one_lamp_(s.addr, level);
  link_.lamp_state(s.addr, 1000);
}

void SengledPulse::on_number(Kind kind, int slot, float value) {
  uint8_t v = (uint8_t) std::clamp(value, 0.0f, 100.0f);
  if (kind == Kind::VOLUME) {
    // like the app's volume slider: set the volume on all lamps, then unmute
    for (size_t i = 0; i < n_slots_; i++) slots_[i].user_muted = false;
    // "all" includes the gateway, so it is unmuted too
    gw_user_muted_ = false;
    link_.volume(ALL, v);
    link_.mute(ALL, false);
    expect_sound_all_(v, 0, -1);
    expect_gateway_mute_(false);
  } else if (kind == Kind::SLOT_VOLUME && bound_(slot)) {
    slots_[slot].user_muted = false;
    link_.volume(slots_[slot].addr, v);
    link_.mute(slots_[slot].addr, false);
    expect_sound_(slot, v, 0, -1);
  }
}

void SengledPulse::on_switch(Kind kind, int slot, bool state) {
  switch (kind) {
    case Kind::MUTE:  // all lamps and the gateway
      for (size_t i = 0; i < n_slots_; i++) slots_[i].user_muted = state;
      gw_user_muted_ = state;
      link_.mute(ALL, state);
      expect_sound_all_(-1, state, -1);
      expect_gateway_mute_(state);
      break;
    case Kind::GATEWAY_MUTE:
      gw_user_muted_ = state;
      link_.mute(MASTER, state);
      expect_gateway_mute_(state);
      break;
    case Kind::LIGHT_MEMORY:
      link_.set_config(ALL, 0, state ? 1 : 0);
      break;
    case Kind::SLOT_MUTE:
      if (!bound_(slot)) break;
      slots_[slot].user_muted = state;
      link_.mute(slots_[slot].addr, state);
      expect_sound_(slot, -1, state, -1);
      break;
    default:
      break;
  }
}

void SengledPulse::on_select(Kind kind, int slot, size_t index) {
  uint8_t v = (uint8_t) index;  // option order = protocol value
  switch (kind) {
    case Kind::EQUALIZER:  // all lamps and the gateway
      link_.eq(ALL, v);
      break;
    case Kind::GATEWAY_CHANNEL:
      link_.channel(MASTER, v);
      break;
    case Kind::AUDIO_SOURCE:
      link_.audio_source(v);
      break;
    case Kind::SLOT_CHANNEL:
      if (!bound_(slot)) break;
      link_.channel(slots_[slot].addr, v);
      expect_sound_(slot, -1, -1, v);
      break;
    default:
      break;
  }
}

void SengledPulse::on_button(Kind kind, int slot) {
  std::string addr = lamp_address_ != nullptr ? lamp_address_->state : std::string();
  switch (kind) {
    case Kind::ADD_LAMP:
      start_add(addr);
      break;
    case Kind::REMOVE_LAMP:
      start_remove(addr);
      break;
    case Kind::REFRESH_NODES:
      link_.enum_slaves();
      break;
    case Kind::SLOT_IDENTIFY:
      if (bound_(slot)) link_.identify(slots_[slot].addr);
      break;
    default:
      break;
  }
}

// ============================================================================
// automatic mute
//
// Mute every speaker (all lamps and the gateway) while nothing is playing.
// When the sound comes back, only the speakers muted by hand (mute switches,
// since the last boot) stay muted. Commands to single lamps get lost now and
// then, broadcasts are more reliable: both directions use a broadcast sent
// twice, like the app's on/off; lamps muted by hand are muted again by
// address afterwards, the gateway gets its own command.

void SengledPulse::auto_mute() {
  if (auto_muted_) return;
  auto_muted_ = true;
  ESP_LOGI(TAG, "nothing playing: muting all speakers");
  link_.mute(ALL, true);
  link_.mute(ALL, true, 200);
  link_.mute(MASTER, true, 400);
  expect_gateway_mute_(true);
}

void SengledPulse::auto_unmute() {
  if (!auto_muted_) return;
  auto_muted_ = false;
  ESP_LOGI(TAG, "playing: unmuting speakers");
  link_.mute(ALL, false);
  link_.mute(ALL, false, 200);
  for (size_t i = 0; i < n_slots_; i++)
    if (slots_[i].addr && slots_[i].user_muted) {
      link_.mute(slots_[i].addr, true, 400);
      link_.mute(slots_[i].addr, true, 600);
    }
  link_.mute(MASTER, gw_user_muted_, 800);
  expect_gateway_mute_(gw_user_muted_);
  for (size_t i = 0; i < n_slots_; i++)  // read the lamps' sound state back
    if (slots_[i].addr) link_.speaker_state(slots_[i].addr, 2000);
}

#ifdef USE_MEDIA_PLAYER
void SengledPulse::set_auto_mute(media_player::MediaPlayer *player, uint32_t delay_ms) {
  player_ = player;
  auto_mute_delay_ms_ = delay_ms;
  player->add_on_state_callback([this](media_player::MediaPlayerState) { check_playing_(); });
}
#endif

// The delay keeps short gaps between tracks quiet for the speakers without
// toggling them.
void SengledPulse::check_playing_() {
#ifdef USE_MEDIA_PLAYER
  if (player_ == nullptr || !boot_done_) return;
  auto st = player_->state;
  if (st == media_player::MEDIA_PLAYER_STATE_PLAYING || st == media_player::MEDIA_PLAYER_STATE_ANNOUNCING) {
    cancel_timeout("auto_mute");
    auto_unmute();
    return;
  }
  set_timeout("auto_mute", auto_mute_delay_ms_, [this]() { auto_mute(); });
#endif
}

// ============================================================================
// add / remove lamps
//
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

enum Step : uint8_t {
  IDLE,
  A_QUERY_CC,
  A_WAIT_CC,
  A_SET_ZERO,
  A_WAIT_ZERO,
  A_CHECK_ZERO,
  A_SET_CC,
  A_WAIT_SET_CC,
  A_CHECK_CC,
  A_HANDSHAKE,
  A_WAIT_HANDSHAKE,
  A_WAIT_ADDED,
  D_HANDSHAKE,
  D_WAIT_HANDSHAKE,
  D_WAIT_DONE,
};

static constexpr uint32_t SETTLE_MS = 8500;  // the app waits 8.5 s after a code change
static constexpr uint32_t REPLY_MS = 3000;   // the app's reply timeout
static constexpr uint8_t TRIES = 3;

void SengledPulse::set_status_(const std::string &s) {
  ESP_LOGI(TAG, "pairing: %s", s.c_str());
  if (pairing_status_ != nullptr) pairing_status_->publish_state(s);
}

void SengledPulse::pairing_go_(uint8_t step) {
  step_ = step;
  t0_ = millis();
  got_reply_ = false;
}

void SengledPulse::pairing_fail_(const std::string &why, bool restore_cc) {
  if (restore_cc && cc_ != 0) link_.set_connect_code(MASTER, cc_);
  set_status_("failed: " + why + " (" + hex8(target_) + ")");
  step_ = IDLE;
  link_.enum_slaves();
}

void SengledPulse::start_add(const std::string &addr_text) {
  if (step_ != IDLE) return set_status_("busy");
  if (!parse_addr(addr_text, target_)) return set_status_("bad address: " + addr_text);
  cc_ = 0;
  tries_ = hs_tries_ = 0;
  set_status_("adding " + hex8(target_) + ": reading gateway code");
  pairing_go_(A_QUERY_CC);
}

void SengledPulse::start_remove(const std::string &addr_text) {
  if (step_ != IDLE) return set_status_("busy");
  if (!parse_addr(addr_text, target_)) return set_status_("bad address: " + addr_text);
  tries_ = 0;
  set_status_("removing " + hex8(target_));
  pairing_go_(D_HANDSHAKE);
}

void SengledPulse::pairing_loop_() {
  uint32_t el = millis() - t0_;
  const std::string t = hex8(target_);
  switch (step_) {
    case IDLE:
      break;

    // --- add ---
    case A_QUERY_CC:
      link_.query_connect_code();
      pairing_go_(A_WAIT_CC);
      break;
    case A_WAIT_CC:
      if (got_reply_) {
        if (reply_code_ == 0 || reply_code_ == UNBOUND_CODE) {
          // the app would invent a new code and drop all lamps here; don't
          pairing_fail_("gateway code is " + hex8(reply_code_) + ", fix it in the app first", false);
          break;
        }
        cc_ = reply_code_;
        tries_ = 0;
        set_status_("adding " + t + ": gateway code " + hex8(cc_) + ", opening network");
        pairing_go_(A_SET_ZERO);
      } else if (el > REPLY_MS) {
        pairing_fail_("no reply from gateway", false);
      }
      break;
    case A_SET_ZERO:
      link_.set_connect_code(MASTER, 0);
      pairing_go_(A_WAIT_ZERO);
      break;
    case A_WAIT_ZERO:
      if (el > SETTLE_MS) {
        link_.query_connect_code();
        pairing_go_(A_CHECK_ZERO);
      }
      break;
    case A_CHECK_ZERO:
      if (got_reply_ && reply_code_ == 0) {
        tries_ = 0;
        set_status_("adding " + t + ": writing code to lamp");
        pairing_go_(A_SET_CC);
      } else if (got_reply_ || el > REPLY_MS) {
        if (++tries_ >= TRIES)
          pairing_fail_("gateway did not accept code 0", true);
        else
          pairing_go_(A_SET_ZERO);
      }
      break;
    case A_SET_CC:
      link_.set_connect_code(target_, cc_);
      link_.set_connect_code(MASTER, cc_);
      pairing_go_(A_WAIT_SET_CC);
      break;
    case A_WAIT_SET_CC:
      if (el > SETTLE_MS) {
        link_.query_connect_code();
        pairing_go_(A_CHECK_CC);
      }
      break;
    case A_CHECK_CC:
      if (got_reply_ && reply_code_ == cc_) {
        tries_ = 0;
        set_status_("adding " + t + ": handshake");
        pairing_go_(A_HANDSHAKE);
      } else if (got_reply_ || el > REPLY_MS) {
        if (++tries_ >= TRIES)
          pairing_fail_("gateway code not restored", true);
        else
          pairing_go_(A_SET_CC);
      }
      break;
    case A_HANDSHAKE:
      link_.handshake(target_);
      pairing_go_(A_WAIT_HANDSHAKE);
      break;
    case A_WAIT_HANDSHAKE:
      if (got_reply_) {
        link_.set_id_address(target_, false);
        set_status_("adding " + t + ": adding to gateway");
        pairing_go_(A_WAIT_ADDED);
      } else if (el > REPLY_MS) {
        if (++hs_tries_ >= TRIES) {
          pairing_fail_("lamp does not answer (power it on, close to the gateway)", true);
        } else {
          tries_ = 0;
          set_status_("adding " + t + ": no handshake, retry " + std::to_string(hs_tries_));
          pairing_go_(A_SET_ZERO);  // the app restarts from "open network"
        }
      }
      break;
    case A_WAIT_ADDED:
      if (el > REPLY_MS) {
        link_.enum_slaves();
        set_status_("added " + t);
        step_ = IDLE;
      }
      break;

    // --- remove ---
    case D_HANDSHAKE:
      link_.handshake(target_);
      pairing_go_(D_WAIT_HANDSHAKE);
      break;
    case D_WAIT_HANDSHAKE:
      if (got_reply_) {
        link_.set_connect_code(target_, UNBOUND_CODE);
        link_.set_id_address(target_, true);
        pairing_go_(D_WAIT_DONE);
      } else if (el > REPLY_MS) {
        if (++tries_ >= 2) {
          // lamp is gone: just drop it from the gateway table
          link_.set_id_address(target_, true);
          pairing_go_(D_WAIT_DONE);
        } else {
          pairing_go_(D_HANDSHAKE);
        }
      }
      break;
    case D_WAIT_DONE:
      if (el > 1000) {
        link_.enum_slaves();
        set_status_("removed " + t + (tries_ >= 2 ? " (lamp did not answer, only removed from gateway)" : ""));
        step_ = IDLE;
      }
      break;
  }
}

}  // namespace esphome::sengled_pulse
