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
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>
#include <stdio.h>

#include "socket_comm.h"
#define USE_JSON_MESSAGE_TEMPLATE
#include "json_utils.h"
#include "g_log.h"

static GMainLoop *loop;
static GstElement *pipeline, *webrtc = NULL;


static SOCKETINFO* g_socket = NULL;

static int g_stream_cnt;
static int g_stream_base_port;
static int g_comm_port;
static char* g_codec_name;
static char* peer_id;


static GOptionEntry entries[] = {
  {"stream_cnt", 0, 0, G_OPTION_ARG_INT, &g_stream_cnt, "stream_cnt", NULL},
  {"stream_base_port", 0, 0, G_OPTION_ARG_INT, &g_stream_base_port, "stream_port", NULL},
  {"comm_socket_port", 0, 0, G_OPTION_ARG_INT, &g_comm_port, "comm_socket_port", NULL},
  {"codec_name", 0, 0, G_OPTION_ARG_STRING, &g_codec_name, "codec_name", NULL},
  {"peer_id", 0, 0, G_OPTION_ARG_STRING, &peer_id, "String ID of the peer to connect to", "ID"},
  {NULL}
};

enum AppState
{
  APP_STATE_UNKNOWN = 0,
  APP_STATE_ERROR = 1,          /* generic error */
  SERVER_CONNECTING = 1000,
  SERVER_CONNECTION_ERROR,
  SERVER_CONNECTED,             /* Ready to register */
  SERVER_REGISTERING = 2000,
  SERVER_REGISTRATION_ERROR,
  SERVER_REGISTERED,            /* Ready to call a peer */
  SERVER_CLOSED,                /* server connection closed by us or the server */
  ROOM_JOINING = 3000,
  ROOM_JOIN_ERROR,
  ROOM_JOINED,
  ROOM_CALL_NEGOTIATING = 4000, /* negotiating with some or all peers */
  ROOM_CALL_OFFERING,           /* when we're the one sending the offer */
  ROOM_CALL_ANSWERING,          /* when we're the one answering an offer */
  ROOM_CALL_STARTED,            /* in a call with some or all peers */
  ROOM_CALL_STOPPING,
  ROOM_CALL_STOPPED,
  ROOM_CALL_ERROR,
};
static enum AppState app_state = 0;


static gboolean
cleanup_and_quit_loop (const gchar * msg, enum AppState state)
{
  if (msg)
    glog_error ("%s\n", msg);
  if (state > 0)
    app_state = state;

  if (loop) {
    g_main_loop_quit (loop);
    loop = NULL;
  }

  /* To allow usage as a GSourceFunc */
  return G_SOURCE_REMOVE;
}

static gchar *
get_string_from_json_object (JsonObject * object)
{
  JsonNode *root;
  JsonGenerator *generator;
  gchar *text;

  /* Make it the root node */
  root = json_node_init_object (json_node_alloc (), object);
  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  text = json_generator_to_data (generator, NULL);

  /* Release everything */
  g_object_unref (generator);
  json_node_free (root);
  return text;
}


static void
send_room_peer_msg (const gchar * action, const gchar * peer_id, const gchar * message_key, const gchar * message_value)
{
 
  gchar *msg;
  msg = g_strdup_printf (json_msssage_template, action, peer_id, message_key ,message_value);
  //glog_trace("send_room_peer_msg [%s]\n", msg);
  send_data_socket_comm(g_socket, msg, strlen(msg), 1);
  g_free (msg);
}

static int first_send_ice = TRUE;
static void
send_ice_candidate_message (GstElement * webrtc G_GNUC_UNUSED, guint mlineindex,
    gchar * candidate, const gchar * data)
{
  gchar *text;
  JsonObject *ice;

  if (app_state < ROOM_CALL_OFFERING) {
    cleanup_and_quit_loop ("Can't send ICE, not in call", APP_STATE_ERROR);
    return;
  }
  
  //먼저 SDP을 보내고 충분히 여유를 가지고 ICE을 전달하여 상대방 peer가 ICE을 준비할 수 있는 시간을 준다.
  //async하게 동작하는 시그널 서버에 의해서.... 문제가 발생하는 경우가 있음.
  //참조 : https://velog.io/@njw1204/WebRTC-%EA%B0%84%ED%97%90%EC%A0%81-%EC%97%B0%EA%B2%B0-%EC%8B%A4%ED%8C%A8-%EB%AC%B8%EC%A0%9C-%ED%95%B4%EA%B2%B0
  if(first_send_ice){
    usleep(1);
    first_send_ice = FALSE;
  }

  ice = json_object_new ();
  json_object_set_string_member (ice, "candidate", candidate);
  json_object_set_int_member (ice, "sdpMLineIndex", mlineindex);
  // msg = json_object_new ();
  // json_object_set_object_member (msg, "ice", ice);
  text = get_string_from_json_object (ice);
  json_object_unref (ice);

  send_room_peer_msg ("candidate", peer_id, "ice", text);
  g_free (text);
}

