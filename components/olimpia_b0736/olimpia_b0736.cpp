#include "olimpia_b0736.h"

#include <cmath>
#include <cstdio>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

namespace esphome {
namespace olimpia_b0736 {

static const char *const TAG = "olimpia_b0736";

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

uint8_t OlimpiaB0736::lrc_(const std::vector<uint8_t> &data) {
  uint8_t sum = 0;
  for (uint8_t b : data)
    sum = static_cast<uint8_t>(sum + b);
  return static_cast<uint8_t>(-static_cast<int8_t>(sum));
}

bool OlimpiaB0736::hex_to_bytes_(const std::string &s, std::vector<uint8_t> &out) {
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

static float decode_setpoint(uint16_t raw) { return 15.5f + 0.5f * static_cast<float>(raw); }

static uint16_t encode_setpoint(float degc) {
  int v = static_cast<int>(lroundf((degc - 15.5f) * 2.0f));
  if (v < 1)
    v = 1;  // 16.0 degC
  if (v > 25)
    v = 25;  // 28.0 degC
  return static_cast<uint16_t>(v);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

void OlimpiaB0736::setup() {
  this->mode = climate::CLIMATE_MODE_OFF;
  this->fan_mode = climate::CLIMATE_FAN_AUTO;
  this->target_temperature = NAN;
  this->current_temperature = NAN;
  this->action = climate::CLIMATE_ACTION_OFF;

#ifdef USE_SENSOR
  // An external probe pushes at its own rate (a BLE sensor advertises every
  // few seconds at best), so latch its value and the time it arrived rather
  // than reading the sensor inline - that is what makes staleness detectable.
  if (this->external_temperature_sensor_ != nullptr) {
    this->external_temperature_sensor_->add_on_state_callback([this](float v) {
      if (std::isnan(v))
        return;
      this->external_temperature_ = v;
      this->external_temperature_at_ = millis();
      if (this->external_stale_logged_) {
        ESP_LOGI(TAG, "external probe back: %.1f degC", v);
        this->external_stale_logged_ = false;
      }
    });
  }
#endif
}

void OlimpiaB0736::dump_config() {
  ESP_LOGCONFIG(TAG, "Olimpia B0736 supervisor:");
  ESP_LOGCONFIG(TAG, "  Slave address: %u", this->address_);
  ESP_LOGCONFIG(TAG, "  Reply timeout: %u ms", this->timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Max retries: %u", this->max_retries_);
  this->check_uart_settings(9600, 1, uart::UART_CONFIG_PARITY_EVEN, 7);
}

// ---------------------------------------------------------------------------
// polling
// ---------------------------------------------------------------------------

void OlimpiaB0736::update() {
  // While the link is down, do not queue the whole poll set: each dead
  // transaction costs timeout x retries, which would outrun update_interval
  // and build a backlog. Just knock on the door.
  if (!this->session_ok_) {
    // Knock with the IDENT read (reg 199) - the handshake that opens a session
    // with the B0736. This is the exact request the working unit uses; the
    // address sweep was removed because it never sent this and so never
    // connected.
    this->queue_.clear();
    this->enqueue_(0x03, REG_IDENT, 1);
    return;
  }

  // Do not pile up work if the bus is slow.
  if (this->queue_.size() > 8) {
    ESP_LOGW(TAG, "Queue backed up (%u), skipping poll", (unsigned) this->queue_.size());
    return;
  }

  this->enqueue_(0x03, REG_ROOM_TEMP, 1);
  this->enqueue_(0x03, REG_CONTROL, 1);
  this->enqueue_(0x03, REG_SETPOINT, 1);
  this->enqueue_(0x03, REG_MODE, 1);
  this->enqueue_(0x03, REG_STATUS, 1);
  this->enqueue_(0x03, REG_FAULT, 1);
}

void OlimpiaB0736::enqueue_(uint8_t fn, uint16_t reg, uint16_t val, uint8_t addr) {
  this->queue_.push_back(Txn{addr, fn, reg, val});
}

// ---------------------------------------------------------------------------
// transmit
// ---------------------------------------------------------------------------

void OlimpiaB0736::send_frame_(const Txn &t) {
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

void OlimpiaB0736::send_next_() {
  if (this->queue_.empty())
    return;
  this->current_ = this->queue_.front();
  this->queue_.erase(this->queue_.begin());
  this->retries_ = 0;
  this->rx_.clear();
  this->collecting_ = false;
  this->send_frame_(this->current_);
  // A Modbus broadcast is never answered. Waiting on it would time out, burn
  // every retry and eventually clear session_ok_, taking the whole link down
  // on a transaction that worked exactly as intended.
  if (this->current_.addr == ADDR_BROADCAST) {
    // INFO, not DEBUG: a broadcast is rare, explicitly triggered, and
    // unverifiable by any reply, so the log line is the only evidence it was
    // ever put on the wire. At DEBUG it is compiled out of a level: INFO build.
    ESP_LOGI(TAG, "broadcast fn=%02X reg=%u val=%u ON THE WIRE (no reply expected)",
             this->current_.fn, this->current_.reg, this->current_.val);
    this->state_ = STATE_IDLE;
    return;
  }
  this->state_ = STATE_WAITING;
  this->sent_at_ = millis();
}

// ---------------------------------------------------------------------------
// receive
// ---------------------------------------------------------------------------

void OlimpiaB0736::loop() {
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
    if (this->rx_.size() > 64) {  // runaway, resynchronise
      this->collecting_ = false;
      this->rx_.clear();
    }
  }

  if (this->state_ == STATE_WAITING && (millis() - this->sent_at_) > this->timeout_ms_)
    this->on_timeout_();

  if (this->state_ == STATE_IDLE)
    this->send_next_();
}

void OlimpiaB0736::handle_frame_() {
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
  // A broadcast never reaches here: send_next_() leaves the state machine IDLE
  // for one, and this function returns early unless state_ == STATE_WAITING.
  const uint8_t expect =
      this->current_.addr != ADDR_USE_CONFIGURED ? this->current_.addr : this->address_;
  if (b[0] != expect)
    return;  // not for us
  if (this->current_.addr != ADDR_USE_CONFIGURED)
    ESP_LOGI(TAG, "*** ADDRESS %u ANSWERED (reg %u) ***", b[0], this->current_.reg);

  this->consecutive_failures_ = 0;
  if (!this->online_) {
    this->online_ = true;
#ifdef USE_BINARY_SENSOR
    if (this->online_binary_sensor_ != nullptr)
      this->online_binary_sensor_->publish_state(true);
#endif
    ESP_LOGI(TAG, "B0736 online");
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
    ESP_LOGD(TAG, "Wrote reg %u = %u", this->current_.reg, this->current_.val);
    this->on_register_(this->current_.reg, this->current_.val);
  }

  this->state_ = STATE_IDLE;
}

void OlimpiaB0736::on_timeout_() {
  if (this->retries_ < this->max_retries_) {
    this->retries_++;
    ESP_LOGD(TAG, "Timeout on reg %u, retry %u", this->current_.reg, this->retries_);
    this->rx_.clear();
    this->collecting_ = false;
    this->send_frame_(this->current_);
    this->sent_at_ = millis();
    return;
  }
  ESP_LOGW(TAG, "No reply for reg %u after %u tries", this->current_.reg, this->max_retries_ + 1);
  this->state_ = STATE_IDLE;
  this->mark_offline_();
}

void OlimpiaB0736::mark_offline_() {
  if (this->consecutive_failures_ < 255)
    this->consecutive_failures_++;

  // Three dead transactions in a row: assume the supervisor session has
  // lapsed and re-open it on the next poll.
  if (this->consecutive_failures_ >= 3) {
    if (this->session_ok_ || this->online_)
      ESP_LOGW(TAG, "Session lost, will re-handshake");
    this->session_ok_ = false;
    this->control_valid_ = false;
    if (this->online_) {
      this->online_ = false;
#ifdef USE_BINARY_SENSOR
      if (this->online_binary_sensor_ != nullptr)
        this->online_binary_sensor_->publish_state(false);
#endif
    }
  }
}

// ---------------------------------------------------------------------------
// register decoding
// ---------------------------------------------------------------------------

void OlimpiaB0736::on_register_(uint16_t reg, uint16_t value) {
  // If this register was asked for by hand, report it clearly.
  for (auto it = this->manual_reads_.begin(); it != this->manual_reads_.end(); ++it) {
    if (*it == reg) {
      ESP_LOGI(TAG, "REG %u (0x%04X) = %u  0x%04X  %s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", reg, reg, value,
               value, (value & 0x8000) ? "1" : "0", (value & 0x4000) ? "1" : "0",
               (value & 0x2000) ? "1" : "0", (value & 0x1000) ? "1" : "0", (value & 0x0800) ? "1" : "0",
               (value & 0x0400) ? "1" : "0", (value & 0x0200) ? "1" : "0", (value & 0x0100) ? "1" : "0",
               (value & 0x0080) ? "1" : "0", (value & 0x0040) ? "1" : "0", (value & 0x0020) ? "1" : "0",
               (value & 0x0010) ? "1" : "0", (value & 0x0008) ? "1" : "0", (value & 0x0004) ? "1" : "0",
               (value & 0x0002) ? "1" : "0", (value & 0x0001) ? "1" : "0");
      this->manual_reads_.erase(it);
      break;
    }
  }

  switch (reg) {
    case REG_IDENT:
      ESP_LOGI(TAG, "Session open (ident = %u)", value);
      this->session_ok_ = true;
      break;

    case REG_ROOM_TEMP: {
      float t = static_cast<float>(value) * 0.1f;
      // current_temperature is chosen in publish_all_(), which may prefer an
      // external probe over this; the B0736's own reading is always kept and
      // always published on its own sensor.
      this->b0736_temperature_ = t;
#ifdef USE_SENSOR
      if (this->room_temperature_sensor_ != nullptr)
        this->room_temperature_sensor_->publish_state(t);
#endif
      break;
    }

    case REG_CONTROL:
      this->control_reg_ = value;
      this->control_valid_ = true;
      if (this->control_pending_)
        this->apply_pending_control_(value);
      break;

    case REG_SETPOINT:
      this->setpoint_reg_ = value;
      this->target_temperature = decode_setpoint(value);
      break;

    case REG_MODE:
      this->mode_reg_ = value;
      break;

    case REG_STATUS:
      this->status_reg_ = value;
#ifdef USE_BINARY_SENSOR
      if (this->chiller_binary_sensor_ != nullptr)
        this->chiller_binary_sensor_->publish_state((value & STATUS_CHILLER) != 0);
      if (this->boiler_binary_sensor_ != nullptr)
        this->boiler_binary_sensor_->publish_state((value & STATUS_BOILER) != 0);
#endif
      break;

    case REG_FAULT:
      this->fault_reg_ = value;
#ifdef USE_BINARY_SENSOR
      if (this->fault_binary_sensor_ != nullptr)
        this->fault_binary_sensor_->publish_state(value != 0);
#endif
      this->publish_all_();  // last register of the poll batch
      break;

    default:
      break;
  }
}

void OlimpiaB0736::publish_all_() {
  const bool standby = (this->control_reg_ & CTRL_STANDBY) != 0;

  // Pick what to report as the current temperature. The B0736 regulates on its
  // own sensor regardless - this only changes what Home Assistant sees.
  this->current_temperature = this->b0736_temperature_;
#ifdef USE_SENSOR
  if (this->external_temperature_sensor_ != nullptr && !std::isnan(this->external_temperature_)) {
    const uint32_t age = millis() - this->external_temperature_at_;
    if (age <= this->external_timeout_ms_) {
      this->current_temperature = this->external_temperature_;
    } else if (!this->external_stale_logged_) {
      ESP_LOGW(TAG, "external probe stale (%u s), falling back to B0736 reg 0", age / 1000);
      this->external_stale_logged_ = true;
    }
  }
#endif

  if (standby) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else {
    this->mode = (this->mode_reg_ == MODE_HEATING) ? climate::CLIMATE_MODE_HEAT : climate::CLIMATE_MODE_COOL;
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
  } else {
    this->action = climate::CLIMATE_ACTION_IDLE;
  }

  this->publish_state();
}

// ---------------------------------------------------------------------------
// control
// ---------------------------------------------------------------------------

void OlimpiaB0736::apply_pending_control_(uint16_t current) {
  uint16_t next = static_cast<uint16_t>((current & ~this->ctrl_clear_mask_) | this->ctrl_set_mask_);
  this->control_pending_ = false;
  this->ctrl_set_mask_ = 0;
  this->ctrl_clear_mask_ = 0;
  if (next != current)
    this->enqueue_(0x06, REG_CONTROL, next);
}

void OlimpiaB0736::control(const climate::ClimateCall &call) {
  uint16_t set_mask = 0;
  uint16_t clear_mask = 0;
  bool touch_control = false;

  if (call.get_mode().has_value()) {
    const climate::ClimateMode m = *call.get_mode();
    if (m == climate::CLIMATE_MODE_OFF) {
      set_mask |= CTRL_STANDBY;
    } else {
      clear_mask |= CTRL_STANDBY;
      const uint16_t want = (m == climate::CLIMATE_MODE_HEAT) ? MODE_HEATING : MODE_COOLING;
      if (want != this->mode_reg_) {
        this->enqueue_(0x06, REG_MODE, want);
        this->mode_reg_ = want;
      }
    }
    this->mode = m;
    touch_control = true;
  }

  if (call.get_fan_mode().has_value()) {
    uint16_t prg = FAN_AUTO;
    switch (*call.get_fan_mode()) {
      case climate::CLIMATE_FAN_LOW:
        prg = FAN_MIN;
        break;
      case climate::CLIMATE_FAN_QUIET:
        prg = FAN_NIGHT;
        break;
      case climate::CLIMATE_FAN_HIGH:
        prg = FAN_MAX;
        break;
      default:
        prg = FAN_AUTO;
        break;
    }
    clear_mask |= CTRL_FAN_MASK;
    set_mask |= prg;
    this->fan_mode = *call.get_fan_mode();
    touch_control = true;
  }

  if (call.get_target_temperature().has_value()) {
    const float t = *call.get_target_temperature();
    const uint16_t raw = encode_setpoint(t);
    this->enqueue_(0x06, REG_SETPOINT, raw);
    this->setpoint_reg_ = raw;
    this->target_temperature = decode_setpoint(raw);
  }

  if (touch_control) {
    // Read-modify-write: bits outside the fan and standby fields are not
    // understood, so they must be preserved rather than overwritten.
    this->ctrl_set_mask_ |= set_mask;
    this->ctrl_clear_mask_ |= clear_mask;
    if (this->control_valid_) {
      this->apply_pending_control_(this->control_reg_);
    } else {
      this->control_pending_ = true;
      this->enqueue_(0x03, REG_CONTROL, 1);
    }
  }

  this->publish_state();
}

// ---------------------------------------------------------------------------
// exploration services
// ---------------------------------------------------------------------------

void OlimpiaB0736::read_register(int reg) {
  if (reg < 0 || reg > 0xFFFF) {
    ESP_LOGW(TAG, "read_register: %d out of range", reg);
    return;
  }
  ESP_LOGI(TAG, "read_register(%d) queued", reg);
  this->manual_reads_.push_back(static_cast<uint16_t>(reg));
  this->enqueue_(0x03, static_cast<uint16_t>(reg), 1);
}

void OlimpiaB0736::write_register(int reg, int value) {
  if (reg < 0 || reg > 0xFFFF || value < 0 || value > 0xFFFF) {
    ESP_LOGW(TAG, "write_register: %d/%d out of range", reg, value);
    return;
  }
  // Registers 200 and above live in EEPROM with limited write endurance.
  if (reg >= 200 && reg <= 200) {
    ESP_LOGW(TAG, "register %d is the slave address in EEPROM - refusing", reg);
    return;
  }
  ESP_LOGI(TAG, "write_register(%d, %d = 0x%04X) queued", reg, value, value);
  this->enqueue_(0x06, static_cast<uint16_t>(reg), static_cast<uint16_t>(value));
  this->manual_reads_.push_back(static_cast<uint16_t>(reg));
  this->enqueue_(0x03, static_cast<uint16_t>(reg), 1);  // read back
}

void OlimpiaB0736::write_register_broadcast(int reg, int value) {
  if (reg < 0 || reg > 0xFFFF || value < 0 || value > 0xFFFF) {
    ESP_LOGW(TAG, "write_register_broadcast: %d/%d out of range", reg, value);
    return;
  }
  if (reg >= 200) {
    ESP_LOGW(TAG, "register %d is EEPROM - refusing to broadcast-write it", reg);
    return;
  }
  ESP_LOGI(TAG, "BROADCAST write reg %d = %d (0x%04X)", reg, value, value);
  this->enqueue_(0x06, static_cast<uint16_t>(reg), static_cast<uint16_t>(value), ADDR_BROADCAST);
}

void OlimpiaB0736::scan_registers(int start, int count) {
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

void OlimpiaB0736::set_control_bit(int bit, bool value) {
  if (bit < 0 || bit > 15) {
    ESP_LOGW(TAG, "set_control_bit: bit %d out of range", bit);
    return;
  }
  const uint16_t mask = static_cast<uint16_t>(1u << bit);
  ESP_LOGI(TAG, "control bit %d -> %s", bit, value ? "1" : "0");
  if (value) {
    this->ctrl_set_mask_ |= mask;
  } else {
    this->ctrl_clear_mask_ |= mask;
  }
  if (this->control_valid_) {
    this->apply_pending_control_(this->control_reg_);
  } else {
    this->control_pending_ = true;
    this->enqueue_(0x03, REG_CONTROL, 1);
  }
}

climate::ClimateTraits OlimpiaB0736::traits() {
  auto traits = climate::ClimateTraits();
  // ESPHome 2025.11 replaced the boolean set_supports_* accessors with a
  // feature-flag bitmask; the old shims were removed in 2026.5.
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
  });
  traits.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_QUIET,
      climate::CLIMATE_FAN_HIGH,
  });
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(28.0f);
  traits.set_visual_target_temperature_step(0.5f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

}  // namespace olimpia_b0736
}  // namespace esphome
