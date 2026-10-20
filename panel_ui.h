#pragma once
// Screen-side table for basement-panel.yaml: the fan coils it shows, and the
// Home Assistant entities that carry their state.
//
// The panel talks to NO fan coil directly. Every value comes from Home
// Assistant (homeassistant sensors on the climate entities) and every command
// goes back through HA as a climate.* action. So this table is the whole
// mapping between a row on the screen and a coil in the house.

#include <array>
#include <cmath>
#include <string>

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace panel {

struct Coil {
  const char *entity;  // climate.* entity id in HA
  const char *label;   // shown on the screen
  bool fan_only;       // offers hvac_mode fan_only (olimpia_fancoil does; olimpia_b0736 does not)
  esphome::text_sensor::TextSensor *mode = nullptr;    // entity state: off/cool/heat/fan_only/auto
  esphome::text_sensor::TextSensor *action = nullptr;  // attribute hvac_action
  esphome::text_sensor::TextSensor *fan = nullptr;     // attribute fan_mode
  esphome::sensor::Sensor *current = nullptr;          // attribute current_temperature
  esphome::sensor::Sensor *target = nullptr;           // attribute temperature
};

static const int N = 6;

inline std::array<Coil, N> &coils() {
  static std::array<Coil, N> c{{
      {"climate.master_bedroom_fcu_fan_coils", "Master bedroom", false},
      {"climate.guest_room_fcu_fan_coils", "Guest room", false},
      {"climate.first_floor_fcu_fan_coils", "First floor", false},
      {"climate.loft_fcu_fan_coil", "Loft", true},
      {"climate.basement_fcu_1_fan_coil", "Basement", true},
      // The T-CAN485 board (olimpia.yaml, not on this branch). Its node is
      // still named olimpia-sniffer - a copy-paste leftover - hence the id.
      {"climate.olimpia_sniffer_fan_coils", "Living room", false},
  }};
  return c;
}

inline std::string text(esphome::text_sensor::TextSensor *t) {
  return (t != nullptr && t->has_state()) ? t->state : std::string();
}

inline float num(esphome::sensor::Sensor *s) {
  return (s != nullptr && s->has_state()) ? s->state : NAN;
}

// HA reports "unavailable" when the coil's own ESPHome node is down.
inline bool available(const Coil &c) {
  std::string m = text(c.mode);
  return !m.empty() && m != "unavailable" && m != "unknown";
}

inline std::string temp(float t) {
  char buf[16];
  if (std::isnan(t))
    return "--.-°";
  snprintf(buf, sizeof(buf), "%.1f°", t);
  return buf;
}

inline const char *mode_label(const std::string &m) {
  if (m == "cool") return "Cool";
  if (m == "heat") return "Heat";
  if (m == "fan_only") return "Fan";
  if (m == "auto") return "Auto";
  if (m == "off") return "Off";
  return "Offline";
}

// What the coil is doing, which can differ from what it was asked to do: a
// coil in "cool" whose action is "idle" is waiting (setpoint met, or water not
// ready on the olimpia_fancoil units).
inline std::string status_line(const Coil &c) {
  if (!available(c))
    return "Offline";
  std::string a = text(c.action);
  std::string m = text(c.mode);
  if (m == "off")
    return "Off";
  if (a == "cooling") return "Cooling";
  if (a == "heating") return "Heating";
  if (a == "fan") return "Fan running";
  // ASCII only: the built-in Montserrat fonts have no "·" and draw a box.
  return std::string(mode_label(m)) + " (idle)";
}

// One accent per mode - the same palette as the loft screen. Off is RED so a
// switched-off coil stands out; offline is a neutral grey (the row also says
// "Offline"), so the two are never confused.
static const uint32_t COLOR_OFF = 0xE53935;
static const uint32_t COLOR_OFFLINE = 0x6B7480;

inline uint32_t accent(const Coil &c) {
  if (!available(c))
    return COLOR_OFFLINE;
  std::string m = text(c.mode);
  if (m == "cool") return 0x2F9BFF;
  if (m == "heat") return 0xFF7043;
  if (m == "fan_only") return 0x26C6A6;
  if (m == "off") return COLOR_OFF;
  return 0x6B7480;
}

}  // namespace panel
