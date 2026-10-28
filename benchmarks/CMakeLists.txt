add_executable(obd-bench-ublk-read ublk_read.cpp)
obd_target_warnings(obd-bench-ublk-read)
target_link_libraries(obd-bench-ublk-read PRIVATE obd_ublk)

add_executable(obd-bench-writable writable.cpp)
obd_target_warnings(obd-bench-writable)
target_link_libraries(obd-bench-writable PRIVATE obd_core)

add_executable(obd-bench-hybrid-log hybrid_log.cpp)
obd_target_warnings(obd-bench-hybrid-log)
target_link_libraries(obd-bench-hybrid-log PRIVATE obd_core)

add_executable(obd-bench-lsmt-read lsmt_read.cpp)
obd_target_warnings(obd-bench-lsmt-read)
target_link_libraries(obd-bench-lsmt-read PRIVATE obd_core)

add_executable(obd-bench-zfile-read zfile_read.cpp)
obd_target_warnings(obd-bench-zfile-read)
target_link_libraries(obd-bench-zfile-read PRIVATE obd_core)
