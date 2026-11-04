# Проверка событий, всех четырёх инвариантов и статистики третьей части.
# В заголовке журнала обязательны количества мест по типам и вместимость очереди.
# Состояния: A — прибыла, Q — очередь, P — назначено место, S — стоянка,
# L — уехала с места, X — отказ/таймаут/отмена.
BEGIN { names[1] = "Обычный"; names[2] = "Крупногабаритный"; names[3] = "Специальный" }
function num(re,   s) {
    if (match($0, re)) { s = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", s); return s + 0 }
    return -1
}
function fail(msg) { print "  FAIL: " msg "  | " $0; errors++ }
function move(id, from, to) {
    if (!(id in state) || index(from, state[id]) == 0)
        fail("авто #" id ": недопустимый переход " state[id] " -> " to)
    if (state[id] == "Q") waiting--
    if (to == "Q") waiting++
    state[id] = to
    if (waiting < 0 || waiting > capacity) fail("фактический размер очереди вне границ")
}
function occupy(s, id) {
    if (s < 1 || s > total_spots) fail("назначено несуществующее место #" s)
    if (busy[s]) fail("место #" s " уже занято")
    if (car_type[id] != spot_type[s]) fail("несовместимые типы автомобиля и места")
    busy[s] = id
    assigned[id] = s
    occupied++
    if (occupied > total_spots) fail("превышено число занятых мест")
}
/Параметры: места/ {
    match($0, /места [0-9]+\/[0-9]+\/[0-9]+/)
    config = substr($0, RSTART, RLENGTH); sub(/^места /, "", config)
    split(config, counts, "/")
    total_spots = 0
    for (t = 1; t <= 3; t++) for (i = 0; i < counts[t]; i++) spot_type[++total_spots] = t
    capacity = num("очередь [0-9]+")
    has_config = 1
}
/Прибытие: авто #/ {
    id = num("авто #[0-9]+")
    if (id in state) fail("повторное прибытие")
    state[id] = "A"; arrived++
    for (t = 1; t <= 3; t++) if (index($0, "(" names[t] ")")) car_type[id] = t
    if (!(id in car_type)) fail("неизвестный тип автомобиля")
}
/Диспетчер: авто #[0-9]+ назначено место/ {
    id = num("авто #[0-9]+")
    move(id, "A", "P"); occupy(num("место #[0-9]+"), id); served++
}
/Диспетчер: место #[0-9]+ назначено авто/ {
    id = num("авто #[0-9]+")
    move(id, "Q", "P"); occupy(num("место #[0-9]+"), id); served++
}
/поставлено в очередь/ {
    move(num("авто #[0-9]+"), "A", "Q")
    match($0, /\([0-9]+\/[0-9]+\)/); split(substr($0, RSTART + 1, RLENGTH - 2), q, "/")
    if (q[1] + 0 != waiting || q[2] + 0 != capacity) fail("неверные размер или вместимость очереди")
}
/очередь заполнена, авто/ { move(num("авто #[0-9]+"), "A", "X"); rejected++ }
/Уход из очереди/ { move(num("авто #[0-9]+"), "Q", "X"); timeouts++ }
/ожидание авто #[0-9]+ прервано/ { move(num("авто #[0-9]+"), "AQ", "X"); cancelled++ }
/не въезжает/ { move(num("авто #[0-9]+"), "A", "X"); cancelled++ }
/Начало стоянки/ {
    id = num("авто #[0-9]+")
    move(id, "P", "S")
    if (assigned[id] != num("месте #[0-9]+")) fail("стоянка на другом месте")
    started++
}
/покидает место/ {
    id = num("авто #[0-9]+")
    move(id, "S", "L")
    if (assigned[id] != num("место #[0-9]+")) fail("отъезд с другого места")
    left++
}
/Освобождение места/ {
    s = num("места #[0-9]+")
    if (!busy[s] || state[busy[s]] != "L") fail("освобождение места без отъезда его автомобиля")
    busy[s] = 0; occupied--; freed++
}
/Обслужено автомобилей:/ { stat_served = num("[0-9]+$") }
/Ушло по таймауту из очереди:/ { stat_timeouts = num("[0-9]+$") }
/Отказано \(очередь заполнена\):/ { stat_rejected = num("[0-9]+$") }
/Ожидание прервано остановкой:/ { stat_cancelled = num("[0-9]+$"); has_stats = 1 }
END {
    $0 = ""
    if (!has_config) fail("нет параметров парковки в журнале")
    if (!has_stats) fail("нет итоговой статистики")
    if (arrived != served + rejected + timeouts + cancelled) fail("нарушен баланс прибывших автомобилей")
    if (served != started || started != left || left != freed) fail("нарушен порядок стоянки и освобождения мест")
    if (has_stats && (stat_served != served || stat_timeouts != timeouts ||
        stat_rejected != rejected || stat_cancelled != cancelled)) fail("статистика не совпадает с событиями")
    for (id in state) if (state[id] != "L" && state[id] != "X") fail("авто #" id " не завершено")
    if (waiting != 0 || occupied != 0) fail("парковка или очередь не пусты в конце")
    for (s in busy) if (busy[s]) fail("место #" s " осталось занято")
    if (!errors) printf "прибыло %d, обслужено %d, таймаут %d, отказ %d, отмена %d\n", arrived, served, timeouts, rejected, cancelled
    exit errors > 0
}
