cmake_minimum_required(VERSION 3.10)
project(sim_receiver_tests C)

set(CMAKE_C_STANDARD 11)

# Mock headers override real ESP-IDF headers
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/unity
)

# Test executable
add_executable(run_tests
    test_main.c
    test_pdu_decoder.c
    test_sms_assembly.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/pdu_decoder.c
)

# Enable warnings
if(MSVC)
    target_compile_options(run_tests PRIVATE /W3)
else()
    target_compile_options(run_tests PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable)
endif()
