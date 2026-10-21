import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, sensor, web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome import pins
from esphome.const import CONF_ID, CONF_FLOW_CONTROL_PIN

CODEOWNERS = []
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "web_server_base"]  # web_server_base: GET /registers.json

ns = cg.esphome_ns.namespace("thermia_gateway")
ThermiaGateway = ns.class_("ThermiaGateway", cg.Component, uart.UARTDevice)

CONF_SLAVE_ADDRESS = "slave_address"
CONF_FRAME_GAP_US = "frame_gap_us"
CONF_LOG_CHANGES = "log_changes"
CONF_REGISTERS = "registers"
CONF_REGISTER = "register"
CONF_SIGNED = "signed"
CONF_TAKEOVER_CAPABLE = "takeover_capable"
CONF_AUTO_TAKEOVER = "auto_takeover"
CONF_AUTO_LISTEN_MS = "auto_listen_ms"


def _unique_registers(value):
    seen = set()
    for r in value:
        if r[CONF_REGISTER] in seen:
            raise cv.Invalid(f"register {r[CONF_REGISTER]} is listed more than once")
        seen.add(r[CONF_REGISTER])
    return value


REGISTER_SCHEMA = sensor.sensor_schema(accuracy_decimals=1).extend(
    {
        cv.Required(CONF_REGISTER): cv.int_range(min=0, max=4095),
        cv.Optional(CONF_SIGNED, default=True): cv.boolean,
    }
)

def _require_flow_pin(config):
    if config[CONF_TAKEOVER_CAPABLE] and CONF_FLOW_CONTROL_PIN not in config:
        raise cv.Invalid("takeover_capable: true requires a flow_control_pin (the RS485 driver-enable)")
    if config[CONF_AUTO_TAKEOVER] and not config[CONF_TAKEOVER_CAPABLE]:
        raise cv.Invalid("auto_takeover: true requires takeover_capable: true")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ThermiaGateway),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
            cv.Optional(CONF_SLAVE_ADDRESS, default=15): cv.int_range(min=1, max=247),
            cv.Optional(CONF_FRAME_GAP_US, default=3000): cv.int_range(min=500, max=20000),
            cv.Optional(CONF_LOG_CHANGES, default=False): cv.boolean,
            # Opt-in to transmit. Default False = the component can never be armed (receive
            # only). A flow_control_pin is REQUIRED when True (the RS485 driver-enable, held
            # low except while an armed reply is sent).
            cv.Optional(CONF_TAKEOVER_CAPABLE, default=False): cv.boolean,
            # Auto-takeover. Requires takeover_capable. The ESP self-arms after listening
            # auto_listen_ms for a real gateway and hearing none, then requests one resync.
            cv.Optional(CONF_AUTO_TAKEOVER, default=False): cv.boolean,
            cv.Optional(CONF_AUTO_LISTEN_MS, default="20s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_FLOW_CONTROL_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_REGISTERS, default=[]): cv.All(
                cv.ensure_list(REGISTER_SCHEMA), _unique_registers
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _require_flow_pin,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_slave_address(config[CONF_SLAVE_ADDRESS]))
    cg.add(var.set_frame_gap_us(config[CONF_FRAME_GAP_US]))
    cg.add(var.set_log_changes(config[CONF_LOG_CHANGES]))
    cg.add(var.set_takeover_capable(config[CONF_TAKEOVER_CAPABLE]))
    cg.add(var.set_auto_takeover(config[CONF_AUTO_TAKEOVER]))
    cg.add(var.set_auto_listen_ms(config[CONF_AUTO_LISTEN_MS]))
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(var.set_web_base(base))
    if CONF_FLOW_CONTROL_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_FLOW_CONTROL_PIN])
        cg.add(var.set_flow_control_pin(pin))
    for r in config[CONF_REGISTERS]:
        s = await sensor.new_sensor(r)
        cg.add(var.add_register(r[CONF_REGISTER], r[CONF_SIGNED], s))
