"""ESPHome external component: Mi Kettle (Xiaomi BLE kettle)."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, number, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_MINUTE,
)

DEPENDENCIES = ["esp32_ble_tracker", "ble_client"]
AUTO_LOAD = ["sensor", "text_sensor", "number"]

mikettle_ns = cg.esphome_ns.namespace("mikettle")
MiKettleComponent = mikettle_ns.class_(
    "MiKettleComponent", ble_client.BLEClientNode, cg.Component
)
MiKettleNumber = mikettle_ns.class_(
    "MiKettleNumber", number.Number, cg.Parented.template(MiKettleComponent)
)

# ── Config key names ──────────────────────────────────────────────────────────
CONF_PRODUCT_ID          = "product_id"
CONF_TOKEN               = "token"
CONF_CURRENT_TEMPERATURE = "current_temperature"
CONF_SET_TEMPERATURE     = "set_temperature"
CONF_TARGET_TEMPERATURE  = "target_temperature"
CONF_ACTION              = "action"
CONF_MODE                = "mode"
CONF_KEEP_WARM_TYPE      = "keep_warm_type"
CONF_KEEP_WARM_TIME      = "keep_warm_time"

_TOKEN_LEN = 12


def _validate_token(value):
    """Accept a 24-character hex string and convert it to a list of 12 bytes."""
    value = cv.string_strict(value)
    # Strip optional spaces/colons (e.g. "01:5C:CB:..." or "01 5C CB ...")
    cleaned = value.replace(":", "").replace(" ", "")
    if len(cleaned) != _TOKEN_LEN * 2:
        raise cv.Invalid(
            f"Token must be {_TOKEN_LEN} bytes ({_TOKEN_LEN * 2} hex characters), "
            f"got {len(cleaned) // 2} bytes"
        )
    try:
        return [int(cleaned[i : i + 2], 16) for i in range(0, _TOKEN_LEN * 2, 2)]
    except ValueError as exc:
        raise cv.Invalid(f"Token contains invalid hex characters: {exc}") from exc


# ── Schema ────────────────────────────────────────────────────────────────────
CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MiKettleComponent),
            # Product ID (default 275 = EU version)
            cv.Optional(CONF_PRODUCT_ID, default=275): cv.uint16_t,
            # Per-device authentication token (12 bytes as a 24-char hex string).
            # Obtain from Mi Home using a token extractor tool (e.g. miio-tokens).
            # If omitted, a built-in default is used, which works for some devices
            # but will cause authentication failures on most production kettles.
            cv.Optional(CONF_TOKEN): _validate_token,
            # ── Sensors ──────────────────────────────────────────────────
            cv.Optional(CONF_CURRENT_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_SET_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_KEEP_WARM_TIME): sensor.sensor_schema(
                unit_of_measurement=UNIT_MINUTE,
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            # ── Writable number ───────────────────────────────────────────
            cv.Optional(CONF_TARGET_TEMPERATURE): number.number_schema(
                MiKettleNumber,
                unit_of_measurement=UNIT_CELSIUS,
                device_class=DEVICE_CLASS_TEMPERATURE,
            ),
            # ── Text sensors ──────────────────────────────────────────────
            cv.Optional(CONF_ACTION): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_MODE):   text_sensor.text_sensor_schema(),
            cv.Optional(CONF_KEEP_WARM_TYPE): text_sensor.text_sensor_schema(),
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

# ── Code generation ───────────────────────────────────────────────────────────

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_product_id(config[CONF_PRODUCT_ID]))

    if token_bytes := config.get(CONF_TOKEN):
        # Use std::array to pass the token by value – no pointer lifetime concerns.
        token_arr = cg.RawExpression(
            "std::array<uint8_t, " + str(_TOKEN_LEN) + ">{{" 
            + ", ".join(f"0x{b:02X}" for b in token_bytes)
            + "}}"
        )
        cg.add(var.set_token(token_arr))

    if current_temp_cfg := config.get(CONF_CURRENT_TEMPERATURE):
        sens = await sensor.new_sensor(current_temp_cfg)
        cg.add(var.set_current_temperature_sensor(sens))

    if set_temp_cfg := config.get(CONF_SET_TEMPERATURE):
        sens = await sensor.new_sensor(set_temp_cfg)
        cg.add(var.set_set_temperature_sensor(sens))

    if action_cfg := config.get(CONF_ACTION):
        tsens = await text_sensor.new_text_sensor(action_cfg)
        cg.add(var.set_action_sensor(tsens))

    if mode_cfg := config.get(CONF_MODE):
        tsens = await text_sensor.new_text_sensor(mode_cfg)
        cg.add(var.set_mode_sensor(tsens))

    if kw_type_cfg := config.get(CONF_KEEP_WARM_TYPE):
        tsens = await text_sensor.new_text_sensor(kw_type_cfg)
        cg.add(var.set_keep_warm_type_sensor(tsens))

    if kw_time_cfg := config.get(CONF_KEEP_WARM_TIME):
        sens = await sensor.new_sensor(kw_time_cfg)
        cg.add(var.set_keep_warm_time_sensor(sens))

    if target_temp_cfg := config.get(CONF_TARGET_TEMPERATURE):
        n = await number.new_number(
            target_temp_cfg,
            min_value=40,
            max_value=95,
            step=1,
        )
        cg.add(var.set_target_temperature_number(n))
