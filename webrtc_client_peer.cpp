//
// Created by user001 on 13-03-2019.
//
#include <gst/gst.h>
#include <gst/sdp/sdp.h>

#define GST_USE_UNSTABLE_API

#include <gst/webrtc/webrtc.h>

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup-websocket.h>

/* For application */
#include <string.h>
#include <stdio.h>
#include <map>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <execinfo.h>
#include <signal.h>
#include <time.h>
#include <string>
#include <regex>
#include <iostream>
#include <list>
#include <thread>

enum AppState {
    APP_STATE_UNKNOWN = 0,
    APP_STATE_ERROR = 1, /* generic error */
    SERVER_CONNECTING = 1000,
    SERVER_CONNECTION_ERROR,
    SERVER_CONNECTED, /* Ready to register */
    SERVER_REGISTERING = 2000,
    SERVER_REGISTRATION_ERROR,
    SERVER_REGISTERED, /* Ready to call a peer */
    SERVER_CLOSED, /* server connection closed by us or the server */
    PEER_CONNECTING = 3000,
    PEER_CONNECTION_ERROR,
    PEER_CONNECTED,
    PEER_CALL_NEGOTIATING = 4000,
    PEER_CALL_STARTED,
    PEER_CALL_STOPPING,
    PEER_CALL_STOPPED,
    PEER_CALL_ERROR,
};


static void on_offer_created(GstPromise *promise, gpointer user_data);

std::string SIGNAL_SERVER = "wss://127.0.0.1:8443";

//CONSTANTS
const std::string LEVEL_ASYMMETRY_ALLOWED = ";level-asymmetry-allowed=1";
const std::string PROFILE_LEVEL_ID_REGEX = "profile-level-id=*[A-za-z0-9]*;";
const std::string H264_BROWSER_PROFILE_LEVEL_ID = "profile-level-id=42e01f;";
const bool CHANGE_PROFILE_LEVEL_ID = false;

#define RTP_CAPS_H264 "application/x-rtp,media=video,encoding-name=H264,payload=96"

class WebrtcViewer {

public:
    //Attributes
    GstElement *pipeline;
    GstElement *webrtc1;
    GMainLoop *loop;
    SoupSession *session;
    SoupWebsocketConnection *ws_conn = NULL;
    enum AppState app_state = APP_STATE_UNKNOWN;
    int pipeline_execution_id;
    std::string peer_id;
    std::string server_url = SIGNAL_SERVER.c_str();
    gboolean disable_ssl = FALSE;

    //Methods
    gboolean start_webrtcbin(void);

    void remove_peer_from_pipeline(void);

    void close_peer_from_server(void);

    gboolean setup_call(void);

    void connect_to_websocket_server_async(void);

    void remove_webrtc_peer_from_pipelinehandler_map();
};

typedef std::shared_ptr<WebrtcViewer> WebrtcViewerPtr;

gboolean cleanup_and_quit_loop(const gchar *msg, enum AppState state, WebrtcViewer *webrtcViewer) {
    if (msg)
        g_printerr("%s\n", msg);
    if (state > 0)
        webrtcViewer->app_state = state;

    if (webrtcViewer->ws_conn) {
        if (soup_websocket_connection_get_state(webrtcViewer->ws_conn) ==
            SOUP_WEBSOCKET_STATE_OPEN)
            /* This will call us again */
            soup_websocket_connection_close(webrtcViewer->ws_conn, 1000, "");
        else
            g_object_unref(webrtcViewer->ws_conn);
    }

    /* To allow usage as a GSourceFunc */
    return G_SOURCE_REMOVE;
}

static void
on_server_closed(SoupWebsocketConnection *conn G_GNUC_UNUSED,
                 gpointer user_data G_GNUC_UNUSED) {
    static_cast<WebrtcViewer *>(user_data)->app_state = SERVER_CLOSED;
    cleanup_and_quit_loop("Server connection closed", APP_STATE_UNKNOWN, static_cast<WebrtcViewer *>(user_data));
}

