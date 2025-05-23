/*
 * Demo gstreamer app for negotiating and streaming a sendrecv audio-only webrtc
 * stream to all the peers in a multiparty room.
 *
 * gcc mp-webrtc-sendrecv.c $(pkg-config --cflags --libs gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0) -o mp-webrtc-sendrecv
 *
 * Author: Nirbheek Chauhan <nirbheek@centricular.com>
 */
#include <gst/gst.h>

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <stdbool.h>
#include <nvbufsurface.h>

#include "config.h"
#include "gstream_main.h"
#include "gstnvdsmeta.h"
#include "nvds_process.h"
#include "curllib.h"
#include "device_setting.h"
#include "event_recorder.h"
#include "nvds_opticalflow_meta.h"
#include "nvds_utils.h"


int g_cam_index = 0;
int g_noti_cam_idx = 0;
int g_top = 0, g_left = 0, g_width = 0, g_height = 0;
int g_move_to_center_running = 0;
int g_frame_count[2];
static pthread_t g_tid;
pthread_mutex_t g_notifier_mutex;
int g_event_class_id = CLASS_NORMAL_COW;
int g_notifier_running = 0;
#if 0
Timer timers[MAX_PTZ_PRESET];
#endif

ObjMonitor obj_info[NUM_CAMS][NUM_OBJS];

int threshold_event_duration[NUM_CLASSES] = 
{ 
  0,          //NORMAL
  15,         //HEAT
  15,         //FLIP
  15,         //LABOR SIGN
  0,          //NORMAL_SITTING
};         


float threshold_confidence[NUM_CLASSES] = 
{ 
  0.2,        //NORMAL
  0.8,        //HEAT
  0.8,        //FLIP
  0.8,        //LABOR SIGN
  0.2,        //NORMAL_SITTING
};         


void set_tracker_analysis(gboolean OnOff)
{
  char element_name[32];

  for(int cam_idx = 0; cam_idx < g_config.device_cnt; cam_idx++) {
    GstElement *dspostproc;
    sprintf(element_name, "dspostproc_%d", cam_idx+1);
    dspostproc = gst_bin_get_by_name (GST_BIN (g_pipeline), element_name);
    if (dspostproc == NULL){
      glog_trace ("Fail get %s element\n", element_name);
      continue;
    }

    gboolean rest_val = OnOff ? FALSE:TRUE ;
    g_object_set(G_OBJECT(dspostproc), "reset-object", rest_val, NULL);
    g_clear_object (&dspostproc);
  }
}


void set_process_analysis(gboolean OnOff)
{
  glog_trace("set_process_analysis analysis_status[%d] OnOff[%d] nv_interval[%d]\n", 
    g_setting.analysis_status, OnOff, g_setting.nv_interval);

  if (OnOff == 0)
    check_events_for_notification(0, 1);        //first parameter is don't care

  for(int cam_idx = 0; cam_idx < g_config.device_cnt; cam_idx++) {
    char element_name[32];
    GstElement *nvinfer;
    sprintf(element_name, "nvinfer_%d", cam_idx+1);
    nvinfer = gst_bin_get_by_name (GST_BIN (g_pipeline), element_name);
    if (nvinfer == NULL){
      glog_trace ("Fail get %s element\n", element_name);
      continue;
    }

    gint interval = OnOff ? g_setting.nv_interval : G_MAXINT;
    g_object_set(G_OBJECT(nvinfer), "interval", interval, NULL);
    g_clear_object (&nvinfer);

    GstElement *dspostproc;
    sprintf(element_name, "dspostproc_%d", cam_idx+1);
    dspostproc = gst_bin_get_by_name (GST_BIN (g_pipeline), element_name);
    if (dspostproc == NULL){
      glog_trace ("Fail get %s element\n", element_name);
      continue;
    }

    gboolean rest_val = OnOff ? FALSE:TRUE ;
    g_object_set(G_OBJECT(dspostproc), "reset-object", rest_val, NULL);
    g_clear_object (&dspostproc);
  } 
}


int is_process_running(const char *process_name) 
{
    char command[256];

    snprintf(command, sizeof(command), "ps aux | grep '%s' | grep -v grep", process_name);
    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        perror("popen");
        return -1;
    }
    // Check if there's any output from the command
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        // If we read a line, the process is running
        fclose(fp);
        return 1; // Process is running
    }
    fclose(fp);
    return 0; // Process is not running
}


void wait_recording_finish()
{
  const char *process_name = "webrtc_event_recorder";     

  while(1) {
    int result = is_process_running(process_name);
    if (result == 1) 
      sleep(1);
    else
      break;
  }
  glog_trace("%s ended\n", process_name);
}


int send_notification_to_server(int class_id)
{
  char event_id[2] = {0};
  int cam_idx = RGB_CAM;

  event_id[0] = class_id + '0'; 
  glog_trace("try sending class_id=%d, enable_event_notify=%d\n", class_id, g_setting.enable_event_notify);
  if(g_setting.enable_event_notify) {
    if (g_curlinfo.position[0] == 0) {
      cam_idx = g_noti_cam_idx;       //eventually this will be same with g_source_cam_idx
      glog_trace("g_noti_cam_idx=%d,g_source_cam_idx=%d\n", g_noti_cam_idx, g_source_cam_idx);
      if (g_noti_cam_idx != g_source_cam_idx){
        glog_trace("g_noti_cam_idx=%d and g_source_cam_idx=%d are different, so return\n", g_noti_cam_idx, g_source_cam_idx);
        return FALSE;
      }
    }
    else {
      cam_idx = g_source_cam_idx;
      glog_trace("position=%s, g_source_cam_idx=%d\n", g_curlinfo.position, g_source_cam_idx);
    }

    if (trigger_event_record(cam_idx, g_curlinfo.video_url) == TRUE) {
#if NOTI_BLOCK_FOR_TEST     
#else	    
      notification_request(g_config.camera_id, event_id, &g_curlinfo); 
#endif
      glog_trace("notification_request cam_idx=%d,class_id=%d,position=%s\n", cam_idx, class_id, g_curlinfo.position);
      g_curlinfo.position[0] = 0;
      return TRUE;
    }
  }
  g_curlinfo.position[0] = 0;

  return FALSE;
}


