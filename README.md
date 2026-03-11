# ESPHome External Component: PCA9698

NXP PCA9698 — 40-bit open-drain I/O port expander for I²C-bus
(5 × 8-bit ports, 40 I/O pins, address range 0x20–0x27).

---

## Features

| Feature | Detail |
|---|---|
| **40 I/O pins** | Pins 0–39 across 5 internal ports |
| **Mixed direction** | Each pin independently configured as input or output |
| **Multiple chips** | Up to 8 PCA9698 chips on one bus (via `MULTI_CONF`) |
| **Interrupt-driven input** | Optional INT pin for near-instant input change detection |
| **Polling fallback** | Configurable polling interval when no INT pin is wired |
| **Output verification** | Periodic read-back + correction of output register mismatches |
| **I²C retry** | Automatic retry (×3) on any I²C error with 5 ms back-off |
| **Comms health tracking** | `is_comms_ok()` reflects live I²C status; drives HA availability |
| **OE dimmer** | Optional LEDC (ESP32) or software PWM (ESP8266) on the Output Enable pin |
| **Platform** | ESP32 (Arduino + IDF) and ESP8266 (Arduino) |

---

## Directory Layout

```
components/
└── pca9698/
    ├── __init__.py       # Python config schema & code-gen
    ├── manifest.yaml     # Component metadata
    ├── pca9698.h         # C++ header
    └── pca9698.cpp       # C++ implementation
examples/
├── esp32_full_example.yaml
└── esp8266_polling_example.yaml
```

---

## Hardware Notes

### I²C Address

| A2 | A1 | A0 | Address |
|----|----|----|---------|
| 0  | 0  | 0  | 0x20    |
| 0  | 0  | 1  | 0x21    |
| 0  | 1  | 0  | 0x22    |
| 0  | 1  | 1  | 0x23    |
| 1  | 0  | 0  | 0x24    |
| 1  | 0  | 1  | 0x25    |
| 1  | 1  | 0  | 0x26    |
| 1  | 1  | 1  | 0x27    |

### INT (Interrupt) Pin

- Active-low, open-drain output on the chip.
- Wire through a pull-up resistor (4.7 kΩ) to VCC.
- Connect to any input-capable GPIO on the ESP module.
- Declare with `mode: INPUT_PULLUP` in the ESPHome pin schema.
- The component attaches a falling-edge interrupt and reads all 5 input
  ports in a single burst I²C transaction on each trigger.

### OE (Output Enable) Pin

- Active-low. Pulling OE LOW enables all outputs; pulling HIGH disables them.
- To use the dimmer: connect a GPIO (LEDC on ESP32 / software PWM on ESP8266)
  to the OE pin.
- Set `inverted: true` on the PWM output so that the brightness scale is intuitive:
  - `dimmer_level: 0%` → PWM duty = 0% → OE HIGH → outputs disabled
  - `dimmer_level: 100%` → PWM duty = 100% → OE LOW → outputs fully enabled
  - Intermediate values produce proportional duty-cycle dimming.
- Expose the dimmer as a **monochromatic light** entity in Home Assistant (see
  examples) so it appears as a dimmable light card with on/off and brightness slider.
- If you do **not** use the dimmer, tie OE directly to GND.

### RESET Pin

- Active-low. Must be held HIGH for normal operation.
- If floating, the chip will be held in reset and will not respond to I²C writes
  even though it may still ACK its address during a bus scan.
- Tie to VCC or drive from a GPIO if software reset is needed.

### Pin Numbering

Pins are numbered 0–39 consecutively:

| Pin range | Port | Chip register offset |
|-----------|------|----------------------|
| 0–7       | P0   | +0                   |
| 8–15      | P1   | +1                   |
| 16–23     | P2   | +2                   |
| 24–31     | P3   | +3                   |
| 32–39     | P4   | +4                   |

---

## Configuration Reference

### `pca9698:` block

```yaml
pca9698:
  - id: expander                 # required
    address: 0x20                # I²C address (default: 0x20)

    # ── Interrupt pin (optional) ─────────────────────────────────────────────
    interrupt_pin:
      number: GPIO19
      mode: INPUT_PULLUP
    # If omitted, the component falls back to polling.

    # ── Polling interval (used only when interrupt_pin is absent) ────────────
    polling_interval: 50ms       # default: 50 ms

    # ── Output-enable dimmer (optional) ──────────────────────────────────────
    oe_output_id: pca9698_oe_pwm  # id of a ledc or esp8266_pwm output
    dimmer_level: 100%            # 0% = outputs off, 100% = fully on (default)
```

### Pin schema (used inside `binary_sensor`, `switch`, etc.)

```yaml
pin:
  pca9698: <component_id>        # required – refers to the pca9698: id
  number: 0                      # required – 0–39
  mode: INPUT                    # INPUT | OUTPUT | INPUT_PULLUP
  inverted: false                # optional – invert logic level
```

---

## Minimal Example (outputs only, no INT pin, no dimmer)

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/thedayowl/esphome-pca9698
      ref: main
    components: [pca9698]

i2c:
  sda: GPIO21
  scl: GPIO22

pca9698:
  - id: expander
    address: 0x20

switch:
  - platform: gpio
    name: "Relay 0"
    pin:
      pca9698: expander
      number: 0
      mode: OUTPUT
