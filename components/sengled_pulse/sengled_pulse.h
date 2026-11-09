// ESPHome component for the Sengled Pulse Link (C01-BR30) gateway, connected
// to its internal UART. One hub owns the link, the lamp slots, pairing and the
// automatic mute; all entities are small classes that call back into the hub.
#pragma once

#include <array>
#include <string>

#include "esphome/components/button/button.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text/text.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#ifdef USE_MEDIA_PLAYER
#include "esphome/components/media_player/media_player.h"
#endif

#include "link.h"

namespace esphome::sengled_pulse {

static constexpr size_t MAX_SLOTS = 8;  // the gateway supports up to 8 lamps

// what an entity controls
enum class Kind : uint8_t {
  VOLUME,
  MUTE,
  EQUALIZER,
  CHANNEL,
  GATEWAY_MUTE,
  GATEWAY_EQUALIZER,
  GATEWAY_CHANNEL,
  AUDIO_SOURCE,
  LIGHT_MEMORY,
  ADD_LAMP,
  REMOVE_LAMP,
  REFRESH_NODES,
  SLOT_VOLUME,
  SLOT_MUTE,
  SLOT_CHANNEL,
  SLOT_IDENTIFY,
};

class SengledPulse;

// base of all entities: the hub, what it controls, which lamp slot (-1 = none)
class Control : public Parented<SengledPulse> {
 public:
  void set_kind(Kind kind, int slot = -1) {
    kind_ = kind;
    slot_ = slot;
  }

 protected:
  Kind kind_{Kind::VOLUME};
  int slot_{-1};
};

// "All lamps" (slot -1) or one lamp slot
class SengledLight : public light::LightOutput, public Parented<SengledPulse> {
 public:
  void set_slot(int slot) { slot_ = slot; }
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return traits;
  }
  void setup_state(light::LightState *state) override { state_ = state; }
  void write_state(light::LightState *state) override;
  // show a state read from the lamps without writing it back to them
  void show(bool on, uint8_t level);

 protected:
  int slot_{-1};
  light::LightState *state_{nullptr};
};

class SengledNumber : public number::Number, public Control {
 protected:
  void control(float value) override;
};

class SengledSwitch : public switch_::Switch, public Control {
 protected:
  void write_state(bool state) override;
};

class SengledSelect : public select::Select, public Control {
 protected:
  void control(size_t index) override;
};

class SengledButton : public button::Button, public Control {
 protected:
  void press_action() override;
};

// lamp address for add / remove, kept over reboots
class SengledText : public text::Text, public Component {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  void control(const std::string &value) override;
  ESPPreferenceObject pref_;
};

class SengledPulse : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ---- configuration (code generation) ----
  void set_slot_count(uint8_t n) { n_slots_ = n < MAX_SLOTS ? n : MAX_SLOTS; }
  void set_all_lamps(SengledLight *l) { all_lamps_ = l; }
  void set_volume(number::Number *n) { volume_ = n; }
  void set_mute(switch_::Switch *s) { mute_ = s; }
  void set_equalizer(select::Select *s) { equalizer_ = s; }
  void set_channel(select::Select *s) { channel_ = s; }
  void set_gateway_mute(switch_::Switch *s) { gw_mute_ = s; }
  void set_gateway_equalizer(select::Select *s) { gw_equalizer_ = s; }
  void set_gateway_channel(select::Select *s) { gw_channel_ = s; }
  void set_audio_source(select::Select *s) { audio_source_ = s; }
  void set_light_memory(switch_::Switch *s) { light_memory_ = s; }
  void set_lamp_address(text::Text *t) { lamp_address_ = t; }
  void set_pairing_status(text_sensor::TextSensor *t) { pairing_status_ = t; }
  void set_gateway_type(text_sensor::TextSensor *t) { gateway_type_ = t; }
  void set_gateway_nodes(text_sensor::TextSensor *t) { gateway_nodes_ = t; }
  void set_slot_light(int i, SengledLight *l) { slots_[i].light = l; }
  void set_slot_volume(int i, number::Number *n) { slots_[i].volume = n; }
  void set_slot_mute(int i, switch_::Switch *s) { slots_[i].mute = s; }
  void set_slot_channel(int i, select::Select *s) { slots_[i].channel = s; }
  void set_slot_address(int i, text_sensor::TextSensor *t) { slots_[i].address = t; }