void *process_notification(void *arg)
{
  while(1) {
    pthread_mutex_lock(&g_notifier_mutex);             //block and wait pthread_mutex_lock()

    if(g_event_class_id == EVENT_EXIT) {
      break;
    }
    else if(g_event_class_id != CLASS_NORMAL_COW && g_event_class_id != CLASS_NORMAL_COW_SITTING) {
      g_notifier_running = 1;
      glog_trace("g_notifier_running = %d\n", g_notifier_running);
      if (send_notification_to_server(g_event_class_id) == TRUE) {
        sleep(1);
        wait_recording_finish();
      }
      g_event_class_id = CLASS_NORMAL_COW;
      g_notifier_running = 0;
      glog_trace("g_notifier_running = %d\n", g_notifier_running);
    }
  }

  return 0;
}


int is_notifier_running() 
{
  return g_notifier_running;
}


void unlock_notification()
{
  if (!is_notifier_running()) {    
      glog_trace("thread paused, unlock g_notifier_mutex, g_event_class_id=%d\n", g_event_class_id);
      pthread_mutex_unlock(&g_notifier_mutex);                                 //unlock process_notification() thread to send event
  } else {
      glog_trace("thread is running\n");
  }
}


void gather_event(int class_id, int obj_id, int cam_idx)
{
  if (obj_id < 0)
    return;
  if (class_id != CLASS_NORMAL_COW && class_id != CLASS_NORMAL_COW_SITTING) {
    obj_info[cam_idx][obj_id].detected_frame_count++;
    obj_info[cam_idx][obj_id].class_id = class_id;
  }
}


void init_opt_flow(int cam_idx, int obj_id, int is_total)
{
  if (g_setting.opt_flow_apply == 0) {
    return;
  }

  obj_info[cam_idx][obj_id].opt_flow_check_count = 0;
  obj_info[cam_idx][obj_id].move_size_avg = 0.0;
  obj_info[cam_idx][obj_id].do_opt_flow = 0;
  if (is_total) {
    obj_info[cam_idx][obj_id].opt_flow_detected_count = 0;
    obj_info[cam_idx][obj_id].prev_x = 0;
    obj_info[cam_idx][obj_id].prev_y = 0;
    obj_info[cam_idx][obj_id].prev_width = 0;
    obj_info[cam_idx][obj_id].prev_height = 0;
    obj_info[cam_idx][obj_id].x = 0;
    obj_info[cam_idx][obj_id].y = 0;
    obj_info[cam_idx][obj_id].width = 0;
    obj_info[cam_idx][obj_id].height = 0;
  }
}


