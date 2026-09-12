# Optional converter-local pinned libext2fs backend.
# The pin intentionally matches upstream containerd/overlaybd's standalone
# libext2fs build (CMake/Finde2fs.cmake).
include(FetchContent)

set(OBD_LIBE2FS_E2FSPROGS_REPOSITORY "https://github.com/data-accelerator/e2fsprogs.git"
    CACHE STRING "Pinned e2fsprogs repository for the obd-convert libe2fs backend")
set(OBD_LIBE2FS_E2FSPROGS_TAG "404deb95e6b0ed0ceb0148d289977e65bee7f8d0"
    CACHE STRING "Pinned e2fsprogs commit for the obd-convert libe2fs backend")
set(OBD_LIBE2FS_INSTALL_SUBDIR "lib/overlaybd-elio"
    CACHE STRING "Install subdirectory for bundled converter libext2fs shared libraries")

FetchContent_Declare(obd_e2fsprogs
    GIT_REPOSITORY ${OBD_LIBE2FS_E2FSPROGS_REPOSITORY}
    GIT_TAG ${OBD_LIBE2FS_E2FSPROGS_TAG})
FetchContent_GetProperties(obd_e2fsprogs)
if(NOT obd_e2fsprogs_POPULATED)
    FetchContent_Populate(obd_e2fsprogs)
endif()

set(OBD_E2FSPROGS_SOURCE_DIR "${obd_e2fsprogs_SOURCE_DIR}" CACHE INTERNAL "")
set(OBD_E2FSPROGS_BUILD_DIR "${OBD_E2FSPROGS_SOURCE_DIR}/build" CACHE INTERNAL "")
set(OBD_LIBE2FS_PREFIX "${OBD_E2FSPROGS_BUILD_DIR}/v1.47.0-opt" CACHE INTERNAL "")
set(OBD_LIBE2FS_STANDALONE_DIR "${OBD_E2FSPROGS_BUILD_DIR}/libext2fs" CACHE INTERNAL "")
set(OBD_LIBE2FS_INCLUDE_DIRS
    "${OBD_LIBE2FS_STANDALONE_DIR}/include"
    "${OBD_LIBE2FS_PREFIX}/include"
    CACHE INTERNAL "")
set(OBD_LIBE2FS_LIBRARY_DIR "${OBD_LIBE2FS_STANDALONE_DIR}/lib" CACHE INTERNAL "")
set(OBD_LIBE2FS_LIBRARY "${OBD_LIBE2FS_LIBRARY_DIR}/libext2fs.so" CACHE INTERNAL "")

file(MAKE_DIRECTORY ${OBD_LIBE2FS_INCLUDE_DIRS})
file(MAKE_DIRECTORY "${OBD_LIBE2FS_LIBRARY_DIR}")

set(OBD_LIBE2FS_COM_ERR_LIBRARY "${OBD_LIBE2FS_PREFIX}/lib/libcom_err.so" CACHE INTERNAL "")

add_custom_command(
    OUTPUT "${OBD_LIBE2FS_LIBRARY}"
    BYPRODUCTS
        "${OBD_LIBE2FS_LIBRARY_DIR}/libext2fs.so.2"
        "${OBD_LIBE2FS_LIBRARY_DIR}/libext2fs.so.2.4"
        "${OBD_LIBE2FS_PREFIX}/include/et/com_err.h"
        "${OBD_LIBE2FS_PREFIX}/lib/libcom_err.so"
        "${OBD_LIBE2FS_PREFIX}/lib/libcom_err.so.2"
        "${OBD_LIBE2FS_PREFIX}/lib/libcom_err.so.2.1"
    WORKING_DIRECTORY "${OBD_E2FSPROGS_SOURCE_DIR}"
    COMMAND /bin/sh -c "chmod 755 build.sh && sed -i 's|CFLAGS=\"-fPIC -O3\"|CFLAGS=\"-fPIC -O3 -std=gnu11\"|' build.sh && CC='${CMAKE_C_COMPILER}' ./build.sh"
    COMMENT "Building pinned libext2fs for obd-convert (${OBD_LIBE2FS_E2FSPROGS_TAG})"
    VERBATIM)
add_custom_target(obd_libe2fs_build DEPENDS "${OBD_LIBE2FS_LIBRARY}")

add_library(obd_pinned_ext2fs UNKNOWN IMPORTED GLOBAL)
set_target_properties(obd_pinned_ext2fs PROPERTIES
    IMPORTED_LOCATION "${OBD_LIBE2FS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OBD_LIBE2FS_INCLUDE_DIRS}")
add_dependencies(obd_pinned_ext2fs obd_libe2fs_build)

add_library(obd_libe2fs_build_com_err UNKNOWN IMPORTED GLOBAL)
set_target_properties(obd_libe2fs_build_com_err PROPERTIES
    IMPORTED_LOCATION "${OBD_LIBE2FS_COM_ERR_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OBD_LIBE2FS_INCLUDE_DIRS}")
add_dependencies(obd_libe2fs_build_com_err obd_libe2fs_build)
