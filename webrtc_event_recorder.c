/*
 * Demo gstreamer app for negotiating and streaming a sendrecv audio-only webrtc
 * stream to all the peers in a multiparty room.
 *
 * gcc mp-webrtc-sendrecv.c $(pkg-config --cflags --libs gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0) -o mp-webrtc-sendrecv
 *
 * Author: Nirbheek Chauhan <nirbheek@centricular.com>
 */
#include <gst/gst.h>
#include <gst/sdp/sdp.h>

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <stdint.h>
#include "g_log.h"

static GMainLoop *main_loop;
static GstElement *pipeline;

static int g_stream_cnt;
static int g_stream_base_port;
static int g_comm_port;
static char* g_codec_name;
static char g_rtp_depay_name[64];

static char* g_location;
static int g_duration;


static GOptionEntry entries[] = {
  {"stream_cnt", 0, 0, G_OPTION_ARG_INT, &g_stream_cnt, "stream_cnt", NULL},
  {"stream_base_port", 0, 0, G_OPTION_ARG_INT, &g_stream_base_port, "stream_port", NULL},
  {"codec_name", 0, 0, G_OPTION_ARG_STRING, &g_codec_name, "codec_name", NULL},
  {"location", 0, 0, G_OPTION_ARG_STRING, &g_location, "store path", NULL},
  {"duration", 0, 0, G_OPTION_ARG_INT, &g_duration, "duratio (second)", NULL},
  {NULL}
};

static int first_call = 1;

gchar* formatted_file_saving_handler(GstChildProxy *splitmux, guint fragment_id, gpointer user_data){
  if(first_call){
    glog_trace("start record [%s]\n", g_location);
    first_call = 0;
  } else {
    glog_trace("end record [%s]\n", g_location);
    exit(0);
  }
  return g_location;
} 


static gboolean
start_pipeline (void)
{
  GstStateChangeReturn ret;
  GError *error = NULL;
  
  /* NOTE: webrtcbin currently does not support dynamic addition/removal of
   * streams, so we use a separate webrtcbin for each peer, but all of them are
   * inside the same pipeline. We start by connecting it to a fakesink so that
   * we can preroll early. */

  char str_pipeline[2048] = {0,};
  char str_video[512];
  for( int i = 0 ; i< g_stream_cnt ;i++){
    snprintf(str_video, 512, 
      "udpsrc port=%d ! queue ! application/x-rtp,media=video,clock-rate=90000,encoding-name=%s, payload=96  ! %s ! splitmuxsink location=%s name=recorder%d max-size-time=%ld muxer=webmmux ",
        g_stream_base_port + i, g_codec_name, g_rtp_depay_name, g_location, i ,  (__int64_t)g_duration*1000000000); 
    strcat(str_pipeline, str_video);
  }
  
  glog_trace("%lu  %s\n", strlen(str_pipeline), str_pipeline);
  pipeline = gst_parse_launch (str_pipeline, &error);
  if (error) {
    glog_error ("Failed to parse launch: %s\n", error->message);
    g_error_free (error);
    goto err;
  }

  static int cam_idx[2] =  {0,1}; 
  for( int i = 0 ; i< g_stream_cnt ;i++){
    char str_element_name[256];
    sprintf(str_element_name, "recorder%d", i);
    GstElement *recorder = gst_bin_get_by_name (GST_BIN (pipeline), str_element_name);
    g_assert_nonnull (recorder);
    if(recorder){
      g_signal_connect_data( recorder, "format-location", G_CALLBACK (formatted_file_saving_handler), &cam_idx[i], NULL, 0);
    } else {
      return FALSE;
    }
    g_clear_object (&recorder);
  }
  
  glog_trace ("Starting pipeline\n");
  ret = gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto err;

  return TRUE;

err:
  if (pipeline)
    g_clear_object (&pipeline);
  return FALSE;
}


int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;
  
  context = g_option_context_new ("- gstreamer webrtc event recorder ");
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_add_group (context, gst_init_get_option_group ());
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    glog_error ("Error initializing: %s\n", error->message);
    return -1;
  }

  glog_trace("start stream_port[%d], stream_cnt[%d], codec_name[%s], location [%s]  duration[%d] \n", 
       g_stream_base_port, g_stream_cnt, g_codec_name, g_location, g_duration);
  
  if (strcmp("VP9",g_codec_name) == 0){
    strcpy(g_rtp_depay_name,"rtpvp9depay");
  } else if(strcmp("VP8",g_codec_name) == 0){
    strcpy(g_rtp_depay_name,"rtpvp8depay");
  } else if(strcmp("H264",g_codec_name) == 0){
    strcpy(g_rtp_depay_name,"rtph264depay");
  } else {
    glog_error ("Wrong Codec : %s\n", g_codec_name);
    return  -1;
  }

  //1. start webrtc
  start_pipeline();
  
  main_loop = g_main_loop_new (NULL, FALSE);
  // guint timeout_duration = g_duration*1000;
  // g_timeout_add(timeout_duration, timeout_callback, NULL);

  g_main_loop_run (main_loop);
  glog_trace ("Pipeline stopped end recorder [%d] \n", g_comm_port);
  
  // gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
  // gst_object_unref (pipeline);

  return 0;
}