#if RESNET_50
void check_heat_count(int cam_idx, int obj_id)
{
  glog_trace("obj_info[%d][%d].heat_count=%d\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].heat_count);
  if (obj_info[cam_idx][obj_id].heat_count < HEAT_COUNT_THRESHOLD) {
    obj_info[cam_idx][obj_id].notification_flag = 0;
  }
  obj_info[cam_idx][obj_id].heat_count = 0;
}
#endif


void check_events_for_notification(int cam_idx, int init)
{
  if (init) {
    for (int i = 0; i < NUM_CAMS; i++) {
      for (int j = 0; j < NUM_OBJS; j++) {
        obj_info[i][j].detected_frame_count = 0;
        obj_info[i][j].duration = 0;
        obj_info[i][j].temp_duration = 0;
        obj_info[i][j].class_id = CLASS_NORMAL_COW;
#if RESNET_50        
        obj_info[i][j].heat_count = 0;
#endif  
        init_opt_flow(i, j, 1);
      }
    }
    return;
  }

  for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++) {
    if (obj_info[cam_idx][obj_id].detected_frame_count >= (PER_CAM_SEC_FRAME - 1)) {    //if detection continued one second
      // glog_trace("cam_idx=%d, obj_id=%d detected_frame_count=%d duration=%d\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].detected_frame_count, obj_info[cam_idx][obj_id].duration);
      obj_info[cam_idx][obj_id].duration++;
      if (obj_info[cam_idx][obj_id].duration >= threshold_event_duration[obj_info[cam_idx][obj_id].class_id]) {   //if duration lasted more than designated time
        obj_info[cam_idx][obj_id].duration = 0;
        // check_for_zoomin(g_total_rect_size, detect_count);      //LJH, in progress
        obj_info[cam_idx][obj_id].notification_flag = 1;                                          //send notification later
#if RESNET_50
        if (g_setting.resnet50_apply) {
          if (obj_info[cam_idx][obj_id].class_id == CLASS_HEAT_COW) {
            check_heat_count(cam_idx, obj_id);          //LJH, if heat count is zero, notification is cancelled
          }
        }
#endif
        glog_trace("[%d][%d].class_id=%d\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].class_id);
      }
      
      if (g_setting.opt_flow_apply) {
        if (obj_info[cam_idx][obj_id].class_id == CLASS_FLIP_COW) {                     //if event was flip do optical flow analysis
          obj_info[cam_idx][obj_id].do_opt_flow = 1;                                    //if detected frame count lasted equal or more than one second then do optical flow analysis
        }
        else {
          init_opt_flow(cam_idx, obj_id, 0);
        }
      }
    }
    else {                        //if detection not continued for one second
      obj_info[cam_idx][obj_id].duration = 0;
      init_opt_flow(cam_idx, obj_id, 1);
    }
    obj_info[cam_idx][obj_id].detected_frame_count = 0;
  }
}


int get_opt_flow_result(int cam_idx, int obj_id)
{
  glog_trace("[%d][%d].confi=%.2f opt_flow_detected_count ==> %d\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].confidence, obj_info[cam_idx][obj_id].opt_flow_detected_count);
  if (obj_info[cam_idx][obj_id].opt_flow_detected_count >= THRESHOLD_OVER_OPTICAL_FLOW_COUNT)
    return 1;
  return 0;
}

#if 0     
#define NOTICATION_TIME_GAP           60       

int get_time_gap_result(int preset)               //need to apply to objects
{
  static int first[MAX_PTZ_PRESET] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  int ret = 0;

  if (first[preset]) {
    first[preset] = 0;
    init_timer(preset, NOTICATION_TIME_GAP);
    return 1;
  }

  ret = check_time_gap(preset);
  if (ret == 1) {
    init_timer(preset, NOTICATION_TIME_GAP);
  }

  return ret;
}
#endif

void trigger_notification(int cam_idx)
{
  for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++) {
    if (obj_info[cam_idx][obj_id].notification_flag) {   
      obj_info[cam_idx][obj_id].notification_flag = 0;
      g_event_class_id = obj_info[cam_idx][obj_id].class_id;
      glog_trace("[15SEC] notification_flag==1,cam_idx=%d,obj_id=%d,g_event_class_id=%d,g_preset_index=%d\n", cam_idx, obj_id, g_event_class_id, g_preset_index);
#if OPTICAL_FLOW_INCLUDE           
      if (g_setting.opt_flow_apply) {
        if (g_event_class_id == CLASS_FLIP_COW) {
          glog_trace("[15SEC] g_event_class_id==CLASS_FLIP_COW\n");
          if (get_opt_flow_result(cam_idx, obj_id) == 0) {
            glog_trace("[15SEC] get_opt_flow_result(cam_idx=%d,obj_id=%d) ==> 0\n", cam_idx, obj_id);
            init_opt_flow(cam_idx, obj_id, 1);
            continue;
          }
          init_opt_flow(cam_idx, obj_id, 1);
          glog_trace("[15SEC] get_opt_flow_result(cam_idx=%d,obj_id=%d) ==> 1\n", cam_idx, obj_id);
        }
      }
#endif
      g_noti_cam_idx = g_cam_index;
      unlock_notification(); 
      glog_trace("[[[NOTIFICATION]]] [%d][%d].confi=%.2f,g_source_cam_idx=%d,g_noti_cam_idx=%d,g_event_class_id=%d unlock_notification()\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].confidence, g_source_cam_idx, g_noti_cam_idx, g_event_class_id);
      obj_info[cam_idx][obj_id].temp_event_time_gap = TEMP_EVENT_TIME_GAP;
    }
  }
}


void print_debug(NvDsObjectMeta * obj_meta)
{
  glog_trace("obj_meta->class_id=%d confi=%f obj_label=%s top=%d left=%d width=%d height=%d x_offset=%d y_offset=%d display_text=%s font_size=%d cam_idx=%d obj_id=%ld\n", 
            obj_meta->class_id, obj_meta->confidence, obj_meta->obj_label, (int)obj_meta->rect_params.top, 
            (int)obj_meta->rect_params.left,  (int)obj_meta->rect_params.width, (int)obj_meta->rect_params.height,
            (int)obj_meta->text_params.x_offset, (int)obj_meta->text_params.y_offset, obj_meta->text_params.display_text, 
            (int)obj_meta->text_params.font_params.font_size, g_cam_index, obj_meta->object_id);
}


#if OPTICAL_FLOW_INCLUDE

int get_opt_flow_object(int cam_idx, int start_obj_id)
{
  for (int obj_id = start_obj_id; obj_id < NUM_OBJS; obj_id++) {
    if (obj_info[cam_idx][obj_id].do_opt_flow) {                    //if do_opt_flow is set, then it means that it is heat state
      return obj_id;
    }
  }

  return -1;
}


double update_average(double previous_average, int count, double new_value) 
{
    return ((previous_average * (count - 1)) + new_value) / count;
}


int get_correction_value(double diagonal)
{
    int corr_value = 0;  // Initialize to a default value

    // If diagonal is less than or equal to SMALL_BBOX_DIAGONAL, calculate the correction value
    if (diagonal <= SMALL_BBOX_DIAGONAL) {
        corr_value = (int)(((SMALL_BBOX_DIAGONAL - diagonal) / 10) + 1);
    }

    return corr_value;
}


int get_move_distance(int cam_idx, int obj_id)
{
  if (obj_info[cam_idx][obj_id].prev_x == 0 || obj_info[cam_idx][obj_id].prev_y == 0)
    return 0;

  int x_dist = abs(obj_info[cam_idx][obj_id].prev_x - obj_info[cam_idx][obj_id].x);
  int y_dist = abs(obj_info[cam_idx][obj_id].prev_y - obj_info[cam_idx][obj_id].y);

  return (int)calculate_sqrt((double)x_dist, (double)y_dist);
}


int get_rect_size_change(int cam_idx, int obj_id)
{
  if (obj_info[cam_idx][obj_id].prev_width == 0 || obj_info[cam_idx][obj_id].prev_height == 0)
    return 0;

  int width_change = abs(obj_info[cam_idx][obj_id].prev_width - obj_info[cam_idx][obj_id].width);
  int height_change = abs(obj_info[cam_idx][obj_id].prev_height - obj_info[cam_idx][obj_id].height);

  return (int)calculate_sqrt((double)width_change, (double)height_change);
}


void set_prev_xy(int cam_idx, int obj_id)
{
  obj_info[cam_idx][obj_id].prev_x = obj_info[cam_idx][obj_id].x;
  obj_info[cam_idx][obj_id].prev_y = obj_info[cam_idx][obj_id].y;
}


void set_prev_rect_size(int cam_idx, int obj_id)
{
  obj_info[cam_idx][obj_id].prev_width = obj_info[cam_idx][obj_id].width;
  obj_info[cam_idx][obj_id].prev_height = obj_info[cam_idx][obj_id].height;
}


int get_flip_color_over_threshold(int cam_idx, int obj_id)
{
  if (g_setting.opt_flow_apply) {
    if (obj_info[cam_idx][obj_id].opt_flow_detected_count > 0){
      return RED_COLOR;
    }
    return YELLO_COLOR;
  }

  return RED_COLOR;
}


int get_heat_color_over_threshold(int cam_idx, int obj_id)
{
  if (g_setting.resnet50_apply) {
    if (obj_info[cam_idx][obj_id].heat_count > 0){
      return RED_COLOR;
    }
    return YELLO_COLOR;
  }

  return RED_COLOR;
}


void process_opt_flow(NvDsFrameMeta *frame_meta, int cam_idx, int obj_id, int cam_sec_interval)
{
  if (obj_id < 0)
    return;

  int count = 0;
  double move_size = 0.0, move_size_total = 0.0, move_size_avg = 0.0;
  double diagonal = 0;
  int row_start = 0, col_start = 0, row_num = 0, col_num = 0;
  int cols = 0;
  int corr_value = 0;
  int bbox_move = 0, rect_size_change = 0;

  for (NvDsMetaList *l_user = frame_meta->frame_user_meta_list; l_user != NULL; l_user = l_user->next) {                //LJH, added this loop
      NvDsUserMeta *user_meta = (NvDsUserMeta *)(l_user->data);
    // https://docs.nvidia.com/metropolis/deepstream/4.0/dev-guide/DeepStream_Development_Guide/baggage/structNvDsOpticalFlowMeta.html
    if (user_meta->base_meta.meta_type == NVDS_OPTICAL_FLOW_META) {
        NvDsOpticalFlowMeta *opt_flow_meta = (NvDsOpticalFlowMeta *)(user_meta->user_meta_data);
        // Access motion vector data
        //rows = opt_flow_meta->rows;
        cols = opt_flow_meta->cols;
        NvOFFlowVector *flow_vectors = (NvOFFlowVector *)(opt_flow_meta->data);
        row_start = (obj_info[cam_idx][obj_id].x / 4) + 1;
        col_start = (obj_info[cam_idx][obj_id].y / 4) + 1;
        row_num = obj_info[cam_idx][obj_id].width / 4;
        col_num = obj_info[cam_idx][obj_id].height / 4;
        // printf("id=%d,rs=%d,cs=%d,rn=%d,cn=%d\n", obj_id, row_start, col_start, row_num, col_num);
        // glog_trace("index=%d,id=%d,x=%d,y=%d,w=%d,h=%d\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].x, obj_info[cam_idx][obj_id].y,
            // obj_info[cam_idx][obj_id].width, obj_info[cam_idx][obj_id].height);
		    diagonal = obj_info[cam_idx][obj_id].diagonal;

        move_size_total = 0.0;
		    count = 0;
        // Process the motion vectors as needed
        for (int row = row_start; row < (row_start + row_num); ++row) {
            for (int col = col_start; col < (col_start + col_num); ++col) {
                int index = row * cols + col;
                NvOFFlowVector flow_vector = flow_vectors[index];              // https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvof.html
                move_size = calculate_sqrt(flow_vector.flowx, flow_vector.flowy);
                move_size_total += move_size;
                count++;
            }
        }
        if (count > 0) {
          move_size_avg = move_size_total / (double)count;
          // glog_trace("count=%d, move_size_avg=%lf\n", count, move_size_avg);
          obj_info[cam_idx][obj_id].opt_flow_check_count++;
          obj_info[cam_idx][obj_id].move_size_avg = update_average(obj_info[cam_idx][obj_id].move_size_avg, 
                                                    obj_info[cam_idx][obj_id].opt_flow_check_count, move_size_avg);
          // glog_trace("[%d][%d].opt_flow_check_count=%d, move_size_avg=%lf diag=%.1f\n", 
          //     cam_idx, obj_id, obj_info[cam_idx][obj_id].opt_flow_check_count, obj_info[cam_idx][obj_id].move_size_avg,
          //     diagonal);
        }
        if (cam_sec_interval) {
          bbox_move = get_move_distance(cam_idx, obj_id);
          rect_size_change = get_rect_size_change(cam_idx, obj_id);
          set_prev_xy(cam_idx, obj_id);
          set_prev_rect_size(cam_idx, obj_id);

          if (obj_info[cam_idx][obj_id].move_size_avg > 0) {
            glog_trace("[SEC] [%d][%d].move_size_avg=%.1f,confi=%.2f,diag=%.1f\n", cam_idx, obj_id, 
                  obj_info[cam_idx][obj_id].move_size_avg, obj_info[cam_idx][obj_id].confidence, diagonal);
          }

          if (bbox_move < THRESHOLD_BBOX_MOVE && rect_size_change < THRESHOLD_RECT_SIZE_CHANGE && g_move_speed == 0) {
            corr_value = get_correction_value(diagonal);                //LJH, when rectangle is small the move size tend to increase
            if (cam_idx == RGB_CAM) {                                   //LJH, RGB optical flow is more sensitive
              corr_value += 9;
            }
            if (obj_info[cam_idx][obj_id].move_size_avg > (g_setting.opt_flow_threshold + corr_value)) {
              obj_info[cam_idx][obj_id].opt_flow_detected_count++;
              glog_trace("[%d][%d].opt_flow_detected_count ==> %d\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].opt_flow_detected_count);
            }
          }
          else {
            glog_trace("[SEC] bbox_move=%d,rect_size_change=%d,g_move_speed=%d\n", bbox_move, rect_size_change, g_move_speed);
          }
          init_opt_flow(cam_idx, obj_id, 0);
        }
    }
  }
}

#endif

void set_obj_rect_id(int cam_idx, NvDsObjectMeta *obj_meta, int cam_sec_interval)
{
  static int x = 0, y = 0, width = 0, height = 0, rect_set = 0;

  if (obj_meta->object_id < 0)
    return;

  if (cam_sec_interval) {
    x = (int)obj_meta->rect_params.left;
    y = (int)obj_meta->rect_params.top;
    width = (int)obj_meta->rect_params.width;
    height = (int)obj_meta->rect_params.height;
    rect_set = 1;
  }

  if (rect_set) {
    obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].x =         x;
    obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].y =         y;
    obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].width =     width;
    obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].height =    height;
  }
  else {
    obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].x =         (int)obj_meta->rect_params.left;
    obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].y =         (int)obj_meta->rect_params.top;
    obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].width =     (int)obj_meta->rect_params.width;
    obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].height =    (int)obj_meta->rect_params.height;
  }
  
  obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].center_x = obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].x + (obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].width/2);
  obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].center_x = obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].y + (obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].height/2);

  obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].class_id =  (int)obj_meta->class_id;
  obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].diagonal =  calculate_sqrt((double)width, (double)height);
  obj_info[cam_idx][obj_meta->object_id % NUM_OBJS].confidence = (float)obj_meta->confidence;
}


