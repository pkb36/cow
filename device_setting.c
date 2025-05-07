/* For json config */
#include <json-glib/json-glib.h>
#include <string.h>
#include "device_setting.h"
#include <stdio.h>
#include "g_log.h"
#include "serial_comm.h"
#include "nvds_process.h"

#if MINDULE_INCLUDE

gboolean load_ranch_setting(const char *file_name, RanchSetting* setting)
{
  JsonParser *parser;
  GError *error;
  JsonNode *root;
  JsonObject *object;   

  parser = json_parser_new();
  error = NULL;
  json_parser_load_from_file(parser, file_name, &error);
  if (error) {
    glog_trace("Unable to parse file '%s': %s\n", file_name, error->message);
    g_error_free(error);
    g_object_unref(parser);
    return FALSE;
  }

  JsonReader *reader = json_reader_new (json_parser_get_root (parser));
  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    g_object_unref (parser);
    return FALSE;
  }

  object = json_node_get_object (root);
  JsonArray *ranch_pos_array = json_object_get_array_member(object, "ranch_pos");
  guint array_size = json_array_get_length(ranch_pos_array);

  for (guint i = 0; i < array_size; ++i) {
      const gchar *pos = json_array_get_string_element(ranch_pos_array, i);
      glog_trace("Ranch Pos %d: %s\n", i, pos);

      unsigned char str_array[MAX_RANCH_POS+1] = {0};
      int data_len = parse_string_to_hex(pos, str_array, MAX_RANCH_POS+1);
      for( int j = 0 ; j < data_len ; j++){
        setting->ranch_pos[i][j] = str_array[j];
      }

      //업데이트 PTZ 설정 ...
      if (str_array[0]) 
        update_ranch_pos(i, &str_array[1], 1);
  }

  g_object_unref (reader);
  g_object_unref(parser);
    
  return TRUE;
}
#endif

void display_confid_duration()
{
  for (int i = CLASS_NORMAL_COW; i < NUM_CLASSES; i++) {
    switch(i) {
      case CLASS_NORMAL_COW:
          glog_trace("CLASS_NORMAL_COW");
          break;
      case CLASS_HEAT_COW:
          glog_trace("CLASS_HEAT_COW");
          break;
      case CLASS_FLIP_COW:
          glog_trace("CLASS_FLIP_COW");
          break;
      case CLASS_LABOR_SIGN_COW:
          glog_trace("CLASS_LABOR_SIGN_COW");
          break;
      case CLASS_NORMAL_COW_SITTING:
          glog_trace("CLASS_NORMAL_COW_SITTING");
          break;
    }
    glog_plain(" threshold:%.2f, duration:%d\n", threshold_confidence[i], threshold_event_duration[i]);
  }
}

