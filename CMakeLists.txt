cmake_minimum_required(VERSION 3.15)
project(ParkingSim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror")

# Сборка с санитайзером для тестов: cmake -DSANITIZE=thread .. (или address)
set(SANITIZE "" CACHE STRING "Санитайзер: thread или address (пусто - без него)")
if(SANITIZE)
    add_compile_options(-fsanitize=${SANITIZE} -g)
    add_link_options(-fsanitize=${SANITIZE})
endif()

include_directories(include)

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

find_package(Threads REQUIRED)
target_link_libraries(parking_sim Threads::Threads)
