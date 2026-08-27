# Included by AzerothCore's module configuration to register Economy integration tests.

if(BUILD_TESTING)
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotCareerPlanTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotCareerPopulationTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotCareerProgressionTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyConsumptionTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyCoordinatorTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyGatheringTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyMarketTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotMaterialCommitmentAuthorityTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotMaterialCommitmentPersistenceTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyPurgeTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyScenarioTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyTravelPlanTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotEconomyTraceTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotGatheringActionTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotProfessionCapabilityTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyConsumption.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyCoordinator.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyGathering.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyMarket.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotMaterialCommitmentAuthority.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotMaterialCommitmentEncoding.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotMaterialCommitmentPersistence.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyPolicy.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyPurge.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyTrace.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotEconomyTravelPlan.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Economy/PlayerbotProfessionCapability.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Personality/PlayerbotCareerPlan.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Personality/PlayerbotCareerPopulation.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Personality/PlayerbotCareerSeeds.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Personality/PlayerbotCareerProgression.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_SOURCE_DIR}/modules/mod-playerbots-personality/src")
endif()
