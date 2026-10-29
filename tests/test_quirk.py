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

    async def test_does_not_match_stock_or_original_fortify(self):
        for manufacturer, model in [('Arteco', 'ZS-304Z'), ('AOYAN  ', 'AY-303Z'), ('Fortify', 'Soil-Moisture')]:
            with self.subTest(model=model):
                self.assertIsNone(DEVICE_REGISTRY.match_entry(self.device(manufacturer, model)))


if __name__ == '__main__':
    unittest.main()
