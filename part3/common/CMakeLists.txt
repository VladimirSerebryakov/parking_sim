add_library(parking_common STATIC
    src/io.c
    src/params.c
    src/parking.c
    src/queue.c
    src/sim_clock.c
    src/strategy.c
    src/ui.c
    src/shared_memory.c
    src/dispatcher.c
    src/process_utils.c
    src/traffic.c
)
target_include_directories(parking_common PUBLIC include)

# Один протокол участников для двух вариантов с дочерними процессами.
add_library(parking_fork_common STATIC src/fork_processes.c src/fork_sync.c)
find_package(Threads REQUIRED)
target_link_libraries(parking_fork_common PUBLIC parking_common Threads::Threads rt)