#ifdef USE_MEDIA_PLAYER
  void set_auto_mute(media_player::MediaPlayer *player, uint32_t delay_ms);
#endif

  // ---- called by the entities ----
  void on_light(int slot, float level);
  void on_number(Kind kind, int slot, float value);
  void on_switch(Kind kind, int slot, bool state);
  void on_select(Kind kind, int slot, size_t index);
  void on_button(Kind kind, int slot);

  // ---- public API, e.g. for lambdas ----
  // Mute every speaker (all lamps and the gateway) / undo it; speakers muted
  // by hand stay muted. auto_unmute() returns whether the gateway was unmuted.
  void auto_mute();
  bool auto_unmute();
  void start_add(const std::string &addr_text);
  void start_remove(const std::string &addr_text);
  Link &link() { return link_; }

 protected:
  // per lamp slot: binding, entities, last state read from the lamp
  struct Slot {
    uint32_t addr{0};
    SengledLight *light{nullptr};
    number::Number *volume{nullptr};
    switch_::Switch *mute{nullptr};
    select::Select *channel{nullptr};
    text_sensor::TextSensor *address{nullptr};
    // light (lamp_state)
    bool light_known{false}, light_dirty{false}, on{false};
    uint8_t level{0};  // the lamp's own brightness 0..100 (the one a slot sets)
    uint32_t light_ignore_until{0};  // replies before this show the state before the last command
    // sound (speaker_state)
    bool sound_known{false}, sound_dirty{false};
    uint8_t channel_v{0}, eq_v{0}, mute_v{0}, volume_v{0};
    uint32_t sound_ignore_until{0};
    bool user_muted{false};  // muted by hand (survives the automatic mute)
  };
  using Table = std::array<uint32_t, MAX_SLOTS>;

  // frames
  void on_frame_(const Frame &f);
  void on_slot_frame_(const Frame &f);
  void on_nodes_(const std::vector<uint32_t> &nodes);
  // state shown before the lamps confirm it; -1 = keep
  void expect_light_all_(bool on, uint8_t level);
  void expect_sound_(int i, int volume, int mute, int channel, int eq);
  void expect_sound_all_(int volume, int mute, int channel, int eq);
  // publish what changed
  void publish_();
  // lamp commands
  void poll_slots_();
  void send_all_lamps_(float level);
  void one_lamp_(uint32_t addr, float level);
  bool bound_(int i) const { return i >= 0 && i < (int) n_slots_ && slots_[i].addr != 0; }
  // pairing
  void pairing_loop_();
  void set_status_(const std::string &s);
  void pairing_go_(uint8_t step);
  void pairing_fail_(const std::string &why, bool restore_cc);
  // automatic mute
  void check_playing_();

  Link link_;
  bool ready_{false};      // light writes are ignored until shortly after boot
  bool boot_done_{false};  // lamps' state known, automatic mute may act

  std::array<Slot, MAX_SLOTS> slots_{};
  uint8_t n_slots_{0};
  ESPPreferenceObject pref_;
  bool all_light_dirty_{false}, all_sound_dirty_{false};

  SengledLight *all_lamps_{nullptr};
  number::Number *volume_{nullptr};
  switch_::Switch *mute_{nullptr}, *gw_mute_{nullptr}, *light_memory_{nullptr};
  select::Select *equalizer_{nullptr}, *channel_{nullptr}, *gw_equalizer_{nullptr}, *gw_channel_{nullptr},
      *audio_source_{nullptr};
  text::Text *lamp_address_{nullptr};
  text_sensor::TextSensor *pairing_status_{nullptr}, *gateway_type_{nullptr}, *gateway_nodes_{nullptr};

  bool auto_muted_{false}, gw_user_muted_{false};
#ifdef USE_MEDIA_PLAYER
  media_player::MediaPlayer *player_{nullptr};
#endif
  uint32_t auto_mute_delay_ms_{3000};

  // pairing state machine
  uint8_t step_{0};
  uint32_t target_{0}, cc_{0}, t0_{0}, reply_code_{0};
  uint8_t tries_{0}, hs_tries_{0};
  bool got_reply_{false};
};

}  // namespace esphome::sengled_pulse
