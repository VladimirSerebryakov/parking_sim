#!/usr/bin/env python3
"""Интеграционные проверки процессов и IPC в Linux; артефакты — только в /tmp."""

import os
import ctypes
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time


DIR = Path(__file__).resolve().parent
BIN = Path(sys.argv[1]).resolve()
VARIANT = sys.argv[2] if len(sys.argv) > 2 else BIN.name.removeprefix("parking_")
assert VARIANT in ("fork_unnamed", "fork_named"), f"неизвестный вариант: {VARIANT}"
NAMED = VARIANT == "fork_named"
PREFIX = "named" if NAMED else "unnamed"
BASE = [str(BIN), "-t", "0", "-R", "7", "-r", "1", "-l", "0", "-s", "0",
        "-c", "8", "-C", "0", "-S", "0", "-a", "0", "-b", "0", "-q", "8"]


def child_pids(pid):
    try:
        return {int(p) for p in Path(f"/proc/{pid}/task/{pid}/children").read_text().split()}
    except FileNotFoundError:
        return set()


def assert_clean(pid, children):
    for child in children:
        assert not Path(f"/proc/{child}").exists(), f"не собран ребёнок {child}"
    leftovers = list(Path("/dev/shm").glob(f"parking_{PREFIX}_{pid}_*"))
    leftovers += list(Path("/dev/shm").glob(f"sem.parking_{PREFIX}_{pid}_*"))
    assert not leftovers, f"остались IPC-ресурсы: {leftovers}"


