cmake_minimum_required(VERSION 3.14)
project(S7Forge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(S7FORGE_STATIC "Link the executable statically where the platform allows it" OFF)
if(MSVC AND S7FORGE_STATIC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(s7forgecore STATIC
    src/json.cpp
    src/value.cpp
    src/memory.cpp
    src/sim.cpp
    src/stl.cpp
    src/absolute.cpp
    src/scenario.cpp)
target_include_directories(s7forgecore PUBLIC src)

add_executable(s7forge src/main.cpp)
target_link_libraries(s7forge PRIVATE s7forgecore)
target_compile_definitions(s7forge PRIVATE S7FORGE_VERSION="${PROJECT_VERSION}")
if(S7FORGE_STATIC AND NOT MSVC AND NOT APPLE)
    target_link_options(s7forge PRIVATE -static)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(s7forge_tests tests/tests.cpp)
    target_link_libraries(s7forge_tests PRIVATE s7forgecore)
    add_test(NAME unit COMMAND s7forge_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    file(GLOB scenarios ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/*.json)
    foreach(s ${scenarios})
        get_filename_component(n ${s} NAME_WE)
        add_test(NAME scenario_${n} COMMAND s7forge test ${s})
    endforeach()
    # small programs from a review of the semantics: overflows, timers, paths
    file(GLOB reviewed ${CMAKE_CURRENT_SOURCE_DIR}/tests/review/*.json)
    foreach(s ${reviewed})
        get_filename_component(n ${s} NAME_WE)
        add_test(NAME review_${n} COMMAND s7forge test ${s})
    endforeach()
    # scenarios that must fail: a wrong value, an operand that is not there,
    # an index or a bit that is not one
    foreach(n wrong_value no_operand idx_garbage slice70)
        add_test(NAME failing_${n} COMMAND s7forge test ${CMAKE_CURRENT_SOURCE_DIR}/tests/failing/${n}.json)
        set_tests_properties(failing_${n} PROPERTIES WILL_FAIL TRUE)
    endforeach()
    # the command line: what it says on a bad call
    add_test(NAME cli_no_args COMMAND s7forge)
    set_tests_properties(cli_no_args PROPERTIES WILL_FAIL TRUE)
    add_test(NAME cli_info COMMAND s7forge info ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/MOTORPLC.json)
    set_tests_properties(cli_info PROPERTIES PASS_REGULAR_EXPRESSION "program cycle: Main")
    add_test(NAME cli_run COMMAND s7forge run ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/MOTORPLC.json
             --set StopPB=TRUE --set Guard=TRUE --set StartPB=TRUE --for 6000 --show MotorSclOut --show "\"MotorData\".starts")
    set_tests_properties(cli_run PROPERTIES PASS_REGULAR_EXPRESSION "MotorSclOut = TRUE\n\"MotorData\".starts = 2")
    add_test(NAME cli_bad_program COMMAND s7forge info ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/s15_motor.json)
    set_tests_properties(cli_bad_program PROPERTIES WILL_FAIL TRUE)
endif()

install(TARGETS s7forge RUNTIME DESTINATION bin)
