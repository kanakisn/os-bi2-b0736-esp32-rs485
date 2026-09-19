#include "olimpia_fancoil.h"

#include <cmath>
#include <cstdio>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

namespace esphome {
namespace olimpia_fancoil {

static const char *const TAG = "olimpia_fancoil";

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

uint8_t OlimpiaFancoil::lrc_(const std::vector<uint8_t> &data) {
  uint8_t sum = 0;
  for (uint8_t b : data)
    sum = static_cast<uint8_t>(sum + b);
  return static_cast<uint8_t>(-static_cast<int8_t>(sum));
}

bool OlimpiaFancoil::hex_to_bytes_(const std::string &s, std::vector<uint8_t> &out) {
  if (s.size() < 2 || (s.size() % 2) != 0)
    return false;
  out.clear();
  for (size_t i = 0; i < s.size(); i += 2) {
    char buf[3] = {s[i], s[i + 1], 0};
    char *end = nullptr;
    long v = strtol(buf, &end, 16);
    if (end != buf + 2)
      return false;
    out.push_back(static_cast<uint8_t>(v));
  }
  return true;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

void OlimpiaFancoil::setup() {
  this->mode = climate::CLIMATE_MODE_OFF;
  this->fan_mode = climate::CLIMATE_FAN_AUTO;
  this->target_temperature = NAN;
  this->current_temperature = NAN;
  this->action = climate::CLIMATE_ACTION_OFF;
}

void OlimpiaFancoil::dump_config() {
  ESP_LOGCONFIG(TAG, "Olimpia Bi2 fan coil (direct, no B0736):");
  ESP_LOGCONFIG(TAG, "  Slave address: %u", this->address_);
  ESP_LOGCONFIG(TAG, "  Reply timeout: %u ms", this->timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Max retries: %u", this->max_retries_);
  ESP_LOGCONFIG(TAG, "  Setpoint limits: %.1f - %.1f degC%s", this->setpoint_min_, this->setpoint_max_,
                this->limits_known_ ? " (read from regs 202/203)" : " (defaults, not read yet)");
  this->check_uart_settings(9600, 1, uart::UART_CONFIG_PARITY_EVEN, 7);
}

// ---------------------------------------------------------------------------
// polling
// ---------------------------------------------------------------------------

void OlimpiaFancoil::update() {
  // Unlike the B0736 there is no IDENT handshake to open a session - the B1130
  // simply starts polling, so we do the same. Reg 0 answering is what marks the
  // coil online.
  if (this->queue_.size() > 8) {
    ESP_LOGW(TAG, "Queue backed up (%u), skipping poll", (unsigned) this->queue_.size());
    return;
  }

  // Setpoint limits are settings, not live values - read them once.
  if (!this->limits_known_) {
    this->enqueue_(0x03, REG_SETPOINT_MIN, 1);
    this->enqueue_(0x03, REG_SETPOINT_MAX, 1);
  }

  this->enqueue_(0x03, REG_AMBIENT, 1);
  this->enqueue_(0x03, REG_WATER, 1);
  this->enqueue_(0x03, REG_CONTROL, 1);
  this->enqueue_(0x03, REG_SETPOINT, 1);
  this->enqueue_(0x03, REG_MODE, 1);
  // REG_STATUS stays LAST: its handler is what calls publish_all_(), so adding
  // anything after it silently breaks state publishing.
  this->enqueue_(0x03, REG_STATUS, 1);
}

void OlimpiaFancoil::enqueue_(uint8_t fn, uint16_t reg, uint16_t val, uint8_t addr) {
  this->queue_.push_back(Txn{addr, fn, reg, val});
}

// ---------------------------------------------------------------------------
// transmit
// ---------------------------------------------------------------------------

void OlimpiaFancoil::send_frame_(const Txn &t) {
  std::vector<uint8_t> payload = {
      t.addr != ADDR_USE_CONFIGURED ? t.addr : this->address_,
      t.fn,
      static_cast<uint8_t>(t.reg >> 8),
      static_cast<uint8_t>(t.reg & 0xFF),
      static_cast<uint8_t>(t.val >> 8),
      static_cast<uint8_t>(t.val & 0xFF),
  };
  uint8_t lrc = lrc_(payload);

  std::string frame = ":";
  char buf[4];
  for (uint8_t b : payload) {
    snprintf(buf, sizeof(buf), "%02X", b);
    frame += buf;
  }
  snprintf(buf, sizeof(buf), "%02X", lrc);
  frame += buf;
  frame += "\r\n";

  ESP_LOGV(TAG, "TX %s", frame.substr(0, frame.size() - 2).c_str());
  this->write_str(frame.c_str());
}

void OlimpiaFancoil::send_next_() {
  if (this->queue_.empty())
    return;
  this->current_ = this->queue_.front();
  this->queue_.erase(this->queue_.begin());
  this->retries_ = 0;
  this->rx_.clear();
  this->collecting_ = false;
  this->send_frame_(this->current_);
  this->state_ = STATE_WAITING;
  this->sent_at_ = millis();
}

// ---------------------------------------------------------------------------
// receive
// ---------------------------------------------------------------------------

void OlimpiaFancoil::loop() {
  while (this->available()) {
    uint8_t c;
    if (!this->read_byte(&c))
      break;
    if (c == ':') {
      this->rx_.clear();
      this->collecting_ = true;
      continue;
    }
    if (!this->collecting_)
      continue;
    if (c == '\r')
      continue;
    if (c == '\n') {
      this->collecting_ = false;
      this->handle_frame_();
      this->rx_.clear();
      continue;
    }
    this->rx_.push_back(static_cast<char>(c));
    if (this->rx_.size() > 64) {
      this->collecting_ = false;
      this->rx_.clear();
    }
  }

  if (this->state_ == STATE_WAITING && (millis() - this->sent_at_) > this->timeout_ms_)
    this->on_timeout_();

  if (this->state_ == STATE_IDLE)
    this->send_next_();
}

void OlimpiaFancoil::handle_frame_() {
  if (this->state_ != STATE_WAITING)
    return;

  std::vector<uint8_t> b;
  if (!hex_to_bytes_(this->rx_, b) || b.size() < 4) {
    ESP_LOGW(TAG, "Malformed frame: %s", this->rx_.c_str());
    return;
  }

  std::vector<uint8_t> body(b.begin(), b.end() - 1);
  if (lrc_(body) != b.back()) {
    ESP_LOGW(TAG, "LRC mismatch: %s", this->rx_.c_str());
    return;
  }

  const uint8_t expect =
      this->current_.addr != ADDR_USE_CONFIGURED ? this->current_.addr : this->address_;
  if (b[0] != expect)
    return;  // not for us

  this->consecutive_failures_ = 0;
  if (!this->online_) {
    this->online_ = true;
#ifdef USE_BINARY_SENSOR
    if (this->online_binary_sensor_ != nullptr)
      this->online_binary_sensor_->publish_state(true);
#endif
    ESP_LOGI(TAG, "Fan coil online at address %u", expect);
  }

  const uint8_t fn = b[1];

  if (fn & 0x80) {
    ESP_LOGW(TAG, "Exception 0x%02X for reg %u", b.size() > 2 ? b[2] : 0, this->current_.reg);
    this->state_ = STATE_IDLE;
    return;
  }

  if (fn == 0x03 && b.size() >= 6) {
    const uint8_t count = b[2];
    if (count >= 2) {
      uint16_t value = (static_cast<uint16_t>(b[3]) << 8) | b[4];
      this->on_register_(this->current_.reg, value);
    }
  } else if (fn == 0x06) {
    // The coil echoes FC06 requests back verbatim - observed on the wire.
    ESP_LOGD(TAG, "Wrote reg %u = %u", this->current_.reg, this->current_.val);
    this->on_register_(this->current_.reg, this->current_.val);
  }

  this->state_ = STATE_IDLE;
}

void OlimpiaFancoil::on_timeout_() {
  if (this->retries_ < this->max_retries_) {
    this->retries_++;
    this->rx_.clear();
    this->collecting_ = false;
    this->send_frame_(this->current_);
    this->sent_at_ = millis();
    return;
  }
  ESP_LOGW(TAG, "Timeout on reg %u after %u retries", this->current_.reg, this->max_retries_);
  this->state_ = STATE_IDLE;
  if (this->consecutive_failures_ < 255)
    this->consecutive_failures_++;
  if (this->consecutive_failures_ >= 3)
    this->mark_offline_();
}

void OlimpiaFancoil::mark_offline_() {
  if (!this->online_)
    return;
  this->online_ = false;
  ESP_LOGW(TAG, "Fan coil offline");
#ifdef USE_BINARY_SENSOR
  if (this->online_binary_sensor_ != nullptr)
    this->online_binary_sensor_->publish_state(false);
#endif
}

void OlimpiaFancoil::on_register_(uint16_t reg, uint16_t value) {
  for (auto it = this->manual_reads_.begin(); it != this->manual_reads_.end(); ++it) {
    if (*it == reg) {
      ESP_LOGI(TAG, "REG %u (0x%04X) = %u  0x%04X  %u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u", reg, reg, value, value,
               (value >> 15) & 1, (value >> 14) & 1, (value >> 13) & 1, (value >> 12) & 1, (value >> 11) & 1,
               (value >> 10) & 1, (value >> 9) & 1, (value >> 8) & 1, (value >> 7) & 1, (value >> 6) & 1,
               (value >> 5) & 1, (value >> 4) & 1, (value >> 3) & 1, (value >> 2) & 1, (value >> 1) & 1,
               value & 1);
      this->manual_reads_.erase(it);
      break;
    }
  }

  switch (reg) {
    case REG_AMBIENT: {
      if (value == TEMP_INVALID) {
        if (!std::isnan(this->ambient_))
          ESP_LOGW(TAG, "ambient reads 0x7FFF - probe not connected?");
        this->ambient_ = NAN;
      } else {
        this->ambient_ = static_cast<float>(value) * 0.1f;
      }
#ifdef USE_SENSOR
      if (this->room_temperature_sensor_ != nullptr)
        this->room_temperature_sensor_->publish_state(this->ambient_);
#endif
      break;
    }

    case REG_WATER: {
      this->water_ = (value == TEMP_INVALID) ? NAN : static_cast<float>(value) * 0.1f;
#ifdef USE_SENSOR
      if (this->water_temperature_sensor_ != nullptr)
        this->water_temperature_sensor_->publish_state(this->water_);
#endif
      break;
    }

    case REG_SETPOINT_MIN:
      this->setpoint_min_ = static_cast<float>(value) * 0.1f;
      this->limits_known_ = true;
      break;

    case REG_SETPOINT_MAX:
      this->setpoint_max_ = static_cast<float>(value) * 0.1f;
      this->limits_known_ = true;
      break;

    case REG_CONTROL:
      this->control_reg_ = value;
      this->control_valid_ = true;
      if (this->control_pending_)
        this->apply_pending_control_(value);
      break;

    case REG_SETPOINT:
      // x0.1 degC written directly - NOT the B0736's (degC-15.5)*2.
      this->target_temperature = static_cast<float>(value) * 0.1f;
      break;

    case REG_MODE:
      this->mode_reg_ = value;
      break;

    case REG_STATUS:
      this->status_reg_ = value;
      this->publish_all_();
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

void OlimpiaFancoil::publish_all_() {
  const bool standby = (this->control_reg_ & CTRL_STANDBY) != 0;

  this->current_temperature = this->ambient_;

  if (standby) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else {
    switch (this->mode_reg_) {
      case MODE_HEATING:
        this->mode = climate::CLIMATE_MODE_HEAT;
        break;
      case MODE_FAN_ONLY:
        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;
      case MODE_AUTO:
        this->mode = climate::CLIMATE_MODE_AUTO;
        break;
      default:
        this->mode = climate::CLIMATE_MODE_COOL;
        break;
    }
  }

  switch (this->control_reg_ & CTRL_FAN_MASK) {
    case FAN_MIN:
      this->fan_mode = climate::CLIMATE_FAN_LOW;
      break;
    case FAN_NIGHT:
      this->fan_mode = climate::CLIMATE_FAN_QUIET;
      break;
    case FAN_MAX:
      this->fan_mode = climate::CLIMATE_FAN_HIGH;
      break;
    default:
      this->fan_mode = climate::CLIMATE_FAN_AUTO;
      break;
  }

  if (standby) {
    this->action = climate::CLIMATE_ACTION_OFF;
  } else if ((this->status_reg_ & STATUS_CHILLER) != 0) {
    this->action = climate::CLIMATE_ACTION_COOLING;
  } else if ((this->status_reg_ & STATUS_BOILER) != 0) {
    this->action = climate::CLIMATE_ACTION_HEATING;
  } else if ((this->status_reg_ & (STATUS_EV1 | STATUS_EV2)) != 0) {
    // Valves open with no explicit demand bit - the coil is circulating.
    this->action = this->mode == climate::CLIMATE_MODE_HEAT ? climate::CLIMATE_ACTION_HEATING
                                                            : climate::CLIMATE_ACTION_COOLING;
  } else {
    this->action = climate::CLIMATE_ACTION_IDLE;
  }

#ifdef USE_BINARY_SENSOR
  if (this->ev1_binary_sensor_ != nullptr)
    this->ev1_binary_sensor_->publish_state((this->status_reg_ & STATUS_EV1) != 0);
  if (this->ev2_binary_sensor_ != nullptr)
    this->ev2_binary_sensor_->publish_state((this->status_reg_ & STATUS_EV2) != 0);
#endif

  this->publish_state();
}

void OlimpiaFancoil::apply_pending_control_(uint16_t current) {
  uint16_t next = static_cast<uint16_t>((current | this->ctrl_set_mask_) & ~this->ctrl_clear_mask_);
  this->ctrl_set_mask_ = 0;
  this->ctrl_clear_mask_ = 0;
  this->control_pending_ = false;
  if (next == current)
    return;
  // Reg 201 is in the persistent block of the community map - write it on user
  // action only, never on a poll cycle.
  ESP_LOGI(TAG, "control 0x%04X -> 0x%04X", current, next);
  this->enqueue_(0x06, REG_CONTROL, next);
}

void OlimpiaFancoil::control(const climate::ClimateCall &call) {
  if (call.get_mode().has_value()) {
    const climate::ClimateMode m = *call.get_mode();
    if (m == climate::CLIMATE_MODE_OFF) {
      this->ctrl_set_mask_ |= CTRL_STANDBY;
    } else {
      this->ctrl_clear_mask_ |= CTRL_STANDBY;
      uint16_t want = MODE_COOLING;
      if (m == climate::CLIMATE_MODE_HEAT)
        want = MODE_HEATING;
      else if (m == climate::CLIMATE_MODE_FAN_ONLY)
        want = MODE_FAN_ONLY;
      else if (m == climate::CLIMATE_MODE_AUTO)
        want = MODE_AUTO;
      if (want != this->mode_reg_)
        this->enqueue_(0x06, REG_MODE, want);
    }
  }

  if (call.get_fan_mode().has_value()) {
    uint16_t fan = FAN_AUTO;
    switch (*call.get_fan_mode()) {
      case climate::CLIMATE_FAN_LOW:
        fan = FAN_MIN;
        break;
      case climate::CLIMATE_FAN_QUIET:
        fan = FAN_NIGHT;
        break;
      case climate::CLIMATE_FAN_HIGH:
        fan = FAN_MAX;
        break;
      default:
        fan = FAN_AUTO;
        break;
    }
    this->ctrl_clear_mask_ |= CTRL_FAN_MASK;
    this->ctrl_set_mask_ |= fan;
  }

  if (call.get_target_temperature().has_value()) {
    float t = *call.get_target_temperature();
    if (t < this->setpoint_min_)
      t = this->setpoint_min_;
    if (t > this->setpoint_max_)
      t = this->setpoint_max_;
    // x0.1 degC, written directly.
    const uint16_t raw = static_cast<uint16_t>(lroundf(t * 10.0f));
    ESP_LOGI(TAG, "setpoint -> %.1f degC (reg 231 = %u)", t, raw);
    this->enqueue_(0x06, REG_SETPOINT, raw);
  }

  if (this->ctrl_set_mask_ != 0 || this->ctrl_clear_mask_ != 0) {
    if (this->control_valid_) {
      this->apply_pending_control_(this->control_reg_);
    } else {
      this->control_pending_ = true;
      this->enqueue_(0x03, REG_CONTROL, 1);
    }
  }
}

// ---------------------------------------------------------------------------
// exploration services
// ---------------------------------------------------------------------------

void OlimpiaFancoil::read_register(int reg) {
  if (reg < 0 || reg > 0xFFFF) {
    ESP_LOGW(TAG, "read_register: %d out of range", reg);
    return;
  }
  ESP_LOGI(TAG, "read_register(%d) queued", reg);
  this->manual_reads_.push_back(static_cast<uint16_t>(reg));
  this->enqueue_(0x03, static_cast<uint16_t>(reg), 1);
}

void OlimpiaFancoil::write_register(int reg, int value) {
  if (reg < 0 || reg > 0xFFFF || value < 0 || value > 0xFFFF) {
    ESP_LOGW(TAG, "write_register: %d/%d out of range", reg, value);
    return;
  }
  if (reg == REG_ADDRESS) {
    ESP_LOGW(TAG, "register %d is the slave address in EEPROM - refusing", reg);
    return;
  }
  ESP_LOGI(TAG, "write_register(%d, %d = 0x%04X) queued", reg, value, value);
  this->enqueue_(0x06, static_cast<uint16_t>(reg), static_cast<uint16_t>(value));
  this->manual_reads_.push_back(static_cast<uint16_t>(reg));
  this->enqueue_(0x03, static_cast<uint16_t>(reg), 1);
}

void OlimpiaFancoil::scan_registers(int start, int count) {
  if (count < 1)
    count = 1;
  if (count > 64) {
    ESP_LOGW(TAG, "scan limited to 64 registers per call");
    count = 64;
  }
  ESP_LOGI(TAG, "scanning registers %d..%d", start, start + count - 1);
  for (int r = start; r < start + count; r++) {
    if (r < 0 || r > 0xFFFF)
      continue;
    this->manual_reads_.push_back(static_cast<uint16_t>(r));
    this->enqueue_(0x03, static_cast<uint16_t>(r), 1);
  }
}

climate::ClimateTraits OlimpiaFancoil::traits() {
  auto traits = climate::ClimateTraits();
  // 2025.11 replaced the set_supports_* accessors with a feature-flag bitmask;
  // the shims were removed in 2026.5.
#if defined(ESPHOME_VERSION_CODE) && ESPHOME_VERSION_CODE >= VERSION_CODE(2025, 11, 0)
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_ACTION);
#else
  traits.set_supports_current_temperature(true);
  traits.set_supports_action(true);
#endif
  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_FAN_ONLY,
  });
  traits.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_QUIET,
      climate::CLIMATE_FAN_HIGH,
  });
  traits.set_visual_min_temperature(this->setpoint_min_);
  traits.set_visual_max_temperature(this->setpoint_max_);
  // The coil takes x0.1 degC, so unlike the B0736 there is no 0.5 degC grid.
  traits.set_visual_target_temperature_step(0.1f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

}  // namespace olimpia_fancoil
}  // namespace esphome
