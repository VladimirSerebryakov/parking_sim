#!/usr/bin/env bash
# Сценарий демонстрации; участников можно запускать вручную по имени сеанса.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$ROOT/build/bin/independent_mq/parking_independent_mq" -L "$ROOT/parking_result.txt" "$@"