#if THERMAL_TEMP_INCLUDE
void get_pixel_color(NvBufSurface *surface, guint batch_idx, guint x, guint y, unsigned char *r, unsigned char *g, unsigned char *b, unsigned char *a)
{
  if (!surface) {
      printf("Surface is NULL.\n");
      return;
  }

  // Get the surface information for the batch
  NvBufSurfaceParams *params = &surface->surfaceList[batch_idx];
  // if (params->memType != NVBUF_MEM_CUDA_DEVICE) {
  //      printf("Surface is not CUDA memory type.\n");
  //      return;
  // }

  // Get the width, height, and color format of the surface
  int width = params->width;
  int height = params->height;
  NvBufSurfaceColorFormat color_format = params->colorFormat;

  // Check if the pixel coordinates are within the bounds
  if (x >= width || y >= height) {
      printf("Pixel coordinates are out of bounds.\n");
      return;
  }

  // Access the pixel data directly
  unsigned char *pixel_data = (unsigned char *)params->dataPtr;

  // Define the pixel size based on the color format
  int pixel_size = 0;

  switch (color_format) {
    case NVBUF_COLOR_FORMAT_RGBA:
        pixel_size = 4;  // RGBA format (4 bytes per pixel)
        break;
    case NVBUF_COLOR_FORMAT_BGR:
        pixel_size = 3;  // BGR format (3 bytes per pixel)
        break;
    case NVBUF_COLOR_FORMAT_NV12:
        // For NV12, you'll need to handle both Y and UV planes separately.
        printf("Pixel color extraction for NV12 is not implemented in this example.\n");
        return;
    default:
        printf("Unsupported color format.\n");
        return;
  }

  // Compute the offset for the pixel at (x, y)
  int pixel_offset = (y * width + x) * pixel_size;

  *r = pixel_data[pixel_offset];     // Red value
  *g = pixel_data[pixel_offset + 1]; // Green value
  *b = pixel_data[pixel_offset + 2]; // Blue value
  *a = (pixel_size == 4) ? pixel_data[pixel_offset + 3] : 255; // Alpha value (if RGBA)
}