static void
send_room_peer_sdp (GstWebRTCSessionDescription * desc, const gchar * peer_id)
{
  JsonObject *sdp;
  gchar *text, *sdptype, *sdptext;

  g_assert_cmpint (app_state, >=, ROOM_CALL_OFFERING);

  if (desc->type == GST_WEBRTC_SDP_TYPE_OFFER)
    sdptype = "offer";
  else if (desc->type == GST_WEBRTC_SDP_TYPE_ANSWER)
    sdptype = "answer";
  else
    g_assert_not_reached ();

  text = gst_sdp_message_as_text (desc->sdp);
  glog_trace ("Sending sdp %s to %s:\n%s\n", sdptype, peer_id, text);

  sdp = json_object_new ();
  json_object_set_string_member (sdp, "type", sdptype);
  json_object_set_string_member (sdp, "sdp", text);
  g_free (text);

  // msg = json_object_new ();
  // json_object_set_object_member (msg, "sdp", sdp);
  sdptext = get_string_from_json_object (sdp);
  json_object_unref (sdp);
  
  send_room_peer_msg (sdptype, peer_id, "sdp", sdptext);
  g_free (sdptext);
}

/* Offer created by our pipeline, to be sent to the peer */
static void
on_offer_created (GstPromise * promise, GstElement * webrtc)
{
  GstWebRTCSessionDescription *offer;
  const GstStructure *reply;

  g_assert_cmpint (app_state, ==, ROOM_CALL_OFFERING);

  g_assert_cmpint (gst_promise_wait (promise), ==, GST_PROMISE_RESULT_REPLIED);
  reply = gst_promise_get_reply (promise);
  gst_structure_get (reply, "offer",
      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
  gst_promise_unref (promise);

  promise = gst_promise_new ();
  g_signal_emit_by_name (webrtc, "set-local-description", offer, promise);
  gst_promise_interrupt (promise);
  gst_promise_unref (promise);

  /* Send offer to peer */
  send_room_peer_sdp (offer, peer_id);
  gst_webrtc_session_description_free (offer);
}

static void
on_negotiation_needed (GstElement * webrtc, const gchar * data)
{
  GstPromise *promise;
  app_state = ROOM_CALL_OFFERING;
  glog_trace ("[%s] on_negotiation_needed called \n", peer_id);

  promise = gst_promise_new_with_change_func (
      (GstPromiseChangeFunc) on_offer_created, (gpointer) webrtc, NULL);
  g_signal_emit_by_name (webrtc, "create-offer", NULL, promise);
}

static void
on_ice_gathering_state_notify (GstElement * webrtcbin, GParamSpec * pspec, gpointer data)
{
  GstWebRTCICEGatheringState ice_gather_state;
  const gchar *new_state = "unknown";
  
  g_object_get (webrtcbin, "ice-gathering-state", &ice_gather_state, NULL);
  switch (ice_gather_state) {
    case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
      new_state = "new";
      break;
    case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
      new_state = "gathering";
      break;
    case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
      new_state = "complete";
      break;
  }
  glog_trace ("[%s] ICE gathering state changed to %s \n", peer_id, new_state);

}

#if 0
static gboolean
bus_msg (GstBus * bus, GstMessage * msg, gpointer data)       //LJH, this code is for debugging
{
  GstPipeline *pipeline = data;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:{
      GError *err;
      gchar *dbg;

      gst_message_parse_error (msg, &err, &dbg);
      glog_error ("SLAVE: ERROR: %s\n", err->message);
      if (dbg != NULL)
        glog_error ("SLAVE: ERROR debug information: %s\n", dbg);
      g_error_free (err);
      g_free (dbg);

      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline),
          GST_DEBUG_GRAPH_SHOW_ALL, "ipc.slave.error");
      break;
    }
    case GST_MESSAGE_WARNING:{
      GError *err;
      gchar *dbg;

      gst_message_parse_warning (msg, &err, &dbg);
      glog_error ("SLAVE: WARNING: %s\n", err->message);
      if (dbg != NULL)
        glog_error ("SLAVE: WARNING debug information: %s\n", dbg);
      g_error_free (err);
      g_free (dbg);

      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline),
          GST_DEBUG_GRAPH_SHOW_ALL, "ipc.slave.warning");
      break;
    }
    case GST_MESSAGE_ASYNC_START:
    glog_trace ("GST_MESSAGE_ASYNC_START\n");
      // GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline),
      //     GST_DEBUG_GRAPH_SHOW_VERBOSE, "ipc.slave.async-start");
      break;
    case GST_MESSAGE_ASYNC_DONE:
    glog_trace ("GST_MESSAGE_ASYNC_DONE\n");
      // GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline),
      //     GST_DEBUG_GRAPH_SHOW_ALL, "ipc.slave.async-done");
      break;

   case GST_MESSAGE_STATE_CHANGED:
        /* We are only interested in state-changed messages from the pipeline */
        //if (GST_MESSAGE_SRC (msg) == GST_OBJECT (pipeline)) {
          {
          GstState old_state, new_state, pending_state;
          gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
          glog_trace ("object [%s] : state changed from %s to %s:\n",GST_MESSAGE_SRC_NAME (msg),
              gst_element_state_get_name (old_state), gst_element_state_get_name (new_state));
              }
        //}
        break;      
    default:
      break;
  }
  return TRUE;
}
#endif

