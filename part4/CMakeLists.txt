# Сетевой вариант использует предметные модули прежней части без изменения её целей.
add_library(parking_tcp_common STATIC
    src/net.c src/client.c src/car_client.c src/model.c src/session.c
    ../part3/common/src/io.c ../part3/common/src/params.c
    ../part3/common/src/queue.c ../part3/common/src/strategy.c
    ../part3/common/src/dispatcher.c ../part3/common/src/traffic.c
    ../part3/common/src/sim_clock.c ../part3/common/src/ui.c
    ../part3/common/src/process_utils.c)
target_include_directories(parking_tcp_common PUBLIC include ../part3/common/include)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/part4")
foreach(role server car arrivals monitor)
    add_executable(parking_tcp_${role} src/${role}.c)
    set_target_properties(parking_tcp_${role} PROPERTIES OUTPUT_NAME parking_${role})
    target_link_libraries(parking_tcp_${role} PRIVATE parking_tcp_common)
endforeach()
add_dependencies(parking_tcp_arrivals parking_tcp_car)
add_custom_target(parking_tcp DEPENDS parking_tcp_server parking_tcp_car parking_tcp_arrivals parking_tcp_monitor)

add_executable(parking_tcp_check_transport EXCLUDE_FROM_ALL tests/check_transport.c)
target_link_libraries(parking_tcp_check_transport PRIVATE parking_tcp_common)
target_link_options(parking_tcp_check_transport PRIVATE -Wl,--wrap=send,--wrap=recv)
add_executable(parking_tcp_check_model EXCLUDE_FROM_ALL tests/check_model.c)
target_link_libraries(parking_tcp_check_model PRIVATE parking_tcp_common)
add_executable(parking_tcp_check_sessions EXCLUDE_FROM_ALL tests/check_sessions.c)
target_link_libraries(parking_tcp_check_sessions PRIVATE parking_tcp_common)
add_executable(parking_tcp_check_client EXCLUDE_FROM_ALL tests/check_client.c)
target_link_libraries(parking_tcp_check_client PRIVATE parking_tcp_common)
target_link_options(parking_tcp_check_client PRIVATE -Wl,--wrap=send,--wrap=poll)
foreach(check transport model sessions client)
    # Проверки и действия в assert должны выполняться и при CMAKE_BUILD_TYPE=Release.
    target_compile_options(parking_tcp_check_${check} PRIVATE -UNDEBUG)
endforeach()
add_custom_target(parking_tcp_checks DEPENDS parking_tcp_check_transport parking_tcp_check_model parking_tcp_check_sessions parking_tcp_check_client)
