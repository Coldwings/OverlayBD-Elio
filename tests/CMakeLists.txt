# Catch2 tests. Names follow the "<area>: <behavior>" convention enforced by
# scripts/check-docs.sh (docs/testing.md).

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

add_executable(obd_unit_tests
    unit/test_common.cpp
    unit/test_format.cpp
    unit/test_source.cpp
    unit/test_layer_store.cpp
    unit/test_registry.cpp
    unit/test_image.cpp
    unit/test_writable.cpp
    unit/test_supervisor.cpp)
target_include_directories(obd_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(obd_unit_tests PRIVATE obd_core Catch2::Catch2WithMain)
catch_discover_tests(obd_unit_tests)

if(OBD_ENABLE_UBLK)
    target_sources(obd_unit_tests PRIVATE unit/test_ublk.cpp)
    target_link_libraries(obd_unit_tests PRIVATE obd_ublk)
endif()

add_executable(obd_integration_tests
    integration/test_integration.cpp
    integration/test_recovery.cpp)
target_include_directories(obd_integration_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(obd_integration_tests
    PRIVATE obd_core Catch2::Catch2WithMain)
if(OBD_ENABLE_UBLK)
    target_sources(obd_integration_tests PRIVATE integration/test_ublk_e2e.cpp)
    target_link_libraries(obd_integration_tests PRIVATE obd_ublk)
endif()
catch_discover_tests(obd_integration_tests
    PROPERTIES SKIP_RETURN_CODE 4)  # Catch2 "all selected tests skipped"
