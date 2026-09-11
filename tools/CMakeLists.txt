add_executable(obdctl obdctl.cpp)
obd_target_warnings(obdctl)
target_link_libraries(obdctl PRIVATE obd_core)

add_executable(obd-mkimage obd_mkimage_main.cpp)
obd_target_warnings(obd-mkimage)
target_link_libraries(obd-mkimage PRIVATE obd_core)

add_executable(obd-convert obd_convert_main.cpp)
obd_target_warnings(obd-convert)
target_link_libraries(obd-convert PRIVATE obd_core)

install(TARGETS obdctl obd-mkimage obd-convert RUNTIME DESTINATION bin)
