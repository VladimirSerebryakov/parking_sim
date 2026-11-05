#!/usr/bin/env python3
"""Проверка независимого запуска C-программ и жизненного цикла сеансов."""

import ctypes
import itertools
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

DIR = Path(__file__).resolve().parent
BIN = Path(sys.argv[1]).resolve().parent
MQ = Path(sys.argv[1]).name == "parking_independent_mq"
LAUNCHER = "parking_independent_mq" if MQ else "parking_independent_shm"
COUNTER = itertools.count()
LIBC = ctypes.CDLL(None, use_errno=True)


class MqAttr(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_long), ("maxmsg", ctypes.c_long),
                ("msgsize", ctypes.c_long), ("curmsgs", ctypes.c_long), ("padding", ctypes.c_long * 4)]


def mq_exists(name):
    q = LIBC.mq_open(name.encode(), os.O_RDONLY | os.O_NONBLOCK)
    if q < 0:
        return False
    LIBC.mq_close(q)
    return True


def actual_id(text):
    pattern = r"POSIX очереди: /parking_mq_(\S+)_requests" if MQ else r"POSIX память: /parking_shm_(\S+)"
    return re.search(pattern, text)[1]


def fault_env(library, name, **extra):
    return dict(os.environ, LD_PRELOAD=str(library), PARKING_FAIL=name, PARKING_SEM_ZERO="1",
                **({"PARKING_MQ_MODE": "1"} if MQ else {}), **extra)