NvBufSurface *get_surface(GstBuffer *buf)
{
  NvBufSurface *surface = NULL;
  GstMapInfo map_info;

  // Map the buffer to access its memory
  if (gst_buffer_map(buf, &map_info, GST_MAP_READ)) {
      surface = (NvBufSurface *)map_info.data;  // Access NvBufSurface from mapped data
      if (!surface) {
          glog_error("Error: Unable to retrieve NvBufSurface\n");
          gst_buffer_unmap(buf, &map_info);
          return NULL;
      }
  }
  
  return surface;
}


// Define a function to map RGBA color to temp
float map_rgba_to_temp(unsigned char r, unsigned char g, unsigned char b)
{
  // Define temp range (e.g., 0°C to 100°C)
  float min_temp = 0.0f;  // Minimum temp (for Blue)
  float max_temp = 100.0f; // Maximum temp (for Red)
  
  // Map the 'Red' channel to temp (simple approach)
  // Assuming the color range is from blue (low temp) to red (high temp)
  float temp = (r / 255.0f) * (max_temp - min_temp) + min_temp;

  return temp;
}


// Function to get RGBA color and map it to temp
float get_pixel_temp(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
  // Calculate the temp based on RGBA
  float temp = map_rgba_to_temp(r, g, b);

  // Print the temp value
  // glog_trace("Pixel Temp.:%.2f°C\n", temp);

  return temp;
}


void get_bbox_temp(GstBuffer *buf, int obj_id)
{
  if (obj_id < 0)
    return;

  int count = 0;
  float temp_total = 0.0, temp_avg = 0.0;
  int x_start = 0, y_start = 0, width = 0, height = 0;
  float pixel_temp = 0;
  unsigned char r = 0, g = 0, b = 0, a = 0;
  NvBufSurface *surface = get_surface(buf);
  if (surface == NULL)
    return;
  
  x_start = (obj_info[THERMAL_CAM][obj_id].x);
  y_start = (obj_info[THERMAL_CAM][obj_id].y);
  width = obj_info[THERMAL_CAM][obj_id].width;
  height = obj_info[THERMAL_CAM][obj_id].height;
  // glog_trace("id=%d,x=%d,y=%d,width=%d,height=%d\n", obj_id, x_start, y_start, width, height);
  
  temp_total = 0.0;
  count = 0;
  // Process the motion vectors as needed
  for (int x = x_start; x < (x_start + width); ++x) {
      if (x % XY_DIVISOR != 0) continue;
      for (int y = y_start; y < (y_start + height); ++y) {
          if (y % XY_DIVISOR != 0) continue;
          get_pixel_color(surface, 0, x, y, &r, &g, &b, &a);
          pixel_temp = get_pixel_temp(r, g, b, a);
          if (pixel_temp < g_setting.threshold_under_temp || pixel_temp > g_setting.threshold_upper_temp)
            continue;
          temp_total += pixel_temp;
          count++;
      }
  }


  if (count > 0) {
    temp_avg = temp_total / (float)count;
    add_value_and_calculate_avg(&obj_info[THERMAL_CAM][obj_id], (int)temp_avg);
    // glog_trace("obj_id=%d,count=%d,temp_avg=%.1f, bbox_temp=%d\n", obj_id, count, temp_avg, obj_info[THERMAL_CAM][obj_id].bbox_temp);
  }
}


#if 1
// Function to update the display text for an object
void update_display_text(NvDsObjectMeta *obj_meta, const char *text) 
{
    // Check if the object meta is valid
    if (!obj_meta) {
        return;
    }
    // Set the display text
    strncpy(obj_meta->text_params.display_text, text, sizeof(obj_meta->text_params.display_text) - 1);
}
#endif

void temp_display_text(NvDsObjectMeta *obj_meta) 
{
  char display_text[100] = "", append_text[100] = "";
  if (obj_meta->object_id < 0)
    return;
  if (obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp < (g_setting.threshold_under_temp + g_setting.temp_diff_threshold))      //LJH, 20250410
    return;

  strcpy(display_text, obj_meta->text_params.display_text);  
  sprintf(append_text, "[%d°C]", obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp);
  strcat(display_text, append_text);  
  remove_newlines(display_text);
  if (obj_meta->text_params.display_text) {
    g_free(obj_meta->text_params.display_text);
    obj_meta->text_params.display_text = g_strdup(display_text);
  }
}


