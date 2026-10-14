cmake_minimum_required(VERSION 3.16)
project(settle CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(settle STATIC
  src/io.cpp
  src/net_balance.cpp
  src/validator.cpp
  src/generators.cpp
  src/naive.cpp
  src/greedy.cpp
  src/subset_dp.cpp
  src/group_plan.cpp
  src/match.cpp
  src/mitm.cpp
  src/fifo_bypass.cpp
  src/drop_violators.cpp
  src/branch_bound.cpp
)
target_include_directories(settle PUBLIC includes)
target_compile_options(settle PRIVATE -Wall -Wextra -Wshadow)

# Debug builds run under AddressSanitizer + UndefinedBehaviorSanitizer.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(settle PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(settle PUBLIC -fsanitize=address,undefined)
endif()

# --- tests -----------------------------------------------------------------
set(UNIT_TESTS test_task1 test_task2 test_task3 test_task4 test_task5 test_task6 test_task7 test_task8 test_task9 test_task10)

set(CMAKE_CTEST_ARGUMENTS "--output-on-failure")
enable_testing()

add_executable(test_task1 tests/unit/test_task1.cpp)
target_link_libraries(test_task1 PRIVATE settle)
target_compile_options(test_task1 PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME task1 COMMAND test_task1)

add_executable(test_task2 tests/unit/test_task2.cpp)
target_link_libraries(test_task2 PRIVATE settle)
target_compile_options(test_task2 PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME task2 COMMAND test_task2)

add_executable(test_task3 tests/unit/test_task3.cpp)
target_link_libraries(test_task3 PRIVATE settle)
target_compile_options(test_task3 PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME task3 COMMAND test_task3)

add_executable(test_task4 tests/unit/test_task4.cpp)
target_link_libraries(test_task4 PRIVATE settle)
target_compile_options(test_task4 PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME task4 COMMAND test_task4)

add_executable(gen gen/gen_main.cpp)
target_link_libraries(gen PRIVATE settle)
target_compile_options(gen PRIVATE -Wall -Wextra -Wshadow)

add_executable(test_task5 tests/unit/test_task5.cpp)
target_link_libraries(test_task5 PRIVATE settle)
target_include_directories(test_task5 PRIVATE tests/brute)
target_compile_options(test_task5 PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME task5 COMMAND test_task5)

add_executable(test_task6 tests/unit/test_task6.cpp)
target_link_libraries(test_task6 PRIVATE settle)
target_compile_options(test_task6 PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME task6 COMMAND test_task6)

add_executable(test_task7 tests/unit/test_task7.cpp)
target_link_libraries(test_task7 PRIVATE settle)
target_compile_options(test_task7 PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME task7 COMMAND test_task7)

add_executable(test_task8 tests/unit/test_task8.cpp)
target_link_libraries(test_task8 PRIVATE settle)
target_include_directories(test_task8 PRIVATE tests/brute)
target_compile_options(test_task8 PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME task8 COMMAND test_task8)

add_executable(test_task9 tests/unit/test_task9.cpp)
target_link_libraries(test_task9 PRIVATE settle)
target_include_directories(test_task9 PRIVATE tests/brute)
target_compile_options(test_task9 PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME task9 COMMAND test_task9)

add_executable(test_task10 tests/unit/test_task10.cpp)
target_link_libraries(test_task10 PRIVATE settle)
target_include_directories(test_task10 PRIVATE tests/brute)
target_compile_options(test_task10 PRIVATE -Wall -Wextra -Wshadow)
add_test(NAME task10 COMMAND test_task10)

# --- make targets ------------------------------------------------------------
#   make         build library + all tests (default target)
#   make test    run already-built tests via ctest (built-in target, does not rebuild)
#   make unit    rebuild the tests, then run them
#   make clean   built-in: remove build outputs
add_custom_target(unit
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  DEPENDS ${UNIT_TESTS}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  USES_TERMINAL
  COMMENT "Building and running unit tests")