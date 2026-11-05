#!/usr/bin/env bash
# Запуск из любой рабочей директории; пользовательские флаги переопределяют значения ниже.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$ROOT/build/bin/fork_unnamed/parking_fork_unnamed" -L "$ROOT/parking_result.txt" "$@"
