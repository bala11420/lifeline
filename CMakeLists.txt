cmake_minimum_required(VERSION 3.16)
project(lifeline CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(LIFELINE_SANITIZE "Build with AddressSanitizer and UBSan" ON)
if(LIFELINE_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(lifeline src/codec.cpp src/journal.cpp src/node.cpp src/messages.cpp src/actuator.cpp)
target_include_directories(lifeline PUBLIC include)
if(NOT MSVC)
  target_compile_options(lifeline PUBLIC -Wall -Wextra)
endif()

add_library(lifeline_sim_lib sim/sim.cpp sim/engines.cpp sim/scenario.cpp)
target_include_directories(lifeline_sim_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lifeline_sim_lib PUBLIC lifeline)

add_executable(lifeline_sim sim/main.cpp)
target_link_libraries(lifeline_sim PRIVATE lifeline_sim_lib)

add_executable(tests
  tests/main.cpp tests/test_codec.cpp tests/test_journal.cpp
  tests/test_dedup.cpp tests/test_node.cpp tests/test_actuator.cpp tests/test_sim.cpp)
target_include_directories(tests PRIVATE third_party tests)
target_link_libraries(tests PRIVATE lifeline_sim_lib)

enable_testing()
add_test(NAME unit COMMAND tests)