```

---

## OE Dimmer as a Monochromatic Light

When the OE pin is driven by a PWM output, all chip outputs dim together.
The natural way to expose this in Home Assistant is as a monochromatic light
entity, which gives you a proper light card with on/off toggle and brightness
slider, and supports scenes and transitions.

```yaml
output:
  - platform: ledc
    id: pca9698_oe_pwm
    pin: GPIO18
    frequency: 1000 Hz
    inverted: true       # OE active-low: 100% duty = fully on, 0% = off

pca9698:
  - id: expander
    address: 0x20
    oe_output_id: pca9698_oe_pwm
    dimmer_level: 100%

light:
  - platform: monochromatic
    id: output_dimmer
    name: "PCA9698 Output Brightness"
    output: pca9698_oe_pwm
    restore_mode: RESTORE_DEFAULT_ON   # restore brightness on reboot
    default_transition_length: 500ms
    on_state:
      - lambda: |-
          if (id(output_dimmer).current_values.is_on()) {
            id(expander).set_dimmer_level(
              id(output_dimmer).current_values.get_brightness());
          } else {
            id(expander).set_dimmer_level(0.0f);
          }
```

Because OE is a global control, all outputs on the chip dim together.
Model the individual output pins as `binary` lights (on/off only) rather
than switches, and use the monochromatic entity as the master brightness:

```yaml
light:
  - platform: binary
    name: "Light 0"
    output: light_0_output

output:
  - platform: gpio
    id: light_0_output
    pin: { pca9698: expander, number: 16, mode: OUTPUT }
```

---

## Two Chips on One Bus

```yaml
pca9698:
  - id: chip_a
    address: 0x20
    interrupt_pin: { number: GPIO19, mode: INPUT_PULLUP }

  - id: chip_b
    address: 0x21
    polling_interval: 100ms

binary_sensor:
  - platform: gpio
    name: "Chip A input"
    pin: { pca9698: chip_a, number: 0, mode: INPUT }

  - platform: gpio
    name: "Chip B input"
    pin: { pca9698: chip_b, number: 0, mode: INPUT }
```

---

## Comms Health & Home Assistant Availability

### How it works

The component tracks I²C communication health with an internal `comms_ok_`
flag that is updated on every I²C transaction:

- After **3 consecutive failures** on any read or write, `comms_ok_` is set to
  `false` and `status_set_error()` is called on the component. A log message
  is emitted at ERROR level.
- As soon as **any subsequent transaction succeeds**, `comms_ok_` is restored
  to `true`, `status_clear_error()` is called, and a recovery message is logged
  at INFO level.

`PCA9698GPIOPin::is_failed()` returns `true` whenever the parent component is
either hard-failed (`mark_failed()`) or has lost comms (`!comms_ok_`). This
means ESPHome's GPIO infrastructure treats the pins as failed during an outage.

### Limitation

ESPHome's native API does not propagate pin-level failure as entity
unavailability for `switch` or `binary_sensor` entities — those entities keep
their last known state in Home Assistant rather than going unavailable. To
surface chip health explicitly, expose `is_comms_ok()` as a template binary
sensor and use it in your automations or dashboards.

### Health sensor (recommended)

Add a template binary sensor for each chip. Use `device_class: connectivity`
so Home Assistant displays it as a connectivity indicator:

```yaml
binary_sensor:
  - platform: template
    id: chip_health
    name: "PCA9698 Health"
    device_class: connectivity
    lambda: "return id(expander).is_comms_ok();"
```

### Using health in Home Assistant

**Dashboard**: Add the health sensor to your dashboard. It will show
"Connected" / "Disconnected" with the standard connectivity icon.

**Alert automation**: Notify when a chip goes offline:

```yaml
automation:
  - alias: "PCA9698 offline alert"
    trigger:
      - platform: state
        entity_id: binary_sensor.pca9698_health
        to: "off"
    action:
      - service: notify.mobile_app
        data:
          message: "PCA9698 I²C chip has gone offline!"
```

**Conditional entity availability**: Use a template in HA to suppress
automations that depend on chip outputs when the chip is offline:

```yaml
condition:
  - condition: state
    entity_id: binary_sensor.pca9698_health
    state: "on"
```

---

## Robustness Details

### Startup Probe

Before any configuration writes, `setup()` performs a burst read of all 5
input ports. If the chip does not respond, `mark_failed()` is called
immediately with a clear log message indicating the address and error code.
This produces a much more informative failure than a generic "unspecified"
error from a mid-setup write failure.

### I²C Retries

Every read and write operation is retried up to **3 times** with a 5 ms
back-off between attempts before the comms health flag is set to `false`.

### Output Verification

Every **5 seconds** the component reads back the output latch registers from
the chip and compares them with the internal shadow registers. Any mismatch
on output-configured bits triggers a full re-write of all output registers.
This guards against transient I²C glitches, power dips that reset the chip,
or any other cause of state divergence between the ESP and the chip.

### Interrupt Mask

The component automatically masks the interrupt for output-configured pins so
that toggling outputs does not trigger spurious INT assertions.

---

## Tested Platforms

| Platform | Framework | Status |
|----------|-----------|--------|
| ESP32    | IDF       | ✅     |
| ESP32    | Arduino   | ✅     |
| ESP8266  | Arduino   | ✅     |

---

## License

MIT — free to use, modify, and distribute.
