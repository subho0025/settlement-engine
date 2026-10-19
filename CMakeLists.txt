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
enable_testing()
set(CMAKE_CTEST_ARGUMENTS "--output-on-failure")

set(UNIT_TESTS)
function(add_unit_test name)
  add_executable(${name} tests/unit/${name}.cpp)
  target_link_libraries(${name} PRIVATE settle)
  target_include_directories(${name} PRIVATE tests/brute)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wshadow)
  add_test(NAME ${name} COMMAND ${name})
  set(UNIT_TESTS ${UNIT_TESTS} ${name} PARENT_SCOPE)
endfunction()

add_unit_test(test_task1)
add_unit_test(test_task2)
add_unit_test(test_task3)
add_unit_test(test_task4)
add_unit_test(test_task5)
add_unit_test(test_task6)
add_unit_test(test_task7)
add_unit_test(test_task8)
add_unit_test(test_task9)
add_unit_test(test_task10)

add_executable(gen gen/gen_main.cpp)
target_link_libraries(gen PRIVATE settle)
target_compile_options(gen PRIVATE -Wall -Wextra -Wshadow)

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