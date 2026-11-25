#!/usr/bin/env bash
# По умолчанию все проверки; --unit не создаёт сетевых сокетов.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${PARKING_TCP_BIN:-$ROOT/build/bin/part4}"
mode="${1:-all}"
case "$mode" in all|--unit) ;; *) echo 'Использование: run_tests.sh [all|--unit]'; exit 2 ;; esac
for program in parking_tcp_check_transport parking_tcp_check_model parking_tcp_check_sessions parking_tcp_check_client; do
    if [ ! -x "$BIN/$program" ]; then
        echo "Не найден $BIN/$program; соберите цели parking_tcp и parking_tcp_checks"; exit 2
    fi
done
TASK_TMP="$(mktemp -d /tmp/parking-tcp-unit-XXXXXX)"
trap 'rm -rf "$TASK_TMP"' EXIT
"$BIN/parking_tcp_check_transport"
echo 'OK: поток TCP, кодирование, фрагментация, частичная отправка, ошибки и предел буфера'
"$BIN/parking_tcp_check_model" "$TASK_TMP/model.txt" > "$TASK_TMP/model.out"
awk -f "$ROOT/part3/tests/check_log.awk" "$TASK_TMP/model.txt"
echo 'OK: состояния модели, повторное прибытие, очередь, таймаут, отказ, отмена и освобождение'
"$BIN/parking_tcp_check_sessions" "$TASK_TMP/sessions.txt" > "$TASK_TMP/sessions.out"
awk -f "$ROOT/part3/tests/check_log.awk" "$TASK_TMP/sessions.txt"
echo 'OK: протокол сеансов, дубликаты, восстановление, планы генератора и STOP/ACK'
"$BIN/parking_tcp_check_client" > "$TASK_TMP/client.out"
echo 'OK: новый/восстановленный клиент, отъезд, STOP/ACK и неверные состояния'
python3 "$ROOT/part4/tests/check_cli.py" "$BIN"
if [ "$mode" = all ]; then
    python3 "$ROOT/part4/tests/check_network.py" "$BIN"
fi