/* One mega message handler for our asynchronous calling mechanism */
static void
on_server_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type,
                  GBytes *message, gpointer user_data) {
    gsize size;
    gchar *text, *data;
    WebrtcViewer *webrtcViewer = static_cast<WebrtcViewer *>(user_data);
    switch (type) {
        case SOUP_WEBSOCKET_DATA_BINARY:
            g_printerr("Received unknown binary message, ignoring\n");
            return;
        case SOUP_WEBSOCKET_DATA_TEXT: {
            gsize size;
            data = static_cast<gchar *>(g_bytes_unref_to_data(message, &size));
            /* Convert to NULL-terminated string */
            text = g_strndup(data, size);
            g_free(data);
            break;
        }
        default:
            g_assert_not_reached ();
    }

    /* Server has accepted our registration, we are ready to send commands */
    if (g_strcmp0(text, "HELLO") == 0) {
        if (webrtcViewer->app_state != SERVER_REGISTERING) {
            cleanup_and_quit_loop("ERROR: Received HELLO when not registering",
                                  APP_STATE_ERROR, webrtcViewer);
            goto out;
        }
        webrtcViewer->app_state = SERVER_REGISTERED;
        g_print("Registered with server, waiting to receive with peerid %s \n", webrtcViewer->peer_id.c_str());
        /* Ask signalling server to connect us with a specific peer */
        /*if (!webrtcViewer->setup_call()) {
            cleanup_and_quit_loop("ERROR: Failed to setup call", PEER_CALL_ERROR, webrtcViewer);
            goto out;
        }*/
        /* Call has been setup by the server, now we can start negotiation */
    } else if (g_strcmp0(text, "SESSION_OK") == 0) {
        if (webrtcViewer->app_state != PEER_CONNECTING) {
            cleanup_and_quit_loop("ERROR: Received SESSION_OK when not calling",
                                  PEER_CONNECTION_ERROR, webrtcViewer);
            goto out;
        }

        webrtcViewer->app_state = PEER_CONNECTED;
        /* Start negotiation (exchange SDP and ICE candidates) */
        if (!webrtcViewer->start_webrtcbin())
            cleanup_and_quit_loop("ERROR: failed to start pipeline",
                                  PEER_CALL_ERROR, webrtcViewer);
        /* Handle errors */
    } else if (g_str_has_prefix(text, "ERROR")) {
        switch (webrtcViewer->app_state) {
            case SERVER_CONNECTING:
                webrtcViewer->app_state = SERVER_CONNECTION_ERROR;
                break;
            case SERVER_REGISTERING:
                webrtcViewer->app_state = SERVER_REGISTRATION_ERROR;
                break;
            case PEER_CONNECTING:
                webrtcViewer->app_state = PEER_CONNECTION_ERROR;
                break;
            case PEER_CONNECTED:
            case PEER_CALL_NEGOTIATING:
                webrtcViewer->app_state = PEER_CALL_ERROR;
            default:
                webrtcViewer->app_state = APP_STATE_ERROR;
        }
        cleanup_and_quit_loop(text, APP_STATE_UNKNOWN, webrtcViewer);
        /* Look for JSON messages containing SDP and ICE candidates */
    } else {
        JsonNode *root;
        JsonObject *object, *child;
        JsonParser *parser = json_parser_new();
        if (!json_parser_load_from_data(parser, text, -1, NULL)) {
            g_printerr("Unknown message '%s', ignoring", text);
            g_object_unref(parser);
            goto out;
        }

        root = json_parser_get_root(parser);
        if (!JSON_NODE_HOLDS_OBJECT (root)) {
            g_printerr("Unknown json message '%s', ignoring", text);
            g_object_unref(parser);
            goto out;
        }

        object = json_node_get_object(root);
        /* Check type of JSON message */
        if (json_object_has_member(object, "sdp")) {

            if (!webrtcViewer->start_webrtcbin()) {
                g_printerr("Failed to create pipeline \n");
                cleanup_and_quit_loop("ERROR: failed to start pipeline",
                                      PEER_CALL_ERROR, webrtcViewer);
                return;
            }
            webrtcViewer->app_state = PEER_CALL_NEGOTIATING;
            int ret;
            GstSDPMessage *sdp;
            const gchar *text, *sdptype;
            GstWebRTCSessionDescription *answer;

            g_assert_cmphex (webrtcViewer->app_state, ==, PEER_CALL_NEGOTIATING);

            child = json_object_get_object_member(object, "sdp");

            if (!json_object_has_member(child, "type")) {
                cleanup_and_quit_loop("ERROR: received SDP without 'type'",
                                      PEER_CALL_ERROR, webrtcViewer);
                goto out;
            }

            sdptype = json_object_get_string_member(child, "type");
            /* In this example, we always create the offer and receive one answer.
             * See tests/examples/webrtcbidirectional.c in gst-plugins-bad for how to
             * handle offers from peers and reply with answers using webrtcbin. */
            g_assert_cmpstr (sdptype, ==, "offer");

            text = json_object_get_string_member(child, "sdp");

            g_print("Received offer:\n%s\n", text);

            ret = gst_sdp_message_new(&sdp);
            g_assert_cmphex (ret, ==, GST_SDP_OK);

            ret = gst_sdp_message_parse_buffer((guint8 *) text, strlen(text), sdp);
            g_assert_cmphex (ret, ==, GST_SDP_OK);

            answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER,
                                                        sdp);
            g_assert_nonnull (answer);

            /* Set remote description on our pipeline */
            {
                GstPromise *promise = gst_promise_new();
                g_signal_emit_by_name(webrtcViewer->webrtc1, "set-remote-description", answer,
                                      promise);
                gst_promise_interrupt(promise);
                gst_promise_unref(promise);

                /* Create an answer that we will send back to the peer */
                promise = gst_promise_new_with_change_func(
                        (GstPromiseChangeFunc) on_offer_created, (gpointer) webrtcViewer, NULL);
                g_signal_emit_by_name(webrtcViewer->webrtc1, "create-answer", NULL, promise);
            }

            webrtcViewer->app_state = PEER_CALL_STARTED;
        } else if (json_object_has_member(object, "ice")) {
            const gchar *candidate;
            gint sdpmlineindex;

            child = json_object_get_object_member(object, "ice");
            candidate = json_object_get_string_member(child, "candidate");
            sdpmlineindex = json_object_get_int_member(child, "sdpMLineIndex");
            g_print("Received ICE \n %s \n", candidate);
            /* Add ice candidate sent by remote peer */
            g_signal_emit_by_name(webrtcViewer->webrtc1, "add-ice-candidate", sdpmlineindex,
                                  candidate);
        } else {
            g_printerr("Ignoring unknown JSON message:\n%s\n", text);
        }
        g_object_unref(parser);
    }

    out:
    g_free(text);
}

