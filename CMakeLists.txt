cmake_minimum_required(VERSION 3.20)
project(reliable_udp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(RUDP_WERROR "Treat warnings as errors" ON)
option(RUDP_ASAN   "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(RUDP_TSAN   "Build with ThreadSanitizer" OFF)
option(RUDP_NATIVE "Build with -march=native (off by default: hurts reproducibility)" OFF)

add_library(rudp_flags INTERFACE)
target_compile_options(rudp_flags INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
  -Woverloaded-virtual -Wconversion -Wsign-compare -Wdouble-promotion
  -Wformat=2 -Wimplicit-fallthrough -Wnull-dereference -Wextra-semi
)
if(RUDP_WERROR)
  target_compile_options(rudp_flags INTERFACE -Werror)
endif()
if(RUDP_NATIVE)
  target_compile_options(rudp_flags INTERFACE -march=native)
endif()

if(RUDP_ASAN AND RUDP_TSAN)
  message(FATAL_ERROR "ASan and TSan cannot be combined")
endif()
if(RUDP_ASAN)
  target_compile_options(rudp_flags INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
  target_link_options(rudp_flags INTERFACE -fsanitize=address,undefined)
endif()
if(RUDP_TSAN)
  target_compile_options(rudp_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(rudp_flags INTERFACE -fsanitize=thread)
endif()

# ---------------------------------------------------------------- production
add_library(reliable_udp STATIC
  src/packet.cpp
  src/sender.cpp
  src/receiver.cpp
  src/sha256.cpp
  src/net/udp_socket.cpp
  src/net/session_runner.cpp
  src/net/server.cpp
)
target_include_directories(reliable_udp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(reliable_udp PUBLIC rudp_flags)
find_package(Threads REQUIRED)
target_link_libraries(reliable_udp PUBLIC Threads::Threads)

# --------------------------------------------------- test / benchmark support
# Kept in a separate target so no simulator code can leak into the production
# library or into the shipped examples.
add_library(rudp_sim STATIC
  src/sim/net_sim.cpp
  src/sim/sim_harness.cpp
)
target_link_libraries(rudp_sim PUBLIC reliable_udp)

# ------------------------------------------------------------------ examples
add_executable(ru_send examples/ru_send.cpp)
target_link_libraries(ru_send PRIVATE reliable_udp)
add_executable(ru_recv examples/ru_recv.cpp)
target_link_libraries(ru_recv PRIVATE reliable_udp)
add_executable(ru_server examples/ru_server.cpp)
target_link_libraries(ru_server PRIVATE reliable_udp)

# --------------------------------------------------------------------- tests
enable_testing()
add_executable(rudp_tests
  tests/test_main.cpp
  tests/test_packet.cpp
  tests/test_seq_rtt.cpp
  tests/test_sim.cpp
  tests/test_transfer.cpp
  tests/test_corruption.cpp
  tests/test_refmodel.cpp
  tests/ref_model.cpp
  tests/test_alloc.cpp
  tests/test_realnet.cpp
)
target_link_libraries(rudp_tests PRIVATE rudp_sim)
target_include_directories(rudp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

set(RUDP_TEST_GROUPS packet seq rtt sim transfer corrupt refmodel alloc realnet)
foreach(g ${RUDP_TEST_GROUPS})
  add_test(NAME group.${g} COMMAND rudp_tests --filter ${g})
  set_tests_properties(group.${g} PROPERTIES TIMEOUT 1800)
endforeach()

# ---------------------------------------------------------------- benchmarks
# Benchmarks embed the exact build configuration so every reported number
# carries its own provenance.
function(rudp_add_benchmark name)
  add_executable(${name} benchmarks/${name}.cpp benchmarks/bench_common.cpp)
  target_link_libraries(${name} PRIVATE rudp_sim)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
  target_compile_definitions(${name} PRIVATE
    RUDP_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    RUDP_CXX_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}}"
  )
endfunction()

rudp_add_benchmark(bench_reliability)
rudp_add_benchmark(bench_window)
rudp_add_benchmark(bench_throughput)
rudp_add_benchmark(bench_concurrency)