class Run:
    def __init__(self, folder, name, args=(), env=None):
        self.log = folder / f"{name}.txt"
        self.err_path = folder / f"{name}.err"
        self.err = self.err_path.open("wb")
        self.process = subprocess.Popen(
            BASE + ["-d", "10", "-p", "100", "-P", "100", "-w", "200",
                    "-L", str(self.log)] + list(args), stdout=subprocess.DEVNULL,
            stderr=self.err, start_new_session=True, env=env)
        self.children = set()

    def observe(self):
        self.children |= child_pids(self.process.pid)

    def text(self):
        return self.log.read_text() if self.log.exists() else ""

    def ready(self, marker="поставлено в очередь"):
        end = time.monotonic() + 5
        while time.monotonic() < end:
            self.observe()
            if marker in self.text():
                return
            assert self.process.poll() is None, self.err_path.read_text()
            time.sleep(0.002)
        raise AssertionError(f"не дождались события: {marker}")

    def finish(self, code=0, check_log=True):
        end = time.monotonic() + 8
        while self.process.poll() is None and time.monotonic() < end:
            self.observe()
            time.sleep(0.002)
        actual = self.process.wait(timeout=1)
        self.err.close()
        stderr = self.err_path.read_text()
        assert actual == code, f"код {actual}, ожидался {code}: {stderr}"
        assert not re.search(r"AddressSanitizer|LeakSanitizer|runtime error:", stderr), stderr
        if code == 0:
            assert not stderr, stderr
        if check_log:
            result = subprocess.run(["awk", "-f", str(DIR / "check_log.awk"), str(self.log)],
                                    capture_output=True, text=True, check=False)
            assert result.returncode == 0, result.stdout + result.stderr
        assert_clean(self.process.pid, self.children)

    def cleanup(self):
        # Ограничиваем последствия проваленного теста его собственной группой процессов.
        try:
            os.killpg(self.process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        self.process.wait(timeout=3)
        self.err.close()


def signal_test(folder, sig, group=False, early=False):
    args = ["-a", "100", "-b", "100"] if early else []
    run = Run(folder, f"signal-{sig}-{group}-{early}", args)
    try:
        run.ready("POSIX память:" if early else "поставлено в очередь")
        if group:
            os.killpg(run.process.pid, sig)
        else:
            run.process.send_signal(sig)
        run.finish()
        expected = "Ctrl+C" if sig == signal.SIGINT else "SIGTERM"
        assert f"СИМУЛЯЦИЯ ПРЕРВАНА ({expected})" in run.text()
        if early:
            assert "Прибытие:" not in run.text()
    finally:
        run.cleanup()


def process_tree(folder):
    run = Run(folder, "tree")
    try:
        run.ready()
        end = time.monotonic() + 3
        while len(child_pids(run.process.pid)) != 10 and time.monotonic() < end:
            run.observe()
            time.sleep(0.002)
        children = child_pids(run.process.pid)
        assert len(children) == 10, f"ожидали диспетчера, табло и 8 авто, получили {children}"
        run.observe()
        for pid in children | {run.process.pid}:
            assert len(list(Path(f"/proc/{pid}/task").iterdir())) == 1, "обнаружены потоки"
            maps = Path(f"/proc/{pid}/maps").read_text()
            assert re.search(rf"rw-s.*parking_{PREFIX}_.*\(deleted\)", maps), "нет общей POSIX-памяти"
            if NAMED:
                assert len(re.findall(r"rw-s.*[/]sem\..*\(deleted\)", maps)) == 11, "нет 11 именованных семафоров"
        assert_clean(run.process.pid, set())  # Имя уже удалено, отображения ещё живут.
        run.process.send_signal(signal.SIGTERM)
        run.finish()
    finally:
        run.cleanup()


def parallel_runs(folder):
    runs = [Run(folder, f"parallel-{i}") for i in range(2)]
    try:
        for run in runs:
            run.ready()
        names = [re.search(r"POSIX память: (\S+)", run.text())[1] for run in runs]
        assert names[0] != names[1], "совпали имена ресурсов"
        for run in runs:
            run.process.send_signal(signal.SIGTERM)
        for run in runs:
            run.finish()
    finally:
        for run in runs:
            run.cleanup()


def repeat_seed(folder):
    inputs = []
    for i in range(2):
        run = Run(folder, f"seed-{i}", ["-d", "1", "-c", "10", "-C", "10", "-S", "10",
                                      "-a", "0", "-b", "2", "-p", "1", "-P", "5", "-w", "3"])
        try:
            run.finish()
            inputs.append(re.findall(r"Генерация: .*", run.text()))
            assert len(inputs[-1]) == 30
        finally:
            run.cleanup()
    assert inputs[0] == inputs[1], "seed не воспроизводит случайные входные значения"


def child_crash(folder):
    run = Run(folder, "crash")
    try:
        run.ready()
        run.observe()
        assert run.children
        os.kill(min(run.children), signal.SIGKILL)
        run.finish(code=1, check_log=False)
        assert "дочерний процесс завершился аварийно" in run.err_path.read_text()
        assert "ИТОГОВАЯ СТАТИСТИКА" not in run.text()
    finally:
        run.cleanup()


def checker_rejects(folder):
    run = Run(folder, "checker", ["-d", "1", "-p", "10", "-P", "10", "-w", "200"])
    try:
        run.finish()
        original = run.text()
        corruptions = [original.replace("места 1/0/0", "места 0/1/0"),
                       original.replace("Освобождение места #1", "пропущено освобождение", 1),
                       re.sub(r"Обслужено автомобилей: \d+", "Обслужено автомобилей: 999", original),
                       re.sub(r"поставлено в очередь \(\d+/8\)", "поставлено в очередь (0/8)", original)]
        for i, text in enumerate(corruptions):
            assert text != original
            path = folder / f"corrupt-{i}.txt"
            path.write_text(text)
            result = subprocess.run(["awk", "-f", str(DIR / "check_log.awk"), str(path)],
                                    stdout=subprocess.DEVNULL, check=False)
            assert result.returncode != 0, f"проверка приняла испорченный журнал {i}"
    finally:
        run.cleanup()


def fault_test(folder, library, fault):
    env = dict(os.environ, LD_PRELOAD=str(library), PARKING_FAIL=fault)
    run = Run(folder, "fault-" + fault.replace(":", "-"), ["-d", "1"], env)
    try:
        run.finish(code=1, check_log=fault.startswith("fork:") and fault != "fork:1")
        assert "Ошибка:" in run.err_path.read_text()
    finally:
        run.cleanup()


def named_lifecycle(folder, library):
    trace = folder / "named-trace.txt"
    # Наличие sem_init здесь привело бы к отказу: реализация обязана использовать sem_open.
    env = dict(os.environ, LD_PRELOAD=str(library), PARKING_FAIL="sem_init:1", PARKING_TRACE=str(trace))
    run = Run(folder, "named-lifecycle", ["-d", "1", "-p", "1", "-P", "1"], env)
    try:
        run.finish()
        lines = [line.split() for line in trace.read_text().splitlines()]
        opened = {line[2] for line in lines if line[0] == "sem_open" and line[3] == "0"}
        unlinked = {line[2] for line in lines if line[0] == "sem_unlink" and line[3] == "0"}
        assert len(opened) == 11 and opened == unlinked, "не все имена семафоров удалены"
        assert not any(line[0] == "sem_init" for line in lines), "именованный вариант вызывает sem_init"
        closes = {}
        for line in lines:
            if line[0] == "sem_close":
                assert line[3] == "0"
                closes[line[1]] = closes.get(line[1], 0) + 1
        assert len(closes) == 11 and set(closes.values()) == {11}, f"неверный sem_close по процессам: {closes}"
    finally:
        run.cleanup()


def named_collision(folder, library):
    trace = folder / "collision-trace.txt"
    env = dict(os.environ, LD_PRELOAD=str(library), PARKING_FAIL="sem_collision:1", PARKING_TRACE=str(trace))
    run = Run(folder, "named-collision", env=env)
    foreign = None
    try:
        assert run.process.wait(timeout=5) == 1
        lines = [line.split() for line in trace.read_text().splitlines()]
        foreign = next(line[2] for line in lines if line[0] == "collision")
        assert Path("/dev/shm", "sem." + foreign.lstrip("/")).exists(), "программа удалила чужой семафор"
        assert not any(line[0] == "sem_unlink" and line[2] == foreign for line in lines)
    finally:
        if foreign is not None:
            ctypes.CDLL(None).sem_unlink(foreign.encode())
        run.cleanup()
    run.finish(code=1, check_log=False)


def main():
    tests = [
        ("дерево процессов и общая память", process_tree),
        ("SIGINT всей группе процессов", lambda p: signal_test(p, signal.SIGINT, group=True)),
        ("SIGTERM всей группе процессов", lambda p: signal_test(p, signal.SIGTERM, group=True)),
        ("SIGINT до первого автомобиля", lambda p: signal_test(p, signal.SIGINT, early=True)),
        ("два одновременных запуска", parallel_runs),
        ("повтор случайных входов по seed", repeat_seed),
        ("авария дочернего процесса", child_crash),
        ("отклонение четырёх испорченных журналов", checker_rejects),
    ]
    failed = 0
    with tempfile.TemporaryDirectory(prefix="parking-ipc-", dir="/tmp") as tmp:
        folder = Path(tmp)
        if os.environ.get("PARKING_SKIP_FAULTS") != "1":
            library = folder / "fail_syscalls.so"
            subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-Wall", "-Wextra",
                            "-Werror", str(DIR / "fail_syscalls.c"), "-o", str(library), "-ldl"], check=True)
            sem_faults = ("sem_open:1", "sem_open:2", "sem_open:4", "sem_open:7", "sem_unlink:1", "sem_unlink:4") if NAMED else (
                "sem_init:1", "sem_init:2", "sem_init:4", "sem_init:7")
            for fault in ("shm_open:1", "ftruncate:1", "posix_fallocate:1", "mmap:1", *sem_faults,
                          "fork:1", "fork:2", "fork:5", "crash_lock:1"):
                tests.append((f"откат при {fault}", lambda p, f=fault: fault_test(p, library, f)))
            if NAMED:
                tests.append(("sem_open/unlink/close без sem_init", lambda p: named_lifecycle(p, library)))
                tests.append(("чужой семафор при совпадении имени", lambda p: named_collision(p, library)))
        for name, test in tests:
            try:
                test(folder)
                print(f"  OK   {name}", flush=True)
            except (AssertionError, OSError, subprocess.SubprocessError) as error:
                failed += 1
                print(f"  FAIL {name}: {error}", flush=True)
        print(f"Процессы и IPC: пройдено {len(tests) - failed}, провалено {failed}")
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main())