gboolean load_device_setting(const char *file_name, DeviceSetting* setting)
{
   JsonParser *parser;
   GError *error;
   JsonNode *root;
   JsonObject *object;   

   parser = json_parser_new();
   error = NULL;
   json_parser_load_from_file(parser, file_name, &error);
   if (error) {
      glog_trace("Unable to parse file '%s': %s\n", file_name, error->message);
      g_error_free(error);
      g_object_unref(parser);
      return FALSE;
   }

   JsonReader *reader = json_reader_new (json_parser_get_root (parser));
   root = json_parser_get_root (parser);
    if (!JSON_NODE_HOLDS_OBJECT (root)) {
      g_object_unref (parser);
      return FALSE;
    }
  
  object = json_node_get_object (root);
  if (json_object_has_member (object, "color_platte")) {
      int value = json_object_get_int_member(object, "color_platte");
      glog_trace("parse member %s : %d\n", "color_platte", value);  
      setting->color_pallet = value;
  } else {
    setting->color_pallet = 0;
  }

  if (json_object_has_member (object, "record_status")) {
      int value = json_object_get_int_member (object, "record_status");
      glog_trace("parse member %s : %d\n", "record_status", value);  
      setting->record_status = value;
  } else {
    setting->record_status = 0;
  }

  if (json_object_has_member (object, "analysis_status")) {
      int value = json_object_get_int_member (object, "analysis_status");
      glog_trace("parse member %s : %d\n", "analysis_status", value);  
      setting->analysis_status = value;
  } else {
    setting->analysis_status = 0;
  }

  if (json_object_has_member (object, "auto_ptz_move_speed")) {
      int value = json_object_get_int_member (object, "auto_ptz_move_speed");
      glog_trace("parse member %s : %d\n", "auto_ptz_move_speed", value);  
      setting->auto_ptz_move_speed = value;
  } else {
    setting->auto_ptz_move_speed = 0x08;
  }

  if (json_object_has_member (object, "ptz_move_speed")) {
      int value = json_object_get_int_member (object, "ptz_move_speed");
      glog_trace("parse member %s : %d\n", "ptz_move_speed", value);  
      setting->ptz_move_speed = value;
  } else {
    setting->auto_ptz_move_speed = 0x10;
  }

  if (json_object_has_member (object, "enable_event_notify")) {
      int value = json_object_get_int_member (object, "enable_event_notify");
      glog_trace("parse member %s : %d\n", "enable_event_notify", value);  
      setting->enable_event_notify = value;
  } else {
    setting->enable_event_notify = 0;
  }

  if (json_object_has_member (object, "auto_ptz_seq")) {
      const char* value = json_object_get_string_member (object, "auto_ptz_seq");
      glog_trace("parse member %s : %s\n", "auto_ptz_seq", value);  
      strcpy(setting->auto_ptz_seq, value);
  } else {
    setting->auto_ptz_seq[0] = 0;
  }

  if (json_object_has_member (object, "camera_dn_mode")) {
      int value = json_object_get_int_member (object, "camera_dn_mode");
      glog_trace("parse member %s : %d\n", "camera_dn_mode", value);  
      setting->camera_dn_mode = value;
  } else {
    setting->camera_dn_mode = 0;
  } 

  JsonArray *ptz_preset_array = json_object_get_array_member(object, "ptz_preset");
  guint array_size = json_array_get_length(ptz_preset_array);
  for (guint i = 0; i < array_size; ++i) {
      const gchar *preset = json_array_get_string_element(ptz_preset_array, i);
      glog_trace("PTZ Preset %d: %s\n", i, preset);

      unsigned char str_array[PTZ_POS_SIZE+1] = {0};
      int data_len = parse_string_to_hex(preset, str_array, PTZ_POS_SIZE+1);
      for( int j = 0 ; j < data_len ; j++){
        setting->ptz_preset[i][j] = str_array[j];
      }

      //업데이트 PTZ 설정 ...
      if (str_array[0]) update_ptz_pos(i, &str_array[1], 0);
  }


  ptz_preset_array = json_object_get_array_member(object, "auto_ptz_preset");
  array_size = json_array_get_length(ptz_preset_array);
  for (guint i = 0; i < array_size; ++i) {
      const gchar *preset = json_array_get_string_element(ptz_preset_array, i);
      glog_trace("Auto PTZ Preset %d: %s\n", i, preset);

      unsigned char str_array[PTZ_POS_SIZE+1] = {0};
      int data_len = parse_string_to_hex(preset, str_array, PTZ_POS_SIZE+1);
      for( int j = 0 ; j < data_len ; j++){
        setting->auto_ptz_preset[i][j] = str_array[j];
      }

      //업데이트 PTZ 설정 ...
      if (str_array[0]) update_ptz_pos(i, &str_array[1], 1);
  }

  if (json_object_has_member (object, "nv_interval")) {
      int value = json_object_get_int_member (object, "nv_interval");
      glog_trace("parse member %s : %d\n", "nv_interval", value);  
      setting->nv_interval = value;
  } else {
      setting->nv_interval = 0;
  } 

  if (json_object_has_member (object, "opt_flow_threshold")) {
      int value = json_object_get_int_member (object, "opt_flow_threshold");
      glog_trace("parse member %s : %d\n", "opt_flow_threshold", value);  
      setting->opt_flow_threshold = value;
  } else {
      setting->opt_flow_threshold = 0;
  } 

  if (json_object_has_member (object, "heat_threshold")) {
      int value = json_object_get_int_member (object, "heat_threshold");
      glog_trace("parse member %s : %d\n", "heat_threshold", value);  
      setting->heat_threshold = value;
      threshold_confidence[CLASS_HEAT_COW] = (float)value/(float)100.0;
  } else {
    setting->heat_threshold = 0;
  } 

  if (json_object_has_member (object, "flip_threshold")) {
      int value = json_object_get_int_member (object, "flip_threshold");
      glog_trace("parse member %s : %d\n", "flip_threshold", value);  
      setting->flip_threshold = value;
      threshold_confidence[CLASS_FLIP_COW] = (float)value/(float)100.0;
  } else {
    setting->flip_threshold = 0;
  } 


  if (json_object_has_member (object, "normal_threshold")) {
    int value = json_object_get_int_member (object, "normal_threshold");
    glog_trace("parse member %s : %d\n", "normal_threshold", value);  
    setting->normal_threshold = value;
    threshold_confidence[CLASS_NORMAL_COW] = (float)value/(float)100.0;
  } else {
    setting->normal_threshold = 0;
  } 

  if (json_object_has_member (object, "labor_sign_threshold")) {
    int value = json_object_get_int_member (object, "labor_sign_threshold");
    glog_trace("parse member %s : %d\n", "labor_sign_threshold", value);  
    setting->labor_sign_threshold = value;
    threshold_confidence[CLASS_LABOR_SIGN_COW] = (float)value/(float)100.0;
  } else {
    setting->labor_sign_threshold = 0;
  } 

  if (json_object_has_member (object, "normal_sitting_threshold")) {
    int value = json_object_get_int_member (object, "normal_sitting_threshold");
    glog_trace("parse member %s : %d\n", "normal_sitting_threshold", value);  
    setting->normal_sitting_threshold = value;
    threshold_confidence[CLASS_NORMAL_COW_SITTING] = (float)value/(float)100.0;
  } else {
    setting->normal_sitting_threshold = 0;
  } 


  if (json_object_has_member (object, "display_temp")) {
      int value = json_object_get_int_member (object, "display_temp");
      glog_trace("parse member %s : %d\n", "display_temp", value);  
      setting->display_temp = value;
  } else {
    setting->display_temp = 0;
  } 


  if (json_object_has_member (object, "camera_index")) {
    int value = json_object_get_int_member (object, "camera_index");
    glog_trace("parse member %s : %d\n", "camera_index", value);  
    setting->camera_index = value;
    g_source_cam_idx = setting->camera_index;
  } else {
    setting->camera_index = 0;
  } 

  if (json_object_has_member (object, "resnet50_apply")) {
    int value = json_object_get_int_member (object, "resnet50_apply");
    glog_trace("parse member %s : %d\n", "resnet50_apply", value);  
    setting->resnet50_apply = value;
  } else {
    setting->resnet50_apply = 0;
  } 

  if (json_object_has_member (object, "resnet50_threshold")) {
    int value = json_object_get_int_member (object, "resnet50_threshold");
    glog_trace("parse member %s : %d\n", "resnet50_threshold", value);  
    setting->resnet50_threshold = value;
  } else {
    setting->resnet50_threshold = RESNET50_THRESHOLD_DEFAULT;
  } 


  if (json_object_has_member (object, "opt_flow_apply")) {
    int value = json_object_get_int_member (object, "opt_flow_apply");
    glog_trace("parse member %s : %d\n", "opt_flow_apply", value);  
    setting->opt_flow_apply = value;
  } else {
    setting->opt_flow_apply = 1;
    glog_trace("opt_flow_apply: %d\n", setting->opt_flow_apply);  
  } 


  if (json_object_has_member (object, "heat_time")) {
    int value = json_object_get_int_member (object, "heat_time");
    glog_trace("parse member %s : %d\n", "heat_time", value);  
    setting->heat_time = value;
    threshold_event_duration[CLASS_HEAT_COW] = value;
    glog_trace("threshold_event_duration[%d] : %d\n", CLASS_HEAT_COW, value);  
  } else {
    setting->heat_time = 15;
    glog_trace("threshold_event_duration[%d] : %d\n", CLASS_HEAT_COW, setting->heat_time);  
  } 

  if (json_object_has_member (object, "temp_diff_threshold")) {
    int value = json_object_get_int_member (object, "temp_diff_threshold");
    glog_trace("parse member %s : %d\n", "temp_diff_threshold", value);  
    setting->temp_diff_threshold = value;
    glog_trace("temp_diff_threshold: %d\n", value);  
  } else {
    setting->temp_diff_threshold = 7;
    glog_trace("temp_diff_threshold: %d\n", setting->temp_diff_threshold);  
  } 


  if (json_object_has_member (object, "over_temp_time")) {
    int value = json_object_get_int_member (object, "over_temp_time");
    glog_trace("parse member %s : %d\n", "over_temp_time", value);  
    setting->over_temp_time = value;
    glog_trace("over_temp_time: %d\n", value);  
  } else {
    setting->over_temp_time = 15;     
    glog_trace("over_temp_time: %d\n", setting->over_temp_time);  
  } 


  if (json_object_has_member (object, "temp_correction")) {
    int value = json_object_get_int_member (object, "temp_correction");
    glog_trace("parse member %s : %d\n", "temp_correction", value);  
    setting->temp_correction = value;
    glog_trace("temp_correction: %d\n", value);  
  } else {
    setting->temp_correction = 0;     
    glog_trace("temp_correction: %d\n", setting->temp_correction);  
  } 


  if (json_object_has_member (object, "temp_apply")) {
    int value = json_object_get_int_member (object, "temp_apply");
    glog_trace("parse member %s : %d\n", "temp_apply", value);  
    setting->temp_apply = value;
    glog_trace("temp_apply: %d\n", value);  
  } else {
    setting->temp_apply = 0;     
    glog_trace("temp_apply: %d\n", setting->temp_apply);  
  } 


  if (json_object_has_member (object, "threshold_upper_temp")) {
    int value = json_object_get_int_member (object, "threshold_upper_temp");
    glog_trace("parse member %s : %d\n", "threshold_upper_temp", value);  
    setting->threshold_upper_temp = value;
    glog_trace("threshold_upper_temp: %d\n", value);  
  } else {
    setting->threshold_upper_temp = THRESHOLD_UPPER_TEMP_DEFAULT;     
    glog_trace("threshold_upper_temp: %d\n", setting->threshold_upper_temp);  
  } 

  if (json_object_has_member (object, "threshold_under_temp")) {
    int value = json_object_get_int_member (object, "threshold_under_temp");
    glog_trace("parse member %s : %d\n", "threshold_under_temp", value);  
    setting->threshold_under_temp = value;
    glog_trace("threshold_under_temp: %d\n", value);  
  } else {
    setting->threshold_under_temp = THRESHOLD_UNDER_TEMP_DEFAULT;
    glog_trace("threshold_under_temp: %d\n", setting->threshold_under_temp);  
  } 


  if (json_object_has_member (object, "flip_time")) {
    int value = json_object_get_int_member (object, "flip_time");
    glog_trace("parse member %s : %d\n", "flip_time", value);  
    setting->flip_time = value;
    threshold_event_duration[CLASS_FLIP_COW] = value;
    glog_trace("threshold_event_duration[%d] : %d\n", CLASS_FLIP_COW, value);  
  } else {
    setting->flip_time = 15;
    glog_trace("threshold_event_duration[%d] : %d\n", CLASS_FLIP_COW, setting->flip_time);  
  } 


  if (json_object_has_member (object, "labor_sign_time")) {
    int value = json_object_get_int_member (object, "labor_sign_time");
    glog_trace("parse member %s : %d\n", "labor_sign_time", value);  
    setting->labor_sign_time = value;
    threshold_event_duration[CLASS_LABOR_SIGN_COW] = value;
    glog_trace("threshold_event_duration[%d] : %d\n", CLASS_LABOR_SIGN_COW, value);  
  } else {
    setting->labor_sign_time = 15;
    glog_trace("threshold_event_duration[%d] : %d\n", CLASS_LABOR_SIGN_COW, setting->labor_sign_time);  
  } 


  if (json_object_has_member (object, "show_normal_text")) {
    int value = json_object_get_int_member (object, "show_normal_text");
    glog_trace("parse member %s : %d\n", "show_normal_text", value);  
    setting->show_normal_text = value;
    glog_trace("show_normal_text: %d\n", value);  
  } else {
    setting->show_normal_text = 0;     
    glog_trace("show_normal_text: %d\n", setting->show_normal_text);  
  } 


  g_object_unref (reader);
  g_object_unref(parser);

  display_confid_duration();

  return TRUE;
}


