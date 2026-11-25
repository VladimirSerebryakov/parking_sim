#!/usr/bin/env bash
# Демонстрация на одном хосте: demo.sh ПОРТ [флаги модели]. PARKING_MONITOR=0 — базовый режим.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${PARKING_TCP_BIN:-$ROOT/build/bin/part4}"
PORT="${1:-5050}"
if [ "$#" -gt 0 ]; then shift; fi
LOGS="$(mktemp -d /tmp/parking-tcp-demo-XXXXXX)"
echo "Журналы демонстрации: $LOGS"
server=0
generator=0
monitor=0
interrupted=0
stop_demo() {
    interrupted=1
    if [ "$server" -gt 0 ]; then kill -TERM "$server" 2>/dev/null || true; fi
}
cleanup() {
    for pid in "$server" "$generator" "$monitor"; do
        if [ "$pid" -gt 0 ]; then kill -TERM "$pid" 2>/dev/null || true; fi
    done
    wait 2>/dev/null || true
}
trap stop_demo INT TERM
trap cleanup EXIT
"$BIN/parking_server" 127.0.0.1 "$PORT" -d 100 -t 0 -R 7 -L "$LOGS/parking.txt" "$@" > "$LOGS/server.out" &
server=$!
ready=0
for ((i=0; i<200; i++)); do
    if grep -q 'Сервер готов:' "$LOGS/server.out"; then ready=1; break; fi
    if ! kill -0 "$server" 2>/dev/null; then break; fi
    sleep 0.02
done
if [ "$ready" -ne 1 ]; then wait "$server"; exit 1; fi
if [ "${PARKING_MONITOR:-1}" = 1 ]; then
    "$BIN/parking_monitor" 127.0.0.1 "$PORT" &
    monitor=$!
fi
"$BIN/parking_arrivals" 127.0.0.1 "$PORT" > "$LOGS/arrivals.out" &
generator=$!
result=0
wait "$server" || result=1
# wait может быть прерван сигналом оболочке; ждём штатного завершения сервера ещё раз.
if kill -0 "$server" 2>/dev/null; then wait "$server" || result=1; fi
server=0
if [ "$interrupted" -eq 1 ]; then result=0; fi
wait "$generator" || result=1
generator=0
if [ "$monitor" -gt 0 ]; then wait "$monitor" || result=1; monitor=0; fi
awk -f "$ROOT/part3/tests/check_log.awk" "$LOGS/parking.txt" || result=1
exit "$result"