static gboolean start_pipeline (void)
{
  GstStateChangeReturn ret;
  GError *error = NULL;
  
  /* NOTE: webrtcbin currently does not support dynamic addition/removal of
   * streams, so we use a separate webrtcbin for each peer, but all of them are
   * inside the same pipeline. We start by connecting it to a fakesink so that
   * we can preroll early. */

  char str_pipeline[2048] = {0,};
  char str_video[512];

  strcpy(str_pipeline, "webrtcbin stun-server=stun://stun.l.google.com:19302 name=sender ");
  for( int i = 0 ; i< g_stream_cnt ;i++){
    snprintf(str_video, 512, 
      "udpsrc port=%d ! queue ! application/x-rtp,media=video,encoding-name=%s,payload=96 !  sender. ", g_stream_base_port + i, g_codec_name); 
    strcat(str_pipeline, str_video);
  }
  glog_trace("%lu  %s\n", strlen(str_pipeline), str_pipeline);
  pipeline = gst_parse_launch (str_pipeline, &error);

  if (error) {
    glog_error ("Failed to parse launch: %s\n", error->message);
    g_error_free (error);
    goto err;
  }

  webrtc = gst_bin_get_by_name (GST_BIN (pipeline), "sender");
  g_assert_nonnull (webrtc);

  //gst_bus_add_watch (GST_ELEMENT_BUS (pipeline), bus_msg, pipeline);

  /* This is the gstwebrtc entry point where we create the offer and so on. It
   * will be called when the pipeline goes to PLAYING. */
  g_signal_connect (webrtc, "on-negotiation-needed",
      G_CALLBACK (on_negotiation_needed), NULL);
  /* We need to transmit this ICE candidate to the browser via the websockets
   * signalling server. Incoming ice candidates from the browser need to be
   * added by us too, see on_server_message() */
  g_signal_connect (webrtc, "on-ice-candidate",
      G_CALLBACK (send_ice_candidate_message), NULL);
  g_signal_connect (webrtc, "notify::ice-gathering-state",
      G_CALLBACK (on_ice_gathering_state_notify), NULL);

  gst_element_set_state (pipeline, GST_STATE_READY);

  glog_trace ("Starting pipeline\n");
  ret = gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto err;

  return TRUE;

err:
  if (pipeline)
    g_clear_object (&pipeline);
  if (webrtc)
    webrtc = NULL;
  return FALSE;
}

