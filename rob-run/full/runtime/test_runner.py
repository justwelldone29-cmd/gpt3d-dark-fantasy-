import importlib.util
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('silent_runner', ROOT / 'run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class FakeRPC:
    def __init__(self, path, arch='x64'):
        self.path = path
        self.arch = arch
        self.calls = []

    def moduleinfo(self):
        return dict(path=str(self.path), platform='windows', arch=self.arch)

    def start(self, config):
        self.calls.append(('start', config.copy()))
        return dict(installed=True, **config)

    def loadmenu(self, path):
        return dict(name='RobRunInternal.dll', path=path)

    def configure(self, config):
        self.calls.append(('configure', config))
        return config

    def stop(self):
        self.calls.append(('stop',))


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dll = Path(self.temp.name) / 'GameAssembly.dll'
        self.dll.write_bytes(b'fixture DLL')
        self.menu = Path(self.temp.name) / 'RobRunInternal.dll'
        self.menu.write_bytes(b'fixture menu DLL')
        self.rpc = FakeRPC(self.dll)
        self.unloaded = self.detached = False
        test = self

        class Script:
            exports_sync = test.rpc
            def on(self, *args): pass
            def load(self): pass
            def unload(self): test.unloaded = True

        class Session:
            def on(self, *args): pass
            def create_script(self, source):
                assert 'const AimMath' in source and 'rpc.exports' in source
                return Script()
            def detach(self): test.detached = True

        class Device:
            def attach(self, pid):
                assert pid == 42
                return Session()

        self.frida = types.SimpleNamespace(get_local_device=lambda: Device())

    def tearDown(self):
        self.temp.cleanup()

    def run_main(self, commands=('quit',), expected=None):
        with patch.dict('sys.modules', {'frida': self.frida}), \
             patch.object(runner, 'TARGET_SHA256', expected or runner.sha256_file(self.dll)), \
             patch('builtins.input', side_effect=commands):
            return runner.main(['--pid', '42', '--menu-dll', str(self.menu)])

    def test_wrong_dll_prevents_hook_installation(self):
        self.assertEqual(self.run_main(expected='0' * 64), 1)
        self.assertEqual(self.rpc.calls, [])
        self.assertTrue(self.unloaded and self.detached)

    def test_unsupported_architecture_prevents_hook_installation(self):
        self.rpc.arch = 'arm64'
        self.assertEqual(self.run_main(), 1)
        self.assertEqual(self.rpc.calls, [])
        self.assertTrue(self.unloaded and self.detached)

    def test_defaults_disabled_and_restores_hooks_on_exit(self):
        self.assertEqual(self.run_main(), 0)
        self.assertFalse(self.rpc.calls[0][1]['enabled'])
        self.assertEqual(self.rpc.calls[-1], ('stop',))
        self.assertTrue(self.unloaded and self.detached)

    def test_console_toggles_and_configures_module(self):
        self.assertEqual(self.run_main(('on', 'fov 6', 'distance 150', 'off', 'quit')), 0)
        self.assertEqual(self.rpc.calls[1:5], [
            ('configure', {'enabled': True}), ('configure', {'fovDegrees': 6.0}),
            ('configure', {'maxDistance': 150.0}), ('configure', {'enabled': False})])
        self.assertEqual(self.rpc.calls[-1], ('stop',))


if __name__ == '__main__':
    unittest.main()
