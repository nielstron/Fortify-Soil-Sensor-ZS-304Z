"""Validate the custom-firmware identity and raw-ADC ZHA contract."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import MagicMock

from zha.quirks import DEVICE_REGISTRY
import zigpy.device
import zigpy.types as t

spec = importlib.util.spec_from_file_location('fortify_arteco', Path(__file__).resolve().parents[1] / 'quirk/fortify_arteco_zs304z.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class QuirkContractTest(unittest.IsolatedAsyncioTestCase):
    def device(self, manufacturer, model):
        device = zigpy.device.Device(MagicMock(), t.EUI64.convert('a4:c1:38:dd:63:10:f5:6b'), t.NWK(0x1234))
        device.manufacturer, device.model = manufacturer, model
        for ep_id, clusters in [(1, [0, 1, 3, 0x402, 0x405]), (2, [0x405])]:
            ep = device.add_endpoint(ep_id)
            ep.profile_id, ep.device_type = 260, 0x0302
            for cluster in clusters:
                ep.add_input_cluster(cluster)
        return device

    async def test_raw_attributes_and_config(self):
        device = DEVICE_REGISTRY.resolve(self.device('Fortify', 'ZS-304Z-Raw'))
        raw = device.endpoints[2].humidity
        self.assertIsInstance(raw, module.SoilRawCluster)
        self.assertEqual(raw.attributes_by_name['raw_adc'].id, 0xF000)
        self.assertEqual(raw.attributes_by_name['adc_diode'].id, 0xF001)
        raw.update_attribute('raw_adc', 4321)
        raw.update_attribute('adc_diode', 8765)
        self.assertEqual(raw.get('raw_adc'), 4321)
        self.assertEqual(raw.get('adc_diode'), 8765)
        metadata = DEVICE_REGISTRY.match_entry(device).zha_device_factory.quirk_definition.entity_metadata
        names = [m.attribute_name for m in metadata]
        self.assertEqual(names.count('raw_adc'), 1)
        self.assertEqual(names.count('adc_diode'), 1)
        self.assertIn('measurement_interval', names)

    async def test_debug_and_probe_timing_entities(self):
        device = DEVICE_REGISTRY.resolve(self.device('Fortify', 'ZS-304Z-Raw'))
        ui = device.endpoints[1].thermostat_ui
        self.assertEqual(ui.attributes_by_name['debug_mode'].id, 0x0125)
        self.assertEqual(ui.attributes_by_name['probe_interval'].id, 0x0126)
        self.assertEqual(ui.attributes_by_name['probe_settle_time'].id, 0x0127)
        self.assertEqual(ui.attributes_by_name['probe_report_change'].id, 0x0128)
        self.assertEqual(ui.attributes_by_name['probe_samples'].id, 0x0129)
        ui.update_attribute('debug_mode', True)
        self.assertIs(ui.get('debug_mode'), True)
        metadata = DEVICE_REGISTRY.match_entry(device).zha_device_factory.quirk_definition.entity_metadata
        kinds = {m.attribute_name: type(m).__name__ for m in metadata}
        self.assertEqual(kinds['debug_mode'], 'SwitchMetadata')
        self.assertEqual(kinds['probe_interval'], 'NumberMetadata')
        self.assertEqual(kinds['probe_settle_time'], 'NumberMetadata')
        self.assertEqual(kinds['probe_report_change'], 'NumberMetadata')
        samples = next(m for m in metadata if m.attribute_name == 'probe_samples')
        self.assertEqual((samples.min, samples.max), (1, 16))
        interval = next(m for m in metadata if m.attribute_name == 'probe_interval')
        self.assertEqual((interval.min, interval.max), (10, 43200))

    async def test_pwm_heartbeat_and_calibration_entities(self):
        device = DEVICE_REGISTRY.resolve(self.device('Fortify', 'ZS-304Z-Raw'))
        ui = device.endpoints[1].thermostat_ui
        expected = {'probe_heartbeat': 0x012A, 'probe_pwm_frequency': 0x012B, 'probe_pwm_duty': 0x012C,
                    'probe_cal_dry': 0x012D, 'probe_cal_wet': 0x012E, 'probe_cal_capture': 0x012F}
        for name, attr_id in expected.items():
            self.assertEqual(ui.attributes_by_name[name].id, attr_id)
        entry = DEVICE_REGISTRY.match_entry(device).zha_device_factory.quirk_definition
        buttons = sorted(m.attribute_value for m in entry.entity_metadata
                         if m.attribute_name == 'probe_cal_capture' and type(m).__name__ == 'WriteAttributeButtonMetadata')
        self.assertEqual(buttons, [1, 2])
        moisture = next(m for m in entry.entity_metadata if m.attribute_name == 'measured_value')
        self.assertEqual((moisture.endpoint_id, moisture.divisor, moisture.device_class.value), (2, 100, 'moisture'))
        # ZHA's default "Humidity" entity on the EP2 soil cluster is suppressed in favour of "Soil moisture"
        self.assertIn((2, 0x0405), [(p.endpoint_id, p.cluster_id) for p in entry.disabled_default_entities])

    async def test_probe_sweep_entities(self):
        device = DEVICE_REGISTRY.resolve(self.device('Fortify', 'ZS-304Z-Raw'))
        self.assertEqual(device.endpoints[1].thermostat_ui.attributes_by_name['probe_sweep'].id, 0x0130)
        soil = device.endpoints[2].humidity
        settle_ms = [1, 5, 20, 50, 100, 300, 1000]
        for i, ms in enumerate(settle_ms):
            self.assertEqual(soil.attributes_by_name[f'sweep_{ms}ms'].id, 0xF010 + i)
        metadata = DEVICE_REGISTRY.match_entry(device).zha_device_factory.quirk_definition.entity_metadata
        button = [m for m in metadata if m.attribute_name == 'probe_sweep']
        self.assertEqual([(type(m).__name__, m.attribute_value) for m in button], [('WriteAttributeButtonMetadata', 1)])
        sensors = [m for m in metadata if m.attribute_name.startswith('sweep_')]
        self.assertEqual(sorted(m.attribute_name for m in sensors), sorted(f'sweep_{ms}ms' for ms in settle_ms))
        self.assertTrue(all(m.endpoint_id == 2 and m.entity_type.value == 'diagnostic' for m in sensors))
        # distinct unique ids: one entity per settle time
        self.assertEqual(len({m.unique_id_suffix or m.attribute_name for m in sensors}), len(settle_ms))

    async def test_does_not_match_stock_or_original_fortify(self):
        for manufacturer, model in [('Arteco', 'ZS-304Z'), ('AOYAN  ', 'AY-303Z'), ('Fortify', 'Soil-Moisture')]:
            with self.subTest(model=model):
                self.assertIsNone(DEVICE_REGISTRY.match_entry(self.device(manufacturer, model)))


if __name__ == '__main__':
    unittest.main()