#if 0                 //LJH, not used
static gboolean play_pipeline (void)          
{
  GstStateChangeReturn ret;
  GError *error = NULL;
  GstElement *udpsrc ;

  /* NOTE: webrtcbin currently does not support dynamic addition/removal of
   * streams, so we use a separate webrtcbin for each peer, but all of them are
   * inside the same pipeline. We start by connecting it to a fakesink so that
   * we can preroll early. */

  char str_pipeline[2048] = {0,};
  char str_video[512];
  for( int i = 0 ; i< g_stream_cnt ;i++){
    snprintf(str_video, 512, 
      "udpsrc port=%d name=udpsrc%d ! queue ! rtpvp8depay ! vp8dec !  queue  !  autovideosink  ", g_stream_base_port + i, i); 
      strcat(str_pipeline, str_video);
    //snprintf(str_pipeline, 512,  "videotestsrc ! autovideosink  "); 
  }
  
  glog_trace("%lu  %s\n", strlen(str_pipeline), str_pipeline);
  pipeline = gst_parse_launch (str_pipeline, &error);
  if (error) {
    glog_error ("Failed to parse launch: %s\n", error->message);
    g_error_free (error);
    goto err;
  }
  //gst_bus_add_watch (GST_ELEMENT_BUS (pipeline), bus_msg, pipeline);

  char sztemp[32];
  for( int i = 0 ; i< g_stream_cnt ;i++){
    snprintf(sztemp, 32, "udpsrc%d",i);
    udpsrc = gst_bin_get_by_name (GST_BIN (pipeline), sztemp);
    GstCaps *caps = gst_caps_new_simple("application/x-rtp",  "media", G_TYPE_STRING, "video",NULL);
    g_object_set(udpsrc, "caps", caps, NULL);
    gst_caps_unref(caps);
    g_assert_nonnull (udpsrc);
  }
  
  ret = gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto err;

  glog_trace ("Starting pipeline\n");
  return TRUE;

err:
  if (pipeline)
    g_clear_object (&pipeline);

  return FALSE;
}
#endif

static void
on_answer_created (GstPromise * promise, const gchar * peer_id)
{
  GstWebRTCSessionDescription *answer;
  const GstStructure *reply;

  g_assert_cmpint (app_state, ==, ROOM_CALL_ANSWERING);

  g_assert_cmpint (gst_promise_wait (promise), ==, GST_PROMISE_RESULT_REPLIED);
  reply = gst_promise_get_reply (promise);
  gst_structure_get (reply, "answer",
      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
  gst_promise_unref (promise);

  promise = gst_promise_new ();
  g_signal_emit_by_name (webrtc, "set-local-description", answer, promise);
  gst_promise_interrupt (promise);
  gst_promise_unref (promise);

  /* Send offer to peer */
  send_room_peer_sdp (answer, peer_id);
  gst_webrtc_session_description_free (answer);

  app_state = ROOM_CALL_STARTED;
}


static void
handle_sdp_offer (const gchar * peer_id, const gchar * text)
{
  int ret;
  GstPromise *promise;
  GstSDPMessage *sdp;
  GstWebRTCSessionDescription *offer;

  g_assert_cmpint (app_state, ==, ROOM_CALL_ANSWERING);

  glog_trace ("Received offer:\n%s\n", text);

  ret = gst_sdp_message_new (&sdp);
  g_assert_cmpint (ret, ==, GST_SDP_OK);

  ret = gst_sdp_message_parse_buffer ((guint8 *) text, strlen (text), sdp);
  g_assert_cmpint (ret, ==, GST_SDP_OK);

  offer = gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_OFFER, sdp);
  g_assert_nonnull (offer);

  /* Set remote description on our pipeline */
  promise = gst_promise_new ();
  g_signal_emit_by_name (webrtc, "set-remote-description", offer, promise);
  /* We don't want to be notified when the action is done */
  gst_promise_interrupt (promise);
  gst_promise_unref (promise);

  /* Create an answer that we will send back to the peer */
  promise = gst_promise_new_with_change_func (
      (GstPromiseChangeFunc) on_answer_created, (gpointer) peer_id, NULL);
  g_signal_emit_by_name (webrtc, "create-answer", NULL, promise);

  gst_webrtc_session_description_free (offer);
  gst_object_unref (webrtc);
}


