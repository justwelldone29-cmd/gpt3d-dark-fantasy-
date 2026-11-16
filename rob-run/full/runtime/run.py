"""Experimental silent-aim sidecar for an existing Windows x64 process."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import threading


TARGET_SHA256 = '03003754353ff08380dc8e91920f2c77f7977b370d413083acfe9b4bc65c18ab'


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, help='Game PID; auto-detect one ROB RUN.exe if omitted')
    parser.add_argument('--menu-dll', type=Path, help='New RobRunInternal.dll path')
    parser.add_argument('--fov', type=float, default=10, help='Full cone angle in degrees (0.1..90)')
    parser.add_argument('--distance', type=float, default=200, help='Maximum target distance (1..500)')
    parser.add_argument('--enabled', action='store_true', help='Enable redirects immediately after verification')
    args = parser.parse_args(argv)
    if (args.pid is not None and args.pid <= 0) or not 0.1 <= args.fov <= 90 or not 1 <= args.distance <= 500:
        parser.error('Invalid PID, FOV, or distance')
    try:
        import frida
    except ImportError:
        parser.error('Install the dependency: python -m pip install frida')
    root = Path(__file__).resolve().parent
    detached = threading.Event()
    session = script = None
    started = False

    def message(item, _data):
        if item.get('type') == 'send':
            print(json.dumps(item['payload'], ensure_ascii=False), flush=True)
        elif item.get('type') == 'error':
            print('AGENT ERROR:', item.get('stack') or item.get('description'), file=sys.stderr, flush=True)

    try:
        dll_root = Path(sys.executable).resolve().parent if getattr(sys, 'frozen', False) else root.parent
        menu_dll = args.menu_dll or dll_root / 'RobRunInternal.dll'
        if not menu_dll.is_file():
            raise RuntimeError('New menu DLL is missing: ' + str(menu_dll))
        device = frida.get_local_device()
        pid = args.pid
        if pid is None:
            matches = [p for p in device.enumerate_processes() if p.name.lower() == 'rob run.exe']
            if len(matches) != 1:
                raise RuntimeError('Start the game first, or specify --pid when multiple game processes exist')
            pid = matches[0].pid
        session = device.attach(pid)
        session.on('detached', lambda reason, crash=None: detached.set())
        source = '\n'.join((root / name).read_text(encoding='utf-8') for name in ('aim_math.js', 'agent.js'))
        script = session.create_script(source)
        script.on('message', message)
        script.load()
        rpc = script.exports_sync
        info = rpc.moduleinfo()
        if info['platform'] != 'windows' or info['arch'] != 'x64':
            raise RuntimeError('Only Windows x64 is supported')
        actual_hash = sha256_file(info['path'])
        if actual_hash != TARGET_SHA256:
            raise RuntimeError('Loaded DLL does not match the supplied build; no hooks installed. SHA-256: ' + actual_hash)
        print('Menu DLL:', rpc.loadmenu(str(menu_dll.resolve())), flush=True)
        config = dict(enabled=args.enabled, fovDegrees=args.fov, maxDistance=args.distance)
        print(json.dumps(rpc.start(config), ensure_ascii=False), flush=True)
        started = True
        print('Press Insert in the game, then Silent Aim. Console: on, off, status, fov 10, distance 200, quit', flush=True)
        while not detached.is_set():
            try:
                parts = input('silent> ').strip().split()
            except EOFError:
                break
            if not parts:
                continue
            command = parts[0].lower()
            if command == 'quit':
                break
            try:
                if command == 'status' and len(parts) == 1:
                    result = rpc.status()
                elif command in ('on', 'off') and len(parts) == 1:
                    result = rpc.configure(dict(enabled=command == 'on'))
                elif command in ('fov', 'distance') and len(parts) == 2:
                    key = 'fovDegrees' if command == 'fov' else 'maxDistance'
                    result = rpc.configure({key: float(parts[1])})
                else:
                    print('Unknown command', flush=True)
                    continue
                print(json.dumps(result, ensure_ascii=False), flush=True)
            except Exception as error:
                print('COMMAND ERROR:', error, file=sys.stderr, flush=True)
        return 0
    except Exception as error:
        print('ERROR:', error, file=sys.stderr)
        return 1
    finally:
        if script is not None:
            if started and not detached.is_set():
                try:
                    script.exports_sync.stop()
                except Exception:
                    pass
            try:
                script.unload()
            except Exception:
                pass
        if session is not None:
            try:
                session.detach()
            except Exception:
                pass


if __name__ == '__main__':
    result = main()
    if result and getattr(sys, 'frozen', False):
        try:
            input('Press Enter to close...')
        except (EOFError, KeyboardInterrupt):
            pass
    raise SystemExit(result)
