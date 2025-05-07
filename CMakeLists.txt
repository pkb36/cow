cmake_minimum_required(VERSION 3.10)
project(gstream_app C)

# 컴파일 옵션
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -ggdb -Wall -fno-omit-frame-pointer")

# include 디렉토리 설정
include_directories(
    /opt/nvidia/deepstream/deepstream/sources/includes
)

# pkg-config로 필요한 플래그 가져오기
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED 
    glib-2.0 
    gstreamer-1.0 
    gstreamer-sdp-1.0 
    gstreamer-webrtc-1.0 
    json-glib-1.0 
    libsoup-2.4 
    libcurl
)

include_directories(${DEPS_INCLUDE_DIRS})
link_directories(${DEPS_LIBRARY_DIRS})
add_definitions(${DEPS_CFLAGS_OTHER})

# DeepStream 라이브러리 경로
set(NVDS_VERSION 6.2)
set(LIB_INSTALL_DIR /opt/nvidia/deepstream/deepstream-${NVDS_VERSION}/lib)

link_directories(${LIB_INSTALL_DIR})

# 공통 링크 라이브러리
set(COMMON_LIBS 
    ${DEPS_LIBRARIES}
    -lnvdsgst_meta 
    -lnvds_meta 
    -lm
    pthread
)

# 각 실행파일 추가
add_executable(gstream_main 
    gstream_main.c config.c serial_comm.c socket_comm.c webrtc_peer.c process_cmd.c json_utils.c gstream_control.c curllib.c 
    device_setting.c nvds_process.c nvds_utils.c g_log.c event_recorder.c ptz_control.c video_convert.c
)
target_link_libraries(gstream_main ${COMMON_LIBS})

add_executable(webrtc_sender webrtc_sender.c socket_comm.c g_log.c)
target_link_libraries(webrtc_sender ${COMMON_LIBS})

add_executable(webrtc_recorder webrtc_recorder.c video_convert.c g_log.c)
target_link_libraries(webrtc_recorder ${COMMON_LIBS})

add_executable(webrtc_event_recorder webrtc_event_recorder.c g_log.c)
target_link_libraries(webrtc_event_recorder ${COMMON_LIBS})

add_executable(disk_check disk_check.c g_log.c)
target_link_libraries(disk_check ${COMMON_LIBS})

# 테스트용 모듈
# add_executable(json_test json_test.c)
# target_link_libraries(json_test ${COMMON_LIBS})

# add_executable(curllib_test curllib_test.c curllib.c json_utils.c g_log.c)
# target_link_libraries(curllib_test ${COMMON_LIBS})

# add_executable(settting_test device_setting.c serial_comm.c g_log.c ptz_control.c gstream_control.c)
# target_compile_definitions(settting_test PRIVATE TEST_SETTING)
# target_link_libraries(settting_test ${COMMON_LIBS})

# add_executable(log_test g_log.c)
# target_compile_definitions(log_test PRIVATE TEST_LOG)
# target_link_libraries(log_test m)

# 설치 및 정리 명령은 필요 시 추가