int objs_temp_avg = 0;
int objs_temp_total = 0; 
int objs_count = 0;

void init_temp_avg()
{
  objs_temp_avg = 0;
  objs_count = 0; 
  objs_temp_total = 0;
}


void get_temp_total(NvDsObjectMeta *obj_meta) 
{
  if (obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp < g_setting.threshold_under_temp)
    return;

  objs_temp_total += obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp; 
  objs_count++;
}

void get_temp_avg() 
{
  if (objs_count == 0 || objs_temp_total == 0) {
    objs_temp_avg = 0;
    return;
  }

  objs_temp_avg = objs_temp_total / objs_count; 
  // glog_trace("objs_temp_total=%d objs_count=%d objs_temp_avg=%d\n", objs_temp_total, objs_count, objs_temp_avg);
}

#endif

#if RESNET_50
static int pgie_probe_callback(NvDsObjectMeta *obj_meta) 
{
  // Bounding Box Coordinates
  float left = obj_meta->rect_params.left;
  float top = obj_meta->rect_params.top;
  float width = obj_meta->rect_params.width;
  float height = obj_meta->rect_params.height;
  glog_trace("Bounding Box: Left: %.2f, Top: %.2f, Width: %.2f, Height: %.2f\n", left, top, width, height);

  // Extract classification results
  for (NvDsMetaList *l_class = obj_meta->classifier_meta_list; l_class != NULL; l_class = l_class->next) {
      NvDsClassifierMeta *class_meta = (NvDsClassifierMeta *)(l_class->data);
      for (NvDsMetaList *l_label = class_meta->label_info_list; l_label != NULL; l_label = l_label->next) {
          NvDsLabelInfo *label_info = (NvDsLabelInfo *)(l_label->data);
          glog_trace("ResNet-50 Classification - Class ID: %d, Label: %s, Confidence: %.2f\n",
                  label_info->result_class_id, label_info->result_label, label_info->result_prob);
          if (label_info->result_class_id == 1 && label_info->result_prob >= g_setting.resnet50_threshold) {
            glog_trace("return CLASS_HEAT_COW\n");
            return CLASS_HEAT_COW;
          }
      }
  }
  // return GST_PAD_PROBE_OK;
  return CLASS_NORMAL_COW;
}
#endif

void remove_newline_text(NvDsObjectMeta *obj_meta) 
{
  char display_text[100] = "";

  if (obj_meta->object_id < 0)
    return;
  if (obj_meta->text_params.display_text[0] == 0)
    return;
  strcpy(display_text, obj_meta->text_params.display_text);  
  remove_newlines(display_text);
  if (obj_meta->text_params.display_text) {
    g_free(obj_meta->text_params.display_text);
    obj_meta->text_params.display_text = g_strdup(display_text);
  }
}

#if TEMP_NOTI
int is_temp_duration()
{
  for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++) {
    if (obj_info[THERMAL_CAM][obj_id].class_id == CLASS_OVER_TEMP && obj_info[THERMAL_CAM][obj_id].temp_duration > 0)
      return 1;
  }

  return 0;
}


void check_for_temp_notification()
{
  if (objs_temp_avg < g_setting.threshold_under_temp || objs_count == 0) {
    init_temp_avg();
    return;
  }

  for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++) {
    if (obj_info[THERMAL_CAM][obj_id].bbox_temp < g_setting.threshold_under_temp) {
      obj_info[THERMAL_CAM][obj_id].temp_duration = 0;
      obj_info[THERMAL_CAM][obj_id].class_id = CLASS_NORMAL_COW;
      continue;
    }

    if (obj_info[THERMAL_CAM][obj_id].bbox_temp > (objs_temp_avg + g_setting.temp_diff_threshold)) {
      obj_info[THERMAL_CAM][obj_id].temp_duration++;
      glog_trace("objs_temp_avg=%d obj_id=%d bbox_temp=%d temp_duration=%d\n", objs_temp_avg, obj_id, obj_info[THERMAL_CAM][obj_id].bbox_temp, obj_info[THERMAL_CAM][obj_id].temp_duration);
      if (obj_info[THERMAL_CAM][obj_id].temp_duration >= g_setting.over_temp_time) {   //if duration lasted more than designated time
        obj_info[THERMAL_CAM][obj_id].temp_duration = 0;
        if (obj_info[THERMAL_CAM][obj_id].temp_event_time_gap == 0){
          obj_info[THERMAL_CAM][obj_id].class_id = CLASS_OVER_TEMP;
          obj_info[THERMAL_CAM][obj_id].notification_flag = 1;                                          //send notification later
          glog_trace("objs_temp_avg=%d obj_id=%d notification_flag=1\n", objs_temp_avg, obj_id);
        }
        else {
          glog_trace("obj_info[THERMAL_CAM][%d].temp_event_time_gap=%d is less than TEMP_EVENT_TIME_GAP=%d\n", obj_id, obj_info[THERMAL_CAM][obj_id].temp_event_time_gap, TEMP_EVENT_TIME_GAP);
        }
      }
    }
    else {
      obj_info[THERMAL_CAM][obj_id].temp_duration = 0;
      obj_info[THERMAL_CAM][obj_id].class_id = CLASS_NORMAL_COW;
    }
  }

  init_temp_avg();
}

#endif


void simulate_get_temp_avg()
{
    for (int i = 0; i < NUM_OBJS; i++)
      obj_info[THERMAL_CAM][i].bbox_temp = 0;

    obj_info[THERMAL_CAM][1].bbox_temp = 20;
    obj_info[THERMAL_CAM][2].bbox_temp = 21;
    obj_info[THERMAL_CAM][3].bbox_temp = 38;
    
    objs_temp_avg = 0;
    objs_temp_avg += obj_info[THERMAL_CAM][1].bbox_temp;
    objs_temp_avg += obj_info[THERMAL_CAM][2].bbox_temp;
    objs_temp_avg += obj_info[THERMAL_CAM][3].bbox_temp;

    objs_temp_avg /= 3;
    objs_count = 3;
    // glog_trace("simulate objs_temp_avg=%d\n", objs_temp_avg);
}


