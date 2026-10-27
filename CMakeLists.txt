cmake_minimum_required(VERSION 3.16)
project(settle CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SETTLE_WARNINGS -Wall -Wextra -Wshadow)

# Debug builds run everything (library, tests, apps) under AddressSanitizer + UndefinedBehaviorSanitizer.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# --- modules -------------------------------------------------------------------
# One static library per module. DEPS is the layering: a module may only use the modules it lists.
#
#   core      types, instance, io, validator, net_balance, rng      (depends on nothing)
#   gen       instance generators                                   -> core
#   netting   min-transfer netting solvers                          -> core
#   flow      max flow / min-cost flow                              -> core
#   gridlock  gridlock resolution solvers                           -> core, flow
#   stream    online engine (Phase 5)                               -> core, netting, gridlock
#
# Headers live in includes/<module>/ and are included by module path: #include "netting/greedy.hpp".
function(settle_module name)
  cmake_parse_arguments(M "" "" "SOURCES;DEPS" ${ARGN})
  add_library(settle_${name} STATIC ${M_SOURCES})
  target_include_directories(settle_${name} PUBLIC ${PROJECT_SOURCE_DIR}/includes)
  target_compile_options(settle_${name} PRIVATE ${SETTLE_WARNINGS})
  target_link_libraries(settle_${name} PUBLIC ${M_DEPS})
endfunction()

settle_module(core
  SOURCES src/core/io.cpp src/core/validator.cpp src/core/net_balance.cpp src/core/components.cpp src/core/day.cpp)

settle_module(gen
  SOURCES src/gen/generators.cpp src/gen/day_gen.cpp
  DEPS    settle_core)

settle_module(netting
  SOURCES src/netting/naive.cpp src/netting/greedy.cpp src/netting/subset_dp.cpp
          src/netting/group_plan.cpp src/netting/match.cpp src/netting/mitm.cpp
  DEPS    settle_core)

settle_module(gridlock
  SOURCES src/gridlock/fifo_bypass.cpp src/gridlock/drop_violators.cpp src/gridlock/branch_bound.cpp
          src/gridlock/maxflow_relax.cpp src/gridlock/lp_bound.cpp
  DEPS    settle_core settle_flow)

settle_module(flow
  SOURCES src/flow/dinic.cpp src/flow/mcmf.cpp
  DEPS    settle_core)

settle_module(routing
  SOURCES src/routing/router.cpp
  DEPS    settle_core settle_flow)

settle_module(stream
  SOURCES src/stream/engine.cpp src/stream/replay.cpp src/stream/sim_runner.cpp
  DEPS    settle_core settle_gridlock)

# everything at once, for tests and tools
add_library(settle INTERFACE)
target_link_libraries(settle INTERFACE settle_core settle_gen settle_netting settle_gridlock
                      settle_flow settle_routing settle_stream)

# --- apps ----------------------------------------------------------------------
add_executable(gen apps/gen_main.cpp)
target_link_libraries(gen PRIVATE settle_gen settle_core)
target_compile_options(gen PRIVATE ${SETTLE_WARNINGS})

add_executable(gen_day apps/gen_day.cpp)
target_link_libraries(gen_day PRIVATE settle_gen settle_core)
target_compile_options(gen_day PRIVATE ${SETTLE_WARNINGS})

add_executable(simulate apps/simulate.cpp)
target_link_libraries(simulate PRIVATE settle_stream settle_gen settle_core)
target_compile_options(simulate PRIVATE ${SETTLE_WARNINGS})

# --- tests ---------------------------------------------------------------------
enable_testing()
set(CMAKE_CTEST_ARGUMENTS "--output-on-failure")

function(add_unit_test path)
  get_filename_component(name ${path} NAME_WE)
  add_executable(${name} tests/unit/${path}.cpp)
  target_link_libraries(${name} PRIVATE settle)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_compile_options(${name} PRIVATE ${SETTLE_WARNINGS})
  add_test(NAME ${name} COMMAND ${name})
  set_property(GLOBAL APPEND PROPERTY SETTLE_UNIT_TESTS ${name})
endfunction()

add_unit_test(core/test_task1)
add_unit_test(core/test_task2)
add_unit_test(gen/test_task3)
add_unit_test(netting/test_task4)
add_unit_test(netting/test_task5)
add_unit_test(netting/test_task6)
add_unit_test(netting/test_task7)
add_unit_test(gridlock/test_task8)
add_unit_test(gridlock/test_task9)
add_unit_test(gridlock/test_task10)
add_unit_test(flow/test_task11_dinic)
add_unit_test(gridlock/test_task11_relax)
add_unit_test(flow/test_task12)
add_unit_test(gridlock/test_task13)
add_unit_test(routing/test_task14)
add_unit_test(stream/test_task15)
add_unit_test(gen/test_task16)
add_unit_test(stream/test_task17)

# the demo apps, end to end: generate a small day, then simulate it under two policies
add_test(NAME app_gen_day COMMAND gen_day --config ${PROJECT_SOURCE_DIR}/configs/demo_small.cfg --out demo_day.csv)
add_test(NAME app_simulate COMMAND simulate --day demo_day.csv
        --sim ${PROJECT_SOURCE_DIR}/configs/sim/recycle_only.cfg --sim ${PROJECT_SOURCE_DIR}/configs/sim/relax_300.cfg)
set_tests_properties(app_simulate PROPERTIES DEPENDS app_gen_day)


# --- make targets ----------------------------------------------------------------
#   make         build libraries, apps and tests (default target)
#   make test    run already-built tests via ctest (does not rebuild)
#   make unit    rebuild the tests, then run them
#   make clean   remove build outputs
get_property(SETTLE_UNIT_TESTS GLOBAL PROPERTY SETTLE_UNIT_TESTS)
add_custom_target(unit
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  DEPENDS ${SETTLE_UNIT_TESTS}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  USES_TERMINAL
  COMMENT "Building and running unit tests")
