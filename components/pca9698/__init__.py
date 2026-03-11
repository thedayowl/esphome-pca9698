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
CONF_PCA9698          = "pca9698"       # pin schema key (matches registry key)
CONF_OE_OUTPUT_ID     = "oe_output_id"
CONF_DIMMER_LEVEL     = "dimmer_level"
CONF_INTERRUPT_PIN    = "interrupt_pin"
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

def _validate_pin_mode(value):
    if not (value[CONF_INPUT] or value[CONF_OUTPUT]):
        raise cv.Invalid("Mode must be either input or output")
    if value[CONF_INPUT] and value[CONF_OUTPUT]:
        raise cv.Invalid("Mode must be either input or output")
    return value


PCA9698_PIN_SCHEMA = pins.gpio_base_schema(
    PCA9698GPIOPin,
    cv.int_range(min=0, max=39),
    modes=[CONF_INPUT, CONF_OUTPUT, CONF_PULLUP],
    mode_validator=_validate_pin_mode,
    invertible=True,
).extend(
    {
        cv.Required(CONF_PCA9698): cv.use_id(PCA9698Component),
    }
)


@pins.PIN_SCHEMA_REGISTRY.register(CONF_PCA9698, PCA9698_PIN_SCHEMA)
async def pca9698_pin_to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    parent = await cg.get_variable(config[CONF_PCA9698])

    cg.add(var.set_parent(parent))
    cg.add(var.set_pin(config[CONF_NUMBER]))
    cg.add(var.set_inverted(config[CONF_INVERTED]))
    cg.add(var.set_flags(pins.gpio_flags_expr(config[CONF_MODE])))

    cg.add(parent.register_pin(var))
    return var
