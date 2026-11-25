# Родитель и дочерние процессы: POSIX-разделяемая память и именованные семафоры.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/fork_named")
add_executable(parking_fork_named src/main.c src/named_resources.c)
target_include_directories(parking_fork_named PRIVATE include)
target_link_libraries(parking_fork_named PRIVATE parking_fork_common)
