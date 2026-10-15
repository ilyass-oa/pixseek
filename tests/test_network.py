"""End-to-end checks against real loopback sockets and subprocesses."""
import concurrent.futures
import contextlib
import os
from pathlib import Path
import re
import selectors
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('PIXSEEK_BUILD', ROOT / 'build')).resolve()
SERVER, CLIENT = BUILD / 'pixseek-server', BUILD / 'pixseek-client'
MAX_IMAGE = 4 * 1024 * 1024


def bmp(seed=1, width=16, height=16, bpp=24, top_down=False):
    rows = []
    state = seed
    stride = ((width * bpp + 31) // 32) * 4
    for _ in range(height):
        row = bytearray()
        for _ in range(width):
            state = (state * 1664525 + 1013904223) & 0xffffffff
            row.extend(((state >> 24) & 255, (state >> 16) & 255, (state >> 8) & 255))
            if bpp == 32:
                row.append(123)
        row.extend(b'\xab' * (stride - len(row)))
        rows.append(bytes(row))
    pixels = b''.join(rows if top_down else reversed(rows))
    return (struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
            + struct.pack('<IiiHHIIiiII', 40, width, -height if top_down else height,
                          1, bpp, 0, len(pixels), 0, 0, 0, 0) + pixels)


def frame(data):
    return struct.pack('!I', len(data)) + data


def reply(sock):
    data = bytearray()
    while len(data) < 4096:
        byte = sock.recv(1)
        if not byte:
            raise EOFError('Peer closed before the NUL-terminated response')
        if byte == b'\0':
            return data.decode()
        data.extend(byte)
    raise AssertionError('Oversized response')


class Server:
    def __init__(self, catalog, *extra):
        self.closed = False
        self.errors = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [str(SERVER), '--images', str(catalog), '--port', '0', '--workers', '2',
             '--max-clients', '32', '--timeout-ms', '1500', *extra],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=self.errors)
        try:
            output = b''
            deadline = time.monotonic() + 20
            with selectors.DefaultSelector() as selector:
                selector.register(self.process.stdout, selectors.EVENT_READ)
                while time.monotonic() < deadline:
                    if self.process.poll() is not None:
                        raise AssertionError('Server exited on startup: ' + self.stderr())
                    if selector.select(.1):
                        output += os.read(self.process.stdout.fileno(), 4096)
                        match = re.search(rb'Listening on [^:\n]+:(\d+)\n', output)
                        if match:
                            self.port = int(match[1])
                            self.startup = output.decode()
                            return
            raise AssertionError('Server startup timed out: ' + self.stderr())
        except BaseException:
            self.close(check=False)
            raise

    def stderr(self):
        self.errors.seek(0)
        return self.errors.read().decode(errors='replace')

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=3)

    def close(self, check=True, sig=signal.SIGTERM):
        if self.closed:
            return
        self.closed = True
        if self.process.poll() is None:
            self.process.send_signal(sig)
        try:
            status = self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError('Server did not shut down')
        finally:
            self.process.stdout.close()
        errors = self.stderr()
        self.errors.close()
        if check and (status != 0 or 'Sanitizer' in errors or 'runtime error:' in errors):
            raise AssertionError(f'Server exit {status}: {errors}')


class NetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='pixseek-')
        cls.directory = Path(cls.temp.name)
        # These characters must be handled as a literal path, without a shell.
        cls.catalog = cls.directory / "catalog 'images; literal"
        cls.catalog.mkdir()
        cls.images = [bmp(seed) for seed in (7, 31, 99)]
        cls.paths = [cls.catalog / name for name in ('a.bmp', 'b.bmp', 'c.BMP')]
        for path, data in zip(cls.paths, cls.images):
            path.write_bytes(data)
        (cls.catalog / 'broken.bmp').write_bytes(b'BMbroken')
        (cls.catalog / 'notes.txt').write_text('Not an image')
        (cls.catalog / 'link.bmp').symlink_to(cls.paths[0])
        (cls.catalog / 'folder.bmp').mkdir()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def start(self, *options, catalog=None):
        server = Server(catalog or self.catalog, *options)
        self.addCleanup(server.close)
        return server

    def client(self, server=None, files=(), stdin=None, *options):
        command = [str(CLIENT), '--port', str(server.port if server else 1),
                   '--timeout-ms', '500', *options, *map(str, files)]
        return subprocess.run(command, input=stdin, text=True, capture_output=True,
                              cwd=ROOT, timeout=5)

    def assert_match(self, response, path=None, distance=0):
        self.assertIn(f"'{path or self.paths[0]}'", response)
        self.assertTrue(response.endswith(f'with a distance of {distance}.'), response)

    def assert_closed(self, sock):
        try:
            self.assertEqual(sock.recv(1), b'')
        except ConnectionResetError:
            pass

    def test_exact_match_and_batch_client(self):
        server = self.start()
        self.assertIn('Indexed 3 images', server.startup)
        result = self.client(server, self.paths)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 3)
        for response, path in zip(result.stdout.splitlines(), self.paths):
            self.assert_match(response, path)

    def test_fragmented_and_pipelined_frames(self):
        server = self.start()
        with server.connect() as sock:
            request = frame(self.images[0])
            for byte in request[:4]:
                sock.sendall(bytes([byte]))
            for offset in range(4, len(request), 13):
                sock.sendall(request[offset:offset + 13])
            self.assert_match(reply(sock))
            sock.sendall(b''.join(frame(data) for data in self.images * 3))
            for path in self.paths * 3:
                self.assert_match(reply(sock), path)

    def test_large_image_and_32bit_top_down(self):
        server = self.start()
        with server.connect() as sock:
            for data in (bmp(7, 16, 16, 32, True), bmp(8, 256, 256)):
                sock.sendall(frame(data))
                response = reply(sock)
                self.assertIn('Most similar image found:', response)
            sock.sendall(frame(bmp(7, 16, 16, 32, True)))
            self.assert_match(reply(sock))

    def test_parallel_clients(self):
        server = self.start()

        def query(index):
            with server.connect() as sock:
                sock.sendall(frame(self.images[index % 3]))
                return reply(sock), self.paths[index % 3]

        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            for response, path in pool.map(query, range(36)):
                self.assert_match(response, path)

    def test_disconnects_and_reused_descriptors(self):
        server = self.start()
        for data in (b'\0', struct.pack('!I', 1024) + b'abc', frame(self.images[2])) * 12:
            with server.connect() as sock:
                sock.sendall(data)
        for _ in range(6):
            with server.connect() as sock:
                sock.sendall(frame(self.images[0]))
                self.assert_match(reply(sock))
        self.assertIsNone(server.process.poll())

    def test_half_closed_write_still_gets_response(self):
        server = self.start()
        with server.connect() as sock:
            sock.sendall(frame(self.images[0]))
            sock.shutdown(socket.SHUT_WR)
            self.assert_match(reply(sock))
            self.assert_closed(sock)

    def test_invalid_lengths_are_rejected(self):
        server = self.start()
        for length in (0, MAX_IMAGE + 1, 0x80000000, 0xffffffff):
            with self.subTest(length=length), server.connect() as sock:
                sock.sendall(struct.pack('!I', length))
                self.assert_closed(sock)
        with server.connect() as sock:
            sock.sendall(frame(self.images[0]))
            self.assert_match(reply(sock))

    def test_malformed_images_do_not_break_connection(self):
        server = self.start()
        invalid = bytearray(self.images[0])
        struct.pack_into('<I', invalid, 18, 0xffffffff)
        with server.connect() as sock:
            for data in (b'x', b'BM', self.images[0][:54], bytes(invalid)):
                sock.sendall(frame(data))
                self.assertIn('no comparison', reply(sock))
            sock.sendall(frame(self.images[0]))
            self.assert_match(reply(sock))

    def test_absolute_partial_frame_and_idle_timeouts(self):
        server = self.start('--timeout-ms', '250')
        with server.connect() as idle, server.connect() as partial:
            partial.sendall(struct.pack('!I', 1000))
            start = time.monotonic()
            for _ in range(3):
                time.sleep(.06)
                partial.sendall(b'x')
            self.assert_closed(partial)
            self.assertLess(time.monotonic() - start, .8)
            self.assert_closed(idle)
        with server.connect() as sock:
            sock.sendall(frame(self.images[0]))
            self.assert_match(reply(sock))

    def test_client_stdin_eof_and_missing_files(self):
        empty = self.client(stdin='')
        self.assertEqual(empty.returncode, 0, empty.stderr)
        server = self.start()
        result = self.client(server, stdin='\n' + str(self.paths[0]) + '\r\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_match(result.stdout.strip())
        mixed = self.client(server, (self.directory / 'missing.bmp', self.paths[1]))
        self.assertEqual(mixed.returncode, 1)
        self.assert_match(mixed.stdout.strip(), self.paths[1])

    def test_client_rejects_nonregular_empty_and_oversized_files(self):
        empty = self.directory / 'empty.bmp'
        empty.touch()
        huge = self.directory / 'huge.bmp'
        with huge.open('wb') as file:
            file.truncate(MAX_IMAGE + 1)
        fifo = self.directory / 'pipe.bmp'
        os.mkfifo(fifo)
        for path in (empty, huge, fifo, self.catalog):
            with self.subTest(path=path.name):
                result = self.client(files=(path,))
                self.assertEqual(result.returncode, 1)
                self.assertIn('Error:', result.stderr)

    def test_client_handles_refusal(self):
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
            result = subprocess.run([str(CLIENT), '--port', str(port), str(self.paths[0])],
                                    text=True, capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 1)
        self.assertIn('connect', result.stderr)

    def test_client_handles_eof_timeout_and_oversized_reply(self):
        for mode in ('close', 'stall', 'oversized'):
            with self.subTest(mode=mode), socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                listener.listen()
                listener.settimeout(3)
                done = threading.Event()
                failures = []

                def peer():
                    try:
                        with listener.accept()[0] as sock:
                            sock.settimeout(3)
                            pending = len(frame(self.images[0]))
                            while pending:
                                chunk = sock.recv(pending)
                                if not chunk:
                                    break
                                pending -= len(chunk)
                            if mode == 'stall':
                                done.wait(2)
                            elif mode == 'oversized':
                                sock.sendall(b'x' * 4097)
                    except BaseException as error:
                        failures.append(error)

                thread = threading.Thread(target=peer)
                thread.start()
                try:
                    result = subprocess.run([str(CLIENT), '--port', str(listener.getsockname()[1]),
                                             '--timeout-ms', '150', str(self.paths[0])],
                                            text=True, capture_output=True, timeout=3)
                finally:
                    done.set()
                    thread.join(timeout=4)
                self.assertFalse(thread.is_alive())
                self.assertFalse(failures, failures)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn({'close': 'disconnected', 'stall': 'timed out',
                               'oversized': 'exceeds'}[mode], result.stderr)

    def test_connection_cap_and_no_idle_spin(self):
        server = self.start('--max-clients', '2', '--workers', '2', '--timeout-ms', '1000')
        with server.connect() as first, server.connect() as second:
            first.sendall(frame(self.images[0]))
            self.assert_match(reply(first))
            second.sendall(frame(self.images[1]))
            self.assert_match(reply(second), self.paths[1])
            with server.connect() as excess:
                self.assert_closed(excess)
            proc = Path('/proc') / str(server.process.pid)
            if proc.exists():
                self.assertEqual(len(list((proc / 'task').iterdir())), 3)
                before = sum(map(int, (proc / 'stat').read_text().split()[13:15]))
                time.sleep(.25)
                after = sum(map(int, (proc / 'stat').read_text().split()[13:15]))
                self.assertLess(after - before, max(5, os.sysconf('SC_CLK_TCK') // 10))
        time.sleep(.07)
        with server.connect() as sock:
            sock.sendall(frame(self.images[0]))
            self.assert_match(reply(sock))

    def test_connections_release_file_descriptors(self):
        server = self.start()
        proc = Path('/proc') / str(server.process.pid) / 'fd'
        baseline = len(list(proc.iterdir())) if proc.exists() else 0
        for _ in range(30):
            with server.connect() as sock:
                sock.sendall(frame(self.images[0]))
                self.assert_match(reply(sock))
        deadline = time.monotonic() + 1
        while proc.exists() and len(list(proc.iterdir())) > baseline and time.monotonic() < deadline:
            time.sleep(.02)
        if baseline:
            self.assertLessEqual(len(list(proc.iterdir())), baseline)

    def test_invalid_cli_and_empty_catalog(self):
        for program, options in ((CLIENT, ['--port', '-1']), (CLIENT, ['--host', 'invalid']),
                                 (CLIENT, ['--unknown']), (SERVER, ['--workers', '0']),
                                 (SERVER, ['--max-clients', '65']), (SERVER, ['--port']),
                                 (SERVER, ['--bind', 'invalid']), (SERVER, ['--unknown'])):
            with self.subTest(program=program.name, options=options):
                result = subprocess.run([str(program), *options], capture_output=True, timeout=3)
                self.assertEqual(result.returncode, 2)
        empty = self.directory / 'empty-catalog'
        empty.mkdir()
        result = subprocess.run([str(SERVER), '--images', str(empty)],
                                text=True, capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 1)
        self.assertIn('no supported BMP', result.stderr)
        for program in (CLIENT, SERVER):
            result = subprocess.run([str(program), '--help'], capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 0)

    def test_existing_catalog_and_query_fixtures(self):
        server = self.start(catalog=ROOT / 'img')
        self.assertIn('Indexed 101 images', server.startup)
        files = sorted((ROOT / 'tests/images').glob('*.bmp'))
        self.assertEqual(len(files), 16)
        result = self.client(server, files)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), len(files))
        self.assertTrue(all('Most similar image found:' in line for line in result.stdout.splitlines()))

    def test_sigint_shutdown_with_incomplete_clients(self):
        server = self.start()
        with contextlib.ExitStack() as stack:
            for _ in range(8):
                sock = stack.enter_context(server.connect())
                sock.sendall(struct.pack('!I', MAX_IMAGE) + b'BM')
            server.close(sig=signal.SIGINT)


if __name__ == '__main__':
    unittest.main()
