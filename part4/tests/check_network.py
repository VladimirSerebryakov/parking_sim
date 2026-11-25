#!/usr/bin/env python3
"""Реальные TCP-проверки. Нужны разрешённые локальные сокеты; артефакты в /tmp."""
import argparse
import contextlib
import os
import pty
from pathlib import Path
import re
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / 'build/bin/part4'
LOGS = None
MAGIC = 0x504b3401
HELLO, WELCOME, ARRIVE, STATE, LEAVE, PLAN, PLAN_REPLY, SNAPSHOT, STOP, ACK, DETACH, ERROR, PING, PONG = range(1, 15)


def frame(kind, body='', seq=1, magic=MAGIC):
    data = body.encode() if isinstance(body, str) else body
    return struct.pack('!IIII', magic, kind, len(data), seq) + data


def receive(sock):
    def exact(n):
        data = b''
        while len(data) < n:
            piece = sock.recv(n - len(data))
            if not piece:
                raise EOFError('соединение закрыто')
            data += piece
        return data
    magic, kind, length, seq = struct.unpack('!IIII', exact(16))
    assert magic == MAGIC and length <= 1048576
    return kind, exact(length).decode(), seq


def until(check, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if check():
            return
        time.sleep(.01)
    raise AssertionError('истёк срок ожидания события')


class Run:
    def __init__(self, folder, *args):
        self.folder = Path(folder)
        self.log = self.folder / 'parking.txt'
        self.processes = []
        self.files = []
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = str(probe.getsockname()[1])
        self.server = self.spawn('parking_server', '0.0.0.0', self.port,
            '-d', '10', '-t', '0', '-R', '7', '-r', '1', '-l', '0', '-s', '0',
            '-c', '3', '-C', '0', '-S', '0', '-a', '0', '-b', '0', '-p', '100', '-P', '100',
            '-q', '10', '-w', '500', '-L', str(self.log), *args)
        try:
            until(lambda: self.log.exists() and 'Параметры:' in self.text() or self.server.poll() is not None)
        except Exception:
            self.close()
            raise

    def spawn(self, name, *args, stdout=None):
        index = len(self.processes)
        out = (self.folder / f'{index}.out').open('wb')
        err = (self.folder / f'{index}.err').open('wb')
        self.files += [out, err]
        p = subprocess.Popen([str(BIN / name), *map(str, args)], stdout=out if stdout is None else stdout, stderr=err,
                             start_new_session=True)
        self.processes.append(p)
        return p

    def car(self, number):
        return self.spawn('parking_car', '127.0.0.1', self.port, number)

    def monitor(self):
        return self.spawn('parking_monitor', '127.0.0.1', self.port, '--plain')

    def generator(self):
        return self.spawn('parking_arrivals', '127.0.0.1', self.port)

    def raw(self, role=1, number=1):
        sock = socket.create_connection(('127.0.0.1', self.port), timeout=3)
        sock.sendall(frame(HELLO, f'{role} {number}'))
        message = receive(sock)
        assert message[0] == WELCOME, message
        return sock

    def text(self):
        return self.log.read_text(errors='replace') if self.log.exists() else ''

    def output(self, process):
        return (self.folder / f'{self.processes.index(process)}.out').read_text(errors='replace')

    def wait(self, p, expected=0):
        code = p.wait(timeout=12)
        index = self.processes.index(p)
        error = (self.folder / f'{index}.err').read_text()
        assert code == expected, (p.args, code, expected, error)
        assert not re.search(r'AddressSanitizer|runtime error:|LeakSanitizer', error), error
        if expected == 0:
            assert not error, error

    def stop(self, sig=signal.SIGTERM):
        self.server.send_signal(sig)
        self.wait(self.server)

    def check(self):
        result = subprocess.run(['awk', '-f', str(ROOT / 'part3/tests/check_log.awk'), str(self.log)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr

    def close(self):
        for p in self.processes:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(p.pid, signal.SIGKILL)
        for p in self.processes:
            p.wait(timeout=5)
        for f in self.files:
            f.close()


class NetworkTests(unittest.TestCase):
    def setUp(self):
        self.temp = None if LOGS else tempfile.TemporaryDirectory(prefix='parking-tcp-test-', dir='/tmp')
        self.folder = Path(tempfile.mkdtemp(prefix=self._testMethodName + '-', dir=LOGS)) if LOGS else Path(self.temp.name)
        self.runs = []

    def run_model(self, *args):
        folder = self.folder / str(len(self.runs))
        folder.mkdir()
        r = Run(folder, *args)
        self.runs.append(r)
        return r

    def tearDown(self):
        for r in self.runs:
            r.close()
        if self.temp:
            self.temp.cleanup()

    def test_manual(self):
        r = self.run_model('-c', '2', '-p', '5', '-P', '5')
        for id in (1, 2):
            r.wait(r.car(id))
        r.wait(r.server); r.check()
        self.assertEqual(r.text().count('Прибытие:'), 2)

    def test_generated(self):
        r = self.run_model('-c', '20', '-C', '8', '-S', '7', '-l', '1', '-s', '1', '-p', '1', '-P', '3')
        r.wait(r.generator()); r.wait(r.server); r.check()
        self.assertEqual(r.text().count('Прибытие:'), 35)

    def test_monitors_reconnect(self):
        r = self.run_model()
        monitors = [r.monitor(), r.monitor(), r.monitor()]
        until(lambda: all('СОСТОЯНИЕ ПАРКОВКИ' in r.output(p) for p in monitors))
        car = r.car(1)
        until(lambda: 'Начало стоянки:' in r.text())
        monitors[0].send_signal(signal.SIGINT); r.wait(monitors[0])
        self.assertIsNone(r.server.poll())
        replacement = r.monitor()
        until(lambda: 'СОСТОЯНИЕ ПАРКОВКИ' in r.output(replacement))
        r.stop()
        for p in [car, *monitors[1:], replacement]:
            r.wait(p)
        r.check()
        for p in [*monitors[1:], replacement]:
            text = (r.folder / f'{r.processes.index(p)}.out').read_text()
            self.assertIn('СОСТОЯНИЕ ПАРКОВКИ', text)
            self.assertIn('ОЧЕРЕДЬ', text)
            self.assertIn('ИТОГОВАЯ СТАТИСТИКА', text)

    def test_observer_does_not_start_clock(self):
        r = self.run_model('-t', '2')
        monitor = r.monitor()
        time.sleep(.1)
        self.assertIsNone(r.server.poll())
        car = r.car(1)
        r.wait(r.server); r.wait(car); r.wait(monitor); r.check()
        self.assertIn('лимит времени', r.text())

    def test_monitor_terminal(self):
        r = self.run_model()
        master, slave = pty.openpty()
        try:
            monitor = r.spawn('parking_monitor', '127.0.0.1', r.port, stdout=slave)
            os.close(slave); slave = -1
            captured = bytearray()

            def read_once():
                if select.select([master], [], [], .05)[0]:
                    try:
                        captured.extend(os.read(master, 16384))
                    except OSError:
                        pass  # После закрытия slave Linux возвращает EIO.

            end = time.monotonic() + 5
            while 'СОСТОЯНИЕ ПАРКОВКИ'.encode() not in captured and time.monotonic() < end:
                read_once()
            self.assertIn(b'\x1b[?1049h', captured)
            self.assertIn('СОСТОЯНИЕ ПАРКОВКИ'.encode(), captured)
            r.server.send_signal(signal.SIGTERM)
            end = time.monotonic() + 5
            while time.monotonic() < end:
                read_once()
                if monitor.poll() is not None:
                    read_once()
                    break
            r.wait(monitor); r.wait(r.server); r.check()
            self.assertIn(b'\x1b[?1049l', captured)
            final = captured.split(b'\x1b[?1049l', 1)[1]
            self.assertIn('ИТОГОВАЯ СТАТИСТИКА'.encode(), final)
            (r.folder / 'terminal.out').write_bytes(captured)
        finally:
            os.close(master)
            if slave >= 0:
                os.close(slave)

    def test_restart_parked_car(self):
        r = self.run_model('-c', '2', '-p', '200', '-P', '200')
        car = r.car(1)
        until(lambda: 'Начало стоянки:' in r.text())
        car.send_signal(signal.SIGTERM); r.wait(car)
        replacement = r.car(1)
        r.wait(replacement)
        self.assertIsNone(r.server.poll())
        r.wait(r.car(2)); r.wait(r.server); r.check()
        self.assertEqual(r.text().count('Прибытие: авто #1 '), 1)
        self.assertEqual(r.text().count('Начало стоянки: авто #1 '), 1)

    def test_restart_queued_car(self):
        r = self.run_model('-c', '3', '-p', '100', '-P', '100')
        first = r.car(1)
        until(lambda: 'Начало стоянки:' in r.text())
        second = r.car(2)
        until(lambda: 'авто #2 поставлено в очередь' in r.text())
        second.send_signal(signal.SIGINT); r.wait(second)
        replacement = r.car(2)
        r.wait(first); r.wait(replacement)
        r.wait(r.car(3)); r.wait(r.server); r.check()
        self.assertEqual(r.text().count('Прибытие: авто #2 '), 1)

    def test_detached_result_persists(self):
        r = self.run_model('-c', '2', '-p', '5', '-P', '5')
        s = r.raw()
        self.assertEqual(receive(s)[0], STATE)
        s.sendall(frame(ARRIVE, '1 0', 2)); s.close()
        until(lambda: 'Окончание стоянки:' in r.text())
        r.wait(r.car(1)); r.wait(r.car(2)); r.wait(r.server); r.check()
        self.assertEqual(r.text().count('Прибытие: авто #1 '), 1)

    def test_duplicate_active_car(self):
        r = self.run_model()
        car = r.car(1)
        until(lambda: 'Начало стоянки:' in r.text())
        r.wait(r.car(1), expected=1)
        self.assertIsNone(r.server.poll())
        r.stop(); r.wait(car); r.check()

    def test_duplicate_generator(self):
        r = self.run_model('-a', '100', '-b', '100')
        generator = r.generator()
        time.sleep(.1)
        r.wait(r.generator(), expected=1)
        r.stop(); r.wait(generator); r.check()

    def test_restart_generator(self):
        r = self.run_model('-c', '6', '-a', '5', '-b', '5', '-p', '40', '-P', '40')
        generator = r.generator()
        until(lambda: r.text().count('Прибытие:') >= 2)
        generator.send_signal(signal.SIGTERM); r.wait(generator)
        self.assertIsNone(r.server.poll())
        r.wait(r.generator()); r.wait(r.server); r.check()
        self.assertEqual(r.text().count('Прибытие:'), 6)

    def test_killed_car(self):
        r = self.run_model('-c', '2', '-p', '10', '-P', '10')
        car = r.car(1)
        until(lambda: 'Начало стоянки:' in r.text())
        car.kill(); r.wait(car, expected=-signal.SIGKILL)
        until(lambda: 'Окончание стоянки:' in r.text())
        r.wait(r.car(1)); r.wait(r.car(2)); r.wait(r.server); r.check()

    def test_server_shutdown(self):
        for sig in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=sig):
                r = self.run_model('-c', '20', '-p', '1000', '-P', '1000')
                monitor = r.monitor(); generator = r.generator()
                until(lambda: 'поставлено в очередь' in r.text())
                r.stop(sig); r.wait(generator); r.wait(monitor); r.check()

    def test_server_crash(self):
        r = self.run_model()
        car = r.car(1); monitor = r.monitor()
        until(lambda: 'Начало стоянки:' in r.text())
        r.server.kill(); r.wait(r.server, expected=-signal.SIGKILL)
        r.wait(car, expected=1); r.wait(monitor, expected=1)

    def test_stop_during_handshake(self):
        r = self.run_model()
        with socket.create_connection(('127.0.0.1', r.port), timeout=3) as s:
            time.sleep(.03)  # Сервер принял сокет, но HELLO ещё не отправлен.
            r.server.send_signal(signal.SIGTERM)
            self.assertEqual(receive(s)[0], STOP)
            s.sendall(frame(ACK, '', 1))
        r.wait(r.server); r.check()

    def test_short_limit_with_connecting_children(self):
        for attempt in range(10):
            with self.subTest(attempt=attempt):
                r = self.run_model('-d', '1', '-t', '20', '-c', '80', '-p', '100', '-P', '100')
                r.wait(r.generator()); r.wait(r.server); r.check()
                self.assertIn('лимит времени', r.text())

    def test_transport_fragmentation(self):
        r = self.run_model()
        with socket.create_connection(('127.0.0.1', r.port), timeout=3) as s:
            for byte in frame(HELLO, '1 1'):
                s.sendall(bytes([byte]))
            self.assertEqual(receive(s)[0], WELCOME)
            self.assertEqual(receive(s)[0], STATE)
            s.sendall(frame(ARRIVE, '1 0', 2) + frame(ARRIVE, '1 0', 3) + frame(PING, '', 4))
            kinds = [receive(s)[0] for _ in range(3)]
            self.assertEqual(kinds, [STATE, STATE, PONG])
        r.stop(); r.check()
        self.assertEqual(r.text().count('Прибытие:'), 1)

    def test_malformed_connections(self):
        r = self.run_model()
        bad = [frame(HELLO, '1 1', magic=0), struct.pack('!IIII', MAGIC, HELLO, 9999999, 1),
               frame(HELLO, '1 0'), frame(HELLO, '9 1'), frame(HELLO, '1 1x'),
               frame(ARRIVE, '1 0'), frame(HELLO, b'1\x001'), frame(HELLO, '1 1', seq=0)]
        for data in bad:
            with self.subTest(data=data), socket.create_connection(('127.0.0.1', r.port), timeout=3) as s:
                s.sendall(data)
                try:
                    self.assertEqual(receive(s)[0], ERROR)
                except (EOFError, ConnectionResetError):
                    pass
        self.assertIsNone(r.server.poll())
        r.stop(); r.check()

    def test_wrong_type_and_role(self):
        r = self.run_model()
        with r.raw() as s:
            receive(s)
            s.sendall(frame(ARRIVE, '1 2', 2))
            self.assertEqual(receive(s)[0], ERROR)
        with r.raw(3, 0) as s:
            receive(s)
            s.sendall(frame(ARRIVE, '1 0', 2))
            self.assertEqual(receive(s)[0], ERROR)
        r.stop(); r.check()
        self.assertNotIn('Прибытие:', r.text())

    def test_early_leave(self):
        r = self.run_model()
        with r.raw() as s:
            receive(s)
            s.sendall(frame(ARRIVE, '1 0', 2))
            self.assertEqual(receive(s)[0], STATE)
            s.sendall(frame(LEAVE, '', 3))
            kind, body, _ = receive(s)
            self.assertEqual(kind, STATE)
            self.assertEqual(int(body.split()[2]), 2)
        r.stop(); r.check()

    def test_idle_and_partial_client(self):
        r = self.run_model()
        with socket.create_connection(('127.0.0.1', r.port), timeout=8) as s:
            s.sendall(frame(HELLO, '1 1')[:10])
            time.sleep(5.2)
            self.assertEqual(s.recv(1), b'')
        self.assertIsNone(r.server.poll())
        r.stop(); r.check()

    def test_slow_monitor(self):
        r = self.run_model('-r', '2048', '-c', '2', '-p', '10', '-P', '10')
        with r.raw(3, 0) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            r.wait(r.car(1)); r.wait(r.car(2)); r.wait(r.server); r.check()

    def test_parallel_servers(self):
        first = self.run_model(); second = self.run_model()
        car = first.car(1)
        until(lambda: 'Прибытие:' in first.text())
        first.stop(); first.wait(car); first.check()
        self.assertIsNone(second.server.poll())
        second.stop(); second.check()

    def test_busy_port_keeps_log(self):
        r = self.run_model()
        log = r.folder / 'foreign.txt'; log.write_text('сохранить')
        duplicate = r.spawn('parking_server', '127.0.0.1', r.port, '-L', log)
        r.wait(duplicate, expected=1)
        self.assertEqual(log.read_text(), 'сохранить')
        r.stop(); r.check()

    def test_repeated_seed(self):
        plans = []
        for _ in range(2):
            r = self.run_model('-c', '10', '-p', '1', '-P', '5', '-a', '0', '-b', '2')
            r.wait(r.generator()); r.wait(r.server); r.check()
            plans.append(sorted(re.findall(r'Генерация: .*', r.text())))
        self.assertEqual(plans[0], plans[1])

    def test_parameter_cases(self):
        # Переиспользуем все 35 наборов третьей части, включая нагрузку 500 машин.
        for line in (ROOT / 'part3/tests/cases.txt').read_text().splitlines():
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            expected, name, arguments = [piece.strip() for piece in line.split('|')]
            with self.subTest(case=name):
                r = self.run_model('-d', '1', '-r', '2', '-l', '1', '-s', '1',
                    '-c', '15', '-C', '12', '-S', '10', '-q', '3', '-w', '8',
                    '-a', '0', '-b', '2', '-p', '7', '-P', '10', *arguments.split())
                if expected == 'error':
                    r.wait(r.server, expected=1)
                    continue
                if name != 'нет автомобилей' and r.server.poll() is None:
                    r.wait(r.generator())
                r.wait(r.server); r.check()
                self.assertIn('лимит времени' if expected == 'limit' else 'СИМУЛЯЦИЯ ЗАВЕРШЕНА', r.text())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('bin', nargs='?', type=Path, default=BIN)
    parser.add_argument('--logs', type=Path, default=os.environ.get('PARKING_TEST_LOGS'))
    args, extra = parser.parse_known_args()
    BIN = args.bin.resolve()
    try:
        with socket.socket():
            pass
    except PermissionError:
        raise SystemExit('Окружение запрещает TCP-сокеты. Запустите тесты в обычной Linux/WSL; сетевые проверки НЕ выполнены.')
    LOGS = args.logs.resolve() if args.logs else Path(tempfile.mkdtemp(prefix='parking-tcp-network-', dir='/tmp'))
    LOGS.mkdir(parents=True, exist_ok=True)
    print(f'Журналы сетевых проверок: {LOGS}', flush=True)
    unittest.main(argv=[__file__, *extra], verbosity=2)
