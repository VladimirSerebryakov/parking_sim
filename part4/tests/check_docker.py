#!/usr/bin/env python3
"""Приёмка Compose в отдельном проекте; существующие контейнеры не затрагиваются."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', action='store_true', help='также собрать образ перед запуском')
    args = parser.parse_args()
    project = f'parking4_check_{os.getpid()}_{uuid.uuid4().hex[:6]}'
    folder = Path(tempfile.mkdtemp(prefix='parking-tcp-docker-', dir='/tmp'))
    shared = ROOT / 'part4/logs'
    shared.mkdir(exist_ok=True)
    override = shared / f'{project}.json'
    journal = shared / f'{project}.txt'
    override.write_text(json.dumps({'services': {'server': {'command': [
        'parking_server', '0.0.0.0', '5050', '-d', '10', '-t', '0', '-R', '7',
        '-r', '2', '-l', '0', '-s', '0', '-c', '100', '-C', '0', '-S', '0',
        '-a', '5', '-b', '5', '-p', '10000', '-P', '10000', '-q', '100', '-w', '100000',
        '-L', f'/results/{project}.txt']}}}), encoding='utf-8')
    base = ['docker', 'compose', '-p', project, '-f', 'part4/compose.yaml',
            '-f', str(override.relative_to(ROOT)), '--profile', 'observe']
    extra = []

    def run(command, check=True, timeout=60):
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=timeout)
        with (folder / 'commands.jsonl').open('a') as log:
            log.write(json.dumps({'argv': command, 'code': result.returncode,
                                  'stdout': result.stdout, 'stderr': result.stderr}, ensure_ascii=False) + '\n')
        if check and result.returncode:
            raise RuntimeError(f'{command}: {result.stdout}\n{result.stderr}')
        return result.stdout.strip()

    def compose(*command, **kwargs):
        return run([*base, *command], **kwargs)

    def inspect(container):
        return json.loads(run(['docker', 'inspect', container]))[0]

    def logs(container):
        return run(['docker', 'logs', container])

    def until(check, message, timeout=20):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if check():
                return
            time.sleep(.1)
        raise AssertionError(message)

    def service_ids(name):
        # Одноразовые ручные автомобили используют образ сервиса arrivals, но не его роль.
        ids = compose('ps', '-a', '-q', name).splitlines()
        return [id for id in ids if inspect(id)['Config']['Labels'].get('com.docker.compose.oneoff', '').lower() != 'true']

    def start_car(name):
        id = compose('run', '-d', '--no-deps', '--name', f'{project}_{name}',
                     'arrivals', 'parking_car', 'server', '5050', '1').splitlines()[-1]
        extra.append(id)
        return id

    def stop(container):
        run(['docker', 'stop', '--time', '10', container])

    def exited_ok(container):
        state = inspect(container)['State']
        return not state['Running'] and state['ExitCode'] == 0

    try:
        if args.build:
            compose('build', 'server', timeout=300)
        compose('up', '-d', '--no-build', '--scale', 'monitor=3', 'server', 'monitor')
        server = service_ids('server')[0]
        monitors = service_ids('monitor')
        assert len(monitors) == 3
        until(lambda: all('СОСТОЯНИЕ ПАРКОВКИ' in logs(id) for id in monitors), 'нет снимков трёх наблюдателей')
        print('OK: сервер и три наблюдателя в отдельных контейнерах', flush=True)

        first = start_car('car_first')
        until(lambda: 'Начало стоянки: авто #1 ' in logs(server), 'ручной автомобиль не начал стоянку')
        stop(first); assert exited_ok(first) and inspect(server)['State']['Running']
        recovered = start_car('car_recovered')
        until(lambda: 'стоянка' in logs(recovered), 'не восстановилась стоянка автомобиля')
        assert logs(server).count('Прибытие: авто #1 ') == 1
        print('OK: отключение и восстановление рабочего клиента без второго прибытия', flush=True)

        stop(monitors[0]); assert exited_ok(monitors[0])
        previous = len(logs(monitors[0]))
        run(['docker', 'start', monitors[0]])
        until(lambda: len(logs(monitors[0])) > previous, 'наблюдатель не получил новый снимок')
        print('OK: отключение и возвращение наблюдателя', flush=True)

        compose('up', '-d', '--no-build', '--no-deps', 'arrivals')
        generator = service_ids('arrivals')[0]
        until(lambda: 'поставлено в очередь' in logs(server), 'генератор не выпустил автомобили')
        stop(generator); assert exited_ok(generator) and inspect(server)['State']['Running']
        previous = len(logs(generator))
        run(['docker', 'start', generator])
        until(lambda: len(logs(generator)) > previous, 'генератор не восстановился')
        print('OK: отключение и возвращение генератора', flush=True)

        ids = [server, generator, *monitors, recovered]
        ips = [next(iter(inspect(id)['NetworkSettings']['Networks'].values()))['IPAddress'] for id in ids]
        assert len(set(ips)) == len(ids) and all(ips), ips
        stop(server)
        until(lambda: all(exited_ok(id) for id in ids), 'не все клиенты завершились с сервером')
        print('OK: остановка сервера завершила генератор, автомобили и всех наблюдателей', flush=True)

        for i, id in enumerate([*ids, first]):
            (folder / f'{i}.out').write_text(logs(id), encoding='utf-8')
        text = journal.read_text()
        (folder / 'parking.txt').write_text(text)
        arrivals = re.findall(r'Прибытие: авто #(\d+)', text)
        assert len(arrivals) == len(set(arrivals))
        checked = subprocess.run(['awk', '-f', str(ROOT / 'part3/tests/check_log.awk'), str(journal)],
                                 capture_output=True, text=True, check=True)
        print('OK: журнал и четыре инварианта — ' + checked.stdout.strip(), flush=True)
        (folder / 'result.json').write_text(json.dumps({'project': project, 'ips': ips,
            'arrivals': len(arrivals), 'exit_codes': [inspect(id)['State']['ExitCode'] for id in ids]}, indent=2))
    finally:
        # Очищаем только созданный этим тестом проект и одноразовые контейнеры.
        try:
            compose('down', '--remove-orphans', check=False)
            if extra:
                run(['docker', 'rm', '-f', *extra], check=False)
        finally:
            override.unlink(missing_ok=True)
            print(f'Журналы Docker-проверки: {folder}', flush=True)


if __name__ == '__main__':
    main()
