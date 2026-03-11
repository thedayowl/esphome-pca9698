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
| **I²C retry** | Automatic retry (×3) on any I²C error, with `mark_failed()` after exhaustion |
| **OE dimmer** | Optional LEDC (ESP32) or software PWM (ESP8266) on the Output Enable pin |
| **Platform** | ESP32 + ESP8266 via Arduino framework |

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
- `dimmer_level: 0%` (default off) → PWM duty = 0 % → OE pulled HIGH → outputs disabled.
- `dimmer_level: 100%` → PWM duty = 100 % → OE pulled LOW → outputs fully enabled.
- Intermediate values produce proportional duty-cycle dimming.
- If you do **not** use the dimmer, tie OE directly to GND.

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
  - id: <component_id>           # required
    address: 0x20                # I²C address  (default: 0x20)

    # ── Interrupt pin (optional) ─────────────────────────────────────────
    interrupt_pin:
      number: GPIO19
      mode: INPUT_PULLUP
    # If omitted, the component falls back to polling.

    # ── Polling interval (used only when interrupt_pin is absent) ────────
    polling_interval: 50ms       # default: 50 ms

    # ── Output-enable dimmer (optional) ──────────────────────────────────
    oe_output_id: pca9698_oe_pwm  # id of a ledc or esp8266_pwm output
    dimmer_level: 0%             # 0 % = fully on, 100 % = fully off
```

### Pin schema (used inside `binary_sensor`, `switch`, etc.)

```yaml
pin:
  pca9698: <component_id>        # required  – refers to the pca9698: id
  number: 0                      # required  – 0–39
  mode: INPUT                    # INPUT | OUTPUT | INPUT_PULLUP
  inverted: false                # optional  – invert logic level
```

---

## Minimal Example (outputs only, no INT pin, no dimmer)

```yaml
external_components:
  - source: { type: local, path: components }
    components: [pca9698]

i2c:
  sda: GPIO21
  scl: GPIO22

pca9698:
  - id: expander
    address: 0x20
    # No interrupt_pin → no polling needed for outputs-only use

switch:
  - platform: gpio
    name: "Relay 0"
    pin:
      pca9698: expander
      number: 0
      mode: OUTPUT
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

## Robustness Details

### I²C Retries

Every read and write operation is retried up to **3 times** before the
component marks itself as failed (`mark_failed()`). A 5 ms back-off is
inserted between attempts.

### Output Verification

Every **5 seconds** the component reads back the output latch registers from
the chip and compares them with the internal shadow registers. Any mismatch
on output-configured bits triggers a full re-write of all output registers.
This guards against transient I²C glitches that corrupted a write, power
dips that reset the chip, or any other cause of state divergence.

### Interrupt Mask

The component automatically masks the interrupt for output-configured pins so
that toggling outputs does not trigger spurious INT assertions.

---

## Tested Frameworks

| Platform | Framework | Status |
|----------|-----------|--------|
| ESP32    | Arduino   | ✅ |
| ESP8266  | Arduino   | ✅ |
| ESP32    | IDF       | ⚠️ LEDC API differences may require minor changes |

---

## License

MIT — free to use, modify, and distribute.
