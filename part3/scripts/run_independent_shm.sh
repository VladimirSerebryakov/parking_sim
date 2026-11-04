#!/usr/bin/env bash
# Необязательный запуск сеанса одной командой; все участники поддерживают отдельный запуск.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$ROOT/build/bin/independent_shm/parking_independent_shm" -L "$ROOT/parking_result.txt" "$@"