void floatToString(float num, char *str) 
{
    // Use sprintf to convert the float to a string and store it in the provided char array
    sprintf(str, "%.6f", num); // You can adjust the precision as needed
}


static gchar device_setting_template_string_json[] = "{\n\
\"color_platte\": %d,\n\
\"record_status\": %d,\n\
\"analysis_status\": %d,\n\
\"auto_ptz_seq\": \"%s\",\n\
\"ptz_preset\": [\n\
%s\
],\n\
\"auto_ptz_preset\": [\n\
%s\
],\n\
\"auto_ptz_move_speed\": %d,\n\
\"ptz_move_speed\": %d,\n\
\"enable_event_notify\": %d,\n\
\"camera_dn_mode\": %d,\n\
\"nv_interval\": %d,\n\
\"opt_flow_threshold\": %d,\n\
\"opt_flow_apply\": %d,\n\
\"resnet50_threshold\": %d,\n\
\"resnet50_apply\": %d,\n\
\"normal_threshold\": %d,\n\
\"heat_threshold\": %d,\n\
\"flip_threshold\": %d,\n\
\"labor_sign_threshold\": %d,\n\
\"normal_sitting_threshold\": %d,\n\
\"display_temp\": %d,\n\
\"temp_diff_threshold\": %d,\n\
\"camera_index\": %d,\n\
\"heat_time\": %d,\n\
\"flip_time\": %d,\n\
\"labor_sign_time\": %d,\n\
\"over_temp_time\": %d,\n\
\"temp_correction\": %d,\n\
\"threshold_upper_temp\": %d,\n\
\"threshold_under_temp\": %d,\n\
\"temp_apply\": %d,\n\
\"show_normal_text\": %d\n\
}";