void add_correction()
{
  if (g_setting.temp_correction == 0)
    return;

  for (int i = 0; i < NUM_OBJS; i++) {
    if (obj_info[THERMAL_CAM][i].corrected == 1){
      continue;
	  }

//    if ((obj_info[THERMAL_CAM][i].center_x < 320 || obj_info[THERMAL_CAM][i].center_x > 960) || obj_info[THERMAL_CAM][i].center_y < 180) 
    {
      obj_info[THERMAL_CAM][i].bbox_temp += g_setting.temp_correction;
      obj_info[THERMAL_CAM][i].corrected = 1;
    }
  }
}


void set_color(NvDsObjectMeta *obj_meta, int color, int set_text_blank)
{
  switch(color){
    case GREEN_COLOR:
      obj_meta->rect_params.border_color.red = 0.0;
      obj_meta->rect_params.border_color.green = 1.0;
      obj_meta->rect_params.border_color.blue = 0.0;
      obj_meta->rect_params.border_color.alpha = 1;
      break;
    case RED_COLOR:
      obj_meta->rect_params.border_color.red = 1.0;
      obj_meta->rect_params.border_color.green = 0.0;
      obj_meta->rect_params.border_color.blue = 0.0;
      obj_meta->rect_params.border_color.alpha = 1;
      break;
    case YELLO_COLOR:
      obj_meta->rect_params.border_color.red = 1.0;
      obj_meta->rect_params.border_color.green = 1.0;
      obj_meta->rect_params.border_color.blue = 0.0;
      obj_meta->rect_params.border_color.alpha = 1;
      break;
    case BLUE_COLOR:
      obj_meta->rect_params.border_color.red = 0.0;
      obj_meta->rect_params.border_color.green = 0.0;
      obj_meta->rect_params.border_color.blue = 1.0;
      obj_meta->rect_params.border_color.alpha = 1;
      break;
    case NO_BBOX:
      obj_meta->rect_params.border_color.red = 0.0;
      obj_meta->rect_params.border_color.green = 0.0;
      obj_meta->rect_params.border_color.blue = 0.0;
      obj_meta->rect_params.border_color.alpha = 0;
      obj_meta->text_params.display_text[0] = 0;      
      break;
  }

  if (set_text_blank) {
    obj_meta->text_params.display_text[0] = 0;      
  }
}


void set_temp_bbox_color(NvDsObjectMeta *obj_meta)
{
  if (obj_info[THERMAL_CAM][obj_meta->object_id].temp_duration > 0) {
    set_color(obj_meta, BLUE_COLOR, 0);
    // glog_trace("blue bbox obj_id=%d\n", obj_meta->object_id);
  }
}


/* osd_sink_pad_buffer_probe  will extract metadata received on OSD sink pad
 * and update params for drawing rectangle, object information etc. */
static GstPadProbeReturn osd_sink_pad_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer u_data)       //LJH, this function is called per frame 
{
  GstBuffer *buf = (GstBuffer *) info->data;
  NvDsObjectMeta *obj_meta = NULL;
  NvDsMetaList * l_frame = NULL;
  NvDsMetaList * l_obj = NULL;
  NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
  
  static float small_obj_diag[2] = {40.0, 40.0};
  static float big_obj_diag[2] = {1000.0, 1000.0};
  int cam_idx = *(int *)u_data;
  int sec_interval[NUM_CAMS] = {0};           //common one second interval for RGB and Thermal
#if TRACK_PERSON_INCLUDE
  static PersonObj object[NUM_OBJS];
  init_objects(object);
#else
  int event_class_id = CLASS_NORMAL_COW;
#endif
#if OPTICAL_FLOW_INCLUDE
  int start_obj_id = 0, obj_id = -1;
#endif
#if TEMP_NOTI_TEST
  cam_idx = THERMAL_CAM;
  g_source_cam_idx = cam_idx;
#endif
  static int do_temp_display = 0;

  g_cam_index = cam_idx;
  g_frame_count[cam_idx]++;
  // glog_trace("cam index = %d\n", cam_idx);  
  if (g_frame_count[cam_idx] >= PER_CAM_SEC_FRAME) {
    g_frame_count[cam_idx] = 0;
    sec_interval[cam_idx] = 1; 
  }

#if TEMP_NOTI    
  if (sec_interval[THERMAL_CAM]) {
    init_temp_avg();
  }
#endif    

  for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
    NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) (l_frame->data);
    for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      obj_meta = (NvDsObjectMeta *) (l_obj->data);
      obj_meta->rect_params.border_width = 1;
      obj_meta->text_params.set_bg_clr = 0;

      if (cam_idx == THERMAL_CAM) {
        obj_meta->text_params.font_params.font_size = 9;
      }
      set_obj_rect_id(cam_idx, obj_meta, sec_interval[cam_idx]);

      event_class_id = CLASS_NORMAL_COW;
#if TRACK_PERSON_INCLUDE
      set_person_obj_state(object, obj_meta);
#else
      if (obj_meta->class_id == CLASS_NORMAL_COW || obj_meta->class_id == CLASS_NORMAL_COW_SITTING) {
        if (obj_meta->confidence >= threshold_confidence[obj_meta->class_id]) {
          set_color(obj_meta, GREEN_COLOR, 0);
          // print_debug(obj_meta);
        }
        else {
          set_color(obj_meta, NO_BBOX, 0);
        }
        if (g_setting.show_normal_text == 0)
          obj_meta->text_params.display_text[0] = 0;      
      }
      else if (obj_meta->class_id == CLASS_HEAT_COW || obj_meta->class_id == CLASS_FLIP_COW || obj_meta->class_id == CLASS_LABOR_SIGN_COW) {
        set_color(obj_meta, RED_COLOR, 0);
        if (obj_meta->confidence >= threshold_confidence[obj_meta->class_id]) {
          event_class_id = obj_meta->class_id;
#if RESNET_50     
          if (g_setting.resnet50_apply) {
            if (event_class_id == CLASS_HEAT_COW && obj_meta->object_id >= 0) {
              if (pgie_probe_callback(obj_meta) == CLASS_HEAT_COW) {
                obj_info[cam_idx][obj_meta->object_id].heat_count++;
              }
            }
          }
#endif
          if (event_class_id == CLASS_HEAT_COW) {
            if (get_heat_color_over_threshold(cam_idx, obj_meta->object_id) == YELLO_COLOR) {
              set_color(obj_meta, YELLO_COLOR, 0);
            }
          }
          else if (event_class_id == CLASS_FLIP_COW) {
            if (get_flip_color_over_threshold(cam_idx, obj_meta->object_id) == YELLO_COLOR) {
              set_color(obj_meta, YELLO_COLOR, 0);
            }
          }
        }
        else {                                              //LJH, if lower than abnormalty threshold then green
          if (g_setting.show_normal_text == 0)
            set_color(obj_meta, GREEN_COLOR, 1);
          else
            set_color(obj_meta, GREEN_COLOR, 0);
        //glog_trace("id=%d yellow confidence=%f\n", obj_meta->object_id, obj_meta->confidence);     //LJH, for test
        }
        // glog_trace("abnormal id=%d class=%d text=%s confidence=%f\n", 
        // obj_meta->object_id, obj_meta->class_id, obj_meta->text_params.display_text, obj_meta->confidence);     //LJH, for test
        // print_debug(obj_meta);
      }
