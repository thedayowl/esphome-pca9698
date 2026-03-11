#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/output/float_output.h"
#include "esphome/core/log.h"

#include <vector>
#include <cstdint>

namespace esphome {
namespace pca9698 {

// ─── PCA9698 Register Map ─────────────────────────────────────────────────────
// The PCA9698 has 5 ports (P0–P4), each 8 bits wide = 40 I/O pins total.
// Registers are accessed with auto-increment via the AI bit in the control byte.
static const uint8_t PCA9698_NUM_PORTS = 5;
static const uint8_t PCA9698_NUM_PINS  = 40;  // 5 ports × 8 pins

// Register base addresses (port 0; add port index for other ports)
static const uint8_t PCA9698_REG_INPUT_BASE     = 0x00;  // Input Port 0–4  (read)
static const uint8_t PCA9698_REG_OUTPUT_BASE    = 0x08;  // Output Port 0–4 (read/write)
static const uint8_t PCA9698_REG_POLARITY_BASE  = 0x10;  // Polarity Inversion 0–4
static const uint8_t PCA9698_REG_CONFIG_BASE    = 0x18;  // Configuration 0–4  (1=input, 0=output)
static const uint8_t PCA9698_REG_MASK_BASE      = 0x20;  // Interrupt Mask 0–4 (0=unmasked)

// Auto-Increment control byte modifier
static const uint8_t PCA9698_AI_BIT             = 0x80;

// Maximum I2C retry attempts before giving up
static const uint8_t PCA9698_MAX_RETRIES        = 3;

// Polling interval for output verification (ms)
static const uint32_t PCA9698_OUTPUT_VERIFY_INTERVAL_MS = 5000;

// ─── Forward declarations ─────────────────────────────────────────────────────
class PCA9698Component;

// ─── Pin Mode Enum ────────────────────────────────────────────────────────────
enum class PCA9698PinMode : uint8_t {
  OUTPUT = 0,
  INPUT  = 1,
  INPUT_PULLUP = 2,  // PCA9698 has internal weak pull-ups; treated same as INPUT
};

// ─── PCA9698GPIOPin ───────────────────────────────────────────────────────────
// Represents a single GPIO pin on the PCA9698.
// Registered as a standard ESPHome InternalGPIOPin-compatible object.
class PCA9698GPIOPin : public GPIOPin {
 public:
  void setup() override;
  void pin_mode(gpio::Flags flags) override;
  bool digital_read() override;
  void digital_write(bool value) override;
  std::string dump_summary() const override;

  void set_parent(PCA9698Component *parent) { parent_ = parent; }
  void set_pin(uint8_t pin) { pin_ = pin; }
  void set_inverted(bool inverted) { inverted_ = inverted; }
  void set_flags(gpio::Flags flags) { flags_ = flags; }

  uint8_t get_pin() const { return pin_; }
  gpio::Flags get_flags() const { return flags_; }

  // Propagate parent health to child components (switches, binary_sensors, etc.)
  // so Home Assistant marks their entities unavailable when comms fail.
  // Covers both hard failure (mark_failed) and transient I2C errors.
  bool is_failed() const {
    return parent_ == nullptr || parent_->is_failed() || !parent_->is_comms_ok();
  }

 protected:
  PCA9698Component *parent_{nullptr};
  uint8_t pin_{0};
  bool inverted_{false};
  gpio::Flags flags_{gpio::FLAG_NONE};
};

// ─── PCA9698Component ─────────────────────────────────────────────────────────
class PCA9698Component : public Component, public i2c::I2CDevice {
 public:
  PCA9698Component() = default;

  // ── ESPHome lifecycle ──────────────────────────────────────────────────────
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::IO; }

  // ── Configuration setters (called from Python codegen) ────────────────────
  void set_interrupt_pin(InternalGPIOPin *pin) { interrupt_pin_ = pin; }
  void set_polling_interval(uint32_t ms) { polling_interval_ms_ = ms; }

  void set_oe_output(output::FloatOutput *oe) { oe_output_ = oe; }

  void set_dimmer_level(float level);  // 0.0 = fully on, 1.0 = all outputs off

  // ── Pin register/access API (used by PCA9698GPIOPin) ──────────────────────
  bool read_pin(uint8_t pin);
  void write_pin(uint8_t pin, bool value);
  void set_pin_mode(uint8_t pin, PCA9698PinMode mode);

  // ── Register all pins with the component ──────────────────────────────────
  void register_pin(PCA9698GPIOPin *pin) { registered_pins_.push_back(pin); }

  // Returns false if I2C communication with the chip is currently failing.
  // Used by PCA9698GPIOPin::is_failed() to propagate unavailability to HA.
  bool is_comms_ok() const { return comms_ok_; }

 protected:
  // ── I2C helpers with retry logic ──────────────────────────────────────────
  bool read_registers(uint8_t base_reg, uint8_t *data, uint8_t len);
  bool write_registers(uint8_t base_reg, const uint8_t *data, uint8_t len);
  bool read_port(uint8_t port, uint8_t &value);
  bool write_output_port(uint8_t port, uint8_t value);
  bool write_config_port(uint8_t port, uint8_t value);

  // ── Shadow register management ────────────────────────────────────────────
  void sync_output_registers_();    // Write shadow → chip
  void verify_output_registers_();  // Read chip → compare with shadow → correct
  void apply_config_registers_();   // Push pin-direction config to chip

  // ── Interrupt / polling ───────────────────────────────────────────────────
  void handle_interrupt_();
  void read_all_inputs_();
  static void IRAM_ATTR gpio_intr_(PCA9698Component *arg);

  // ── Shadow registers ──────────────────────────────────────────────────────
  uint8_t output_shadow_[PCA9698_NUM_PORTS]{};   // What we last wrote to output regs
  uint8_t config_shadow_[PCA9698_NUM_PORTS]{};   // 1=input, 0=output  (chip default: all input)
  uint8_t input_shadow_[PCA9698_NUM_PORTS]{};    // Last known input state
  uint8_t mask_shadow_[PCA9698_NUM_PORTS]{};     // Interrupt mask (0=unmasked)

  // ── Interrupt / polling state ─────────────────────────────────────────────
  InternalGPIOPin *interrupt_pin_{nullptr};
  volatile bool interrupt_pending_{false};
  uint32_t polling_interval_ms_{50};             // Default poll interval (ms) when no IRQ pin
  uint32_t last_poll_ms_{0};
  uint32_t last_verify_ms_{0};

  bool use_interrupt_{false};
  bool initialised_{false};
  bool has_inputs_{false};
  bool comms_ok_{true};  // Tracks whether the last I2C operation succeeded

  // ── Output-enable / dimmer ────────────────────────────────────────────────
  output::FloatOutput *oe_output_{nullptr};
  float dimmer_level_{0.0f};  // 0.0 = outputs fully enabled (OE pulled low via PWM duty=0)

  // ── Registered pins ───────────────────────────────────────────────────────
  std::vector<PCA9698GPIOPin *> registered_pins_;
};

}  // namespace pca9698
}  // namespace esphome