static void
handle_media_stream(GstPad *pad, GstElement *pipe, const char *convert_name,
                    const char *sink_name) {
    GstPad *qpad;
    GstElement *q, *conv, *resample, *sink;
    GstPadLinkReturn ret;

    g_print("Trying to handle stream with %s ! %s", convert_name, sink_name);

    q = gst_element_factory_make("queue", NULL);
    g_assert_nonnull (q);
    conv = gst_element_factory_make(convert_name, NULL);
    g_assert_nonnull (conv);
    sink = gst_element_factory_make(sink_name, NULL);
    g_assert_nonnull (sink);

    if (g_strcmp0(convert_name, "audioconvert") == 0) {
        /* Might also need to resample, so add it just in case.
         * Will be a no-op if it's not required. */
        resample = gst_element_factory_make("audioresample", NULL);
        g_assert_nonnull (resample);
        gst_bin_add_many(GST_BIN (pipe), q, conv, resample, sink, NULL);
        gst_element_sync_state_with_parent(q);
        gst_element_sync_state_with_parent(conv);
        gst_element_sync_state_with_parent(resample);
        gst_element_sync_state_with_parent(sink);
        gst_element_link_many(q, conv, resample, sink, NULL);
    } else {
        gst_bin_add_many(GST_BIN (pipe), q, conv, sink, NULL);
        gst_element_sync_state_with_parent(q);
        gst_element_sync_state_with_parent(conv);
        gst_element_sync_state_with_parent(sink);
        gst_element_link_many(q, conv, sink, NULL);
    }

    qpad = gst_element_get_static_pad(q, "sink");

    ret = gst_pad_link(pad, qpad);
    g_assert_cmphex (ret, ==, GST_PAD_LINK_OK);
}

