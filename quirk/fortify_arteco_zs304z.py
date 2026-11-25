"""Arteco ZS-304Z / ZSSF21 (Fortify custom firmware, raw-ADC port).

Exposes:
  - Temperature offset / Humidity offset / Reporting interval  (HVAC UI cluster 0x0204)
  - Soil raw ADC (0xF000) and Soil ADC reference / diode (0xF001) on the EP2
    relative-humidity cluster, for DIY calibration in Home Assistant.
  - Debug mode switch (0x0204/0x0125): probe read and both raw values reported
    every 500 ms; switches itself off after 10 min (RAM only).
  - Probe interval (0x0126, s) and probe settle time (0x0127, ms), saved on the
    device. Temperature/humidity/battery keep the reporting interval.
  - Two-point calibration (0x012D-0x012F) with battery compensation of both
    points (0x0131-0x0134; stock slopes 905/804 per mille).

Requires the custom firmware built with the raw_adc/adc_diode attributes.
Based on https://github.com/pvvx/ZigbeeTLc (zhaquirks/custom/zigbeetlc.py).
"""

from zhaquirks.builder import QuirkBuilder
from zhaquirks.builder import PERCENTAGE, UnitOfTemperature, UnitOfTime
from zhaquirks.builder import EntityType, SensorDeviceClass, SensorStateClass
import zigpy.types as t
from zigpy.zcl import ClusterType
from zigpy.zcl.clusters.hvac import UserInterface
from zigpy.zcl.clusters.measurement import RelativeHumidity
from zigpy.zcl.foundation import ZCLAttributeDef

from zhaquirks.clusters import CustomCluster


class TxPowerMode(t.enum8):
    """Adaptive TX power control mode."""

    Fixed = 0
    Adaptive = 1


