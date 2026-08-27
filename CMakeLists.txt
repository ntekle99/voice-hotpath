cmake_minimum_required(VERSION 3.20)
project(voice_hotpath LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# -O2, not -O3: -O3's extra inlining and vectorisation buys nothing measurable
# here and costs I-cache footprint, which does show up in the tail.
# -march=native is opt-in because a binary built with it will SIGILL on an older
# host, and the same portfolio binary tends to get run on more than one machine.
option(VHP_NATIVE_ARCH "Build with -march=native" OFF)
option(VHP_TSAN "Also build ThreadSanitizer copies of the tests" ON)

add_library(vhp INTERFACE)
target_include_directories(vhp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(vhp INTERFACE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion)
if(VHP_NATIVE_ARCH)
  target_compile_options(vhp INTERFACE -march=native)
endif()

find_package(Threads REQUIRED)
target_link_libraries(vhp INTERFACE Threads::Threads)
# librt is folded into glibc >= 2.34; link it only where it exists.
find_library(RT_LIBRARY rt)
if(RT_LIBRARY)
  target_link_libraries(vhp INTERFACE ${RT_LIBRARY})
endif()

add_executable(vhp_consumer src/vhp_consumer.cpp)
target_link_libraries(vhp_consumer PRIVATE vhp)

add_executable(vhp_producer src/vhp_producer.cpp)
target_link_libraries(vhp_producer PRIVATE vhp)

add_executable(bench_ring bench/bench_ring.cpp)
target_link_libraries(bench_ring PRIVATE vhp)

enable_testing()

set(VHP_TESTS test_ring test_shm test_chunker test_histogram test_vad_parity test_session)
foreach(test_name IN LISTS VHP_TESTS)
  add_executable(${test_name} test/${test_name}.cpp)
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(${test_name} PRIVATE vhp)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# The ring's correctness rests on acquire/release pairs. x86-64 gives those for
# free and will happily execute an incorrectly-ordered ring without complaint;
# the bug only appears on a weakly-ordered CPU. TSan models the C++ memory model
# rather than the hardware, so it catches what the hardware forgives. Worth the
# ~10x slowdown on the concurrent tests.
if(VHP_TSAN)
  foreach(test_name test_ring test_shm)
    add_executable(${test_name}_tsan test/${test_name}.cpp)
    target_include_directories(${test_name}_tsan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    target_link_libraries(${test_name}_tsan PRIVATE vhp)
    target_compile_options(${test_name}_tsan PRIVATE -fsanitize=thread -O1 -g)
    target_link_options(${test_name}_tsan PRIVATE -fsanitize=thread)
    # setarch -R disables ASLR. Without it TSan aborts with "unexpected memory
    # mapping" on kernels that randomise into ranges its shadow map claims.
    add_test(NAME ${test_name}_tsan
             COMMAND setarch ${CMAKE_HOST_SYSTEM_PROCESSOR} -R $<TARGET_FILE:${test_name}_tsan>)
    set_tests_properties(${test_name}_tsan PROPERTIES
                         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} TIMEOUT 300)
  endforeach()
endif()

# UBSan/ASan pass over the single-threaded logic. Not combined with TSan: ASan
# and TSan are mutually exclusive.
foreach(test_name test_chunker test_histogram test_vad_parity test_session)
  add_executable(${test_name}_asan test/${test_name}.cpp)
  target_include_directories(${test_name}_asan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(${test_name}_asan PRIVATE vhp)
  target_compile_options(${test_name}_asan PRIVATE -fsanitize=address,undefined -O1 -g)
  target_link_options(${test_name}_asan PRIVATE -fsanitize=address,undefined)
  add_test(NAME ${test_name}_asan COMMAND ${test_name}_asan)
  set_tests_properties(${test_name}_asan PROPERTIES
                       WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
