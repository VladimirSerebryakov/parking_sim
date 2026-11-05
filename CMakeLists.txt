cmake_minimum_required(VERSION 3.15)
project(ParkingSim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

# Общие настройки сборки всех реализаций.
set(SANITIZE "" CACHE STRING "Санитайзер: thread, address или address,undefined (пусто - без него)")
if(SANITIZE)
    add_compile_options(-fsanitize=${SANITIZE} -g)
    add_link_options(-fsanitize=${SANITIZE})
endif()

add_subdirectory(part2)
add_subdirectory(part3)
