# Changelog

All notable changes to this project will be documented here.

---

## [Unreleased]

### Added — Comms Health Tracking (`is_comms_ok`)

**Files changed:** `pca9698.h`, `pca9698.cpp`, `examples/esp32_full_example.yaml`, `README.md`

#### C++ changes

- Added `comms_ok_` boolean member to `PCA9698Component` (default `true`).
  Tracks whether the most recent I²C transaction succeeded.
- Added public `is_comms_ok() const` accessor so YAML lambdas can read the
  health state directly.
- `read_registers()` and `write_registers()` now update `comms_ok_` on every
  transaction:
  - After 3 consecutive failures: sets `comms_ok_ = false`, calls
    `status_set_error("I2C communication failed")`, and logs at ERROR level.
  - On the next successful transaction after a failure: restores
    `comms_ok_ = true`, calls `status_clear_error()`, and logs recovery at
    INFO level.
- `PCA9698GPIOPin::is_failed()` now returns `true` when the parent component
  is either hard-failed (`mark_failed()`) or has lost comms (`!comms_ok_`),
  so ESPHome's GPIO infrastructure treats pins as failed during an outage.

#### Behaviour notes

ESPHome's native API does not propagate pin-level failure as entity
unavailability for `switch` or `binary_sensor` entities. The recommended
workaround is to expose `is_comms_ok()` as a template binary sensor (see
example below and the README) and use it in HA dashboards and automations.

#### Example usage

```yaml
binary_sensor:
  - platform: template
    id: chip_health
    name: "PCA9698 Health"
    device_class: connectivity
    lambda: "return id(expander).is_comms_ok();"
```

---

### Added — Startup Connectivity Probe

**Files changed:** `pca9698.cpp`

`setup()` now performs a burst read of all 5 input ports as its very first
operation before any configuration writes. If the chip does not respond,
`mark_failed()` is called immediately with a log message that includes the
configured address and I²C error code. This replaces the previous behaviour
where a failed mid-setup write would call `mark_failed()` with no message,
producing a cryptic `"unspecified"` error in the ESPHome log.

---

### Changed — `mark_failed()` removed from I²C helpers

**Files changed:** `pca9698.cpp`

`read_registers()` and `write_registers()` no longer call `mark_failed()` on
exhausting retries. This prevents a transient I²C glitch during normal
operation from permanently killing the component and requiring a reboot to
recover. The component now self-heals when comms are restored.
`mark_failed()` is only called from `setup()` when the chip is genuinely
unreachable at startup.

---

### Added — OE dimmer exposed as monochromatic light

**Files changed:** `examples/esp32_full_example.yaml`,
`examples/esp8266_polling_example.yaml`, `README.md`

Replaced the `number:` template entity (0–100% slider) with a
`light: monochromatic` entity for the Output Enable dimmer. This provides a
proper HA light card with on/off toggle, brightness slider, transition
support, and scene integration. `restore_mode: RESTORE_DEFAULT_ON` ensures
outputs are enabled at boot and the last-set brightness is remembered across
reboots.

---

### Changed — Chip 1 outputs modelled as lights in ESP32 example

**Files changed:** `examples/esp32_full_example.yaml`

When a chip has its OE pin driven by PWM, all outputs dim together. The
ESP32 full example now correctly models chip 1 outputs as `binary` light
entities (on/off, participate in the dimmed group) rather than switches or
relays, which would imply independent control. Chip 2 (OE tied to GND)
retains `switch` entities (relays, valves, pump) since its outputs are
full on/off only.

---

### Added — Chip health sensors in ESP32 example

**Files changed:** `examples/esp32_full_example.yaml`

Added `binary_sensor: template` entries for each chip using
`is_comms_ok()`, with `device_class: connectivity`. These serve as the
recommended availability signal for HA dashboards and automations.
