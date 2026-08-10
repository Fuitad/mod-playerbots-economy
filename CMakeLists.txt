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
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbot-personality/src/Bot/Personality/PlayerbotPersonality.h")
    message(FATAL_ERROR "mod-playerbots-economy requires mod-playerbot-personality")
  endif()

  AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src")

  if(BUILD_TESTING)
    set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotCareerPlanTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyConsumptionTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyCoordinatorTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyGatheringTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyMarketTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyPolicyTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyScenarioTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyTraceTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotGatheringActionTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotProfessionCapabilityTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyConsumption.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyCoordinator.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyGathering.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyMarket.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyPolicy.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyTrace.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotProfessionCapability.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Personality/PlayerbotCareerPlan.cpp")
    set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
      "${CMAKE_CURRENT_LIST_DIR}/src"
      "${CMAKE_SOURCE_DIR}/modules/mod-playerbot-personality/src")
  endif()
endif()
