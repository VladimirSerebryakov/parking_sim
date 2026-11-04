# Проверка журнала parking_result.txt: инварианты из условия и согласованность статистики.
# Запуск: awk -f check_log.awk parking_result.txt   (код выхода 0 - всё в порядке)
#
# Каждая машина проходит состояния:
#   A - прибыла, Q - в очереди, P - диспетчер назначил место, S - стоит, L - уехала с места,
#   X - завершилась, не заехав (отказ, уход из очереди по таймауту, отмена при остановке)
# Допустимые пути: A -> P -> S -> L,  A -> Q -> P -> S -> L,  A -> X,  A -> Q -> X
#
# Проверяется:
#   - одно место не назначено двум машинам одновременно;
#   - машина не стоит в очереди и на парковке одновременно (нет перехода Q -> S минуя P и т.п.);
#   - размер очереди не превышает вместимость;
#   - прибыло = обслужено + отказ + таймаут + отмена; назначено = начали = закончили стоянку = освобождено;
#   - итоговая статистика совпадает с событиями журнала;
#   - к концу никто не «завис» в промежуточном состоянии.
# Совместимость типа места и машины гарантирует find_free_spot/pick_car, по журналу она не проверяется.

# число из первого совпадения с регулярным выражением re
function num(re,   s) {
    if (match($0, re)) { s = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", s); return s + 0 }
    return -1
}
function fail(msg) { print "  FAIL: " msg "  | " $0; errors++ }
# переход машины id в состояние to; from - строка допустимых текущих состояний
function move(id, from, to) {
    if (!(id in state) || index(from, state[id]) == 0) fail("авто #" id ": недопустимый переход " state[id] " -> " to)
    state[id] = to
}
function occupy(s) { if (busy[s]) fail("место #" s " назначено, хотя уже занято"); busy[s] = 1 }

/Прибытие: авто #/                         { id = num("авто #[0-9]+"); if (id in state) fail("повторное прибытие"); state[id] = "A"; arrived++ }
/Диспетчер: авто #[0-9]+ назначено место/  { move(num("авто #[0-9]+"), "A", "P"); occupy(num("место #[0-9]+")); served++ }
/Диспетчер: место #[0-9]+ назначено авто/  { move(num("авто #[0-9]+"), "Q", "P"); occupy(num("место #[0-9]+")); served++ }
/поставлено в очередь/ {
    move(num("авто #[0-9]+"), "A", "Q")
    match($0, /\([0-9]+\/[0-9]+\)/); split(substr($0, RSTART + 1, RLENGTH - 2), q, "/")
    if (q[1] + 0 > q[2] + 0) fail("размер очереди больше вместимости")
}
/очередь заполнена, авто/                  { move(num("авто #[0-9]+"), "A", "X"); rejected++ }
/Уход из очереди/                          { move(num("авто #[0-9]+"), "Q", "X"); timeouts++ }
/ожидание авто #[0-9]+ прервано/           { move(num("авто #[0-9]+"), "AQ", "X"); cancelled++ }
/не въезжает/                              { move(num("авто #[0-9]+"), "A", "X"); cancelled++ }
/Начало стоянки/                           { move(num("авто #[0-9]+"), "P", "S"); started++ }
/покидает место/                           { move(num("авто #[0-9]+"), "S", "L"); left++ }
/Освобождение места/                       { s = num("места #[0-9]+"); if (!busy[s]) fail("освобождено свободное место #" s); busy[s] = 0; freed++ }

/Обслужено автомобилей:/        { stat_served = num("[0-9]+$") }
/Ушло по таймауту из очереди:/  { stat_timeouts = num("[0-9]+$") }
/Отказано \(очередь заполнена\):/ { stat_rejected = num("[0-9]+$") }
/Ожидание прервано остановкой:/ { stat_cancelled = num("[0-9]+$"); has_stats = 1 }

END {
    $0 = ""
    if (!has_stats) fail("в журнале нет итоговой статистики")
    if (arrived != served + rejected + timeouts + cancelled)
        fail("прибыло " arrived " != обслужено " served " + отказ " rejected " + таймаут " timeouts " + отмена " cancelled)
    if (served != started || started != left || left != freed)
        fail("назначено " served ", начали стоянку " started ", уехали " left ", освобождено мест " freed)
    if (has_stats && (stat_served != served || stat_timeouts != timeouts || stat_rejected != rejected || stat_cancelled != cancelled))
        fail("итоговая статистика не совпадает с событиями журнала")
    for (id in state) if (state[id] != "L" && state[id] != "X") fail("авто #" id " осталось в состоянии " state[id])
    if (!errors) printf "прибыло %d, обслужено %d, таймаут %d, отказ %d, отмена %d\n", arrived, served, timeouts, rejected, cancelled
    exit errors > 0
}
