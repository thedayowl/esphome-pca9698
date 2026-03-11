#include "pca9698.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace pca9698 {

static const char *const TAG = "pca9698";

// ═══════════════════════════════════════════════════════════════════════════════#include "pca9698.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace pca9698 {

static const char *const TAG = "pca9698";

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698GPIOPin
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698GPIOPin::setup() {
  PCA9698PinMode mode = PCA9698PinMode::INPUT;
  if (flags_ & gpio::FLAG_OUTPUT) {
    mode = PCA9698PinMode::OUTPUT;
  } else if (flags_ & gpio::FLAG_PULLUP) {
    mode = PCA9698PinMode::INPUT_PULLUP;
  }
  parent_->set_pin_mode(pin_, mode);
}

void PCA9698GPIOPin::pin_mode(gpio::Flags flags) {
  flags_ = flags;
  PCA9698PinMode mode = (flags & gpio::FLAG_OUTPUT) ? PCA9698PinMode::OUTPUT : PCA9698PinMode::INPUT;
  parent_->set_pin_mode(pin_, mode);
}

bool PCA9698GPIOPin::digital_read() {
  return parent_->read_pin(pin_) != inverted_;
}

void PCA9698GPIOPin::digital_write(bool value) {
  parent_->write_pin(pin_, value != inverted_);
}

