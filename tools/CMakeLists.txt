add_executable(obdctl obdctl.cpp)
target_link_libraries(obdctl PRIVATE obd_core)

add_executable(obd-mkimage obd_mkimage_main.cpp)
target_link_libraries(obd-mkimage PRIVATE obd_core)

install(TARGETS obdctl obd-mkimage RUNTIME DESTINATION bin)
