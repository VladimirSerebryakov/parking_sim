# Родитель и дочерние процессы: POSIX-разделяемая память и неименованные семафоры.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/fork_unnamed")
add_executable(parking_fork_unnamed
    src/main.c
    src/unnamed_resources.c
)
target_include_directories(parking_fork_unnamed PRIVATE include)
target_link_libraries(parking_fork_unnamed PRIVATE parking_fork_common)
