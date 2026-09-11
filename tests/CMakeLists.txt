# Catch2 tests. Names follow the "<area>: <behavior>" convention enforced by
# scripts/check-docs.sh (docs/testing.md).
#
# CTest timeout guardrails (issue #11): a hung test must fail with a bounded
# timeout, never run unbounded. Each discovered Catch2 TEST_CASE and each
# auxiliary CTest entry carries a TIMEOUT property below, and the
# ctest-timeout-coverage audit fails CI if a future entry omits one.

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

# Required when tests are enabled: never silently skip the source safety guard.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(NAME test-await-assertions
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/scripts/check_test_awaits.py
        ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test-await-assertions-selftest
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_check_test_awaits.py)
add_test(NAME ctest-timeout-coverage-selftest
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_check_ctest_timeouts.py)
set_tests_properties(
    test-await-assertions
    test-await-assertions-selftest
    ctest-timeout-coverage-selftest
    PROPERTIES TIMEOUT 30)

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
    unit/test_convert.cpp
    unit/test_supervisor.cpp)
obd_target_warnings(obd_unit_tests)
target_include_directories(obd_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(obd_unit_tests PRIVATE obd_core Catch2::Catch2WithMain)
# The CLI test EXECUTES the real obdctl binary (argv → wire translation),
# so the test target depends on it and is told its path.
target_compile_definitions(obd_unit_tests
    PRIVATE OBD_TEST_OBDCTL_BIN="$<TARGET_FILE:obdctl>"
            OBD_TEST_OBD_CONVERT_BIN="$<TARGET_FILE:obd-convert>")
add_dependencies(obd_unit_tests obdctl obd-convert)
catch_discover_tests(obd_unit_tests TEST_SPEC "~[shutdown]"
    PROPERTIES TIMEOUT 60)

if(OBD_ENABLE_UBLK)
    target_sources(obd_unit_tests PRIVATE
        unit/test_ublk.cpp unit/test_ublk_shutdown.cpp)
    target_link_libraries(obd_unit_tests PRIVATE obd_ublk)
    catch_discover_tests(obd_unit_tests TEST_SPEC "[shutdown]"
        PROPERTIES TIMEOUT 30)
    # obd-device is built only with the ublk backend; the flag-validation
    # test executes the real binary.
    target_compile_definitions(obd_unit_tests
        PRIVATE OBD_TEST_DEVICE_BIN="$<TARGET_FILE:obd-device>")
    add_dependencies(obd_unit_tests obd-device)
endif()

# Test-only fake obd-device for the commit integration test (L6: it
# checkpoints its LSMT-RW upper only on SIGTERM, like the real device).
add_executable(obd-test-fake-device fake_device_main.cpp)
obd_target_warnings(obd-test-fake-device)
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
obd_target_warnings(obd_integration_tests)
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
    PROPERTIES SKIP_RETURN_CODE 4  # Catch2 "all selected tests skipped"
               TIMEOUT 300)

add_test(NAME issue28-harness-failure-selftest
    COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_issue28_harness_failure.py
        $<TARGET_FILE:obd_integration_tests>)
set_tests_properties(issue28-harness-failure-selftest PROPERTIES TIMEOUT 120)

# Inspect every first-party translation unit, including binary/test helpers.
# The check uses the real build commands, so a forgotten target is observable.
if(CMAKE_GENERATOR MATCHES "Makefiles|Ninja")
    add_test(NAME build-warning-coverage
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json
            -DPROJECT_SOURCE=${PROJECT_SOURCE_DIR}
            -DWARNINGS_AS_ERRORS=${OBD_WARNINGS_AS_ERRORS}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_warning_flags.cmake)
    set_tests_properties(build-warning-coverage PROPERTIES TIMEOUT 30)
endif()

# CI guardrail for issue #11: normal CTest runs include this audit, which
# rejects any registered test entry that lacks an explicit TIMEOUT property.
add_test(NAME ctest-timeout-coverage
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/scripts/check_ctest_timeouts.py
        --build-dir ${CMAKE_BINARY_DIR}
        --ctest ${CMAKE_CTEST_COMMAND})
set_tests_properties(ctest-timeout-coverage PROPERTIES TIMEOUT 30)
