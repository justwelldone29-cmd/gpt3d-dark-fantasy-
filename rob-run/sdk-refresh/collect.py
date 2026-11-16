"""Collect a fresh SDK from an existing local process; preserve the old dump."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import math


TARGET_SHA256 = '03003754353ff08380dc8e91920f2c77f7977b370d413083acfe9b4bc65c18ab'


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


class Collector:
    def __init__(self, output, expected_hash):
        self.output = Path(output)
        self.expected_hash = expected_hash
        self.event = threading.Event()
        self.error = None
        self.header = None
        self.done = False
        self.count = 0
        self.stream = None
        self.temporary = None

    def message(self, message, _data=None):
        if self.event.is_set():
            return
        try:
            if message.get('type') == 'error':
                raise RuntimeError(message.get('stack') or message.get('description') or 'Agent error')
            if message.get('type') != 'send':
                return
            payload = message['payload']
            kind = payload['kind']
            if kind == 'begin':
                if self.header is not None or payload['schema'] != 2:
                    raise ValueError('Unexpected SDK header')
                binary = Path(payload['module_path'])
                actual = sha256_file(binary)
                if actual != self.expected_hash:
                    raise ValueError(f'Loaded DLL SHA-256 mismatch: {actual}; expected {self.expected_hash}')
                self.header = dict(payload, sha256=actual)
                self.output.parent.mkdir(parents=True, exist_ok=True)
                self.stream = tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                    prefix=self.output.name + '.', suffix='.partial', dir=self.output.parent, delete=False)
                self.temporary = Path(self.stream.name)
                print('DLL verified:', actual, flush=True)
            elif kind == 'batch':
                if self.stream is None:
                    raise ValueError('Received records before the verified header')
                records = payload['records']
                if not isinstance(records, list):
                    raise ValueError('Invalid record batch')
                for record in records:
                    if not all(k in record for k in ('a', 'n', 'p', 'f', 'm', 'signatures')):
                        raise ValueError('Incomplete class record')
                    if len(record['m']) != len(record['signatures']):
                        raise ValueError('Method/signature count mismatch')
                    self.stream.write(json.dumps(record, ensure_ascii=False) + '\n')
                    self.count += 1
            elif kind == 'progress':
                print(payload['assembly'], 'classes:', payload['classes'], flush=True)
            elif kind == 'done':
                if self.header is None or self.count == 0 or payload['classes'] != self.count:
                    raise ValueError('Incomplete dump: final record count does not match')
                self.done = True
                self.event.set()
            elif kind == 'failure':
                raise RuntimeError(payload['error'])
            else:
                raise ValueError('Unknown agent message: ' + str(kind))
        except Exception as error:
            self.error = str(error)
            self.event.set()

    def commit(self):
        if self.error or not self.done:
            raise RuntimeError(self.error or 'Dump did not complete')
        self.stream.flush()
        os.fsync(self.stream.fileno())
        self.stream.close()
        self.stream = None
        os.replace(self.temporary, self.output)
        self.temporary = None
        return self.output

    def close(self):
        if self.stream is not None:
            self.stream.close()
            self.stream = None
        if self.temporary is not None:
            self.temporary.unlink(missing_ok=True)
            self.temporary = None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True, help='PID of the already running local game process')
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent / 'full_sdk.current.jsonl')
    parser.add_argument('--expected-sha256', default=TARGET_SHA256, help='SHA-256 of the target GameAssembly.dll')
    parser.add_argument('--timeout', type=float, default=300, help='Maximum collection time in seconds')
    args = parser.parse_args(argv)
    if args.pid <= 0 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('--pid and --timeout must be positive')
    if len(args.expected_sha256) != 64 or any(c not in '0123456789abcdefABCDEF' for c in args.expected_sha256):
        parser.error('--expected-sha256 must contain 64 hexadecimal characters')
    if args.out.exists() or args.out.with_suffix('.manifest.json').exists():
        parser.error('Output already exists; choose a new --out path to preserve it')
    try:
        import frida
    except ImportError:
        parser.error('Install the dependency with: python -m pip install frida')
    collector = Collector(args.out, args.expected_sha256.lower())
    session = script = None
    try:
        session = frida.get_local_device().attach(args.pid)
        session.on('detached', lambda reason, crash=None:
            collector.message({'type': 'error', 'description': 'Process detached: ' + str(reason)}))
        source = Path(__file__).with_name('dump_runtime.js').read_text(encoding='utf-8')
        script = session.create_script(source)
        script.on('message', collector.message)
        script.load()
        if not collector.event.wait(args.timeout):
            raise TimeoutError('Collection timed out; no final dump was saved')
        result = collector.commit()
        manifest = dict(collector.header, classes=collector.count,
            dump_sha256=sha256_file(result), complete=True)
        result.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        print('Saved:', result, 'classes:', collector.count, flush=True)
        return 0
    except Exception as error:
        print('ERROR:', error, file=sys.stderr)
        return 1
    finally:
        for obj, method in ((script, 'unload'), (session, 'detach')):
            if obj is not None:
                try:
                    getattr(obj, method)()
                except Exception:
                    pass
        collector.close()


if __name__ == '__main__':
    raise SystemExit(main())