class CustomUserInterfaceCluster(CustomCluster, UserInterface):
    """pvvx ZigbeeTLc config attributes on cluster 0x0204."""

    class AttributeDefs(UserInterface.AttributeDefs):
        temperature_offset = ZCLAttributeDef(
            id=0x0100, type=t.int16s, access="rw", is_manufacturer_specific=False
        )
        humidity_offset = ZCLAttributeDef(
            id=0x0101, type=t.int16s, access="rw", is_manufacturer_specific=False
        )
        measurement_interval = ZCLAttributeDef(
            id=0x0107, type=t.uint8_t, access="rw", is_manufacturer_specific=False
        )
        # Adaptive TX power control (Fortify firmware >= 0x01413001)
        tx_power_mode = ZCLAttributeDef(
            id=0x0120, type=t.uint8_t, access="rw", is_manufacturer_specific=False
        )
        tx_power_min = ZCLAttributeDef(
            id=0x0121, type=t.uint8_t, access="rw", is_manufacturer_specific=False
        )
        tx_power_max = ZCLAttributeDef(
            id=0x0122, type=t.uint8_t, access="rw", is_manufacturer_specific=False
        )
        tx_power_fixed = ZCLAttributeDef(
            id=0x0123, type=t.uint8_t, access="rw", is_manufacturer_specific=False
        )
        tx_power_current = ZCLAttributeDef(
            id=0x0124, type=t.int8s, access="rp", is_manufacturer_specific=False
        )
        # Debug mode: 500 ms raw-probe reports, auto-off after 10 min (RAM only)
        debug_mode = ZCLAttributeDef(
            id=0x0125, type=t.Bool, access="rwp", is_manufacturer_specific=False
        )
        # Soil probe timing (saved on the device)
        probe_interval = ZCLAttributeDef(
            id=0x0126, type=t.uint16_t, access="rw", is_manufacturer_specific=False
        )
        probe_settle_time = ZCLAttributeDef(
            id=0x0127, type=t.uint16_t, access="rw", is_manufacturer_specific=False
        )
        probe_report_change = ZCLAttributeDef(
            id=0x0128, type=t.uint16_t, access="rw", is_manufacturer_specific=False
        )
        probe_samples = ZCLAttributeDef(
            id=0x0129, type=t.uint8_t, access="rw", is_manufacturer_specific=False
        )
        probe_heartbeat = ZCLAttributeDef(
            id=0x012A, type=t.uint8_t, access="rw", is_manufacturer_specific=False
        )
        probe_pwm_frequency = ZCLAttributeDef(
            id=0x012B, type=t.uint16_t, access="rw", is_manufacturer_specific=False
        )
        probe_pwm_duty = ZCLAttributeDef(
            id=0x012C, type=t.uint8_t, access="rw", is_manufacturer_specific=False
        )
        probe_cal_dry = ZCLAttributeDef(
            id=0x012D, type=t.uint16_t, access="rwp", is_manufacturer_specific=False
        )
        probe_cal_wet = ZCLAttributeDef(
            id=0x012E, type=t.uint16_t, access="rwp", is_manufacturer_specific=False
        )
        # Command: write 1 / 2 to store the current reading as the dry / wet point
        probe_cal_capture = ZCLAttributeDef(
            id=0x012F, type=t.uint8_t, access="rwp", is_manufacturer_specific=False
        )
        # Command: write 1 to run a probe sweep (results on EP2 0xF010..0xF016);
        # reported back as 0 when the sweep has finished
        probe_sweep = ZCLAttributeDef(
            id=0x0130, type=t.uint8_t, access="rwp", is_manufacturer_specific=False
        )
        # Command: write 1 to rejoin through the best parent in range; reported
        # back as 0 just before the device starts the rejoin
        rehome = ZCLAttributeDef(
            id=0x0137, type=t.uint8_t, access="rwp", is_manufacturer_specific=False
        )
        # Battery compensation of the calibration points (stock Arteco model)
        probe_cal_dry_voltage = ZCLAttributeDef(
            id=0x0131, type=t.uint16_t, access="rwp", is_manufacturer_specific=False
        )
        probe_cal_wet_voltage = ZCLAttributeDef(
            id=0x0132, type=t.uint16_t, access="rwp", is_manufacturer_specific=False
        )
        probe_comp_slope_dry = ZCLAttributeDef(
            id=0x0133, type=t.uint16_t, access="rw", is_manufacturer_specific=False
        )
        probe_comp_slope_wet = ZCLAttributeDef(
            id=0x0134, type=t.uint16_t, access="rw", is_manufacturer_specific=False
        )


class SoilRawCluster(CustomCluster, RelativeHumidity):
    """EP2 relative-humidity (soil) cluster + raw ADC / diode-reference attributes."""

    class AttributeDefs(RelativeHumidity.AttributeDefs):
        raw_adc = ZCLAttributeDef(id=0xF000, type=t.uint16_t, access="rp")
        adc_diode = ZCLAttributeDef(id=0xF001, type=t.uint16_t, access="rp")
        # Probe sweep results: raw ADC at each settle time (0 = not measured yet)
        sweep_1ms = ZCLAttributeDef(id=0xF010, type=t.uint16_t, access="rp")
        sweep_5ms = ZCLAttributeDef(id=0xF011, type=t.uint16_t, access="rp")
        sweep_20ms = ZCLAttributeDef(id=0xF012, type=t.uint16_t, access="rp")
        sweep_50ms = ZCLAttributeDef(id=0xF013, type=t.uint16_t, access="rp")
        sweep_100ms = ZCLAttributeDef(id=0xF014, type=t.uint16_t, access="rp")
        sweep_300ms = ZCLAttributeDef(id=0xF015, type=t.uint16_t, access="rp")
        sweep_1000ms = ZCLAttributeDef(id=0xF016, type=t.uint16_t, access="rp")


PROBE_SWEEP_MS = (1, 5, 20, 50, 100, 300, 1000)


