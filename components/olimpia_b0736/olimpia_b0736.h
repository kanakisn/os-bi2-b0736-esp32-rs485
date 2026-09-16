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
namespace olimpia_b0736 {

// ---------------------------------------------------------------------------
// Register map of the B0736 acting as a Modbus slave on its A'/B' port.
// Derived by capturing traffic between a B1130 WiFi kit and the thermostat.
// ---------------------------------------------------------------------------
static const uint16_t REG_ROOM_TEMP = 0;    // R   room temperature, x0.1 degC
static const uint16_t REG_STATUS = 9;       // R   b14 EV1, b13 boiler, b12 chiller, b11 EV2
static const uint16_t REG_FAULT = 105;      // R   0 = healthy
static const uint16_t REG_IDENT = 199;      // R   constant 1200, read once per session
static const uint16_t REG_CONTROL = 201;    // R/W b2-b0 fan (0 auto, 1 min, 2 night, 3 max), b7 standby
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

// Fan-coil registers, reachable only by Modbus broadcast (address 0). The
// B0736 does not answer reads of these on its supervisor port - they are not
// its registers, they belong to the fan coils on the internal A/B bus. The
// B1130 writes all three periodically in capture.log.
static const uint16_t REG_FC_CONTROL = 101;   // W   control word
static const uint16_t REG_FC_SETPOINT = 102;  // W   setpoint, x0.1 degC
static const uint16_t REG_FC_AMBIENT = 103;   // W   ambient, x0.1 degC

// Txn::addr sentinel: "use the configured slave address". Address 0 has to
// stay available as the real Modbus broadcast address, so it cannot double as
// the "unset" marker.
static const uint8_t ADDR_USE_CONFIGURED = 0xFF;
static const uint8_t ADDR_BROADCAST = 0x00;

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

  // --- exploration services, exposed to Home Assistant ---------------------
  // Read one register and log the result.
  void read_register(int reg);
  // Write one register. No read-modify-write: the value is used verbatim.
  void write_register(int reg, int value);
  // Read a contiguous span and log every answer. Use for mapping unknowns.
  void scan_registers(int start, int count);
  // Set or clear a single bit of the control word, preserving everything else.
  void set_control_bit(int bit, bool value);
  // Write one register as a Modbus BROADCAST (address 0). Nothing replies, so
  // nothing can be verified - this is the only way to reach the fan-coil
  // registers 101/102/103, which the B0736 does not expose on its own address.
  void write_register_broadcast(int reg, int value);
  // Read/write one register at an EXPLICIT slave address, not the configured
  // one. The fan coils may sit at their own addresses on the same wire; a
  // unicast to them is answered, so unlike a broadcast it can be verified.
  // Write one register with FC16 (write multiple registers, quantity 1)
  // instead of FC06. Some slaves accept only FC16 for certain registers.
  void write_register_fc16(int reg, int value);
  void read_register_at(int addr, int reg);
  void write_register_at(int addr, int reg, int value);

#ifdef USE_SENSOR
  void set_room_temperature_sensor(sensor::Sensor *s) { this->room_temperature_sensor_ = s; }
  // Report an external probe (e.g. a BLE room sensor) as the climate entity's
  // current temperature instead of the B0736's own reg 0. The B0736 keeps
  // regulating on its internal sensor either way - this only changes what is
  // published. Falls back to reg 0 if the external sensor goes stale.
  void set_external_temperature_sensor(sensor::Sensor *s) { this->external_temperature_sensor_ = s; }
  void set_external_temperature_timeout(uint32_t ms) { this->external_timeout_ms_ = ms; }
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
    uint8_t addr;  // ADDR_USE_CONFIGURED = the configured address; 0 = broadcast
    uint8_t fn;
    uint16_t reg;
    uint16_t val;  // FC03: quantity, FC06: value to write
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

  // registers requested by hand, so their replies get logged prominently
  std::vector<uint16_t> manual_reads_;

  // Room temperature as reported by the B0736 itself (reg 0), always kept even
  // when an external probe is what gets published.
  float b0736_temperature_{NAN};

#ifdef USE_SENSOR
  sensor::Sensor *room_temperature_sensor_{nullptr};
  sensor::Sensor *external_temperature_sensor_{nullptr};
  float external_temperature_{NAN};
  uint32_t external_temperature_at_{0};
  // A BLE sensor that has stopped advertising must not freeze the reported
  // temperature forever; after this long we fall back to the B0736's own.
  uint32_t external_timeout_ms_{900000};  // 15 min
  bool external_stale_logged_{false};
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
