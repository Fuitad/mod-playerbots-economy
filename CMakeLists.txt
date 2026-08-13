if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(mod_playerbots_economy LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  include(CTest)

  add_executable(playerbots_economy_tests
    tests/cpp/PlayerbotEconomyConfigTest.cpp
    src/Bot/Economy/PlayerbotEconomyTelemetry.cpp)
  target_include_directories(playerbots_economy_tests PRIVATE src)
  target_compile_options(playerbots_economy_tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

  add_test(NAME playerbots_economy_tests
    COMMAND playerbots_economy_tests)
else()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/Bot/PlayerbotAI.h")
    message(FATAL_ERROR "mod-playerbots-economy requires mod-playerbots")
  endif()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots-personality/src/Bot/Personality/PlayerbotPersonality.h")
    message(FATAL_ERROR "mod-playerbots-economy requires mod-playerbots-personality")
  endif()

  AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src")
  include("${CMAKE_CURRENT_LIST_DIR}/mod-playerbots-economy.cmake")
endif()
