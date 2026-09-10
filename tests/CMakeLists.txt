# Catch2 tests. Names follow the "<area>: <behavior>" convention enforced by
# scripts/check-docs.sh (docs/testing.md).

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

add_executable(obd_unit_tests
    unit/test_common.cpp
    unit/test_format.cpp
    unit/test_trace.cpp
    unit/test_trace_replay.cpp
    unit/test_device_control.cpp
    unit/test_trace_record.cpp
    unit/test_structural_warmup.cpp
    unit/test_source.cpp
    unit/test_admission.cpp
    unit/test_layer_store.cpp
    unit/test_registry.cpp
    unit/test_image.cpp
    unit/test_writable.cpp
    unit/test_cli.cpp
    unit/test_supervisor.cpp)
target_include_directories(obd_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(obd_unit_tests PRIVATE obd_core Catch2::Catch2WithMain)
# The CLI test EXECUTES the real obdctl binary (argv → wire translation),
# so the test target depends on it and is told its path.
target_compile_definitions(obd_unit_tests
    PRIVATE OBD_TEST_OBDCTL_BIN="$<TARGET_FILE:obdctl>")
add_dependencies(obd_unit_tests obdctl)
catch_discover_tests(obd_unit_tests)

if(OBD_ENABLE_UBLK)
    target_sources(obd_unit_tests PRIVATE unit/test_ublk.cpp)
    target_link_libraries(obd_unit_tests PRIVATE obd_ublk)
    # obd-device is built only with the ublk backend; the flag-validation
    # test executes the real binary.
    target_compile_definitions(obd_unit_tests
        PRIVATE OBD_TEST_DEVICE_BIN="$<TARGET_FILE:obd-device>")
    add_dependencies(obd_unit_tests obd-device)
endif()

# Test-only fake obd-device for the commit integration test (L6: it
# checkpoints its LSMT-RW upper only on SIGTERM, like the real device).
add_executable(obd-test-fake-device fake_device_main.cpp)
target_include_directories(obd-test-fake-device
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(obd-test-fake-device PRIVATE obd_core)
# The unit tests execute the fake binary directly for the CLI-parity
# checks (its flags must reject exactly what obd-device rejects).
target_compile_definitions(obd_unit_tests
    PRIVATE OBD_TEST_FAKE_DEVICE_BIN="$<TARGET_FILE:obd-test-fake-device>")
add_dependencies(obd_unit_tests obd-test-fake-device)

add_executable(obd_integration_tests
    integration/test_integration.cpp
    integration/test_recovery.cpp
    integration/test_commit.cpp
    integration/test_trace_record.cpp
    integration/test_hello.cpp
    integration/test_resize.cpp)
target_include_directories(obd_integration_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(obd_integration_tests
    PRIVATE obd_core Catch2::Catch2WithMain)
target_compile_definitions(obd_integration_tests
    PRIVATE OBD_TEST_FAKE_DEVICE_BIN="$<TARGET_FILE:obd-test-fake-device>")
add_dependencies(obd_integration_tests obd-test-fake-device)
if(OBD_ENABLE_UBLK)
    target_sources(obd_integration_tests PRIVATE integration/test_ublk_e2e.cpp)
    target_link_libraries(obd_integration_tests PRIVATE obd_ublk)
endif()
catch_discover_tests(obd_integration_tests
    PROPERTIES SKIP_RETURN_CODE 4)  # Catch2 "all selected tests skipped"