static void
handle_sdp_answer (const gchar * peer_id, const gchar * text)
{
  int ret;
  GstPromise *promise;
  GstSDPMessage *sdp;
  GstWebRTCSessionDescription *answer;

  g_assert_cmpint (app_state, >=, ROOM_CALL_OFFERING);

  glog_trace ("Received answer:\n%s\n", text);

  ret = gst_sdp_message_new (&sdp);
  g_assert_cmpint (ret, ==, GST_SDP_OK);

  ret = gst_sdp_message_parse_buffer ((guint8 *) text, strlen (text), sdp);
  g_assert_cmpint (ret, ==, GST_SDP_OK);

  answer = gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
  g_assert_nonnull (answer);

  /* Set remote description on our pipeline */
  promise = gst_promise_new ();
  g_signal_emit_by_name (webrtc, "set-remote-description", answer, promise);
  
  /* We don't want to be notified when the action is done */
  gst_promise_interrupt (promise);
  gst_promise_unref (promise);
}


static void handle_peer_message (gchar * msg, int len, void* arg)
{ 
  //Check Exit Message
  if(strcmp(msg, "ENDUP") == 0){
    glog_trace ("ENDUP Message ... Exit program %s\n",  msg);  
    exit(0); 
    return ;
  }
  
  JsonNode *root;
  JsonObject *object, *child;
  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, msg, -1, NULL)) {
    glog_error ("Unknown message '%s' from '%s', ignoring", msg, peer_id);
    g_object_unref (parser);
    //return FALSE;
  }

  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    glog_error ("Unknown json message '%s' from '%s', ignoring", msg,
        peer_id);
    g_object_unref (parser);
    //return FALSE;
  }

  glog_trace ("Message from peer %s: %s\n", peer_id, msg);

  object = json_node_get_object (root);
  /* Check type of JSON message */
  if (json_object_has_member (object, "sdp")) {
    const gchar *text, *sdp_type;

    g_assert_cmpint (app_state, >=, ROOM_JOINED);

    child = json_object_get_object_member (object, "sdp");

    if (!json_object_has_member (child, "type")) {
      cleanup_and_quit_loop ("ERROR: received SDP without 'type'",
          ROOM_CALL_ERROR);
      //return FALSE;
    }

    sdp_type = json_object_get_string_member (child, "type");
    text = json_object_get_string_member (child, "sdp");

    if (g_strcmp0 (sdp_type, "offer") == 0) {
      app_state = ROOM_CALL_ANSWERING;
      handle_sdp_offer (peer_id, text);
    } else if (g_strcmp0 (sdp_type, "answer") == 0) {
      g_assert_cmpint (app_state, >=, ROOM_CALL_OFFERING);
      handle_sdp_answer (peer_id, text);
      app_state = ROOM_CALL_STARTED;
    } else {
      cleanup_and_quit_loop ("ERROR: invalid sdp_type", ROOM_CALL_ERROR);
      //return FALSE;
    }
  } else if (json_object_has_member (object, "ice")) {
    const gchar *candidate;
    gint sdpmlineindex;

    child = json_object_get_object_member (object, "ice");
    candidate = json_object_get_string_member (child, "candidate");
    sdpmlineindex = json_object_get_int_member (child, "sdpMLineIndex");

    /* Add ice candidate sent by remote peer */
    g_signal_emit_by_name (webrtc, "add-ice-candidate", sdpmlineindex, candidate);
  } else {
    glog_error ("Ignoring unknown JSON message:\n%s\n", msg);
  }
  g_object_unref (parser);
  //return TRUE;
}


int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;

  context = g_option_context_new ("- gstreamer webrtc sender ");
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_add_group (context, gst_init_get_option_group ());
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    glog_error ("Error initializing: %s\n", error->message);
    return -1;
  }

  glog_trace("start peer_id : [%s], comm_port[%d], stream_port[%d], stream_cnt[%d] codec_name[%s]\n", 
      peer_id, g_comm_port, g_stream_base_port, g_stream_cnt, g_codec_name);

  //1. connect socket 
  g_socket = init_socket_comm_client(g_comm_port);
  send_data_socket_comm(g_socket, "CONNECT", 8, 1);
  g_socket->call_fun = handle_peer_message;

  //2. start webrtc
  start_pipeline();
  
  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
  glog_trace ("Pipeline stopped end client [%d] \n", g_comm_port);

  gst_object_unref (pipeline);
  return 0;
}