builder = (
    QuirkBuilder("Fortify", "ZS-304Z-Raw")
    .removes(CustomUserInterfaceCluster.cluster_id, cluster_type=ClusterType.Client)
    .adds(CustomUserInterfaceCluster)
    .replaces(SoilRawCluster, endpoint_id=2)
    .number(
        CustomUserInterfaceCluster.AttributeDefs.temperature_offset.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=-327.67,
        max_value=327.67,
        step=0.01,
        unit=UnitOfTemperature.CELSIUS,
        translation_key="temperature_offset",
        fallback_name="Temperature offset",
        multiplier=0.01,
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.humidity_offset.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=-327.67,
        max_value=327.67,
        step=0.01,
        unit=PERCENTAGE,
        translation_key="humidity_offset",
        fallback_name="Humidity offset",
        multiplier=0.01,
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.measurement_interval.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=3,
        max_value=255,
        unit=UnitOfTime.SECONDS,
        translation_key="reporting_interval",
        fallback_name="Reporting interval",
        mode="box",
    )
    .enum(
        CustomUserInterfaceCluster.AttributeDefs.tx_power_mode.name,
        TxPowerMode,
        CustomUserInterfaceCluster.cluster_id,
        translation_key="tx_power_mode",
        fallback_name="TX power mode",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.tx_power_min.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=3,
        max_value=10,
        step=1,
        unit="dBm",
        translation_key="tx_power_min",
        fallback_name="TX power min",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.tx_power_max.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=3,
        max_value=10,
        step=1,
        unit="dBm",
        translation_key="tx_power_max",
        fallback_name="TX power max",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.tx_power_fixed.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=3,
        max_value=10,
        step=1,
        unit="dBm",
        translation_key="tx_power_fixed",
        fallback_name="TX power (fixed mode)",
        mode="box",
    )
    .sensor(
        CustomUserInterfaceCluster.AttributeDefs.tx_power_current.name,
        CustomUserInterfaceCluster.cluster_id,
        state_class=SensorStateClass.MEASUREMENT,
        unit="dBm",
        translation_key="tx_power_current",
        fallback_name="TX power current",
    )
    .switch(
        CustomUserInterfaceCluster.AttributeDefs.debug_mode.name,
        CustomUserInterfaceCluster.cluster_id,
        translation_key="debug_mode",
        fallback_name="Debug mode (500 ms probe reports, 10 min)",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_interval.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=10,
        max_value=43200,
        step=1,
        unit=UnitOfTime.SECONDS,
        translation_key="probe_interval",
        fallback_name="Probe interval",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_settle_time.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=1,
        max_value=1000,
        step=1,
        unit=UnitOfTime.MILLISECONDS,
        translation_key="probe_settle_time",
        fallback_name="Probe settle time",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_report_change.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=1,
        max_value=2000,
        step=1,
        translation_key="probe_report_change",
        fallback_name="Probe report threshold (raw counts)",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_samples.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=1,
        max_value=16,
        step=1,
        translation_key="probe_samples",
        fallback_name="Probe ADC samples per reading",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_heartbeat.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=1,
        max_value=100,
        step=1,
        translation_key="probe_heartbeat",
        fallback_name="Probe heartbeat (every N probe intervals)",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_pwm_frequency.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=10,
        max_value=2000,
        step=1,
        unit="kHz",
        translation_key="probe_pwm_frequency",
        fallback_name="Probe excitation frequency",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_pwm_duty.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=1,
        max_value=99,
        step=1,
        unit=PERCENTAGE,
        translation_key="probe_pwm_duty",
        fallback_name="Probe excitation duty",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_cal_dry.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=0,
        max_value=65535,
        step=1,
        translation_key="probe_cal_dry",
        fallback_name="Probe calibration dry (raw, 0 = unset)",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_cal_wet.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=0,
        max_value=65535,
        step=1,
        translation_key="probe_cal_wet",
        fallback_name="Probe calibration wet (raw, 0 = unset)",
        mode="box",
    )
    # Command switches: on = pending/running, the device switches them back off
    # (reports 0) once the calibration point is stored.
    .switch(
        CustomUserInterfaceCluster.AttributeDefs.probe_cal_capture.name,
        CustomUserInterfaceCluster.cluster_id,
        on_value=1,
        off_value=0,
        unique_id_suffix="cal_dry_now",
        translation_key="probe_cal_dry_now",
        fallback_name="Calibrate dry (use current reading)",
    )
    .switch(
        CustomUserInterfaceCluster.AttributeDefs.probe_cal_capture.name,
        CustomUserInterfaceCluster.cluster_id,
        on_value=2,
        off_value=0,
        unique_id_suffix="cal_wet_now",
        translation_key="probe_cal_wet_now",
        fallback_name="Calibrate wet (use current reading)",
    )
    # Only ZHA's default "Humidity" sensor on the soil endpoint (replaced by "Soil
    # moisture" below). The filter also applies to quirk-defined entities (generic
    # `Sensor` class), so it must not match the whole endpoint/cluster.
    .prevent_default_entity_creation(
        endpoint_id=2,
        cluster_id=SoilRawCluster.cluster_id,
        function=lambda entity: type(entity).__name__ == "Humidity",
    )
    .sensor(
        SoilRawCluster.AttributeDefs.measured_value.name,
        SoilRawCluster.cluster_id,
        endpoint_id=2,
        divisor=100,
        device_class=SensorDeviceClass.MOISTURE,
        state_class=SensorStateClass.MEASUREMENT,
        unit=PERCENTAGE,
        translation_key="soil_moisture",
        fallback_name="Soil moisture",
    )
    .sensor(
        SoilRawCluster.AttributeDefs.raw_adc.name,
        SoilRawCluster.cluster_id,
        endpoint_id=2,
        state_class=SensorStateClass.MEASUREMENT,
        translation_key="soil_raw_adc",
        fallback_name="Soil raw ADC",
    )
    .sensor(
        SoilRawCluster.AttributeDefs.adc_diode.name,
        SoilRawCluster.cluster_id,
        endpoint_id=2,
        state_class=SensorStateClass.MEASUREMENT,
        translation_key="soil_adc_reference",
        fallback_name="Soil ADC reference",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_cal_dry_voltage.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=0,
        max_value=4000,
        step=1,
        unit="mV",
        translation_key="probe_cal_dry_voltage",
        fallback_name="Probe calibration dry: battery (0 = no compensation)",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_cal_wet_voltage.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=0,
        max_value=4000,
        step=1,
        unit="mV",
        translation_key="probe_cal_wet_voltage",
        fallback_name="Probe calibration wet: battery (0 = no compensation)",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_comp_slope_dry.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=0,
        max_value=2000,
        step=1,
        unit="‰",
        translation_key="probe_comp_slope_dry",
        fallback_name="Battery compensation slope dry (stock 905)",
        mode="box",
    )
    .number(
        CustomUserInterfaceCluster.AttributeDefs.probe_comp_slope_wet.name,
        CustomUserInterfaceCluster.cluster_id,
        min_value=0,
        max_value=2000,
        step=1,
        unit="‰",
        translation_key="probe_comp_slope_wet",
        fallback_name="Battery compensation slope wet (stock 804)",
        mode="box",
    )
    .switch(
        CustomUserInterfaceCluster.AttributeDefs.probe_sweep.name,
        CustomUserInterfaceCluster.cluster_id,
        translation_key="probe_sweep",
        fallback_name="Run probe sweep",
    )
    .switch(
        CustomUserInterfaceCluster.AttributeDefs.rehome.name,
        CustomUserInterfaceCluster.cluster_id,
        translation_key="rehome",
        fallback_name="Find better parent (rejoin)",
    )
)
for _ms in PROBE_SWEEP_MS:
    builder = builder.sensor(
        f"sweep_{_ms}ms",
        SoilRawCluster.cluster_id,
        endpoint_id=2,
        state_class=SensorStateClass.MEASUREMENT,
        entity_type=EntityType.DIAGNOSTIC,
        translation_key="probe_sweep_result",
        translation_placeholders={"settle": f"{_ms} ms"},
        fallback_name=f"Probe sweep {_ms} ms",
    )
builder.add_to_registry()
