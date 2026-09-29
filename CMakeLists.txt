cmake_minimum_required(VERSION 3.15)
project(ParkingSim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror")

include_directories(include)

add_executable(parking_sim src/main.c src/parking.c)
