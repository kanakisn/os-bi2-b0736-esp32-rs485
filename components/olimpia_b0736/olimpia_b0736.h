#pragma once

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
namespace olimpia_b0736 {

// ---------------------------------------------------------------------------
// Register map of the B0736 acting as a Modbus slave on its A'/B' port.
// Derived by capturing traffic between a B1130 WiFi kit and the thermostat.
// ---------------------------------------------------------------------------
static const uint16_t REG_ROOM_TEMP = 0;    // R   room temperature, x0.1 degC
static const uint16_t REG_STATUS = 9;       // R   b14 EV1, b13 boiler, b12 chiller, b11 EV2
static const uint16_t REG_FAULT = 105;      // R   0 = healthy
static const uint16_t REG_IDENT = 199;      // R   constant 1200, read once per session
static const uint16_t REG_CONTROL = 201;    // R/W b2..b0 fan, b7 standby
static const uint16_t REG_SETPOINT = 231;   // R/W (degC - 15.5) * 2
static const uint16_t REG_MODE = 233;       // R/W 3 = heating, 5 = cooling

static const uint16_t CTRL_STANDBY = 0x0080;
static const uint16_t CTRL_FAN_MASK = 0x0007;

static const uint16_t FAN_AUTO = 0;
static const uint16_t FAN_MIN = 1;
static const uint16_t FAN_NIGHT = 2;
static const uint16_t FAN_MAX = 3;

static const uint16_t MODE_HEATING = 3;
static const uint16_t MODE_COOLING = 5;

static const uint16_t STATUS_EV2 = 0x0800;
static const uint16_t STATUS_CHILLER = 0x1000;
static const uint16_t STATUS_BOILER = 0x2000;
static const uint16_t STATUS_EV1 = 0x4000;

class OlimpiaB0736 : public climate::Climate, public uart::UARTDevice, public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_address(uint8_t address) { this->address_ = address; }
  void set_timeout(uint32_t ms) { this->timeout_ms_ = ms; }
  void set_max_retries(uint8_t n) { this->max_retries_ = n; }

#ifdef USE_SENSOR
  void set_room_temperature_sensor(sensor::Sensor *s) { this->room_temperature_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_fault_binary_sensor(binary_sensor::BinarySensor *s) { this->fault_binary_sensor_ = s; }
  void set_chiller_binary_sensor(binary_sensor::BinarySensor *s) { this->chiller_binary_sensor_ = s; }
  void set_boiler_binary_sensor(binary_sensor::BinarySensor *s) { this->boiler_binary_sensor_ = s; }
  void set_online_binary_sensor(binary_sensor::BinarySensor *s) { this->online_binary_sensor_ = s; }
#endif

 protected:
  void control(const climate::ClimateCall &call) override;
  climate::ClimateTraits traits() override;

  // --- transaction plumbing -------------------------------------------------
  struct Txn {
    uint8_t fn;
    uint16_t reg;
    uint16_t val;  // FC03: quantity, FC06: value to write
  };

  enum State : uint8_t { STATE_IDLE, STATE_WAITING };

  void enqueue_(uint8_t fn, uint16_t reg, uint16_t val);
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

  bool session_ok_{false};
  bool online_{false};
  uint8_t consecutive_failures_{0};

  // cached register values
  bool control_valid_{false};
  uint16_t control_reg_{0};
  uint16_t mode_reg_{MODE_COOLING};
  uint16_t setpoint_reg_{0};
  uint16_t status_reg_{0};
  uint16_t fault_reg_{0};

  // deferred read-modify-write of the control register
  bool control_pending_{false};
  uint16_t ctrl_set_mask_{0};
  uint16_t ctrl_clear_mask_{0};

#ifdef USE_SENSOR
  sensor::Sensor *room_temperature_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *fault_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *chiller_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *boiler_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *online_binary_sensor_{nullptr};
#endif
};

}  // namespace olimpia_b0736
}  // namespace esphome
