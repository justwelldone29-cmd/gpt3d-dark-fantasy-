import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('collect', ROOT / 'collect.py')
collect = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collect)


class CollectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.binary = self.root / 'GameAssembly.dll'
        self.binary.write_bytes(b'test DLL identity')
        self.output = self.root / 'sdk.jsonl'
        self.output.write_text('previous working dump\n')
        self.collector = collect.Collector(self.output, collect.sha256_file(self.binary))

    def tearDown(self):
        self.collector.close()
        self.temp.cleanup()

    def send(self, **payload):
        self.collector.message({'type': 'send', 'payload': payload})

    def begin(self):
        self.send(kind='begin', schema=2, module_path=str(self.binary), pid=42)

    def records(self):
        self.send(kind='batch', records=[dict(a='Battle.dll', n='Example', p='System.Object',
            f=[], m=[['Fire', 1, 4096]], signatures=[dict(name='Fire', rva=4096,
                return_type='System.Void', parameters=[dict(name='direction', type='System.Numerics.Vector3')])])])

    def assert_previous_preserved(self):
        self.assertEqual(self.output.read_text(), 'previous working dump\n')

    def test_success_preserves_old_until_complete(self):
        self.begin()
        self.records()
        self.assert_previous_preserved()
        self.send(kind='done', classes=1)
        self.collector.commit()
        row = json.loads(self.output.read_text())
        self.assertEqual(row['signatures'][0]['parameters'][0]['type'], 'System.Numerics.Vector3')
        self.assertEqual(row['m'][0], ['Fire', 1, 4096])

    def test_wrong_build_does_not_write_a_dump(self):
        self.collector.expected_hash = '0' * 64
        self.begin()
        self.assertIn('SHA-256 mismatch', self.collector.error)
        self.assertIsNone(self.collector.temporary)
        self.assert_previous_preserved()

    def test_disconnect_preserves_old_dump(self):
        self.begin()
        self.records()
        self.collector.message({'type': 'error', 'description': 'Process detached'})
        with self.assertRaisesRegex(RuntimeError, 'Process detached'):
            self.collector.commit()
        self.collector.close()
        self.assert_previous_preserved()
        self.assertFalse(list(self.root.glob('*.partial')))

    def test_truncated_record_stream_is_rejected(self):
        self.begin()
        self.records()
        self.send(kind='done', classes=2)
        with self.assertRaisesRegex(RuntimeError, 'record count'):
            self.collector.commit()
        self.assert_previous_preserved()

    def test_incomplete_dump_cannot_be_committed(self):
        self.begin()
        self.records()
        with self.assertRaisesRegex(RuntimeError, 'did not complete'):
            self.collector.commit()
        self.assert_previous_preserved()

    def test_missing_signatures_cannot_replace_dump(self):
        self.begin()
        self.send(kind='batch', records=[dict(a='Battle.dll', n='Example', p='', f=[], m=[])])
        self.assertIn('Incomplete class record', self.collector.error)
        with self.assertRaises(RuntimeError):
            self.collector.commit()
        self.assert_previous_preserved()

    def test_records_before_build_verification_are_rejected(self):
        self.records()
        self.assertIn('verified header', self.collector.error)
        self.assert_previous_preserved()

    def test_late_messages_cannot_mutate_completed_dump(self):
        self.begin()
        self.records()
        self.send(kind='done', classes=1)
        self.records()
        self.assertEqual(self.collector.count, 1)
        self.collector.commit()
        self.assertEqual(len(self.output.read_text().splitlines()), 1)


if __name__ == '__main__':
    unittest.main()
