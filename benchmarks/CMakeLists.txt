add_executable(obd-bench-ublk-read ublk_read.cpp)
obd_target_warnings(obd-bench-ublk-read)
target_link_libraries(obd-bench-ublk-read PRIVATE obd_ublk)
