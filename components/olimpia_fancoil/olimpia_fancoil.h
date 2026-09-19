#pragma once

#include <cmath>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome {
namespace olimpia_fancoil {

// ---------------------------------------------------------------------------
// Bi2 fan coil spoken to DIRECTLY - no B0736 thermostat in the path.
//
// Decoded from capture-b1130.log: a B1130 WiFi kit polling this coil at
// address 1 every 2.82 s, plus writes observed live while changing setpoint,
// fan speed and on/off in the Olimpia app.
//
// DO NOT confuse this with the olimpia_b0736 component. That one talks to a
// B0736 thermostat's supervisor port, and although several register NUMBERS
// coincide the ENCODINGS do not:
//
//     setpoint      here reg 231 = degC x10        (220 = 22.0)
//                 B0736 reg 231 = (degC-15.5)*2    (13  = 22.0)
//     min/max      here regs 202/203
//                 B0736 regs 245/246
//
// Using the B0736 encoder here would command wildly wrong temperatures.
// ---------------------------------------------------------------------------
static const uint16_t REG_AMBIENT = 0;     // R   ambient temperature, x0.1 degC
static const uint16_t REG_WATER = 1;       // R   water temperature, x0.1 degC (per community map, unverified here)
static const uint16_t REG_STATUS = 9;      // R   b14 EV1, b13 boiler, b12 chiller, b11 EV2
static const uint16_t REG_FAN_TARGET = 15; // R   fan speed target (community map, unverified here)
static const uint16_t REG_FAN_ACTUAL = 16; // R   actual fan speed (community map, unverified here)
static const uint16_t REG_ADDRESS = 200;   // R/W slave address, EEPROM - never written here
static const uint16_t REG_CONTROL = 201;   // R/W b0-b2 fan (0 auto, 1 min, 2 night, 3 max), b7 standby
static const uint16_t REG_SETPOINT_MIN = 202;  // R   minimum setpoint, x0.1 degC
static const uint16_t REG_SETPOINT_MAX = 203;  // R   maximum setpoint, x0.1 degC
static const uint16_t REG_SETPOINT = 231;  // R/W setpoint, x0.1 degC
static const uint16_t REG_MODE = 233;      // R/W 0 auto, 3 heating, 5 cooling, 7 fan only

// A disconnected probe reports 0x7FFF. It means "no reading", not 3276.7 degC.
static const uint16_t TEMP_INVALID = 0x7FFF;

static const uint16_t CTRL_STANDBY = 0x0080;
static const uint16_t CTRL_FAN_MASK = 0x0007;

static const uint16_t FAN_AUTO = 0;
static const uint16_t FAN_MIN = 1;
static const uint16_t FAN_NIGHT = 2;
static const uint16_t FAN_MAX = 3;

static const uint16_t MODE_AUTO = 0;
static const uint16_t MODE_HEATING = 3;
static const uint16_t MODE_COOLING = 5;
static const uint16_t MODE_FAN_ONLY = 7;

static const uint16_t STATUS_EV2 = 0x0800;
static const uint16_t STATUS_CHILLER = 0x1000;
static const uint16_t STATUS_BOILER = 0x2000;
static const uint16_t STATUS_EV1 = 0x4000;

// Txn::addr sentinel - "use the configured slave address". 0 stays free as the
// real Modbus broadcast address.
static const uint8_t ADDR_USE_CONFIGURED = 0xFF;

class OlimpiaFancoil : public climate::Climate, public uart::UARTDevice, public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_address(uint8_t address) { this->address_ = address; }
  void set_timeout(uint32_t ms) { this->timeout_ms_ = ms; }
  void set_max_retries(uint8_t n) { this->max_retries_ = n; }

  // --- exploration services -------------------------------------------------
  void read_register(int reg);
  void write_register(int reg, int value);
  void scan_registers(int start, int count);

#ifdef USE_SENSOR
  void set_room_temperature_sensor(sensor::Sensor *s) { this->room_temperature_sensor_ = s; }
  void set_water_temperature_sensor(sensor::Sensor *s) { this->water_temperature_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_ev1_binary_sensor(binary_sensor::BinarySensor *s) { this->ev1_binary_sensor_ = s; }
  void set_ev2_binary_sensor(binary_sensor::BinarySensor *s) { this->ev2_binary_sensor_ = s; }
  void set_online_binary_sensor(binary_sensor::BinarySensor *s) { this->online_binary_sensor_ = s; }
#endif

 protected:
  void control(const climate::ClimateCall &call) override;
  climate::ClimateTraits traits() override;

  struct Txn {
    uint8_t addr;
    uint8_t fn;
    uint16_t reg;
    uint16_t val;
  };

  enum State : uint8_t { STATE_IDLE, STATE_WAITING };

  void enqueue_(uint8_t fn, uint16_t reg, uint16_t val, uint8_t addr = ADDR_USE_CONFIGURED);
  void send_next_();
  void send_frame_(const Txn &t);
  void handle_frame_();
  void on_timeout_();
  void on_register_(uint16_t reg, uint16_t value);
  void mark_offline_();

  static uint8_t lrc_(const std::vector<uint8_t> &data);
  static bool hex_to_bytes_(const std::string &s, std::vector<uint8_t> &out);

  void apply_pending_control_(uint16_t current);
  void publish_all_();

  uint8_t address_{1};
  uint32_t timeout_ms_{500};
  uint8_t max_retries_{2};

  std::vector<Txn> queue_;
  Txn current_{};
  State state_{STATE_IDLE};
  uint32_t sent_at_{0};
  uint8_t retries_{0};

  std::string rx_;
  bool collecting_{false};

  bool online_{false};
  uint8_t consecutive_failures_{0};

  bool control_valid_{false};
  uint16_t control_reg_{0};
  uint16_t mode_reg_{MODE_COOLING};
  uint16_t status_reg_{0};
  float ambient_{NAN};
  float water_{NAN};

  // Setpoint limits, read from the coil itself (regs 202/203) rather than
  // hardcoded - they are 15.0/30.0 on the captured unit but are settings.
  float setpoint_min_{15.0f};
  float setpoint_max_{30.0f};
  bool limits_known_{false};

  bool control_pending_{false};
  uint16_t ctrl_set_mask_{0};
  uint16_t ctrl_clear_mask_{0};

  std::vector<uint16_t> manual_reads_;

#ifdef USE_SENSOR
  sensor::Sensor *room_temperature_sensor_{nullptr};
  sensor::Sensor *water_temperature_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *ev1_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *ev2_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *online_binary_sensor_{nullptr};
#endif
};

}  // namespace olimpia_fancoil
}  // namespace esphome
