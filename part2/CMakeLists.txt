add_executable(parking_sim
    src/main.c
    src/params.c
    src/queue.c
    src/logger.c
    src/parking.c
    src/ui.c
    src/sim_clock.c
    src/threads.c
    src/strategy.c
)

target_include_directories(parking_sim PRIVATE include)
set_target_properties(parking_sim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/part2"
)

find_package(Threads REQUIRED)
target_link_libraries(parking_sim PRIVATE Threads::Threads)
