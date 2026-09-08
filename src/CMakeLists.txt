# obd_core: everything except binary entry points and the privileged ublk
# data plane helpers (kept separate so unit tests never pull liburing-only
# code paths in when OBD_ENABLE_UBLK=OFF).

add_library(obd_core STATIC
    common/crc32c.cpp
    common/sha256.cpp
    format/block_codec.cpp
    format/zfile_format.cpp
    format/zfile.cpp
    format/lsmt_format.cpp
    format/lsmt.cpp
    format/writer.cpp
    format/sparse_rw.cpp
    format/lsmt_rw.cpp
    format/merged_writable.cpp
    source/local_file.cpp
    source/tar_offset.cpp
    source/credentials.cpp
    source/dart.cpp
    source/registry.cpp
    source/chunk_cache.cpp
    source/downloader.cpp
    source/switch_source.cpp
    source/layer_store.cpp
    image/config.cpp
    image/image_file.cpp
    supervisor/protocol.cpp
    supervisor/child.cpp
    supervisor/daemon.cpp)

target_include_directories(obd_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(obd_core PUBLIC
    elio_http
    nlohmann_json::nlohmann_json
    lz4_static
    ZLIB::ZLIB)

if(OBD_ENABLE_ZSTD)
    target_compile_definitions(obd_core PUBLIC OBD_HAVE_ZSTD=1)
    target_link_libraries(obd_core PUBLIC libzstd_static)
else()
    target_compile_definitions(obd_core PUBLIC OBD_HAVE_ZSTD=0)
endif()

if(OBD_WARNINGS_AS_ERRORS)
    target_compile_options(obd_core PRIVATE -Wall -Wextra -Werror)
else()
    target_compile_options(obd_core PRIVATE -Wall -Wextra)
endif()

if(OBD_ENABLE_UBLK)
    add_library(obd_ublk STATIC
        ublk/ctrl.cpp
        ublk/queue.cpp
        ublk/device.cpp
        ublk/elio_bridge.cpp)
    target_include_directories(obd_ublk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(obd_ublk PUBLIC OBD_HAVE_UBLK=1)
    target_link_libraries(obd_ublk PUBLIC obd_core liburing::liburing)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cmd/CMakeLists.txt)
    add_subdirectory(cmd)
endif()