static void
on_incoming_decodebin_stream(GstElement *decodebin, GstPad *pad,
                             GstElement *pipe) {
    GstCaps *caps;
    const gchar *name;

    if (!gst_pad_has_current_caps(pad)) {
        g_printerr("Pad '%s' has no caps, can't do anything, ignoring\n",
                   GST_PAD_NAME (pad));
        return;
    }

    caps = gst_pad_get_current_caps(pad);
    name = gst_structure_get_name(gst_caps_get_structure(caps, 0));

    if (g_str_has_prefix(name, "video")) {
        handle_media_stream(pad, pipe, "videoconvert", "fakesink");
    } else if (g_str_has_prefix(name, "audio")) {
        handle_media_stream(pad, pipe, "audioconvert", "fakesink");
    } else {
        g_printerr("Unknown pad %s, ignoring", GST_PAD_NAME (pad));
    }
}

static gchar *
get_string_from_json_object(JsonObject *object) {
    JsonNode *root;
    JsonGenerator *generator;
    gchar *text;

    /* Make it the root node */
    root = json_node_init_object(json_node_alloc(), object);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    text = json_generator_to_data(generator, NULL);

    /* Release everything */
    g_object_unref(generator);
    json_node_free(root);
    return text;
}

static void
on_incoming_stream(GstElement *webrtc, GstPad *pad, GstElement *pipe) {

    g_print("Triggered on_incoming_stream \n");

    if (GST_PAD_DIRECTION (pad) != GST_PAD_SRC) {
        g_printerr("Incorrect pad direction \n");
        return;
    }

    GstCaps *caps;
    const gchar *name;
    gchar *dynamic_pad_name;
    GstElement *rtph264depay;
    dynamic_pad_name = gst_pad_get_name (pad);

    if (!gst_pad_has_current_caps(pad)) {
        g_printerr("Pad '%s' has no caps, can't do anything, ignoring\n",
                   GST_PAD_NAME (pad));
        return;
    }

    caps = gst_pad_get_current_caps(pad);
    name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    g_print("on_incoming_stream caps name %s \n", name);
    //if (g_str_has_prefix(name, "video")) {
    rtph264depay = gst_bin_get_by_name(GST_BIN (pipe), "rtpdepay");
    if (gst_element_link_pads(webrtc, dynamic_pad_name, rtph264depay, "sink")) {
        g_print("on_rtspsrc_pad_added rtspsrc pad %s linked to h264depay \n ", dynamic_pad_name);
        gst_object_unref(rtph264depay);
        g_free(dynamic_pad_name);
        return;
    }
    //}
    return;
}

void static send_ice_candidate_message(GstElement *webrtc G_GNUC_UNUSED, guint mlineindex,
                                       gchar *candidate, WebrtcViewer *user_data G_GNUC_UNUSED) {
    gchar *text;
    JsonObject *ice, *msg;

    if (user_data->app_state < PEER_CALL_NEGOTIATING) {
        cleanup_and_quit_loop("Can't send ICE, not in call", APP_STATE_ERROR, user_data);
        return;
    }

    ice = json_object_new();
    json_object_set_string_member(ice, "candidate", candidate);
    json_object_set_int_member(ice, "sdpMLineIndex", mlineindex);
    msg = json_object_new();
    json_object_set_object_member(msg, "ice", ice);
    text = get_string_from_json_object(msg);
    json_object_unref(msg);
    g_print("Sending ICE \n %s \n", text);
    soup_websocket_connection_send_text(user_data->ws_conn, text);
    g_free(text);
}

