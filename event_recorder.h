#ifndef __EVENT_RECORDER_H__
#define __EVENT_RECORDER_H__

#include "device_setting.h"

typedef enum {
	SENDER = 0,
	RECORDER,
	EVENT_RECORDER,
} UDPClientProcess;

typedef enum {
	RGB_CAM = 0,
	THERMAL_CAM,
	NUM_CAMS,
} CameraDevice;

typedef enum {
	MAIN_STREAM = 0,
	SECOND_STREAM,
} StreamChoice;

#define MAIN_STREAM_PORT_SPACE 	100

void start_event_buf_process(int cam_idx);
int trigger_event_record(int cam_idx, char* http_str_path_out);
int get_udp_port(UDPClientProcess process, CameraDevice device, StreamChoice stream_choice, int stream_cnt);

extern DeviceSetting g_setting;
extern WebRTCConfig g_config;

#endif