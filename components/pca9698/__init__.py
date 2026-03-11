"""ESPHome external component configuration for PCA9698 40-bit I/O expander."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import i2c, output
from esphome.const import (
    CONF_ID,
    CONF_INPUT,
    CONF_INVERTED,
    CONF_MODE,
    CONF_NUMBER,
    CONF_OUTPUT,
    CONF_PULLUP,
)

DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["output"]
MULTI_CONF = True  # Allow multiple PCA9698 instances (one per I2C address)

# ── Local config key constants ────────────────────────────────────────────────
CONF_PCA9698_ID      = "pca9698_id"
CONF_OE_OUTPUT_ID    = "oe_output_id"
CONF_DIMMER_LEVEL    = "dimmer_level"
CONF_INTERRUPT_PIN   = "interrupt_pin"
CONF_POLLING_INTERVAL = "polling_interval"

# ── C++ namespace / class references ──────────────────────────────────────────
pca9698_ns = cg.esphome_ns.namespace("pca9698")
PCA9698Component = pca9698_ns.class_("PCA9698Component", cg.Component, i2c.I2CDevice)
PCA9698GPIOPin   = pca9698_ns.class_("PCA9698GPIOPin",   cg.GPIOPin)

# ── Component config schema ───────────────────────────────────────────────────
CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PCA9698Component),
            # Interrupt pin is fully optional
            cv.Optional(CONF_INTERRUPT_PIN): pins.internal_gpio_input_pin_schema,
            # Polling interval used when no interrupt pin is present
            cv.Optional(CONF_POLLING_INTERVAL, default="50ms"): cv.positive_time_period_milliseconds,
            # Optional output-enable PWM output for dimming (any FloatOutput)
            cv.Optional(CONF_OE_OUTPUT_ID): cv.use_id(output.FloatOutput),
            # Initial dimmer level: 0.0 = outputs off, 1.0 = fully on
            cv.Optional(CONF_DIMMER_LEVEL, default=1.0): cv.percentage,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x20))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if CONF_INTERRUPT_PIN in config:
        irq_pin = await cg.gpio_pin_expression(config[CONF_INTERRUPT_PIN])
        cg.add(var.set_interrupt_pin(irq_pin))

    cg.add(var.set_polling_interval(config[CONF_POLLING_INTERVAL]))

    if CONF_OE_OUTPUT_ID in config:
        oe = await cg.get_variable(config[CONF_OE_OUTPUT_ID])
        cg.add(var.set_oe_output(oe))
        cg.add(var.set_dimmer_level(config[CONF_DIMMER_LEVEL]))


# ═════════════════════════════════════════════════════════════════════════════
# Pin schema – used by binary_sensor, switch, etc. via the `pin:` key
# ═════════════════════════════════════════════════════════════════════════════

def _validate_pin_number(value):
    value = cv.int_(value)
    if not 0 <= value <= 39:
        raise cv.Invalid(f"PCA9698 pin number must be 0–39, got {value}")
    return value


# Mode schema: accept INPUT, OUTPUT, or INPUT_PULLUP as a dict like
#   mode: INPUT  or  mode: { input: true, pullup: true }
def _validate_pin_mode(value):
    if isinstance(value, str):
        value = value.upper()
        mapping = {
            "INPUT":        {CONF_INPUT: True,  CONF_OUTPUT: False, CONF_PULLUP: False},
            "OUTPUT":       {CONF_INPUT: False, CONF_OUTPUT: True,  CONF_PULLUP: False},
            "INPUT_PULLUP": {CONF_INPUT: True,  CONF_OUTPUT: False, CONF_PULLUP: True},
        }
        if value not in mapping:
            raise cv.Invalid(f"Invalid pin mode '{value}'. Use INPUT, OUTPUT, or INPUT_PULLUP.")
        return mapping[value]
    return cv.Schema(
        {
            cv.Optional(CONF_INPUT,  default=False): cv.boolean,
            cv.Optional(CONF_OUTPUT, default=False): cv.boolean,
            cv.Optional(CONF_PULLUP, default=False): cv.boolean,
        }
    )(value)


PCA9698_PIN_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PCA9698GPIOPin),
        cv.Required(CONF_PCA9698_ID): cv.use_id(PCA9698Component),
        cv.Required(CONF_NUMBER): _validate_pin_number,
        cv.Optional(CONF_MODE, default="INPUT"): _validate_pin_mode,
        cv.Optional(CONF_INVERTED, default=False): cv.boolean,
    }
)


@pins.PIN_SCHEMA_REGISTRY.register("pca9698", PCA9698_PIN_SCHEMA)
async def pca9698_pin_to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    parent = await cg.get_variable(config[CONF_PCA9698_ID])

    cg.add(var.set_parent(parent))
    cg.add(var.set_pin(config[CONF_NUMBER]))
    cg.add(var.set_inverted(config[CONF_INVERTED]))

    # Build gpio::Flags expression from the mode dict
    mode = config[CONF_MODE]
    flag_parts = []
    if mode.get(CONF_INPUT):
        flag_parts.append("gpio::FLAG_INPUT")
    if mode.get(CONF_OUTPUT):
        flag_parts.append("gpio::FLAG_OUTPUT")
    if mode.get(CONF_PULLUP):
        flag_parts.append("gpio::FLAG_PULLUP")
    flags_expr = " | ".join(flag_parts) if flag_parts else "gpio::FLAG_NONE"
    cg.add(var.set_flags(cg.RawExpression(flags_expr)))

    cg.add(parent.register_pin(var))
    return var