void static send_sdp_offer(GstWebRTCSessionDescription *offer, WebrtcViewer *webrtcViewer) {
    gchar *text;
    JsonObject *msg, *sdp;

    if (webrtcViewer->app_state < PEER_CALL_NEGOTIATING) {
        cleanup_and_quit_loop("Can't send offer, not in call", APP_STATE_ERROR, webrtcViewer);
        return;
    }

    text = gst_sdp_message_as_text(offer->sdp);
    g_print("Sending answer:\n%s\n", text);

    sdp = json_object_new();
    json_object_set_string_member(sdp, "type", "answer");
    json_object_set_string_member(sdp, "sdp", text);
    g_free(text);

    msg = json_object_new();
    json_object_set_object_member(msg, "sdp", sdp);
    text = get_string_from_json_object(msg);
    json_object_unref(msg);

    soup_websocket_connection_send_text(webrtcViewer->ws_conn, text);
    g_free(text);
}

/* Offer created by our pipeline, to be sent to the peer */
static void on_offer_created(GstPromise *promise, gpointer user_data) {
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply;
    WebrtcViewer *webrtcViewer = static_cast<WebrtcViewer *>(user_data);
    //g_assert_cmphex (webrtcViewer->app_state, ==, PEER_CALL_NEGOTIATING);

    g_assert_cmphex (gst_promise_wait(promise), ==, GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "answer",
                      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);


    if (CHANGE_PROFILE_LEVEL_ID) {
        const gchar *text_fmtp = gst_sdp_media_get_attribute_val(
                (GstSDPMedia *) &g_array_index(offer->sdp->medias, GstSDPMedia, 0), "fmtp");
        if (strstr(text_fmtp, "profile-level-id") != NULL) {
            g_print("Found source fmtp attribute as:  %s\n", text_fmtp);
            std::string delimiter = ";";
            std::string fmtp_attr(text_fmtp);
            fmtp_attr.append(LEVEL_ASYMMETRY_ALLOWED);
            //Replacing profile-level-id
            fmtp_attr = std::regex_replace(fmtp_attr, std::regex(PROFILE_LEVEL_ID_REGEX),
                                           H264_BROWSER_PROFILE_LEVEL_ID);
            printf("Updated fmtp attribute as: %s\n", fmtp_attr.c_str());

            guint attr_len = gst_sdp_media_attributes_len(
                    (GstSDPMedia *) &g_array_index(offer->sdp->medias, GstSDPMedia, 0));
            printf("Attributes Length: %d\n", attr_len);
            guint fmtp_index;
            for (guint index = 0; index < attr_len; index++) {
                const GstSDPAttribute *gstSDPAttribute = gst_sdp_media_get_attribute(
                        (GstSDPMedia *) &g_array_index(offer->sdp->medias, GstSDPMedia, 0), index);
                const gchar *attr_val = gstSDPAttribute->value;
                if (attr_val != NULL && strstr(attr_val, "profile-level-id") != NULL) {
                    printf("Found fmtp attribute at index: %d\n", index);
                    fmtp_index = index;
                }
            }
            if (fmtp_index > 0) {
                printf("Replacing fmtp attribute at index: %d \n", fmtp_index);
                gst_sdp_media_remove_attribute((GstSDPMedia *) &g_array_index(offer->sdp->medias, GstSDPMedia, 0),
                                               fmtp_index);
                gst_sdp_media_add_attribute((GstSDPMedia *) &g_array_index(offer->sdp->medias, GstSDPMedia, 0), "fmtp",
                                            fmtp_attr.c_str());
                //Frame rate hard code - Disabled
                /*gst_sdp_media_add_attribute((GstSDPMedia *) &g_array_index(offer->sdp->medias, GstSDPMedia, 0), "framerate", "29.985014985014985");*/
            }
        }
    }


    promise = gst_promise_new();
    g_signal_emit_by_name(webrtcViewer->webrtc1, "set-local-description", offer, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);

    /* Send offer to peer */
    send_sdp_offer(offer, webrtcViewer);
    gst_webrtc_session_description_free(offer);
}