def until(check, message, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if check():
            return
        time.sleep(0.003)
    raise AssertionError(message)


class Scenario:
    def __init__(self, folder):
        self.id = f"test_{os.getpid()}_{next(COUNTER)}"
        self.folder = folder
        self.log = folder / f"{self.id}.txt"
        self.actors = []
        self.owner = None

    def spawn(self, program, args=(), expected=0, env=None, cwd=None):
        path = self.folder / f"{self.id}-{len(self.actors)}.err"
        with path.open("wb") as error:
            process = subprocess.Popen([str(BIN / program), *map(str, args)], stdout=subprocess.DEVNULL,
                                       stderr=error, start_new_session=True, env=env, cwd=cwd)
        self.actors.append((process, path, expected))
        return process

    def start(self, args=(), expected=0, env=None):
        self.owner = self.spawn("parking_dispatcher", [self.id, "-d", "10", "-t", "0", "-R", "7",
            "-r", "1", "-l", "0", "-s", "0", "-c", "1", "-C", "0", "-S", "0",
            "-a", "0", "-b", "0", "-p", "30", "-P", "30", "-w", "200", "-q", "10",
            "-L", self.log, *args], expected, env)
        until(lambda: "Параметры:" in self.text(), "сеанс не готов")
        return self.owner

    def text(self):
        return self.log.read_text() if self.log.exists() else ""

    def resources(self):
        if MQ:
            base = f"parking_mq_{self.id}"
            return [Path('/dev/mqueue', base + suffix) for suffix in ("_requests", "_replies")
                    if mq_exists('/' + base + suffix)] + list(Path('/dev/shm').glob(f"sem.{base}_sem_*"))
        return list(Path("/dev/shm").glob(f"parking_shm_{self.id}")) + list(
            Path("/dev/shm").glob(f"sem.parking_shm_{self.id}_*"))

    def finish(self, check_log=True):
        for process, err, expected in self.actors:
            rc = process.wait(timeout=8)
            text = err.read_text()
            assert rc == expected, f"{process.args}: код {rc}, ожидался {expected}: {text}"
            assert not re.search(r"AddressSanitizer|LeakSanitizer|runtime error:", text), text
            if expected == 0:
                assert not text, text
        assert not self.resources(), f"остались ресурсы: {self.resources()}"
        if check_log:
            result = subprocess.run(["awk", "-f", str(DIR / "check_log.awk"), str(self.log)],
                                    text=True, capture_output=True)
            assert result.returncode == 0, result.stdout + result.stderr

    def cleanup(self):
        for process, _, _ in self.actors:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        for process, _, _ in self.actors:
            process.wait(timeout=3)
        # Только объекты этого тестового сеанса; проверка отсутствия ресурсов сделана до cleanup.
        for resource in self.resources():
            if resource.name.startswith("sem."):
                LIBC.sem_unlink(("/" + resource.name[4:]).encode())
            elif MQ:
                LIBC.mq_unlink(("/" + resource.name).encode())
            else:
                LIBC.shm_unlink(("/" + resource.name).encode())


def manual(folder):
    s = Scenario(folder)
    try:
        owner = s.start(["-p", "50", "-P", "50"])
        car = s.spawn("parking_car", [s.id, 1], cwd="/tmp")
        until(lambda: "Начало стоянки" in s.text(), "автомобиль не подключился")
        children = Path(f"/proc/{owner.pid}/task/{owner.pid}/children").read_text().strip()
        assert not children, "диспетчер запускает автомобили вместо независимого подключения"
        assert car.pid != owner.pid
        for process in (owner, car):
            maps = Path(f"/proc/{process.pid}/maps").read_text()
            if MQ:
                assert "parking_shm_" not in maps, "MQ-участник отображает модель в общую память"
                links = []
                for descriptor in Path(f"/proc/{process.pid}/fd").iterdir():
                    try:
                        links.append(os.readlink(descriptor))
                    except FileNotFoundError:
                        pass
                assert any(f"parking_mq_{s.id}_requests" in name for name in links)
            else:
                assert f"parking_shm_{s.id}" in maps
            assert len(list(Path(f"/proc/{process.pid}/task").iterdir())) == 1
        assert len(s.resources()) == (6 if MQ else 4), "не сохранены имена ресурсов"
        s.finish()
    finally:
        s.cleanup()


def early_connect(folder, program):
    s = Scenario(folder)
    try:
        args = [s.id, 1] if program == "parking_car" else [s.id]
        s.spawn(program, args)
        time.sleep(0.03)
        s.start(["-d", "1", "-p", "1", "-P", "1"])
        s.finish()
    finally:
        s.cleanup()


def empty_session(folder):
    s = Scenario(folder)
    try:
        s.start(["-c", "0", "-C", "0", "-S", "0"])
        s.finish()
        assert "Прибытие:" not in s.text()
    finally:
        s.cleanup()


def duplicate_car(folder):
    s = Scenario(folder)
    try:
        owner = s.start(["-c", "2", "-p", "200", "-P", "200"])
        s.spawn("parking_car", [s.id, 1])
        until(lambda: "Начало стоянки" in s.text(), "нет первого авто")
        duplicate = s.spawn("parking_car", [s.id, 1], expected=1)
        assert duplicate.wait(timeout=3) == 1
        s.spawn("parking_car", [s.id, 2])
        until(lambda: "поставлено в очередь" in s.text(), "нет второго авто")
        owner.send_signal(signal.SIGTERM)
        s.finish()
        assert s.text().count("Прибытие:") == 2
    finally:
        s.cleanup()


def invalid_car(folder):
    s = Scenario(folder)
    try:
        s.start()
        for number in (0, "abc", 2):
            p = s.spawn("parking_car", [s.id, number], expected=1)
            assert p.wait(timeout=3) == 1
        s.spawn("parking_car", [s.id, 1])
        s.finish()
    finally:
        s.cleanup()


def collision(folder):
    s = Scenario(folder)
    try:
        owner = s.start()
        before = s.text()
        duplicate = s.spawn("parking_dispatcher", [s.id, "-L", s.log], expected=1)
        assert duplicate.wait(timeout=3) == 1
        assert s.text() == before and s.resources(), "чужой сеанс изменён"
        owner.send_signal(signal.SIGINT)
        s.finish()
    finally:
        s.cleanup()


def generator_collision(folder):
    s = Scenario(folder)
    try:
        owner = s.start(["-c", "4", "-p", "200", "-P", "200"])
        s.spawn("parking_arrivals", [s.id])
        until(lambda: "поставлено в очередь" in s.text(), "генератор не запустился")
        duplicate = s.spawn("parking_arrivals", [s.id], expected=1)
        assert duplicate.wait(timeout=3) == 1
        owner.send_signal(signal.SIGTERM)
        s.finish()
    finally:
        s.cleanup()


def participant_signal(folder, program):
    s = Scenario(folder)
    try:
        s.start(["-p", "200", "-P", "200"])
        args = [s.id, 1] if program == "parking_car" else [s.id]
        participant = s.spawn(program, args)
        until(lambda: "Начало стоянки" in s.text(), "автомобиль не начал стоянку")
        participant.send_signal(signal.SIGINT)
        s.finish()
        assert "СИМУЛЯЦИЯ ПРЕРВАНА (Ctrl+C)" in s.text()
    finally:
        s.cleanup()


def parallel(folder):
    sessions = [Scenario(folder), Scenario(folder)]
    try:
        for s in sessions:
            s.start(["-p", "200", "-P", "200"])
            s.spawn("parking_car", [s.id, 1])
            until(lambda: "Начало стоянки" in s.text(), "нет стоянки")
        sessions[0].owner.send_signal(signal.SIGTERM)
        sessions[0].finish()
        assert sessions[1].owner.poll() is None and sessions[1].resources()
        sessions[1].owner.send_signal(signal.SIGINT)
        sessions[1].finish()
    finally:
        for s in sessions:
            s.cleanup()


def whole_group(folder):
    s = Scenario(folder)
    try:
        launcher = s.spawn(LAUNCHER, ["-t", "0", "-d", "10", "-R", "7",
            "-a", "0", "-b", "0", "-p", "200", "-P", "200", "-L", s.log])
        until(lambda: "Начало стоянки" in s.text(), "нет стоянки")
        name = actual_id(s.text())
        s.id = name
        os.killpg(launcher.pid, signal.SIGINT)
        s.finish()
        assert "СИМУЛЯЦИЯ ПРЕРВАНА (Ctrl+C)" in s.text()
    finally:
        s.cleanup()


def repeat_seed(folder):
    inputs = []
    for _ in range(2):
        s = Scenario(folder)
        try:
            s.start(["-d", "1", "-c", "8", "-C", "8", "-S", "8", "-p", "1", "-P", "3", "-w", "2"])
            s.spawn("parking_arrivals", [s.id])
            s.finish()
            inputs.append(sorted(re.findall(r"Генерация: .*", s.text())))
        finally:
            s.cleanup()
    assert len(inputs[0]) == 24 and inputs[0] == inputs[1]


def client_crash(folder):
    s = Scenario(folder)
    try:
        s.start(["-p", "200", "-P", "200"], expected=1)
        car = s.spawn("parking_car", [s.id, 1], expected=-signal.SIGKILL)
        until(lambda: "Начало стоянки" in s.text(), "нет стоянки")
        car.kill()
        car.wait(timeout=3)
        s.finish(check_log=False)
        assert "ИТОГОВАЯ СТАТИСТИКА" not in s.text()
    finally:
        s.cleanup()


def missing_owner(folder):
    s = Scenario(folder)
    try:
        s.spawn("parking_car", [s.id, 1], expected=1)
        s.spawn("parking_arrivals", [s.id], expected=1)
        s.finish(check_log=False)
    finally:
        s.cleanup()


def malformed(folder):
    if MQ:
        return malformed_mq(folder)
    s = Scenario(folder)
    sem = None
    try:
        name = f"/parking_shm_{s.id}"
        fd = LIBC.shm_open(name.encode(), os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        assert fd >= 0
        os.ftruncate(fd, 16)
        os.close(fd)
        LIBC.sem_open.restype = ctypes.c_void_p
        sem = LIBC.sem_open((name + "_sem_0").encode(), os.O_CREAT | os.O_EXCL, 0o600, 1)
        assert sem
        car = s.spawn("parking_car", [s.id, 1], expected=1)
        assert car.wait(timeout=3) == 1
        assert s.resources(), "подключение удалило чужую память"
    finally:
        if sem:
            LIBC.sem_close(ctypes.c_void_p(sem))
        s.cleanup()


def malformed_mq(folder):
    s = Scenario(folder)
    opened, sems = [], []
    try:
        for suffix in ("requests", "replies"):
            q = LIBC.mq_open(f"/parking_mq_{s.id}_{suffix}".encode(),
                            os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NONBLOCK,
                            0o600, ctypes.byref(MqAttr(0, 2, 16, 0)))
            assert q >= 0
            opened.append(q)
        LIBC.sem_open.restype = ctypes.c_void_p
        for i in range(4):
            sem = LIBC.sem_open(f"/parking_mq_{s.id}_sem_{i}".encode(),
                               os.O_CREAT | os.O_EXCL, 0o600, 1 if i < 2 else 0)
            assert sem
            sems.append(sem)
        car = s.spawn("parking_car", [s.id, 1], expected=1)
        assert car.wait(timeout=3) == 1
        assert len(s.resources()) == 6, "клиент удалил чужие MQ-ресурсы"
    finally:
        for q in opened:
            LIBC.mq_close(q)
        for sem in sems:
            LIBC.sem_close(ctypes.c_void_p(sem))
        s.cleanup()


def bad_messages(folder):
    s = Scenario(folder)
    try:
        s.start()
        q = LIBC.mq_open(f"/parking_mq_{s.id}_requests".encode(), os.O_WRONLY | os.O_NONBLOCK)
        assert q >= 0
        try:
            attr = MqAttr()
            assert LIBC.mq_getattr(q, ctypes.byref(attr)) == 0
            assert LIBC.mq_send(q, b"x", ctypes.c_size_t(1), 0) == 0
            data = ctypes.create_string_buffer(attr.msgsize)
            assert LIBC.mq_send(q, data, ctypes.c_size_t(attr.msgsize), 0) == 0
        finally:
            LIBC.mq_close(q)
        until(lambda: s.text().count("некорректное сообщение") == 2, "ошибочные сообщения не отклонены")
        s.spawn("parking_car", [s.id, 1])
        s.finish()
    finally:
        s.cleanup()


def bounded_queues(folder):
    s = Scenario(folder)
    try:
        owner = s.start(["-c", "30", "-p", "200", "-P", "200"])
        s.spawn("parking_arrivals", [s.id])
        until(lambda: s.text().count("Прибытие:") >= 20, "не создалась нагрузка на MQ")
        assert len(s.resources()) == 6, "число ресурсов зависит от числа машин"
        for suffix in ("requests", "replies"):
            q = LIBC.mq_open(f"/parking_mq_{s.id}_{suffix}".encode(), os.O_RDONLY | os.O_NONBLOCK)
            assert q >= 0
            attr = MqAttr()
            assert LIBC.mq_getattr(q, ctypes.byref(attr)) == 0 and attr.maxmsg == 10
            LIBC.mq_close(q)
        owner.send_signal(signal.SIGTERM)
        s.finish()
    finally:
        s.cleanup()


def mq_order(folder):
    executable = folder / "check-mq-order"
    common = DIR.parent / "common"
    mq = DIR.parent / "independent_mq"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(common / "include"), "-I" + str(mq / "include"),
                    str(DIR / "check_mq_order.c"), str(mq / "src/protocol.c"),
                    str(common / "src/io.c"), str(common / "src/sim_clock.c"),
                    "-pthread", "-lrt", "-o", str(executable)], check=True)
    s = Scenario(folder)
    try:
        subprocess.run([str(executable), s.id], timeout=5, check=True)
        assert not s.resources()
    finally:
        s.cleanup()


