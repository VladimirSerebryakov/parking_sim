#!/usr/bin/env python3
"""Ошибки CLI проверяются до создания сокета, в том числе в песочнице."""
from pathlib import Path
import subprocess
import sys

binary = Path(sys.argv[1]).resolve()
cases = []
for arguments in ('-c abc', '-c 99999999999999', '-R 4294967296', '-q -1', '-d 0',
                  '-a 3 -b 2', '-p 5 -P 1', '-g bogus', '-x', '-t -5', 'лишнее',
                  '-c 2147483647 -C 1 -S 0', '-d 2147483647 -p 2147483647 -P 2147483647'):
    cases.append(('parking_server', ['127.0.0.1', '5050', *arguments.split()]))
for port in ('0', '-1', '65536', 'abc', '5050x'):
    cases.append(('parking_server', ['127.0.0.1', port]))
for number in ('0', '-1', '4097', 'abc', '1x'):
    cases.append(('parking_car', ['127.0.0.1', '5050', number]))
for name in ('parking_server', 'parking_car', 'parking_arrivals', 'parking_monitor'):
    cases.append((name, []))
cases.append(('parking_monitor', ['127.0.0.1', '5050', '--wrong']))
for name, args in cases:
    result = subprocess.run([str(binary / name), *args], capture_output=True, text=True, timeout=3)
    assert result.returncode == 1, (name, args, result.returncode, result.stderr)
    assert not any(marker in result.stderr for marker in ('AddressSanitizer', 'runtime error:', 'LeakSanitizer')), result.stderr
print(f'OK: {len(cases)} проверок ошибок командной строки')
