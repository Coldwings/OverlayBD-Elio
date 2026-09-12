if(NOT DEFINED BUILD_DIR)
    message(FATAL_ERROR "BUILD_DIR is required")
endif()
if(NOT DEFINED INSTALL_PREFIX)
    message(FATAL_ERROR "INSTALL_PREFIX is required")
endif()
if(NOT DEFINED LIBE2FS_INSTALL_SUBDIR)
    message(FATAL_ERROR "LIBE2FS_INSTALL_SUBDIR is required")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${INSTALL_PREFIX}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "cmake --install failed with ${install_result}\n"
        "${install_stdout}\n${install_stderr}")
endif()

set(converter "${INSTALL_PREFIX}/bin/obd-convert")
set(libe2fs_dir "${INSTALL_PREFIX}/${LIBE2FS_INSTALL_SUBDIR}")
set(libe2fs_soname "${libe2fs_dir}/libext2fs.so.2")

if(NOT EXISTS "${converter}")
    message(FATAL_ERROR "installed obd-convert not found: ${converter}")
endif()
if(NOT EXISTS "${libe2fs_soname}")
    message(FATAL_ERROR "bundled libext2fs SONAME not installed: ${libe2fs_soname}")
endif()

file(GLOB bundled_com_err "${libe2fs_dir}/libcom_err.so*")
if(bundled_com_err)
    message(FATAL_ERROR
        "libcom_err must not be bundled with obd-convert; found ${bundled_com_err}")
endif()

find_program(READELF_EXECUTABLE readelf REQUIRED)
find_program(LDD_EXECUTABLE ldd REQUIRED)

execute_process(
    COMMAND "${READELF_EXECUTABLE}" -d "${converter}"
    RESULT_VARIABLE readelf_result
    OUTPUT_VARIABLE readelf_stdout
    ERROR_VARIABLE readelf_stderr)
if(NOT readelf_result EQUAL 0)
    message(FATAL_ERROR
        "readelf failed with ${readelf_result}\n"
        "${readelf_stdout}\n${readelf_stderr}")
endif()

set(expected_origin_path "$ORIGIN/../${LIBE2FS_INSTALL_SUBDIR}")
string(FIND "${readelf_stdout}" "${expected_origin_path}" origin_index)
if(origin_index EQUAL -1)
    message(FATAL_ERROR
        "installed obd-convert does not advertise ${expected_origin_path} in RPATH/RUNPATH\n"
        "${readelf_stdout}")
endif()

execute_process(
    COMMAND "${LDD_EXECUTABLE}" "${converter}"
    RESULT_VARIABLE ldd_result
    OUTPUT_VARIABLE ldd_stdout
    ERROR_VARIABLE ldd_stderr)
if(NOT ldd_result EQUAL 0)
    message(FATAL_ERROR
        "ldd failed with ${ldd_result}\n"
        "${ldd_stdout}\n${ldd_stderr}")
endif()

string(REGEX MATCH "libext2fs\\.so\\.2 => ([^ \n]+)" libe2fs_match "${ldd_stdout}")
if(NOT libe2fs_match)
    message(FATAL_ERROR "ldd output does not resolve libext2fs.so.2\n${ldd_stdout}")
endif()
set(actual_libe2fs "${CMAKE_MATCH_1}")
file(REAL_PATH "${actual_libe2fs}" actual_libe2fs_real)
file(REAL_PATH "${libe2fs_soname}" expected_libe2fs_real)
if(NOT actual_libe2fs_real STREQUAL expected_libe2fs_real)
    message(FATAL_ERROR
        "installed obd-convert resolved the wrong libext2fs.so.2\n"
        "expected: ${expected_libe2fs_real}\n"
        "actual:   ${actual_libe2fs_real}\n"
        "${ldd_stdout}")
endif()

string(REGEX MATCH "libcom_err\\.so\\.2 => ([^ \n]+)" com_err_match "${ldd_stdout}")
if(NOT com_err_match)
    message(FATAL_ERROR "ldd output does not resolve system libcom_err.so.2\n${ldd_stdout}")
endif()
set(actual_com_err "${CMAKE_MATCH_1}")
file(REAL_PATH "${actual_com_err}" actual_com_err_real)
file(REAL_PATH "${libe2fs_dir}" libe2fs_dir_real)
string(FIND "${actual_com_err_real}" "${libe2fs_dir_real}/" bundled_com_err_index)
if(bundled_com_err_index EQUAL 0)
    message(FATAL_ERROR
        "installed obd-convert resolved bundled libcom_err.so.2; expected system library\n"
        "${ldd_stdout}")
endif()