std::string PCA9698GPIOPin::dump_summary() const {
  char buf[32];
  snprintf(buf, sizeof(buf), "PCA9698 pin %u", pin_);
  return buf;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – setup
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up PCA9698 at address 0x%02X...", address_);

  // ── Connectivity probe ────────────────────────────────────────────────────
  // Attempt a zero-byte write to the device address to confirm it is present
  // on the bus before attempting any register operations.
  {
    uint8_t probe_reg = PCA9698_REG_INPUT_BASE | PCA9698_AI_BIT;
    uint8_t probe_data[PCA9698_NUM_PORTS];
    i2c::ErrorCode probe_err = write_read(&probe_reg, 1, probe_data, PCA9698_NUM_PORTS);
    if (probe_err != i2c::ERROR_OK) {
      ESP_LOGE(TAG, "PCA9698 at 0x%02X not responding on I2C bus (error %d). "
               "Check wiring and address pins.", address_, (int)probe_err);
      mark_failed();
      return;
    }
  }

  // Determine whether any pin is configured as input
  has_inputs_ = false;
  for (auto *pin : registered_pins_) {
    if (pin->get_flags() & gpio::FLAG_INPUT) {
      has_inputs_ = true;
      break;
    }
  }

  // By default the PCA9698 powers up with all pins as inputs.
  // We push our desired config_shadow_ which was populated by set_pin_mode()
  // calls that arrive from pin->setup() below.  Those happen in a second pass
  // after setup(), so we do an initial write with the final directions after
  // all registered pins' setup() calls have been made.

  // Initialise shadow registers
  memset(output_shadow_, 0xFF, sizeof(output_shadow_));  // default outputs HIGH
  memset(config_shadow_, 0xFF, sizeof(config_shadow_));  // default all-input
  memset(mask_shadow_,   0x00, sizeof(mask_shadow_));    // all interrupts unmasked

  // Determine interrupt usage
  if (interrupt_pin_ != nullptr && has_inputs_) {
    use_interrupt_ = true;
    ESP_LOGD(TAG, "  Interrupt pin enabled on GPIO %d", interrupt_pin_->get_pin());
  } else {
    use_interrupt_ = false;
    if (interrupt_pin_ == nullptr && has_inputs_) {
      ESP_LOGD(TAG, "  No interrupt pin – using polling every %u ms", polling_interval_ms_);
    }
  }

  // Set up OE (output-enable / dimmer) PWM output if provided
  if (oe_output_ != nullptr) {
    oe_output_->set_level(0.0f);  // OE low = outputs fully enabled
    ESP_LOGD(TAG, "  Output-enable dimmer configured");
  }

  // Run pin setup for all registered pins – this calls set_pin_mode() on us
  for (auto *pin : registered_pins_) {
    pin->setup();
  }

  // Push configuration (direction) registers to chip
  apply_config_registers_();

  // Push initial output values
  sync_output_registers_();

  // Do an initial read of inputs
  if (has_inputs_) {
    read_all_inputs_();
  }

  // Configure interrupt pin
  if (use_interrupt_) {
    interrupt_pin_->setup();
    interrupt_pin_->attach_interrupt(PCA9698Component::gpio_intr_, this, gpio::INTERRUPT_FALLING_EDGE);
  }

  initialised_ = true;
  last_poll_ms_ = millis();
  last_verify_ms_ = millis();

  ESP_LOGCONFIG(TAG, "PCA9698 at 0x%02X initialised OK", address_);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – loop
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698Component::loop() {
  if (!initialised_) return;

  uint32_t now = millis();

  // ── Interrupt-driven input read ────────────────────────────────────────────
  if (use_interrupt_ && interrupt_pending_) {
    interrupt_pending_ = false;
    handle_interrupt_();
  }

  // ── Polling input read (when no interrupt or as fallback) ─────────────────
  if (!use_interrupt_ && has_inputs_) {
    if (now - last_poll_ms_ >= polling_interval_ms_) {
      last_poll_ms_ = now;
      read_all_inputs_();
    }
  }

  // ── Periodic output verification ──────────────────────────────────────────
  if (now - last_verify_ms_ >= PCA9698_OUTPUT_VERIFY_INTERVAL_MS) {
    last_verify_ms_ = now;
    verify_output_registers_();
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – dump_config
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698Component::dump_config() {
  ESP_LOGCONFIG(TAG, "PCA9698:");
  LOG_I2C_DEVICE(this);
  if (interrupt_pin_ != nullptr) {
    LOG_PIN("  Interrupt Pin: ", interrupt_pin_);
  } else {
    ESP_LOGCONFIG(TAG, "  Interrupt Pin: not configured (polling every %u ms)", polling_interval_ms_);
  }
  if (oe_output_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Output-Enable Dimmer: configured (level %.2f)", dimmer_level_);
  }
  ESP_LOGCONFIG(TAG, "  Registered Pins: %zu", registered_pins_.size());
  for (auto *pin : registered_pins_) {
    ESP_LOGCONFIG(TAG, "    Pin %2u  dir=%s", pin->get_pin(),
                  (pin->get_flags() & gpio::FLAG_OUTPUT) ? "OUTPUT" : "INPUT");
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – Public pin API
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698Component::set_pin_mode(uint8_t pin, PCA9698PinMode mode) {
  if (pin >= PCA9698_NUM_PINS) return;
  uint8_t port   = pin / 8;
  uint8_t bit    = pin % 8;

  if (mode == PCA9698PinMode::OUTPUT) {
    config_shadow_[port] &= ~(1u << bit);  // 0 = output
  } else {
    config_shadow_[port] |= (1u << bit);   // 1 = input
    has_inputs_ = true;
  }

  // If chip is already set up, push the new direction immediately
  if (initialised_) {
    write_config_port(port, config_shadow_[port]);
    // Un-mask interrupt for input pins; mask for output pins
    if (mode == PCA9698PinMode::OUTPUT) {
      mask_shadow_[port] |= (1u << bit);
    } else {
      mask_shadow_[port] &= ~(1u << bit);
    }
    write_registers(PCA9698_REG_MASK_BASE + port, &mask_shadow_[port], 1);
  }
}

bool PCA9698Component::read_pin(uint8_t pin) {
  if (pin >= PCA9698_NUM_PINS) return false;
  uint8_t port = pin / 8;
  uint8_t bit  = pin % 8;

  // For output pins, return the shadow (last written) value
  if (!(config_shadow_[port] & (1u << bit))) {
    return (output_shadow_[port] >> bit) & 1u;
  }

  // For input pins, return cached value; a fresh read happens in loop()
  return (input_shadow_[port] >> bit) & 1u;
}

void PCA9698Component::write_pin(uint8_t pin, bool value) {
  if (pin >= PCA9698_NUM_PINS) return;
  uint8_t port = pin / 8;
  uint8_t bit  = pin % 8;

  if (value) {
    output_shadow_[port] |= (1u << bit);
  } else {
    output_shadow_[port] &= ~(1u << bit);
  }

  write_output_port(port, output_shadow_[port]);
}

void PCA9698Component::set_dimmer_level(float level) {
  dimmer_level_ = clamp(level, 0.0f, 1.0f);
  if (oe_output_ != nullptr) {
    // OE is active-low: level 0.0 → duty 0 → OE low → outputs fully enabled.
    // level 1.0 → duty 1.0 → OE high (via PWM) → outputs fully disabled.
    oe_output_->set_level(dimmer_level_);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – I2C helpers with retry
// ═══════════════════════════════════════════════════════════════════════════════

bool PCA9698Component::read_registers(uint8_t base_reg, uint8_t *data, uint8_t len) {
  // Command byte: bit7=AI (auto-increment), bits[6:3]=register bank, bits[2:0]=port.
  // The PCA9698 auto-increments bits[2:0] after each byte when AI=1, allowing
  // all 5 ports of a bank to be read in a single transaction.
  uint8_t reg = (len > 1) ? (base_reg | PCA9698_AI_BIT) : base_reg;

  for (uint8_t attempt = 0; attempt < PCA9698_MAX_RETRIES; attempt++) {
    i2c::ErrorCode err = write_read(&reg, 1, data, len);
    if (err == i2c::ERROR_OK) return true;

    ESP_LOGW(TAG, "I2C read error (reg 0x%02X, attempt %u/%u): %d",
             base_reg, attempt + 1, PCA9698_MAX_RETRIES, (int)err);
    delay(5);
  }
  return false;
}

bool PCA9698Component::write_registers(uint8_t base_reg, const uint8_t *data, uint8_t len) {
  // Command byte: AI bit set for multi-byte burst writes, clear for single writes.
  // write_register() prepends the command byte and sends [cmd, data...] in one transaction.
  uint8_t reg = (len > 1) ? (base_reg | PCA9698_AI_BIT) : base_reg;

  for (uint8_t attempt = 0; attempt < PCA9698_MAX_RETRIES; attempt++) {
    i2c::ErrorCode err = write_register(reg, data, len);
    if (err == i2c::ERROR_OK) return true;

    ESP_LOGW(TAG, "I2C write error (reg 0x%02X, attempt %u/%u): %d",
             base_reg, attempt + 1, PCA9698_MAX_RETRIES, (int)err);
    delay(5);
  }
  return false;
}

bool PCA9698Component::read_port(uint8_t port, uint8_t &value) {
  return read_registers(PCA9698_REG_INPUT_BASE + port, &value, 1);
}

bool PCA9698Component::write_output_port(uint8_t port, uint8_t value) {
  return write_registers(PCA9698_REG_OUTPUT_BASE + port, &value, 1);
}

bool PCA9698Component::write_config_port(uint8_t port, uint8_t value) {
  return write_registers(PCA9698_REG_CONFIG_BASE + port, &value, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – Shadow register helpers
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698Component::apply_config_registers_() {
  // Build interrupt mask: mask output pins (1=masked), unmask input pins (0=unmasked)
  for (uint8_t p = 0; p < PCA9698_NUM_PORTS; p++) {
    // config_shadow_: 1=input, 0=output  →  mask should be inverted (mask outputs)
    mask_shadow_[p] = ~config_shadow_[p];
  }

  if (!write_registers(PCA9698_REG_CONFIG_BASE, config_shadow_, PCA9698_NUM_PORTS)) {
    ESP_LOGE(TAG, "Failed to write configuration registers");
    return;
  }
  if (!write_registers(PCA9698_REG_MASK_BASE, mask_shadow_, PCA9698_NUM_PORTS)) {
    ESP_LOGE(TAG, "Failed to write interrupt mask registers");
  }
}

void PCA9698Component::sync_output_registers_() {
  if (!write_registers(PCA9698_REG_OUTPUT_BASE, output_shadow_, PCA9698_NUM_PORTS)) {
    ESP_LOGE(TAG, "Failed to sync output registers");
  }
}

void PCA9698Component::verify_output_registers_() {
  // Read back current output latch registers from the chip and compare
  uint8_t chip_output[PCA9698_NUM_PORTS];
  if (!read_registers(PCA9698_REG_OUTPUT_BASE, chip_output, PCA9698_NUM_PORTS)) {
    ESP_LOGW(TAG, "Output verification read failed");
    return;
  }

  bool mismatch = false;
  for (uint8_t p = 0; p < PCA9698_NUM_PORTS; p++) {
    // Only compare bits that are configured as outputs (config_shadow_ bit = 0)
    uint8_t output_mask = ~config_shadow_[p];
    if ((chip_output[p] & output_mask) != (output_shadow_[p] & output_mask)) {
      ESP_LOGW(TAG, "Output mismatch on port %u: expected 0x%02X, got 0x%02X (mask 0x%02X)",
               p, output_shadow_[p] & output_mask, chip_output[p] & output_mask, output_mask);
      mismatch = true;
    }
  }

  if (mismatch) {
    ESP_LOGW(TAG, "Correcting output register mismatch");
    sync_output_registers_();
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – Interrupt / input reading
// ═══════════════════════════════════════════════════════════════════════════════

void IRAM_ATTR PCA9698Component::gpio_intr_(PCA9698Component *arg) {
  arg->interrupt_pending_ = true;
}

void PCA9698Component::handle_interrupt_() {
  // Read all input ports in a burst read for efficiency
  read_all_inputs_();
}

void PCA9698Component::read_all_inputs_() {
  uint8_t new_input[PCA9698_NUM_PORTS];
  if (!read_registers(PCA9698_REG_INPUT_BASE, new_input, PCA9698_NUM_PORTS)) {
    ESP_LOGW(TAG, "Failed to read input registers");
    return;
  }

  for (uint8_t p = 0; p < PCA9698_NUM_PORTS; p++) {
    uint8_t input_mask = config_shadow_[p];  // 1=input bits
    uint8_t changed = (new_input[p] ^ input_shadow_[p]) & input_mask;
    if (changed) {
      ESP_LOGV(TAG, "Input change on port %u: 0x%02X → 0x%02X (changed bits: 0x%02X)",
               p, input_shadow_[p], new_input[p], changed);
      input_shadow_[p] = new_input[p];

      // Notify any binary_sensor / switch components that are polling the pin
      // ESPHome handles this automatically via GPIOBinarySensor polling digital_read()
    }
  }
}

}  // namespace pca9698
}  // namespace esphome
// PCA9698GPIOPin
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698GPIOPin::setup() {
  PCA9698PinMode mode = PCA9698PinMode::INPUT;
  if (flags_ & gpio::FLAG_OUTPUT) {
    mode = PCA9698PinMode::OUTPUT;
  } else if (flags_ & gpio::FLAG_PULLUP) {
    mode = PCA9698PinMode::INPUT_PULLUP;
  }
  parent_->set_pin_mode(pin_, mode);
}

void PCA9698GPIOPin::pin_mode(gpio::Flags flags) {
  flags_ = flags;
  PCA9698PinMode mode = (flags & gpio::FLAG_OUTPUT) ? PCA9698PinMode::OUTPUT : PCA9698PinMode::INPUT;
  parent_->set_pin_mode(pin_, mode);
}

bool PCA9698GPIOPin::digital_read() {
  return parent_->read_pin(pin_) != inverted_;
}

void PCA9698GPIOPin::digital_write(bool value) {
  parent_->write_pin(pin_, value != inverted_);
}

std::string PCA9698GPIOPin::dump_summary() const {
  char buf[32];
  snprintf(buf, sizeof(buf), "PCA9698 pin %u", pin_);
  return buf;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – setup
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up PCA9698 at address 0x%02X...", address_);

  // Determine whether any pin is configured as input
  has_inputs_ = false;
  for (auto *pin : registered_pins_) {
    if (pin->get_flags() & gpio::FLAG_INPUT) {
      has_inputs_ = true;
      break;
    }
  }

  // By default the PCA9698 powers up with all pins as inputs.
  // We push our desired config_shadow_ which was populated by set_pin_mode()
  // calls that arrive from pin->setup() below.  Those happen in a second pass
  // after setup(), so we do an initial write with the final directions after
  // all registered pins' setup() calls have been made.

  // Initialise shadow registers
  memset(output_shadow_, 0xFF, sizeof(output_shadow_));  // default outputs HIGH
  memset(config_shadow_, 0xFF, sizeof(config_shadow_));  // default all-input
  memset(mask_shadow_,   0x00, sizeof(mask_shadow_));    // all interrupts unmasked

  // Determine interrupt usage
  if (interrupt_pin_ != nullptr && has_inputs_) {
    use_interrupt_ = true;
    ESP_LOGD(TAG, "  Interrupt pin enabled on GPIO %d", interrupt_pin_->get_pin());
  } else {
    use_interrupt_ = false;
    if (interrupt_pin_ == nullptr && has_inputs_) {
      ESP_LOGD(TAG, "  No interrupt pin – using polling every %u ms", polling_interval_ms_);
    }
  }

  // Set up OE (output-enable / dimmer) PWM output if provided
  if (oe_output_ != nullptr) {
    oe_output_->set_level(0.0f);  // OE low = outputs fully enabled
    ESP_LOGD(TAG, "  Output-enable dimmer configured");
  }

  // Run pin setup for all registered pins – this calls set_pin_mode() on us
  for (auto *pin : registered_pins_) {
    pin->setup();
  }

  // Push configuration (direction) registers to chip
  apply_config_registers_();

  // Push initial output values
  sync_output_registers_();

  // Do an initial read of inputs
  if (has_inputs_) {
    read_all_inputs_();
  }

  // Configure interrupt pin
  if (use_interrupt_) {
    interrupt_pin_->setup();
    interrupt_pin_->attach_interrupt(PCA9698Component::gpio_intr_, this, gpio::INTERRUPT_FALLING_EDGE);
  }

  initialised_ = true;
  last_poll_ms_ = millis();
  last_verify_ms_ = millis();

  ESP_LOGCONFIG(TAG, "PCA9698 at 0x%02X initialised OK", address_);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – loop
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698Component::loop() {
  if (!initialised_) return;

  uint32_t now = millis();

  // ── Interrupt-driven input read ────────────────────────────────────────────
  if (use_interrupt_ && interrupt_pending_) {
    interrupt_pending_ = false;
    handle_interrupt_();
  }

  // ── Polling input read (when no interrupt or as fallback) ─────────────────
  if (!use_interrupt_ && has_inputs_) {
    if (now - last_poll_ms_ >= polling_interval_ms_) {
      last_poll_ms_ = now;
      read_all_inputs_();
    }
  }

  // ── Periodic output verification ──────────────────────────────────────────
  if (now - last_verify_ms_ >= PCA9698_OUTPUT_VERIFY_INTERVAL_MS) {
    last_verify_ms_ = now;
    verify_output_registers_();
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – dump_config
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698Component::dump_config() {
  ESP_LOGCONFIG(TAG, "PCA9698:");
  LOG_I2C_DEVICE(this);
  if (interrupt_pin_ != nullptr) {
    LOG_PIN("  Interrupt Pin: ", interrupt_pin_);
  } else {
    ESP_LOGCONFIG(TAG, "  Interrupt Pin: not configured (polling every %u ms)", polling_interval_ms_);
  }
  if (oe_output_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Output-Enable Dimmer: configured (level %.2f)", dimmer_level_);
  }
  ESP_LOGCONFIG(TAG, "  Registered Pins: %zu", registered_pins_.size());
  for (auto *pin : registered_pins_) {
    ESP_LOGCONFIG(TAG, "    Pin %2u  dir=%s", pin->get_pin(),
                  (pin->get_flags() & gpio::FLAG_OUTPUT) ? "OUTPUT" : "INPUT");
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – Public pin API
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698Component::set_pin_mode(uint8_t pin, PCA9698PinMode mode) {
  if (pin >= PCA9698_NUM_PINS) return;
  uint8_t port   = pin / 8;
  uint8_t bit    = pin % 8;

  if (mode == PCA9698PinMode::OUTPUT) {
    config_shadow_[port] &= ~(1u << bit);  // 0 = output
  } else {
    config_shadow_[port] |= (1u << bit);   // 1 = input
    has_inputs_ = true;
  }

  // If chip is already set up, push the new direction immediately
  if (initialised_) {
    write_config_port(port, config_shadow_[port]);
    // Un-mask interrupt for input pins; mask for output pins
    if (mode == PCA9698PinMode::OUTPUT) {
      mask_shadow_[port] |= (1u << bit);
    } else {
      mask_shadow_[port] &= ~(1u << bit);
    }
    write_registers(PCA9698_REG_MASK_BASE + port, &mask_shadow_[port], 1);
  }
}

bool PCA9698Component::read_pin(uint8_t pin) {
  if (pin >= PCA9698_NUM_PINS) return false;
  uint8_t port = pin / 8;
  uint8_t bit  = pin % 8;

  // For output pins, return the shadow (last written) value
  if (!(config_shadow_[port] & (1u << bit))) {
    return (output_shadow_[port] >> bit) & 1u;
  }

  // For input pins, return cached value; a fresh read happens in loop()
  return (input_shadow_[port] >> bit) & 1u;
}

void PCA9698Component::write_pin(uint8_t pin, bool value) {
  if (pin >= PCA9698_NUM_PINS) return;
  uint8_t port = pin / 8;
  uint8_t bit  = pin % 8;

  if (value) {
    output_shadow_[port] |= (1u << bit);
  } else {
    output_shadow_[port] &= ~(1u << bit);
  }

  write_output_port(port, output_shadow_[port]);
}

void PCA9698Component::set_dimmer_level(float level) {
  dimmer_level_ = clamp(level, 0.0f, 1.0f);
  if (oe_output_ != nullptr) {
    // OE is active-low: level 0.0 → duty 0 → OE low → outputs fully enabled.
    // level 1.0 → duty 1.0 → OE high (via PWM) → outputs fully disabled.
    oe_output_->set_level(dimmer_level_);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – I2C helpers with retry
// ═══════════════════════════════════════════════════════════════════════════════

bool PCA9698Component::read_registers(uint8_t base_reg, uint8_t *data, uint8_t len) {
  // Use the AI (auto-increment) bit for multi-byte reads.
  // write_read() sends the register address then reads len bytes in one transaction.
  uint8_t reg = base_reg | PCA9698_AI_BIT;

  for (uint8_t attempt = 0; attempt < PCA9698_MAX_RETRIES; attempt++) {
    i2c::ErrorCode err = write_read(&reg, 1, data, len);
    if (err == i2c::ERROR_OK) return true;

    ESP_LOGW(TAG, "I2C read error (reg 0x%02X, attempt %u/%u): %d",
             base_reg, attempt + 1, PCA9698_MAX_RETRIES, (int)err);
    delay(5);
  }
  mark_failed();
  return false;
}

bool PCA9698Component::write_registers(uint8_t base_reg, const uint8_t *data, uint8_t len) {
  uint8_t reg = (len > 1) ? (base_reg | PCA9698_AI_BIT) : base_reg;

  for (uint8_t attempt = 0; attempt < PCA9698_MAX_RETRIES; attempt++) {
    i2c::ErrorCode err = write_register(reg, data, len);
    if (err == i2c::ERROR_OK) return true;

    ESP_LOGW(TAG, "I2C write error (reg 0x%02X, attempt %u/%u): %d",
             base_reg, attempt + 1, PCA9698_MAX_RETRIES, (int)err);
    delay(5);
  }
  mark_failed();
  return false;
}

bool PCA9698Component::read_port(uint8_t port, uint8_t &value) {
  return read_registers(PCA9698_REG_INPUT_BASE + port, &value, 1);
}

bool PCA9698Component::write_output_port(uint8_t port, uint8_t value) {
  return write_registers(PCA9698_REG_OUTPUT_BASE + port, &value, 1);
}

bool PCA9698Component::write_config_port(uint8_t port, uint8_t value) {
  return write_registers(PCA9698_REG_CONFIG_BASE + port, &value, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – Shadow register helpers
// ═══════════════════════════════════════════════════════════════════════════════

void PCA9698Component::apply_config_registers_() {
  // Build interrupt mask: mask output pins (1=masked), unmask input pins (0=unmasked)
  for (uint8_t p = 0; p < PCA9698_NUM_PORTS; p++) {
    // config_shadow_: 1=input, 0=output  →  mask should be inverted (mask outputs)
    mask_shadow_[p] = ~config_shadow_[p];
  }

  if (!write_registers(PCA9698_REG_CONFIG_BASE, config_shadow_, PCA9698_NUM_PORTS)) {
    ESP_LOGE(TAG, "Failed to write configuration registers");
    return;
  }
  if (!write_registers(PCA9698_REG_MASK_BASE, mask_shadow_, PCA9698_NUM_PORTS)) {
    ESP_LOGE(TAG, "Failed to write interrupt mask registers");
  }
}

void PCA9698Component::sync_output_registers_() {
  if (!write_registers(PCA9698_REG_OUTPUT_BASE, output_shadow_, PCA9698_NUM_PORTS)) {
    ESP_LOGE(TAG, "Failed to sync output registers");
  }
}

void PCA9698Component::verify_output_registers_() {
  // Read back current output latch registers from the chip and compare
  uint8_t chip_output[PCA9698_NUM_PORTS];
  if (!read_registers(PCA9698_REG_OUTPUT_BASE, chip_output, PCA9698_NUM_PORTS)) {
    ESP_LOGW(TAG, "Output verification read failed");
    return;
  }

  bool mismatch = false;
  for (uint8_t p = 0; p < PCA9698_NUM_PORTS; p++) {
    // Only compare bits that are configured as outputs (config_shadow_ bit = 0)
    uint8_t output_mask = ~config_shadow_[p];
    if ((chip_output[p] & output_mask) != (output_shadow_[p] & output_mask)) {
      ESP_LOGW(TAG, "Output mismatch on port %u: expected 0x%02X, got 0x%02X (mask 0x%02X)",
               p, output_shadow_[p] & output_mask, chip_output[p] & output_mask, output_mask);
      mismatch = true;
    }
  }

  if (mismatch) {
    ESP_LOGW(TAG, "Correcting output register mismatch");
    sync_output_registers_();
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PCA9698Component – Interrupt / input reading
// ═══════════════════════════════════════════════════════════════════════════════

void IRAM_ATTR PCA9698Component::gpio_intr_(PCA9698Component *arg) {
  arg->interrupt_pending_ = true;
}

void PCA9698Component::handle_interrupt_() {
  // Read all input ports in a burst read for efficiency
  read_all_inputs_();
}

void PCA9698Component::read_all_inputs_() {
  uint8_t new_input[PCA9698_NUM_PORTS];
  if (!read_registers(PCA9698_REG_INPUT_BASE, new_input, PCA9698_NUM_PORTS)) {
    ESP_LOGW(TAG, "Failed to read input registers");
    return;
  }

  for (uint8_t p = 0; p < PCA9698_NUM_PORTS; p++) {
    uint8_t input_mask = config_shadow_[p];  // 1=input bits
    uint8_t changed = (new_input[p] ^ input_shadow_[p]) & input_mask;
    if (changed) {
      ESP_LOGV(TAG, "Input change on port %u: 0x%02X → 0x%02X (changed bits: 0x%02X)",
               p, input_shadow_[p], new_input[p], changed);
      input_shadow_[p] = new_input[p];

      // Notify any binary_sensor / switch components that are polling the pin
      // ESPHome handles this automatically via GPIOBinarySensor polling digital_read()
    }
  }
}

}  // namespace pca9698
}  // namespace esphome