def mq_contract(folder, library, fault_name):
    s = Scenario(folder)
    trace = folder / f"{s.id}.trace"
    env = fault_env(library, fault_name, PARKING_TRACE=str(trace))
    try:
        s.start(["-c", "3", "-d", "1", "-p", "2", "-P", "2"], env=env)
        s.spawn("parking_arrivals", [s.id], env=env)
        s.finish()
        lines = [line.split() for line in trace.read_text().splitlines()]
        assert not any(line[0] in ("shm_open", "sem_init") for line in lines)
        queues = {line[2] for line in lines if line[0] == "mq_open" and line[3] == "0"}
        removed = {line[2] for line in lines if line[0] == "mq_unlink" and line[3] == "0"}
        assert len(queues) == 2 and queues == removed
    finally:
        s.cleanup()


def mq_foreign_objects(folder):
    for semaphore in (False, True):
        s = Scenario(folder)
        handle = None
        try:
            name = f"/parking_mq_{s.id}_sem_0" if semaphore else f"/parking_mq_{s.id}_replies"
            if semaphore:
                LIBC.sem_open.restype = ctypes.c_void_p
                handle = LIBC.sem_open(name.encode(), os.O_CREAT | os.O_EXCL, 0o600, 1)
                assert handle
            else:
                handle = LIBC.mq_open(name.encode(), os.O_CREAT | os.O_EXCL | os.O_RDWR,
                                     0o600, ctypes.byref(MqAttr(0, 2, 16, 0)))
                assert handle >= 0
            s.log.write_text("сохранить журнал")
            p = s.spawn("parking_dispatcher", [s.id, "-L", s.log], expected=1)
            assert p.wait(timeout=3) == 1
            assert len(s.resources()) == 1 and s.log.read_text() == "сохранить журнал"
            if semaphore:
                assert LIBC.sem_unlink(name.encode()) == 0
            else:
                assert LIBC.mq_unlink(name.encode()) == 0
            s.finish(check_log=False)
        finally:
            if handle is not None:
                if semaphore:
                    LIBC.sem_close(ctypes.c_void_p(handle))
                else:
                    LIBC.mq_close(handle)
            s.cleanup()