gboolean update_setting(const char *file_name, DeviceSetting* setting)
{
  //1. make PTZ code
  char ptz_code[(MAX_PTZ_PRESET + 2) * PTZ_POS_SIZE *4] = {0};
  char auto_ptz_code[(MAX_PTZ_PRESET + 2) * PTZ_POS_SIZE*4] = {0};
  char temp[8];
  for(int i = 0 ; i < MAX_PTZ_PRESET ; i++){
    sprintf(temp, "\"%02x",setting->ptz_preset[i][0]);
    strcat(ptz_code,temp);
    for(int j = 1 ; j < PTZ_POS_SIZE ; j++){
      sprintf(temp, ",%02x",setting->ptz_preset[i][j]);
      strcat(ptz_code,temp);
    }
    if (i < MAX_PTZ_PRESET-1 ) 
      strcat(ptz_code,"\",\n");
    else 
      strcat(ptz_code,"\"\n");
  }

  for(int i = 0 ; i < MAX_PTZ_PRESET ; i++){
    sprintf(temp, "\"%02x",setting->auto_ptz_preset[i][0]);
    strcat(auto_ptz_code,temp);
    for(int j = 1 ; j < PTZ_POS_SIZE ; j++){
      sprintf(temp, ",%02x",setting->auto_ptz_preset[i][j]);
      strcat(auto_ptz_code,temp);
    }
    if (i < MAX_PTZ_PRESET-1 ) 
      strcat(auto_ptz_code,"\",\n");
    else 
      strcat(auto_ptz_code,"\"\n");
  }
    
  //2. update config
  FILE *fp = fopen(file_name, "w+t, ccs=UTF-8");
  if(fp == NULL){
    glog_trace("fail open device setting  %s \n", file_name);  
    return FALSE;
  }

  fprintf(fp,device_setting_template_string_json,
    setting->color_pallet, 
    setting->record_status, 
    setting->analysis_status, 
    setting->auto_ptz_seq, 
    ptz_code, 
    auto_ptz_code, 
    setting->auto_ptz_move_speed, 
    setting->ptz_move_speed, 
    setting->enable_event_notify, 
    setting->camera_dn_mode, 
    setting->nv_interval, 
    setting->opt_flow_threshold, 
    setting->opt_flow_apply,
    setting->resnet50_threshold, 
    setting->resnet50_apply,
    setting->normal_threshold, 
    setting->heat_threshold, 
    setting->flip_threshold, 
    setting->labor_sign_threshold, 
    setting->normal_sitting_threshold, 
    setting->display_temp, 
    setting->temp_diff_threshold, 
    setting->camera_index,
    setting->heat_time,
    setting->flip_time,
    setting->labor_sign_time,
    setting->over_temp_time,
    setting->temp_correction,
    setting->threshold_upper_temp,
    setting->threshold_under_temp,
    setting->temp_apply,
    setting->show_normal_text
  );

  fclose(fp);

  return TRUE; 
}


