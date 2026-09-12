add_executable(obdctl obdctl.cpp)
obd_target_warnings(obdctl)
target_link_libraries(obdctl PRIVATE obd_core)

add_executable(obd-mkimage obd_mkimage_main.cpp)
obd_target_warnings(obd-mkimage)
target_link_libraries(obd-mkimage PRIVATE obd_core)

add_executable(obd-convert obd_convert_main.cpp gzip_index_builder.cpp turbo_package.cpp turbo_import.cpp oci_layer_plan.cpp oci_ext2_builder.cpp turbo_layered.cpp)
obd_target_warnings(obd-convert)
target_link_libraries(obd-convert PRIVATE obd_core)
if(OBD_ENABLE_LIBE2FS_BACKEND)
    target_compile_definitions(obd-convert PRIVATE OBD_HAVE_LIBE2FS=1)
    target_link_libraries(obd-convert PRIVATE obd_pinned_ext2fs obd_libe2fs_build_com_err)
    add_dependencies(obd-convert obd_libe2fs_build)
    set_target_properties(obd-convert PROPERTIES
        BUILD_RPATH "${OBD_LIBE2FS_LIBRARY_DIR};${OBD_LIBE2FS_PREFIX}/lib"
        INSTALL_RPATH "$ORIGIN/../${OBD_LIBE2FS_INSTALL_SUBDIR}")
    install(DIRECTORY "${OBD_LIBE2FS_LIBRARY_DIR}/"
        DESTINATION "${OBD_LIBE2FS_INSTALL_SUBDIR}"
        USE_SOURCE_PERMISSIONS
        FILES_MATCHING PATTERN "libext2fs.so*")
else()
    target_compile_definitions(obd-convert PRIVATE OBD_HAVE_LIBE2FS=0)
endif()

install(TARGETS obdctl obd-mkimage obd-convert RUNTIME DESTINATION bin)