void static on_negotiation_needed(GstElement *element, WebrtcViewer *user_data) {
    GstPromise *promise;
    WebrtcViewer *webrtcViewer = static_cast<WebrtcViewer *>(user_data);
    webrtcViewer->app_state = PEER_CALL_NEGOTIATING;
    promise = gst_promise_new_with_change_func(on_offer_created, webrtcViewer, NULL);;
    g_signal_emit_by_name(webrtcViewer->webrtc1, "create-offer", NULL, promise);
}

gboolean WebrtcViewer::start_webrtcbin(void) {

    GstWebRTCRTPTransceiver *trans;
    GArray *transceivers;

    GstStateChangeReturn ret;
    GstElement *rtph264depay, *avdec_h264, *videosink;

    /* Create Elements */
    pipeline = gst_pipeline_new(g_strdup_printf("pipeline-%s", peer_id.c_str()));
    webrtc1 = gst_element_factory_make("webrtcbin", "rtspsource");
    rtph264depay = gst_element_factory_make("rtph264depay", "rtpdepay");
    avdec_h264 = gst_element_factory_make("avdec_h264", "avdec_h264");
    videosink = gst_element_factory_make("autovideosink", "autovideosink");

    //Add elements to pipeline
    gst_bin_add_many(GST_BIN (pipeline), webrtc1, rtph264depay, avdec_h264, videosink, NULL);

    if (!gst_element_link_many(rtph264depay, avdec_h264, videosink, NULL)) {
        g_printerr("start_webrtcbin: Error linking rtph264depay to avdech264 for device %s \n",
                   peer_id.c_str());
        return FALSE;
    }
    g_assert_nonnull (webrtc1);

    /* This is the gstwebrtc entry point where we create the offer and so on. It
     * will be called when the pipeline goes to PLAYING. */
    //No required when acting as receiver
    /*
    g_signal_connect (webrtc1, "on-negotiation-needed",
                      G_CALLBACK(on_negotiation_needed), this);*/
    /* We need to transmit this ICE candidate to the browser via the websockets
     * signalling server. Incoming ice candidates from the browser need to be
     * added by us too, see on_server_message() */
    g_signal_connect (webrtc1, "on-ice-candidate",
                      G_CALLBACK(send_ice_candidate_message), this);

    //Change webrtcbin to send only
    /*g_signal_emit_by_name(webrtc1, "get-transceivers", &transceivers);
    if (transceivers != NULL) {
        g_print("Changing webrtcbin to sendonly...\n");
        trans = g_array_index (transceivers, GstWebRTCRTPTransceiver *, 0);
        trans->direction = GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY;
        g_object_unref(trans);
        g_array_unref(transceivers);
    }*/

    /* Incoming streams will be exposed via this signal */
    g_signal_connect (webrtc1, "pad-added", G_CALLBACK(on_incoming_stream),
                      pipeline);

    g_print("Created webrtc bin for peer %s\n", this->peer_id.c_str());

    ret = gst_element_set_state(GST_ELEMENT (pipeline), GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Failure in GST_STATE_PLAYING \n");
        return FALSE;
    }
    return TRUE;
}

gboolean WebrtcViewer::setup_call(void) {
    gchar *msg;

    if (soup_websocket_connection_get_state(this->ws_conn) != SOUP_WEBSOCKET_STATE_OPEN) {
        g_print("Websocket connection is not in state SOUP_WEBSOCKET_STATE_OPEN \n");
        return FALSE;
    }

    if (this->peer_id == "") {
        g_print("WebrtcViewer::setup_call peer id is blank\n");
        return FALSE;
    }

    g_print("Setting up signalling server call with %s\n", this->peer_id.c_str());
    app_state = PEER_CONNECTING;
    msg = g_strdup_printf("SESSION %s", this->peer_id.c_str());
    soup_websocket_connection_send_text(ws_conn, msg);
    g_free(msg);
    return TRUE;
}