def fault(folder, library, name):
    s = Scenario(folder)
    env = fault_env(library, name)
    try:
        program = "parking_dispatcher"
        args = [s.id, "-d", "1", "-c", "3", "-C", "0", "-S", "0", "-L", s.log]
        p = s.spawn(program, args, expected=1, env=env)
        assert p.wait(timeout=5) == 1
        s.finish(check_log=False)
    finally:
        s.cleanup()


def launcher_fault(folder, library, name):
    s = Scenario(folder)
    env = fault_env(library, name)
    try:
        p = s.spawn(LAUNCHER, ["-d", "1", "-t", "0", "-a", "0", "-b", "0", "-L", s.log],
                    expected=1, env=env)
        assert p.wait(timeout=8) == 1
        if "POSIX память:" in s.text() or "POSIX очереди:" in s.text():
            s.id = actual_id(s.text())
        s.finish(check_log=False)
    finally:
        s.cleanup()


def named_contract(folder, library, unlink_fault=False):
    s = Scenario(folder)
    trace = folder / f"{s.id}.trace"
    env = fault_env(library, "sem_unlink:1" if unlink_fault else "sem_init:1", PARKING_TRACE=str(trace))
    try:
        s.start(["-d", "1", "-p", "2", "-P", "2", "-c", "2"], env=env)
        s.spawn("parking_arrivals", [s.id], env=env)
        s.finish()
        lines = [line.split() for line in trace.read_text().splitlines()]
        names = {line[2] for line in lines if line[0] == "sem_open" and line[3] == "0"}
        removed = {line[2] for line in lines if line[0] == "sem_unlink" and line[3] == "0"}
        assert len(names) == 4 and removed == names
        assert not any(line[0] == "sem_init" for line in lines)
        assert sum(line[0] == "sem_close" and line[3] == "0" for line in lines) == (16 if MQ else 12)
    finally:
        s.cleanup()