#if THERMAL_TEMP_INCLUDE
      if (g_setting.temp_apply) {
        if (cam_idx == THERMAL_CAM && obj_meta->object_id >= 0) {
          if (sec_interval[THERMAL_CAM]) {
            get_bbox_temp(buf, obj_meta->object_id);
            if (obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp > g_setting.threshold_under_temp) {
              // glog_trace("id=%d bbox_temp=%d\n", obj_meta->object_id, obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp);
              add_correction();        
  #if TEMP_NOTI
              get_temp_total(obj_meta);          //get temperature total before getting average
  #endif
            }
          }
          if (g_setting.display_temp || do_temp_display) {
            temp_display_text(obj_meta);
          }
          set_temp_bbox_color(obj_meta);     //if temperature is too high then set color      
        }
      }
#endif
      // glog_trace("obj_id=%d,class_id=%d\n", obj_meta->object_id, obj_meta->class_id);
      if (g_move_speed > 0) {             //if ptz is moving don't display bounding box
        set_color(obj_meta, NO_BBOX, 0);
      }
      else if (obj_meta->object_id >= 0 && (obj_info[cam_idx][obj_meta->object_id].diagonal < small_obj_diag[cam_idx] || 
            obj_info[cam_idx][obj_meta->object_id].diagonal > big_obj_diag[cam_idx])) {           //if bounding box is too small or too big don't display bounding box
        set_color(obj_meta, NO_BBOX, 0);
        //glog_trace("small||big [%d][%d].diagonal=%f\n", cam_idx, obj_meta->object_id, obj_info[cam_idx][obj_meta->object_id].diagonal);
      }
      remove_newline_text(obj_meta);
      //glog_trace("g_move_speed=%d id=%d text=%s\n", g_move_speed, obj_meta->object_id, obj_meta->text_params.display_text);     //LJH, for test
      if (cam_idx == g_source_cam_idx) {            //if cam index is identifical to the set source cam
        gather_event(event_class_id, obj_meta->object_id, cam_idx);
      }
#endif
    }

#if TEMP_NOTI_TEST
    simulate_get_temp_avg();                       //LJH, for simulation
#endif

    if (cam_idx == g_source_cam_idx){              //if cam index is identifical to the set source cam
      if (sec_interval[cam_idx]) {
        check_events_for_notification(cam_idx, 0);    
#if TEMP_NOTI
        if (g_setting.temp_apply) {
          if (cam_idx == THERMAL_CAM) {
            get_temp_avg();                        //get average temperature for objects in the screen
            check_for_temp_notification();
            do_temp_display = is_temp_duration();   //if over temp state is being counted for notification
          }
        }
#endif
      }
#if OPTICAL_FLOW_INCLUDE    
      if (g_setting.opt_flow_apply) {
        start_obj_id = 0;
        while ((obj_id = get_opt_flow_object(cam_idx, start_obj_id)) != -1) {
          process_opt_flow(frame_meta, cam_idx, obj_id, sec_interval[cam_idx]);     //if object is heat state, then check optical flow
          start_obj_id = (obj_id + 1) % NUM_OBJS;
        }
      }
#endif
      if (sec_interval[cam_idx]) {
        trigger_notification(cam_idx);
      }
    }
  }
#if TRACK_PERSON_INCLUDE           
  object_state = track_object(object_state, object);
#endif

  return GST_PAD_PROBE_OK;
}


void setup_nv_analysis()
{
  glog_trace("g_config.device_cnt=%d\n", g_config.device_cnt);
  static int index1, index2;

  for(int cam_idx = 0 ; cam_idx < g_config.device_cnt ; cam_idx++){
    GstPad *osd_sink_pad = NULL;
    char element_name[32];
    GstElement *nvosd;
    sprintf(element_name, "nvosd_%d", cam_idx+1);
    glog_trace ("element_name=%s\n", element_name);
    nvosd = gst_bin_get_by_name(GST_BIN (g_pipeline), element_name);
    if (nvosd == NULL){
      glog_error ("Fail get %s element\n", element_name);
      continue;
    }
    //g_object_set (G_OBJECT (nvosd), "display-text", 0, NULL);
    osd_sink_pad = gst_element_get_static_pad(nvosd, "sink");
    if (!osd_sink_pad){
      g_print ("Unable to get sink pad\n");
      return;
    }
    else {
      g_print("osd_sink_pad_buffer_probe cam_idx=%d\n", cam_idx);
      if (cam_idx == 0) {
        index1 = cam_idx;
        gst_pad_add_probe(osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER, osd_sink_pad_buffer_probe, &index1, NULL);   //RGB camera
      }
      else if (cam_idx == 1) {
        index2 = cam_idx;
        gst_pad_add_probe(osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER, osd_sink_pad_buffer_probe, &index2, NULL);   //Thermal camera
      }
    }
    gst_object_unref(osd_sink_pad);
  } 
  //start wait event thread .
  pthread_mutex_init(&g_notifier_mutex, NULL);                  
  pthread_create(&g_tid, NULL, process_notification, NULL);    
}


void endup_nv_analysis()
{
  if(g_tid){
    g_event_class_id = EVENT_EXIT;
    pthread_mutex_unlock(&g_notifier_mutex);
    
    pthread_join(g_tid, NULL);
  }
}

