#!/usr/bin/env bash
# Прогон тестовых наборов из cases.txt и тестов сигналов (SIGINT, SIGTERM).
# Запуск: part3/tests/run_tests.sh [all|fork_unnamed|fork_named|independent_shm|independent_mq|бинарник]
# По умолчанию проверяются все реализованные варианты.
# Код выхода: 0 - все тесты пройдены, 1 - есть проваленные, 2 - не найден исполняемый файл.

set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
selection="${1:-all}"
if [ "$selection" = all ]; then
    result=0
    for variant in fork_unnamed fork_named independent_shm independent_mq; do
        if [ -n "${PARKING_TEST_LOGS:-}" ]; then
            PARKING_TEST_LOGS="$PARKING_TEST_LOGS/$variant" "$DIR/run_tests.sh" "$variant" || result=1
        else
            "$DIR/run_tests.sh" "$variant" || result=1
        fi
    done
    exit "$result"
fi
case "$selection" in
    fork_unnamed|fork_named|independent_shm|independent_mq)
        VARIANT="$selection"
        BIN="$DIR/../../build/bin/$VARIANT/parking_$VARIANT"
        ;;
    *)
        BIN="$selection"
        case "$(basename "$BIN")" in
            parking_fork_unnamed) VARIANT=fork_unnamed ;;
            parking_fork_named) VARIANT=fork_named ;;
            parking_independent_shm) VARIANT=independent_shm ;;
            parking_independent_mq) VARIANT=independent_mq ;;
            *) echo "Неизвестный вариант бинарника: $BIN"; exit 2 ;;
        esac
        ;;
esac
LOGS="${PARKING_TEST_LOGS:-$DIR/logs/$VARIANT}"
TIMEOUT=120 # секунд на один прогон: дольше - считаем, что программа зависла

if [ ! -x "$BIN" ]; then
    echo "Не найден исполняемый файл: $BIN"
    echo "Соберите проект из корня: cmake -S . -B build && cmake --build build"
    exit 2
fi
mkdir -p "$LOGS"
rm -f "$LOGS"/*.txt "$LOGS"/*.err

passed=0
failed=0
n=0

# Проверка одного прогона. $1 - ожидание (done|limit|error|INT|TERM), $2 - код выхода,
# $3 - журнал, $4 - stderr. Печатает пояснение, возвращает 0 при успехе
check_run() {
    local expect=$1 code=$2 log=$3 err=$4 reason
    if grep -qE "ThreadSanitizer|AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:" "$err"; then
        echo "сообщение санитайзера (см. $err)"
        return 1
    fi
    if [ "$code" -eq 124 ]; then
        echo "программа не завершилась за $TIMEOUT с"
        return 1
    fi
    if [ "$expect" = error ]; then
        [ "$code" -eq 1 ] || { echo "ожидался код выхода 1, получен $code"; return 1; }
        local first_line
        first_line=$(head -n 1 "$err")
        echo "отклонено: ${first_line:0:60}..."
        return 0
    fi
    [ "$code" -eq 0 ] || { echo "код выхода $code"; return 1; }

    reason=$(grep -E "^ СИМУЛЯЦИЯ" "$log")
    case $expect in
        done)  [ "$reason" = " СИМУЛЯЦИЯ ЗАВЕРШЕНА" ] ;;
        limit) [ "$reason" = " СИМУЛЯЦИЯ ЗАВЕРШЕНА (лимит времени)" ] ;;
        INT)   [ "$reason" = " СИМУЛЯЦИЯ ПРЕРВАНА (Ctrl+C)" ] ;;
        TERM)  [ "$reason" = " СИМУЛЯЦИЯ ПРЕРВАНА (SIGTERM)" ] ;;
    esac || { echo "неожиданная причина завершения: '${reason# }'"; return 1; }

    awk -f "$DIR/check_log.awk" "$log"
}

# Название, дополненное пробелами до 36 символов (printf %-36s считает байты, а русская буква - 2 байта)
pad_name() {
    printf "%s%*s" "$1" $((36 - ${#1})) ""
}

# Печать результата: $1 - номер, $2 - название, $3 - код check_run, $4 - пояснение
report() {
    if [ "$3" -eq 0 ]; then
        printf "  \033[32mOK\033[0m   %2d. %s %s\n" "$1" "$(pad_name "$2")" "$4"
        passed=$((passed + 1))
    else
        printf "  \033[31mFAIL\033[0m %2d. %s журнал: %s\n%s\n" "$1" "$(pad_name "$2")" "$LOGS/$(printf %02d "$1").txt" "$4"
        failed=$((failed + 1))
    fi
}

echo "Исполняемый файл: $BIN"
echo "Наборы параметров (part3/tests/cases.txt):"
while IFS='|' read -r expect name args; do
    expect="${expect//[[:space:]]/}"
    [ -z "$expect" ] || [ "${expect:0:1}" = "#" ] && continue
    name=$(echo "$name" | sed 's/^ *//; s/ *$//')
    n=$((n + 1))
    log="$LOGS/$(printf %02d $n).txt"
    err="$LOGS/$(printf %02d $n).err"

    # $args без кавычек намеренно: строка разбивается на отдельные аргументы
    timeout $TIMEOUT "$BIN" -d 1 -t 0 -R 7 -L "$log" $args > /dev/null 2> "$err" < /dev/null
    code=$?
    msg=$(check_run "$expect" $code "$log" "$err")
    report $n "$name" $? "$msg"
done < "$DIR/cases.txt"

# Тест сигнала: запускаем симуляцию с заполненной очередью, через секунду шлём сигнал $1
# и проверяем, что программа корректно завершилась не позже чем через 10 с
signal_test() {
    local sig=$1 log err pid code msg rc
    n=$((n + 1))
    log="$LOGS/$(printf %02d $n).txt"
    err="$LOGS/$(printf %02d $n).err"
    "$BIN" -d 50 -t 0 -q 10 -w 100 -a 0 -b 0 -p 30 -P 40 -L "$log" > /dev/null 2> "$err" < /dev/null &
    pid=$!
    sleep 1
    kill -"$sig" $pid
    for _ in $(seq 100); do
        kill -0 $pid 2> /dev/null || break
        sleep 0.1
    done
    if kill -0 $pid 2> /dev/null; then
        kill -9 $pid
        wait $pid 2> /dev/null
        report $n "сигнал SIG$sig" 1 "  не завершилась за 10 с после сигнала"
        return
    fi
    wait $pid
    code=$?
    msg=$(check_run "$sig" $code "$log" "$err")
    rc=$?
    report $n "сигнал SIG$sig" $rc "$msg"
}

echo "Сигналы:"
signal_test INT
signal_test TERM

echo
echo "Итого: пройдено $passed, провалено $failed"
[ $failed -eq 0 ] || exit 1
if [ "$VARIANT" = independent_shm ] || [ "$VARIANT" = independent_mq ]; then
    python3 "$DIR/check_independent.py" "$BIN"
else
    python3 "$DIR/check_processes.py" "$BIN" "$VARIANT"
fi