def main():
    tests = [
        ("ручной запуск из разных cwd, имена и отдельные процессы", manual),
        ("нулевой сеанс без генератора", empty_session),
        ("автомобиль раньше диспетчера", lambda p: early_connect(p, "parking_car")),
        ("генератор раньше диспетчера", lambda p: early_connect(p, "parking_arrivals")),
        ("повторный номер автомобиля", duplicate_car),
        ("неверные номера автомобилей", invalid_car),
        ("совпадение имени сеанса", collision),
        ("повторный генератор", generator_collision),
        ("SIGINT автомобилю", lambda p: participant_signal(p, "parking_car")),
        ("SIGINT генератору", lambda p: participant_signal(p, "parking_arrivals")),
        ("два независимых сеанса", parallel),
        ("SIGINT всей группе запуска", whole_group),
        ("воспроизводимые планы по seed", repeat_seed),
        ("авария независимого автомобиля", client_crash),
        ("нет диспетчера: ограниченное ожидание", missing_owner),
        ("повреждённые атрибуты очередей" if MQ else "повреждённый заголовок общей памяти", malformed),
    ]
    if MQ:
        tests.extend([("порядок ответов при разных адресатах", mq_order),
                      ("неверные размер и версия сообщений", bad_messages),
                      ("две очереди при множестве клиентов и остановка", bounded_queues),
                      ("чужая очередь и семафор при частичном создании", mq_foreign_objects)])
    failed = 0
    with tempfile.TemporaryDirectory(prefix="parking-independent-tests-", dir="/tmp") as tmp:
        folder = Path(tmp)
        if os.environ.get("PARKING_SKIP_FAULTS") != "1":
            library = folder / "fail.so"
            subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                            str(DIR / "fail_syscalls.c"), "-o", str(library), "-ldl"], check=True)
            faults = ("mq_open:1", "mq_open:2", "sem_open:1", "sem_open:2", "sem_open:4") if MQ else (
                "shm_open:1", "ftruncate:1", "posix_fallocate:1", "mmap:1", "sem_open:1", "sem_open:2", "sem_open:4")
            for name in faults:
                tests.append((f"откат владельца при {name}", lambda p, n=name: fault(p, library, n)))
            for name in ("posix_spawn:1", "posix_spawn:2", "posix_spawn:3"):
                tests.append((f"ошибка запуска при {name}", lambda p, n=name: launcher_fault(p, library, n)))
            tests.append(("именованные семафоры и sem_close без sem_init", lambda p: named_contract(p, library)))
            tests.append(("повтор удаления имени после ошибки sem_unlink", lambda p: named_contract(p, library, True)))
            if MQ:
                for name in ("shm_open:1", "mq_send:1", "mq_receive:1", "mq_unlink:1"):
                    tests.append((f"MQ-протокол при {name}", lambda p, n=name: mq_contract(p, library, n)))
        for name, test in tests:
            try:
                test(folder)
                print(f"  OK   {name}", flush=True)
            except (AssertionError, OSError, subprocess.SubprocessError) as error:
                failed += 1
                print(f"  FAIL {name}: {error}", flush=True)
        print(f"Независимые программы: пройдено {len(tests) - failed}, провалено {failed}")
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main())