#ifdef TEST_SETTING
int main(int argc, char *argv[])
{
  DeviceSetting setting ={};
  load_device_setting("device_setting.json", &setting);

  glog_trace("color_pallet  %d \n", setting.color_pallet);  
  glog_trace("record_status  %d \n", setting.record_status);  

  for(int i = 0 ; i < MAX_PTZ_PRESET ; i++){
    glog_trace("ptz_preset  %d : %d\n", i , setting.ptz_preset[i][0]);  
  }

/*
  char ptz_code[360];
  char temp[8];
  for(int i = 0 ; i < MAX_PTZ_PRESET ; i++){
    sprintf(temp, "\"%02x",setting.ptz_preset[i][0]);
    strcat(ptz_code,temp);
    for(int j = 1 ; j < 11 ; j++){
      sprintf(temp, ",%02x",setting.ptz_preset[i][j]);
      strcat(ptz_code,temp);
    }
    if (i < MAX_PTZ_PRESET-1 ) 
      strcat(ptz_code,"\",\n");
    else 
      strcat(ptz_code,"\"\n");
  }
  glog_trace("ptz_code \n  %s ", ptz_code);  
  */

 update_setting("device_setting_2.json", &setting);

 DeviceSetting setting2 ={};  
 load_device_setting("device_setting_2.json", &setting2);
 glog_trace("color_pallet  %d \n", setting2.color_pallet);  
 glog_trace("record_status  %d \n", setting2.record_status);  


}
#endif