static gboolean
register_with_server(WebrtcViewer *webrtcViewer) {
    gchar *hello;
    gint32 our_id;

    if (soup_websocket_connection_get_state(webrtcViewer->ws_conn) !=
        SOUP_WEBSOCKET_STATE_OPEN)
        return FALSE;

    our_id = g_random_int_range(10, 10000);
    g_print("Registering id %i with server\n", our_id);
    webrtcViewer->app_state = SERVER_REGISTERING;
    webrtcViewer->peer_id = std::string(g_strdup_printf("%i", our_id));

    /* Register with the server with a random integer id. Reply will be received
     * by on_server_message() */
    hello = g_strdup_printf("HELLO %i", our_id);
    soup_websocket_connection_send_text(webrtcViewer->ws_conn, hello);
    g_free(hello);

    return TRUE;
}

static void
on_server_connected(SoupSession *session, GAsyncResult *res,
                    WebrtcViewer *webrtcViewer) {
    GError *error = NULL;

    webrtcViewer->ws_conn = soup_session_websocket_connect_finish(session, res, &error);
    if (error) {
        cleanup_and_quit_loop(error->message, SERVER_CONNECTION_ERROR, webrtcViewer);
        g_error_free(error);
        return;
    }

    g_assert_nonnull (webrtcViewer->ws_conn);

    webrtcViewer->app_state = SERVER_CONNECTED;
    g_print("Connected to signalling server\n");

    g_signal_connect (webrtcViewer->ws_conn, "closed", G_CALLBACK(on_server_closed), webrtcViewer);
    g_signal_connect (webrtcViewer->ws_conn, "message", G_CALLBACK(on_server_message), webrtcViewer);

    /* Register with the server so it knows about us and can accept commands */
    register_with_server(webrtcViewer);
}

void WebrtcViewer::connect_to_websocket_server_async(void) {
    SoupLogger *logger;
    SoupMessage *message;
    const char *https_aliases[] = {"wss", NULL};

    session = soup_session_new_with_options(SOUP_SESSION_SSL_STRICT, !disable_ssl,
                                            SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
            //SOUP_SESSION_SSL_CA_FILE, "/etc/ssl/certs/ca-bundle.crt",
                                            SOUP_SESSION_HTTPS_ALIASES, https_aliases, NULL);

    logger = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
    soup_session_add_feature(session, SOUP_SESSION_FEATURE (logger));
    g_object_unref(logger);

    message = soup_message_new(SOUP_METHOD_GET, server_url.c_str());

    g_print("Connecting to server...\n");

    /* Once connected, we will register */
    soup_session_websocket_connect_async(session, message, NULL, NULL, NULL,
                                         (GAsyncReadyCallback) on_server_connected, this);
    app_state = SERVER_CONNECTING;
}


static gboolean
check_plugins(void) {
    int i;
    gboolean ret;
    GstPlugin *plugin;
    GstRegistry *registry;
    const gchar *needed[] = {"opus", "vpx", "nice", "webrtc", "dtls", "srtp",
                             "rtpmanager", "videotestsrc", "audiotestsrc", NULL};

    registry = gst_registry_get();
    ret = TRUE;
    for (i = 0; i < g_strv_length((gchar **) needed); i++) {
        plugin = gst_registry_find_plugin(registry, needed[i]);
        if (!plugin) {
            g_print("Required gstreamer plugin '%s' not found\n", needed[i]);
            ret = FALSE;
            continue;
        }
        gst_object_unref(plugin);
    }
    return ret;
}

void handler(int sig) {
    void *array[10];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 10);

    // print out all the frames to stderr
    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}

int
main(int argc, char *argv[]) {
    signal(SIGSEGV, handler);
    GOptionContext *context;
    GError *error = NULL;

    context = g_option_context_new("- gstreamer webrtc peer demo");

    g_option_context_add_group(context, gst_init_get_option_group());
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Error initializing: %s\n", error->message);
        return -1;
    }

    if (!check_plugins())
        return -1;

    WebrtcViewerPtr webrtcViewerPtr = std::make_shared<WebrtcViewer>();
    webrtcViewerPtr->loop = g_main_loop_new(NULL, FALSE);
    webrtcViewerPtr->disable_ssl = TRUE;
    webrtcViewerPtr->connect_to_websocket_server_async();
    g_main_loop_run(webrtcViewerPtr->loop);

    if (webrtcViewerPtr->loop) {
        g_main_quit(webrtcViewerPtr->loop);
    }
    return 0;

}