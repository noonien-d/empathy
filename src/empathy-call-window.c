/*
 * empathy-call-window.c - Source for EmpathyCallWindow
 * Copyright (C) 2008-2011 Collabora Ltd.
 * @author Sjoerd Simons <sjoerd.simons@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "empathy-call-window.h"

#include <glib/gi18n.h>
#include <clutter-gst/clutter-gst.h>
#include <telepathy-farstream/telepathy-farstream.h>
#include <farstream/fs-element-added-notifier.h>
#include <farstream/fs-utils.h>
#include <tp-account-widgets/tpaw-builder.h>
#include <tp-account-widgets/tpaw-camera-monitor.h>
#include <tp-account-widgets/tpaw-images.h>
#include <tp-account-widgets/tpaw-pixbuf-utils.h>
#include <tp-account-widgets/tpaw-utils.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-about-dialog.h"
#include "empathy-audio-sink.h"
#include "empathy-call-utils.h"
#include "empathy-call-window-fullscreen.h"
#include "empathy-camera-menu.h"
#include "empathy-dialpad-widget.h"
#include "empathy-geometry.h"
#include "empathy-gsettings.h"
#include "empathy-images.h"
#include "empathy-mic-menu.h"
#include "empathy-preferences.h"
#include "empathy-request-util.h"
#include "empathy-rounded-actor.h"
#include "empathy-rounded-rectangle.h"
#include "empathy-rounded-texture.h"
#include "empathy-sound-manager.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_VOIP
#include "empathy-debug.h"

#define CONTENT_HBOX_SPACING 3
#define CONTENT_HBOX_CHILDREN_PACKING_PADDING 0
#define OVERLAY_MARGIN 6

#define REMOTE_VIDEO_DEFAULT_WIDTH 320
#define REMOTE_VIDEO_DEFAULT_HEIGHT 240

#define SELF_VIDEO_SECTION_WIDTH 120
#define SELF_VIDEO_SECTION_HEIGHT 90
#define SELF_VIDEO_SECTION_MARGIN 2
#define SELF_VIDEO_SECTION_BORDER SELF_VIDEO_SECTION_MARGIN*2

/* The avatar's default width and height are set to the same value because we
   want a square icon. */
#define REMOTE_CONTACT_AVATAR_DEFAULT_HEIGHT REMOTE_VIDEO_DEFAULT_HEIGHT
#define REMOTE_CONTACT_AVATAR_DEFAULT_WIDTH REMOTE_VIDEO_DEFAULT_HEIGHT

#define SMALL_TOOLBAR_SIZE 36

/* If an video input error occurs, the error message will start with "v4l" */
#define VIDEO_INPUT_ERROR_PREFIX "v4l"

/* The time interval in milliseconds between 2 outgoing rings */
#define MS_BETWEEN_RING 500

/* The roundedness of preview box and placeholders */
#define PREVIEW_ROUND_FACTOR 16

#define PREVIEW_BUTTON_OPACITY 180

G_DEFINE_TYPE(EmpathyCallWindow, empathy_call_window, GTK_TYPE_WINDOW)

enum {
  PROP_CALL_HANDLER = 1,
};

enum {
  SIG_INHIBIT,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

typedef enum {
  RINGING,       /* Incoming call */
  CONNECTING,    /* Outgoing call */
  CONNECTED,     /* Connected */
  HELD,          /* Connected, but on hold */
  DISCONNECTED,  /* Disconnected */
  REDIALING      /* Redialing (special case of CONNECTING) */
} CallState;

typedef enum {
  CAMERA_STATE_OFF = 0,
  CAMERA_STATE_ON,
} CameraState;

typedef enum {
  PREVIEW_POS_NONE,
  PREVIEW_POS_TOP_LEFT,
  PREVIEW_POS_TOP_RIGHT,
  PREVIEW_POS_BOTTOM_LEFT,
  PREVIEW_POS_BOTTOM_RIGHT,
} PreviewPosition;

struct _EmpathyCallWindowPriv
{
  gboolean dispose_has_run;
  EmpathyCallHandler *handler;

  EmpathyContact *contact;

  TpawCameraMonitor *camera_monitor;

  CallState call_state;
  gboolean outgoing;

  GtkUIManager *ui_manager;
  GtkWidget *errors_vbox;
  /* widget displays the video received from the remote user. This widget is
   * alive only during call. */
  ClutterActor *video_output;
  ClutterActor *video_preview;
  ClutterActor *drag_preview;
  ClutterActor *preview_shown_button;
  ClutterActor *preview_hidden_button;
  ClutterActor *preview_rectangle1;
  ClutterActor *preview_rectangle2;
  ClutterActor *preview_rectangle3;
  ClutterActor *preview_rectangle4;
  ClutterActor *preview_spinner_actor;
  GtkWidget *preview_spinner_widget;
  GtkWidget *video_container;
  GtkWidget *remote_user_avatar_widget;
  GtkWidget *remote_user_avatar_toolbar;
  GtkWidget *remote_user_name_toolbar;
  GtkWidget *remote_user_status_toolbar;
  GtkWidget *status_label;
  GtkWidget *hangup_button;
  GtkWidget *audio_call_button;
  GtkWidget *video_call_button;
  GtkWidget *mic_button;
  GtkWidget *microphone_icon;
  GtkWidget *volume_button;
  GtkWidget *camera_button;
  GtkWidget *dialpad_button;
  GtkWidget *toolbar;
  GtkWidget *bottom_toolbar;
  ClutterActor *floating_toolbar;
  GtkWidget *pane;
  GtkAction *menu_fullscreen;
  GtkAction *menu_swap_camera;

  ClutterState *transitions;

  /* The main box covering all the stage, contaning remote avatar/video */
  ClutterActor *video_box;
  ClutterLayoutManager *video_layout;

  /* A bin layout manager containing a bin for previews
   * and the floating toolbar */
  ClutterActor *overlay_bin;
  ClutterLayoutManager *overlay_layout;

  /* Bin layout for the previews */
  ClutterActor *preview_box;
  ClutterLayoutManager *preview_layout;

  /* Coordinates of the preview drag event's start. */
  PreviewPosition preview_pos;

  /* We keep a reference on the hbox which contains the main content so we can
     easilly repack everything when toggling fullscreen */
  GtkWidget *content_hbox;

  /* These are used to accept or reject an incoming call when the status
     is RINGING. */
  GtkWidget *incoming_call_dialog;
  TpCallChannel *pending_channel;
  TpChannelDispatchOperation *pending_cdo;
  TpAddDispatchOperationContext *pending_context;

  gulong video_output_motion_handler_id;
  guint bus_message_source_id;

  GtkWidget *dtmf_panel;

  /* Details vbox */
  GtkWidget *details_vbox;
  GtkWidget *vcodec_encoding_label;
  GtkWidget *acodec_encoding_label;
  GtkWidget *vcodec_decoding_label;
  GtkWidget *acodec_decoding_label;

  GtkWidget *audio_remote_candidate_label;
  GtkWidget *audio_local_candidate_label;
  GtkWidget *video_remote_candidate_label;
  GtkWidget *video_local_candidate_label;
  GtkWidget *video_remote_candidate_info_img;
  GtkWidget *video_local_candidate_info_img;
  GtkWidget *audio_remote_candidate_info_img;
  GtkWidget *audio_local_candidate_info_img;

  GstElement *video_input;
  GstElement *video_preview_sink;
  GstElement *video_output_sink;
  GstElement *audio_input;
  GstElement *audio_output;
  gboolean audio_output_added;
  GstElement *pipeline;
  GstElement *video_tee;

  GstElement *funnel;

  GList *notifiers;

  GTimer *timer;
  guint timer_id;

  GMutex lock;
  gboolean call_started;
  gboolean sending_video;
  CameraState camera_state;

  EmpathyCallWindowFullscreen *fullscreen;
  gboolean is_fullscreen;

  gboolean got_video;
  guint got_video_src;

  guint inactivity_src;

  /* Those fields represent the state of the window before it actually was in
     fullscreen mode. */
  gboolean dialpad_was_visible_before_fs;
  gint original_width_before_fs;
  gint original_height_before_fs;

  gint x, y, w, h, dialpad_width;
  gboolean maximized;

  /* TRUE if the call should be started when the pipeline is playing */
  gboolean start_call_when_playing;
  /* TRUE if we requested to set the pipeline in the playing state */
  gboolean pipeline_playing;

  EmpathySoundManager *sound_mgr;

  GSettings *settings;
  EmpathyMicMenu *mic_menu;
  EmpathyCameraMenu *camera_menu;

  gboolean muted;
};

#define GET_PRIV(o) (EMPATHY_CALL_WINDOW (o)->priv)

static void empathy_call_window_realized_cb (GtkWidget *widget,
  EmpathyCallWindow *window);

static gboolean empathy_call_window_delete_cb (GtkWidget *widget,
  GdkEvent *event, EmpathyCallWindow *window);

static gboolean empathy_call_window_state_event_cb (GtkWidget *widget,
  GdkEventWindowState *event, EmpathyCallWindow *window);

static void empathy_call_window_set_send_video (EmpathyCallWindow *window,
  CameraState state);

static void empathy_call_window_hangup_cb (gpointer object,
  EmpathyCallWindow *window);

static void empathy_call_window_fullscreen_cb (gpointer object,
  EmpathyCallWindow *window);

static void empathy_call_window_fullscreen_toggle (EmpathyCallWindow *window);

static gboolean empathy_call_window_video_button_press_cb (
  GtkWidget *video_output, GdkEventButton *event, EmpathyCallWindow *window);

static gboolean empathy_call_window_key_press_cb (GtkWidget *video_output,
  GdkEventKey *event, EmpathyCallWindow *window);

static gboolean empathy_call_window_video_output_motion_notify (
  GtkWidget *widget, GdkEventMotion *event, EmpathyCallWindow *window);

static void empathy_call_window_video_menu_popup (EmpathyCallWindow *window,
  guint button);

static void empathy_call_window_connect_handler (EmpathyCallWindow *self);

static void empathy_call_window_dialpad_cb (GtkToggleToolButton *button,
  EmpathyCallWindow *window);

static void empathy_call_window_restart_call (EmpathyCallWindow *window);

static void empathy_call_window_status_message (EmpathyCallWindow *window,
  gchar *message);

static gboolean empathy_call_window_bus_message (GstBus *bus,
  GstMessage *message, gpointer user_data);

static gboolean empathy_call_window_update_timer (gpointer user_data);

static void
make_background_transparent (GtkClutterActor *actor)
{
  GdkRGBA transparent = { 0., 0., 0., 0. };
  GtkWidget *widget;

  widget = gtk_clutter_actor_get_widget (actor);
  gtk_widget_override_background_color (widget, GTK_STATE_FLAG_NORMAL, &transparent);
}

static void
empathy_call_window_show_hangup_button (EmpathyCallWindow *self,
    gboolean show)
{
  gtk_widget_set_visible (self->priv->hangup_button, show);
  gtk_widget_set_visible (self->priv->audio_call_button, !show);
  gtk_widget_set_visible (self->priv->video_call_button, !show);
}

static void
empathy_call_window_audio_call_cb (GtkToggleToolButton *button,
    EmpathyCallWindow *self)
{
  g_object_set (self->priv->handler, "initial-video", FALSE, NULL);
  empathy_call_window_restart_call (self);
}

static void
empathy_call_window_video_call_cb (GtkToggleToolButton *button,
    EmpathyCallWindow *self)
{
  empathy_call_window_set_send_video (self, CAMERA_STATE_ON);
  g_object_set (self->priv->handler, "initial-video", TRUE, NULL);
  empathy_call_window_restart_call (self);
}

static void
dtmf_start_tone_cb (EmpathyDialpadWidget *dialpad,
    TpDTMFEvent event,
    EmpathyCallWindow *self)
{
  TpCallChannel *call;
  gchar tones[2];

  g_object_get (self->priv->handler, "call-channel", &call, NULL);

  tones[0] = tp_dtmf_event_to_char (event);
  tones[1] = '\0';
  tp_call_channel_send_tones_async (call, tones, NULL, NULL, NULL);

  g_object_unref (call);
}

static void
empathy_call_window_show_video_output (EmpathyCallWindow *self,
    gboolean show)
{
  if (self->priv->video_output != NULL)
    g_object_set (self->priv->video_output, "visible", show, NULL);

  gtk_widget_set_visible (self->priv->remote_user_avatar_widget, !show);

  clutter_actor_raise_top (self->priv->overlay_bin);
}

static void
create_video_output_widget (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  g_assert (priv->video_output == NULL);
  g_assert (priv->pipeline != NULL);

  priv->video_output_sink = GST_ELEMENT (clutter_gst_video_sink_new ());
  priv->video_output = g_object_new (CLUTTER_TYPE_ACTOR,
      "content", g_object_new (CLUTTER_GST_TYPE_ASPECTRATIO,
                               "sink", priv->video_output_sink,
                               NULL),
      NULL);

  clutter_container_add_actor (CLUTTER_CONTAINER (priv->video_box),
      priv->video_output);
  clutter_actor_add_constraint (priv->video_output,
      clutter_bind_constraint_new (priv->video_box, CLUTTER_BIND_SIZE, 0));

  gtk_widget_add_events (priv->video_container,
      GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
  g_signal_connect (G_OBJECT (priv->video_container), "button-press-event",
      G_CALLBACK (empathy_call_window_video_button_press_cb), self);
}

static void
create_video_input (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  g_assert (priv->video_input == NULL);
  priv->video_input = empathy_video_src_new ();
  gst_object_ref_sink (priv->video_input);
}

static gboolean
audio_control_volume_to_element (GBinding *binding,
  const GValue *source_value,
  GValue *target_value,
  gpointer user_data)
{
  /* AudioControl volume is 0-255, with -1 for unknown */
  gint hv;

  hv = g_value_get_int (source_value);
  if (hv < 0)
    return FALSE;

  hv = MIN (hv, 255);
  g_value_set_double (target_value, hv/255.0);

  return TRUE;
}

static gboolean
element_volume_to_audio_control (GBinding *binding,
  const GValue *source_value,
  GValue *target_value,
  gpointer user_data)
{
  gdouble ev;

  ev = g_value_get_double (source_value);
  ev = CLAMP (ev, 0.0, 1.0);

  g_value_set_int (target_value, ev * 255);
  return TRUE;
}

static void
audio_input_mute_notify_cb (GObject *obj, GParamSpec *spec,
  EmpathyCallWindow *self)
{
  gboolean muted;
  g_object_get (obj, "mute", &muted, NULL);

  self->priv->muted = muted;
  if (muted && self->priv->transitions)
    clutter_state_set_state (self->priv->transitions, "fade-in");

  if (muted)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->priv->microphone_icon),
          EMPATHY_IMAGE_MIC_MUTED, GTK_ICON_SIZE_MENU);
    }
  else
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->priv->microphone_icon),
          EMPATHY_IMAGE_MIC, GTK_ICON_SIZE_MENU);
    }

  empathy_call_window_update_timer (self);
}

static void
create_audio_input (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  g_assert (priv->audio_input == NULL);
  priv->audio_input = empathy_audio_src_new ();
  gst_object_ref_sink (priv->audio_input);

  g_signal_connect (priv->audio_input, "notify::mute",
    G_CALLBACK (audio_input_mute_notify_cb), self);
}

static void
add_video_preview_to_pipeline (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GstElement *preview;

  g_assert (priv->video_preview != NULL);
  g_assert (priv->pipeline != NULL);
  g_assert (priv->video_input != NULL);
  g_assert (priv->video_tee != NULL);

  preview = priv->video_preview_sink;

  if (!gst_bin_add (GST_BIN (priv->pipeline), priv->video_input))
    {
      g_warning ("Could not add video input to pipeline");
      return;
    }

  if (!gst_bin_add (GST_BIN (priv->pipeline), preview))
    {
      g_warning ("Could not add video preview to pipeline");
      return;
    }

  if (!gst_element_link (priv->video_input, priv->video_tee))
    {
      g_warning ("Could not link video input to video tee");
      return;
    }

  if (!gst_element_link (priv->video_tee, preview))
    {
      g_warning ("Could not link video tee to video preview");
      return;
    }
}

static void
empathy_call_window_disable_camera_cb (GtkAction *action,
    EmpathyCallWindow *self)
{
  clutter_actor_destroy (self->priv->preview_hidden_button);

  gtk_toggle_button_set_active (
      GTK_TOGGLE_BUTTON (self->priv->camera_button), FALSE);
}

static void
empathy_call_window_minimise_camera_cb (GtkAction *action,
    EmpathyCallWindow *self)
{
  clutter_actor_hide (self->priv->video_preview);
  clutter_actor_show (self->priv->preview_hidden_button);
}

static void
empathy_call_window_maximise_camera_cb (GtkAction *action,
    EmpathyCallWindow *self)
{
  clutter_actor_show (self->priv->video_preview);
  clutter_actor_hide (self->priv->preview_hidden_button);
}

static void
empathy_call_window_swap_camera_cb (GtkAction *action,
    EmpathyCallWindow *self)
{
  const GList *cameras, *l;
  gchar *current_cam;

  DEBUG ("Swapping the camera");

  cameras = tpaw_camera_monitor_get_cameras (self->priv->camera_monitor);
  current_cam = empathy_video_src_dup_device (
      EMPATHY_GST_VIDEO_SRC (self->priv->video_input));

  for (l = cameras; l != NULL; l = l->next)
    {
      TpawCamera *camera = l->data;

      if (!tp_strdiff (camera->device, current_cam))
        {
          TpawCamera *next;

          if (l->next != NULL)
            next = l->next->data;
          else
            next = cameras->data;

          /* EmpathyCameraMenu will update itself and do the actual change
           * for us */
          g_settings_set_string (self->priv->settings,
              EMPATHY_PREFS_CALL_CAMERA_DEVICE,
              next->device);

          break;
        }
    }

  g_free (current_cam);
}

static void
empathy_call_window_update_swap_camera (EmpathyCallWindow *self)
{
  const GList *cameras = tpaw_camera_monitor_get_cameras (
      self->priv->camera_monitor);

  gtk_action_set_visible (self->priv->menu_swap_camera,
      g_list_length ((GList *) cameras) >= 2);
}

static void
empathy_call_window_camera_added_cb (TpawCameraMonitor *monitor,
    TpawCamera *camera,
    EmpathyCallWindow *self)
{
  empathy_call_window_update_swap_camera (self);
}

static void
empathy_call_window_camera_removed_cb (TpawCameraMonitor *monitor,
    TpawCamera *camera,
    EmpathyCallWindow *self)
{
  empathy_call_window_update_swap_camera (self);
}

static void
empathy_call_window_preview_button_clicked_cb (ClutterClickAction *action,
    ClutterActor *actor,
    EmpathyCallWindow *self)
{
  GtkWidget *menu;

  menu = gtk_ui_manager_get_widget (self->priv->ui_manager,
      "/preview-menu");
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL,
      0, gtk_get_current_event_time ());
  gtk_menu_shell_select_first (GTK_MENU_SHELL (menu), FALSE);
}

static void
empathy_call_window_preview_hidden_button_clicked_cb (
    ClutterClickAction *action,
    ClutterActor *actor,
    EmpathyCallWindow *self)
{
  GtkWidget *menu;

  menu = gtk_ui_manager_get_widget (self->priv->ui_manager,
      "/preview-hidden-menu");
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL,
      0, gtk_get_current_event_time ());
  gtk_menu_shell_select_first (GTK_MENU_SHELL (menu), FALSE);
}

static ClutterActor *
empathy_call_window_create_preview_rectangle (EmpathyCallWindow *self,
    ClutterBinAlignment x,
    ClutterBinAlignment y)
{
  ClutterActor *rectangle;

  rectangle = CLUTTER_ACTOR (empathy_rounded_rectangle_new (
      SELF_VIDEO_SECTION_WIDTH, SELF_VIDEO_SECTION_HEIGHT,
      PREVIEW_ROUND_FACTOR));

  clutter_bin_layout_add (CLUTTER_BIN_LAYOUT (self->priv->preview_layout),
      rectangle, x, y);
  clutter_actor_hide (rectangle);

  return rectangle;
}

static void
empathy_call_window_create_preview_rectangles (EmpathyCallWindow *self)
{
  ClutterActor *box;

  self->priv->preview_layout = clutter_bin_layout_new (
      CLUTTER_BIN_ALIGNMENT_CENTER, CLUTTER_BIN_ALIGNMENT_CENTER);
  self->priv->preview_box = box = clutter_box_new (self->priv->preview_layout);

  clutter_bin_layout_add (CLUTTER_BIN_LAYOUT (self->priv->overlay_layout),
      box,
      CLUTTER_BIN_ALIGNMENT_FILL, CLUTTER_BIN_ALIGNMENT_FILL);

  self->priv->preview_rectangle1 =
      empathy_call_window_create_preview_rectangle (self,
          CLUTTER_BIN_ALIGNMENT_START, CLUTTER_BIN_ALIGNMENT_START);
  self->priv->preview_rectangle2 =
      empathy_call_window_create_preview_rectangle (self,
          CLUTTER_BIN_ALIGNMENT_START, CLUTTER_BIN_ALIGNMENT_END);
  self->priv->preview_rectangle3 =
      empathy_call_window_create_preview_rectangle (self,
          CLUTTER_BIN_ALIGNMENT_END, CLUTTER_BIN_ALIGNMENT_START);
  self->priv->preview_rectangle4 =
      empathy_call_window_create_preview_rectangle (self,
          CLUTTER_BIN_ALIGNMENT_END, CLUTTER_BIN_ALIGNMENT_END);
}

static void
empathy_call_window_show_preview_rectangles (EmpathyCallWindow *self,
    gboolean show)
{
  g_object_set (self->priv->preview_rectangle1, "visible", show, NULL);
  g_object_set (self->priv->preview_rectangle2, "visible", show, NULL);
  g_object_set (self->priv->preview_rectangle3, "visible", show, NULL);
  g_object_set (self->priv->preview_rectangle4, "visible", show, NULL);
}

static void
empathy_call_window_get_preview_coordinates (EmpathyCallWindow *self,
    PreviewPosition pos,
    guint *x,
    guint *y)
{
  guint ret_x = 0, ret_y = 0;
  ClutterGeometry box;

  if (!clutter_actor_has_allocation (self->priv->preview_box))
    goto out;

  clutter_actor_get_geometry (self->priv->preview_box, &box);

  switch (pos)
    {
      case PREVIEW_POS_TOP_LEFT:
        ret_x = ret_y = SELF_VIDEO_SECTION_MARGIN;
        break;
      case PREVIEW_POS_TOP_RIGHT:
        ret_x = box.width - SELF_VIDEO_SECTION_MARGIN
            - SELF_VIDEO_SECTION_WIDTH;
        ret_y = SELF_VIDEO_SECTION_MARGIN;
        break;
      case PREVIEW_POS_BOTTOM_LEFT:
        ret_x = SELF_VIDEO_SECTION_MARGIN;
        ret_y = box.height - SELF_VIDEO_SECTION_MARGIN
            - SELF_VIDEO_SECTION_HEIGHT;
        break;
      case PREVIEW_POS_BOTTOM_RIGHT:
        ret_x = box.width - SELF_VIDEO_SECTION_MARGIN
            - SELF_VIDEO_SECTION_WIDTH;
        ret_y = box.height - SELF_VIDEO_SECTION_MARGIN
            - SELF_VIDEO_SECTION_HEIGHT;
        break;
      default:
        g_warn_if_reached ();
    }

out:
  if (x != NULL)
    *x = ret_x;

  if (y != NULL)
    *y = ret_y;
}

static PreviewPosition
empathy_call_window_get_preview_position (EmpathyCallWindow *self,
    gfloat event_x,
    gfloat event_y)
{
  ClutterGeometry box;
  PreviewPosition pos = PREVIEW_POS_NONE;

  if (!clutter_actor_has_allocation (self->priv->preview_box))
    return pos;

  clutter_actor_get_geometry (self->priv->preview_box, &box);

  if (0 + SELF_VIDEO_SECTION_MARGIN <= event_x &&
      event_x <= (0 + SELF_VIDEO_SECTION_MARGIN + (gint) SELF_VIDEO_SECTION_WIDTH) &&
      0 + SELF_VIDEO_SECTION_MARGIN <= event_y &&
      event_y <= (0 + SELF_VIDEO_SECTION_MARGIN + (gint) SELF_VIDEO_SECTION_HEIGHT))
    {
      pos = PREVIEW_POS_TOP_LEFT;
    }
  else if (box.width - SELF_VIDEO_SECTION_MARGIN >= event_x &&
      event_x >= (box.width - SELF_VIDEO_SECTION_MARGIN - (gint) SELF_VIDEO_SECTION_WIDTH) &&
      0 + SELF_VIDEO_SECTION_MARGIN <= event_y &&
      event_y <= (0 + SELF_VIDEO_SECTION_MARGIN + (gint) SELF_VIDEO_SECTION_HEIGHT))
    {
      pos = PREVIEW_POS_TOP_RIGHT;
    }
  else if (0 + SELF_VIDEO_SECTION_MARGIN <= event_x &&
      event_x <= (0 + SELF_VIDEO_SECTION_MARGIN + (gint) SELF_VIDEO_SECTION_WIDTH) &&
      box.height - SELF_VIDEO_SECTION_MARGIN >= event_y &&
      event_y >= (box.height - SELF_VIDEO_SECTION_MARGIN - (gint) SELF_VIDEO_SECTION_HEIGHT))
    {
      pos = PREVIEW_POS_BOTTOM_LEFT;
    }
  else if (box.width - SELF_VIDEO_SECTION_MARGIN >= event_x &&
      event_x >= (box.width - SELF_VIDEO_SECTION_MARGIN - (gint) SELF_VIDEO_SECTION_WIDTH) &&
      box.height - 2 * SELF_VIDEO_SECTION_MARGIN >= event_y &&
      event_y >= (box.height - SELF_VIDEO_SECTION_MARGIN - (gint) SELF_VIDEO_SECTION_HEIGHT))
    {
      pos = PREVIEW_POS_BOTTOM_RIGHT;
    }

  return pos;
}

static ClutterActor *
empathy_call_window_get_preview_rectangle (EmpathyCallWindow *self,
    PreviewPosition pos)
{
  ClutterActor *rectangle;

  switch (pos)
    {
      case PREVIEW_POS_TOP_LEFT:
        rectangle = self->priv->preview_rectangle1;
        break;
      case PREVIEW_POS_TOP_RIGHT:
        rectangle = self->priv->preview_rectangle3;
        break;
      case PREVIEW_POS_BOTTOM_LEFT:
        rectangle = self->priv->preview_rectangle2;
        break;
      case PREVIEW_POS_BOTTOM_RIGHT:
        rectangle = self->priv->preview_rectangle4;
        break;
      default:
        rectangle = NULL;
    }

  return rectangle;
}

static void
empathy_call_window_move_video_preview (EmpathyCallWindow *self,
    PreviewPosition pos)
{
  ClutterBinLayout *layout = CLUTTER_BIN_LAYOUT (self->priv->preview_layout);

  DEBUG ("moving the video preview to %d", pos);

  self->priv->preview_pos = pos;

  switch (pos)
    {
      case PREVIEW_POS_TOP_LEFT:
        clutter_bin_layout_set_alignment (layout,
            self->priv->video_preview,
            CLUTTER_BIN_ALIGNMENT_START,
            CLUTTER_BIN_ALIGNMENT_START);
        break;
      case PREVIEW_POS_TOP_RIGHT:
        clutter_bin_layout_set_alignment (layout,
            self->priv->video_preview,
            CLUTTER_BIN_ALIGNMENT_END,
            CLUTTER_BIN_ALIGNMENT_START);
        break;
      case PREVIEW_POS_BOTTOM_LEFT:
        clutter_bin_layout_set_alignment (layout,
            self->priv->video_preview,
            CLUTTER_BIN_ALIGNMENT_START,
            CLUTTER_BIN_ALIGNMENT_END);
        break;
      case PREVIEW_POS_BOTTOM_RIGHT:
        clutter_bin_layout_set_alignment (layout,
            self->priv->video_preview,
            CLUTTER_BIN_ALIGNMENT_END,
            CLUTTER_BIN_ALIGNMENT_END);
        break;
      default:
        g_warn_if_reached ();
    }

  g_settings_set_enum (self->priv->settings, "camera-position", pos);
}

static void
empathy_call_window_highlight_preview_rectangle (EmpathyCallWindow *self,
    PreviewPosition pos)
{
  ClutterActor *rectangle;
  ClutterColor white = { 0xff, 0xff, 0xff, 0xff};

  rectangle = empathy_call_window_get_preview_rectangle (self, pos);

  empathy_rounded_rectangle_set_border_width (
      EMPATHY_ROUNDED_RECTANGLE (rectangle), 2 * SELF_VIDEO_SECTION_MARGIN);
  empathy_rounded_rectangle_set_border_color (
      EMPATHY_ROUNDED_RECTANGLE (rectangle), &white);
}

static void
empathy_call_window_darken_preview_rectangle (EmpathyCallWindow *self,
    ClutterActor *rectangle)
{
  ClutterColor white = { 0xff, 0xff, 0xff, 0xff}, darker;

  clutter_color_shade (&white, 0.55, &darker);

  empathy_rounded_rectangle_set_border_width (
      EMPATHY_ROUNDED_RECTANGLE (rectangle), 1);
  empathy_rounded_rectangle_set_border_color (
      EMPATHY_ROUNDED_RECTANGLE (rectangle), &darker);
}

static void
empathy_call_window_darken_preview_rectangles (EmpathyCallWindow *self)
{
  ClutterActor *rectangle;

  rectangle = empathy_call_window_get_preview_rectangle (self,
      self->priv->preview_pos);

  /* We don't want to darken the rectangle where the preview
   * currently is. */

  if (self->priv->preview_rectangle1 != rectangle)
    empathy_call_window_darken_preview_rectangle (self,
        self->priv->preview_rectangle1);

  if (self->priv->preview_rectangle2 != rectangle)
    empathy_call_window_darken_preview_rectangle (self,
        self->priv->preview_rectangle2);

  if (self->priv->preview_rectangle3 != rectangle)
    empathy_call_window_darken_preview_rectangle (self,
        self->priv->preview_rectangle3);

  if (self->priv->preview_rectangle4 != rectangle)
    empathy_call_window_darken_preview_rectangle (self,
        self->priv->preview_rectangle4);
}

static void
empathy_call_window_preview_on_drag_begin_cb (ClutterDragAction *action,
    ClutterActor *actor,
    gfloat event_x,
    gfloat event_y,
    ClutterModifierType modifiers,
    EmpathyCallWindow *self)
{
  ClutterActor *stage = clutter_actor_get_stage (actor);
  gfloat rel_x, rel_y;

  self->priv->drag_preview = clutter_clone_new (actor);

  clutter_container_add_actor (CLUTTER_CONTAINER (stage),
      self->priv->drag_preview);

  clutter_actor_transform_stage_point (actor, event_x, event_y,
      &rel_x, &rel_y);

  clutter_actor_set_position (self->priv->drag_preview,
      event_x - rel_x, event_y - rel_y);

  clutter_drag_action_set_drag_handle (action,
      self->priv->drag_preview);

  clutter_actor_set_opacity (actor, 0);
  clutter_actor_hide (self->priv->preview_shown_button);

  empathy_call_window_show_preview_rectangles (self, TRUE);
  empathy_call_window_darken_preview_rectangles (self);
}

static void
empathy_call_window_on_animation_completed_cb (ClutterAnimation *animation,
    ClutterActor *actor)
{
  clutter_actor_set_opacity (actor, 255);
}

static void
empathy_call_window_preview_on_drag_end_cb (ClutterDragAction *action,
    ClutterActor *actor,
    gfloat event_x,
    gfloat event_y,
    ClutterModifierType modifiers,
    EmpathyCallWindow *self)
{
  PreviewPosition pos;
  guint x, y;

  /* Get the position before destroying the drag actor, otherwise the
   * preview_box allocation won't be valid and we won't be able to
   * calculate the position. */
  pos = empathy_call_window_get_preview_position (self, event_x, event_y);

  empathy_call_window_get_preview_coordinates (self,
      pos != PREVIEW_POS_NONE ? pos : self->priv->preview_pos,
      &x, &y);

  /* Move the preview to the destination and destroy it afterwards */
  clutter_actor_animate (self->priv->drag_preview, CLUTTER_LINEAR, 500,
      "x", (gfloat) x,
      "y", (gfloat) y,
      "signal-swapped-after::completed",
        clutter_actor_destroy, self->priv->drag_preview,
      "signal-swapped-after::completed",
        clutter_actor_show, self->priv->preview_shown_button,
      "signal::completed",
        empathy_call_window_on_animation_completed_cb, actor,
      NULL);

  self->priv->drag_preview = NULL;

  if (pos != PREVIEW_POS_NONE)
    empathy_call_window_move_video_preview (self, pos);

  empathy_call_window_show_preview_rectangles (self, FALSE);
}

static void
empathy_call_window_preview_on_drag_motion_cb (ClutterDragAction *action,
    ClutterActor *actor,
    gfloat delta_x,
    gfloat delta_y,
    EmpathyCallWindow *self)
{
  PreviewPosition pos;
  gfloat event_x, event_y;

  clutter_drag_action_get_motion_coords (action, &event_x, &event_y);

  pos = empathy_call_window_get_preview_position (self, event_x, event_y);

  if (pos != PREVIEW_POS_NONE)
    empathy_call_window_highlight_preview_rectangle (self, pos);
  else
    empathy_call_window_darken_preview_rectangles (self);
}

static gboolean
empathy_call_window_preview_enter_event_cb (ClutterActor *actor,
    ClutterCrossingEvent *event,
    EmpathyCallWindow *self)
{
  ClutterActor *rectangle;

  rectangle = empathy_call_window_get_preview_rectangle (self,
      self->priv->preview_pos);

  empathy_call_window_highlight_preview_rectangle (self,
      self->priv->preview_pos);

  clutter_actor_show (rectangle);

  return FALSE;
}

static gboolean
empathy_call_window_preview_leave_event_cb (ClutterActor *actor,
    ClutterCrossingEvent *event,
    EmpathyCallWindow *self)
{
  ClutterActor *rectangle;

  rectangle = empathy_call_window_get_preview_rectangle (self,
      self->priv->preview_pos);

  empathy_call_window_darken_preview_rectangle (self, rectangle);

  clutter_actor_hide (rectangle);

  return FALSE;
}

static void
create_video_preview (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  ClutterLayoutManager *layout;
  ClutterActor *preview;
  ClutterActor *b;
  ClutterAction *action;
  PreviewPosition pos;

  g_assert (priv->video_preview == NULL);

  pos = g_settings_get_enum (priv->settings, "camera-position");

  preview = empathy_rounded_texture_new ();
  clutter_actor_set_size (preview,
      SELF_VIDEO_SECTION_WIDTH, SELF_VIDEO_SECTION_HEIGHT);

  priv->video_preview_sink = GST_ELEMENT (clutter_gst_video_sink_new ());
  g_object_add_weak_pointer (G_OBJECT (priv->video_preview_sink), (gpointer) &priv->video_preview_sink);

  /* Add a little offset to the video preview */
  layout = clutter_bin_layout_new (CLUTTER_BIN_ALIGNMENT_CENTER,
      CLUTTER_BIN_ALIGNMENT_CENTER);
  priv->video_preview = clutter_box_new (layout);
  clutter_actor_set_size (priv->video_preview,
      SELF_VIDEO_SECTION_WIDTH + 2 * SELF_VIDEO_SECTION_MARGIN,
      SELF_VIDEO_SECTION_HEIGHT + 2 * SELF_VIDEO_SECTION_MARGIN);
  clutter_actor_set_content (priv->video_preview,
      g_object_new (CLUTTER_GST_TYPE_ASPECTRATIO,
                    "sink", priv->video_preview,
                    NULL));

  /* Spinner for when changing the camera device */
  priv->preview_spinner_widget = gtk_spinner_new ();
  priv->preview_spinner_actor = empathy_rounded_actor_new (PREVIEW_ROUND_FACTOR);

  g_object_set (priv->preview_spinner_widget, "expand", TRUE, NULL);
  make_background_transparent (GTK_CLUTTER_ACTOR (priv->preview_spinner_actor));
  gtk_widget_show (priv->preview_spinner_widget);

  gtk_container_add (
      GTK_CONTAINER (gtk_clutter_actor_get_widget (
          GTK_CLUTTER_ACTOR (priv->preview_spinner_actor))),
      priv->preview_spinner_widget);
  clutter_actor_set_size (priv->preview_spinner_actor,
      SELF_VIDEO_SECTION_WIDTH, SELF_VIDEO_SECTION_HEIGHT);
  clutter_actor_set_opacity (priv->preview_spinner_actor, 128);
  clutter_actor_hide (priv->preview_spinner_actor);

  clutter_container_add_actor (CLUTTER_CONTAINER (priv->video_preview),
      preview);
  clutter_container_add_actor (CLUTTER_CONTAINER (priv->video_preview),
      priv->preview_spinner_actor);

  g_object_set (priv->video_preview_sink,
      "sync", FALSE,
      "async", FALSE,
      NULL);

  /* Preview show */
  priv->preview_shown_button = b = gtk_clutter_actor_new_with_contents (
      gtk_image_new_from_icon_name ("emblem-system-symbolic",
        GTK_ICON_SIZE_MENU));
  clutter_actor_set_margin_right (b, 4);
  clutter_actor_set_margin_bottom (b, 2);
  clutter_actor_set_opacity (b, PREVIEW_BUTTON_OPACITY);
  make_background_transparent (GTK_CLUTTER_ACTOR (b));

  clutter_bin_layout_add (CLUTTER_BIN_LAYOUT (layout), b,
      CLUTTER_BIN_ALIGNMENT_END, CLUTTER_BIN_ALIGNMENT_END);

  action = clutter_click_action_new ();
  clutter_actor_add_action (b, action);
  g_signal_connect (action, "clicked",
      G_CALLBACK (empathy_call_window_preview_button_clicked_cb), self);

  /* Preview hidden */
  priv->preview_hidden_button = b = gtk_clutter_actor_new_with_contents (
      gtk_image_new_from_icon_name ("emblem-system-symbolic",
        GTK_ICON_SIZE_MENU));
  make_background_transparent (GTK_CLUTTER_ACTOR (b));

  clutter_bin_layout_add (CLUTTER_BIN_LAYOUT (priv->preview_layout),
      priv->preview_hidden_button,
      CLUTTER_BIN_ALIGNMENT_START,
      CLUTTER_BIN_ALIGNMENT_END);

  self->priv->preview_pos = PREVIEW_POS_BOTTOM_LEFT;

  clutter_actor_hide (priv->preview_hidden_button);

  action = clutter_click_action_new ();
  clutter_actor_add_action (b, action);
  g_signal_connect (action, "clicked",
      G_CALLBACK (empathy_call_window_preview_hidden_button_clicked_cb), self);

  clutter_bin_layout_add (CLUTTER_BIN_LAYOUT (priv->preview_layout),
      priv->video_preview,
      CLUTTER_BIN_ALIGNMENT_START,
      CLUTTER_BIN_ALIGNMENT_END);

  empathy_call_window_move_video_preview (self, pos);

  action = clutter_drag_action_new ();
  g_signal_connect (action, "drag-begin",
      G_CALLBACK (empathy_call_window_preview_on_drag_begin_cb), self);
  g_signal_connect (action, "drag-end",
      G_CALLBACK (empathy_call_window_preview_on_drag_end_cb), self);
  g_signal_connect (action, "drag-motion",
      G_CALLBACK (empathy_call_window_preview_on_drag_motion_cb), self);

  g_signal_connect (preview, "enter-event",
      G_CALLBACK (empathy_call_window_preview_enter_event_cb), self);
  g_signal_connect (preview, "leave-event",
      G_CALLBACK (empathy_call_window_preview_leave_event_cb), self);

  clutter_actor_add_action (preview, action);
  clutter_actor_set_reactive (preview, TRUE);
  clutter_actor_set_reactive (priv->preview_shown_button, TRUE);
}

static void
empathy_call_window_start_camera_spinning (EmpathyCallWindow *self)
{
  clutter_actor_show (self->priv->preview_spinner_actor);
  gtk_spinner_start (GTK_SPINNER (self->priv->preview_spinner_widget));
}

static void
empathy_call_window_stop_camera_spinning (EmpathyCallWindow *self)
{
  clutter_actor_hide (self->priv->preview_spinner_actor);
  gtk_spinner_stop (GTK_SPINNER (self->priv->preview_spinner_widget));
}

void
empathy_call_window_play_camera (EmpathyCallWindow *self,
    gboolean play)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GstElement *preview;
  GstState state;

  if (priv->video_preview == NULL)
    {
      create_video_preview (self);
      add_video_preview_to_pipeline (self);
    }

  if (play)
    {
      state = GST_STATE_PLAYING;
    }
  else
    {
      empathy_call_window_start_camera_spinning (self);
      state = GST_STATE_NULL;
    }

  preview = priv->video_preview_sink;

  gst_element_set_state (preview, state);
  gst_element_set_state (priv->video_tee, state);
  gst_element_set_state (priv->video_input, state);
}

static void
display_video_preview (EmpathyCallWindow *self,
    gboolean display)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  if (priv->video_preview == NULL)
    {
      create_video_preview (self);
      add_video_preview_to_pipeline (self);
    }

  if (display)
    {
      /* Display the video preview */
      DEBUG ("Show video preview");

      empathy_call_window_play_camera (self, TRUE);
      clutter_actor_show (priv->video_preview);
      clutter_actor_raise_top (priv->floating_toolbar);
    }
  else
    {
      /* Hide the video preview */
      DEBUG ("Hide video preview");

      if (priv->video_preview != NULL)
        {
          clutter_actor_hide (priv->video_preview);
          empathy_call_window_play_camera (self, FALSE);
        }
    }
}

static void
empathy_call_window_set_state_connecting (EmpathyCallWindow *window)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);

  empathy_call_window_status_message (window, _("Connecting…"));
  priv->call_state = CONNECTING;

  /* Show the toolbar */
  clutter_state_set_state (priv->transitions, "fade-in");

  if (priv->outgoing)
    empathy_sound_manager_start_playing (priv->sound_mgr, GTK_WIDGET (window),
        EMPATHY_SOUND_PHONE_OUTGOING, MS_BETWEEN_RING);
}

static void
disable_camera (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  if (priv->camera_state == CAMERA_STATE_OFF)
    return;

  DEBUG ("Disable camera");

  empathy_call_window_set_send_video (self, CAMERA_STATE_OFF);

  priv->camera_state = CAMERA_STATE_OFF;
}

static void
enable_camera (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  if (priv->camera_state == CAMERA_STATE_ON)
    return;

  if (priv->video_input == NULL)
    {
      DEBUG ("Can't enable camera, no input");
      return;
    }

  DEBUG ("Enable camera");

  empathy_call_window_set_send_video (self, CAMERA_STATE_ON);

  priv->camera_state = CAMERA_STATE_ON;
}

static void
empathy_call_window_camera_toggled_cb (GtkToggleButton *toggle,
  EmpathyCallWindow *self)
{
  if (gtk_toggle_button_get_active (toggle))
    enable_camera (self);
  else
    disable_camera (self);
}

static void
create_pipeline (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GstBus *bus;

  g_assert (priv->pipeline == NULL);

  priv->pipeline = gst_pipeline_new (NULL);
  priv->pipeline_playing = FALSE;

  priv->video_tee = gst_element_factory_make ("tee", NULL);
  gst_object_ref_sink (priv->video_tee);

  gst_bin_add (GST_BIN (priv->pipeline), priv->video_tee);

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));
  priv->bus_message_source_id = gst_bus_add_watch (bus,
      empathy_call_window_bus_message, self);

  g_object_unref (bus);
}

static void
empathy_call_window_settings_cb (GtkAction *action,
    EmpathyCallWindow *self)
{
  gchar *args = g_strdup_printf ("-p %s",
      empathy_preferences_tab_to_string (EMPATHY_PREFERENCES_TAB_CALLS));

  empathy_launch_program (BIN_DIR, "empathy", args);

  g_free (args);
}

static void
empathy_call_window_contents_cb (GtkAction *action,
    EmpathyCallWindow *self)
{
  empathy_url_show (GTK_WIDGET (self), "help:empathy/audio-video");
}

static void
show_png (GPid pid, gint status, gpointer user_data)
{
  gtk_show_uri (NULL, (gchar *) user_data, GDK_CURRENT_TIME, NULL);
  g_spawn_close_pid (pid);
  g_free (user_data);
}

static void
empathy_call_window_debug_gst_cb (GtkAction *action,
    EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GDateTime *date_time;
  GPid dot_pid;
  const gchar *dot_dir;
  const gchar *prgname;
  gchar *dot_cmd;
  gchar *filename;
  gchar **argv;
  gint argc;

  if (priv->pipeline == NULL)
    DEBUG ("No pipeline");

  date_time = g_date_time_new_now_utc ();
  prgname = g_get_prgname ();
  filename = g_strdup_printf ("%s-%" G_GINT64_FORMAT, prgname,
      g_date_time_to_unix (date_time));

  GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (priv->pipeline),
      GST_DEBUG_GRAPH_SHOW_ALL, filename);

  dot_dir = g_getenv ("GST_DEBUG_DUMP_DOT_DIR");
  dot_cmd = g_strdup_printf ("dot -Tpng -o %s.png %s.dot",
      filename,
      filename);
  g_shell_parse_argv (dot_cmd, &argc, &argv, NULL);

  if (g_spawn_async (dot_dir,
          argv,
          NULL,
          G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
          NULL,
          NULL,
          &dot_pid,
          NULL))
    {
      gchar *uri = g_strdup_printf ("file://%s/%s.png", dot_dir, filename);
      g_child_watch_add (dot_pid, show_png, uri);
    }

  g_strfreev (argv);
  g_free (dot_cmd);
  g_free (filename);
  g_date_time_unref (date_time);
}

static void
empathy_call_window_debug_tp_cb (GtkAction *action,
    EmpathyCallWindow *self)
{
  empathy_launch_program (BIN_DIR, "empathy-debugger", "-s Empathy.Call");
}

static void
empathy_call_window_about_cb (GtkAction *action,
    EmpathyCallWindow *self)
{
  empathy_about_dialog_new (GTK_WINDOW (self));
}

static gboolean
empathy_call_window_toolbar_timeout (gpointer data)
{
  EmpathyCallWindow *self = data;

  /* We don't want to hide the toolbar if we're not in a call, as
   * to show the call status all the time. Also don't hide if we're muted
   * to prevent the awkward, talking when muted situation */
  if (self->priv->call_state != CONNECTING &&
      self->priv->call_state != DISCONNECTED &&
      !self->priv->muted)
    clutter_state_set_state (self->priv->transitions, "fade-out");

  return TRUE;
}

static gboolean
empathy_call_window_motion_notify_cb (GtkWidget *widget,
    GdkEvent *event,
    EmpathyCallWindow *self)
{
  clutter_state_set_state (self->priv->transitions, "fade-in");

  if (self->priv->inactivity_src > 0)
    g_source_remove (self->priv->inactivity_src);

  self->priv->inactivity_src = g_timeout_add_seconds (3,
      empathy_call_window_toolbar_timeout, self);

  return FALSE;
}

static gboolean
empathy_call_window_configure_event_cb (GtkWidget *widget,
    GdkEvent  *event,
    EmpathyCallWindow *self)
{
  GdkWindow *gdk_window;
  GdkWindowState window_state;

  gtk_window_get_position (GTK_WINDOW (self), &self->priv->x, &self->priv->y);
  gtk_window_get_size (GTK_WINDOW (self), &self->priv->w, &self->priv->h);

  gtk_widget_get_preferred_width (self->priv->dtmf_panel,
      &self->priv->dialpad_width, NULL);

  gdk_window = gtk_widget_get_window (widget);
  window_state = gdk_window_get_state (gdk_window);
  self->priv->maximized = (window_state & GDK_WINDOW_STATE_MAXIMIZED);

  return FALSE;
}

static void
empathy_call_window_destroyed_cb (GtkWidget *object,
    EmpathyCallWindow *self)
{
  if (gtk_widget_get_visible (self->priv->dtmf_panel))
    {
      /* Save the geometry as if the dialpad was hidden. */
      empathy_geometry_save_values (GTK_WINDOW (self),
          self->priv->x, self->priv->y,
          self->priv->w - self->priv->dialpad_width, self->priv->h,
          self->priv->maximized);
    }
}

static void
empathy_call_window_incoming_call_response_cb (GtkDialog *dialog,
    gint response_id,
    EmpathyCallWindow *self)
{
  switch (response_id)
    {
      case GTK_RESPONSE_ACCEPT:
        tp_channel_dispatch_operation_handle_with_time_async (
            self->priv->pending_cdo, EMPATHY_CALL_TP_BUS_NAME,
            empathy_get_current_action_time (), NULL, NULL);

        tp_clear_object (&self->priv->pending_cdo);
        tp_clear_object (&self->priv->pending_channel);
        tp_clear_object (&self->priv->pending_context);

        break;
      case GTK_RESPONSE_CANCEL:
        tp_channel_dispatch_operation_close_channels_async (
            self->priv->pending_cdo, NULL, NULL);

        empathy_call_window_status_message (self, _("Disconnected"));
        self->priv->call_state = DISCONNECTED;
        break;
      default:
        g_warn_if_reached ();
    }
}

static void
empathy_call_window_set_state_ringing (EmpathyCallWindow *self)
{
  gboolean video;

  g_assert (self->priv->call_state != CONNECTED);

  video = tp_call_channel_has_initial_video (self->priv->pending_channel, NULL);

  empathy_call_window_status_message (self, _("Incoming call"));
  self->priv->call_state = RINGING;

  self->priv->incoming_call_dialog = gtk_message_dialog_new (
      GTK_WINDOW (self), GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
      video ? _("Incoming video call from %s") : _("Incoming call from %s"),
      empathy_contact_get_alias (self->priv->contact));

  gtk_dialog_add_buttons (GTK_DIALOG (self->priv->incoming_call_dialog),
      _("Reject"), GTK_RESPONSE_CANCEL,
      _("Answer"), GTK_RESPONSE_ACCEPT,
      NULL);

  g_signal_connect (self->priv->incoming_call_dialog, "response",
      G_CALLBACK (empathy_call_window_incoming_call_response_cb), self);
  gtk_widget_show (self->priv->incoming_call_dialog);
}

static void
empathy_call_window_cdo_invalidated_cb (TpProxy *channel,
    guint domain,
    gint code,
    gchar *message,
    EmpathyCallWindow *self)
{
  tp_clear_object (&self->priv->pending_cdo);
  tp_clear_object (&self->priv->pending_channel);
  tp_clear_object (&self->priv->pending_context);

  /* We don't know if the incoming call has been accepted or not, so we
   * assume it hasn't and if it has, we'll set the proper status when
   * we get the new handler. */
  empathy_call_window_status_message (self, _("Disconnected"));
  self->priv->call_state = DISCONNECTED;

  gtk_widget_destroy (self->priv->incoming_call_dialog);
  self->priv->incoming_call_dialog = NULL;
}

void
empathy_call_window_start_ringing (EmpathyCallWindow *self,
    TpCallChannel *channel,
    TpChannelDispatchOperation *dispatch_operation,
    TpAddDispatchOperationContext *context)
{
  g_assert (self->priv->pending_channel == NULL);
  g_assert (self->priv->pending_context == NULL);
  g_assert (self->priv->pending_cdo == NULL);

  /* Start ringing and delay until the user answers or hangs. */
  self->priv->pending_channel = g_object_ref (channel);
  self->priv->pending_context = g_object_ref (context);
  self->priv->pending_cdo = g_object_ref (dispatch_operation);

  g_signal_connect (self->priv->pending_cdo, "invalidated",
      G_CALLBACK (empathy_call_window_cdo_invalidated_cb), self);

  empathy_call_window_set_state_ringing (self);
  tp_add_dispatch_operation_context_accept (context);
}

static void
mic_button_clicked (GtkWidget *button,
    EmpathyCallWindow *self)
{
  /* Toggle the muted state. We rely on audio_input_mute_notify_cb to update
   * the icon. */
  empathy_audio_src_set_mute (EMPATHY_GST_AUDIO_SRC (self->priv->audio_input),
      !self->priv->muted);
}

static void
empathy_call_window_init (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv;
  GtkBuilder *gui;
  GtkWidget *top_vbox;
  gchar *filename;
  ClutterConstraint *constraint;
  ClutterActor *remote_avatar;
  ClutterColor black = { 0, 0, 0, 0 };
  ClutterMargin overlay_margin = { OVERLAY_MARGIN, OVERLAY_MARGIN,
    OVERLAY_MARGIN, OVERLAY_MARGIN };

  priv = self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
    EMPATHY_TYPE_CALL_WINDOW, EmpathyCallWindowPriv);

  priv->settings = g_settings_new (EMPATHY_PREFS_CALL_SCHEMA);
  priv->timer = g_timer_new ();

  filename = empathy_file_lookup ("empathy-call-window.ui", "src");
  gui = tpaw_builder_get_file (filename,
    "call_window_vbox", &top_vbox,
    "errors_vbox", &priv->errors_vbox,
    "pane", &priv->pane,
    "remote_user_name_toolbar", &priv->remote_user_name_toolbar,
    "remote_user_status_toolbar", &priv->remote_user_status_toolbar,
    "remote_user_avatar_toolbar", &priv->remote_user_avatar_toolbar,
    "status_label", &priv->status_label,
    "audiocall", &priv->audio_call_button,
    "videocall", &priv->video_call_button,
    "microphone", &priv->mic_button,
    "microphone_icon", &priv->microphone_icon,
    "volume", &priv->volume_button,
    "camera", &priv->camera_button,
    "hangup", &priv->hangup_button,
    "dialpad", &priv->dialpad_button,
    "toolbar", &priv->toolbar,
    "bottom_toolbar", &priv->bottom_toolbar,
    "ui_manager", &priv->ui_manager,
    "menufullscreen", &priv->menu_fullscreen,
    "menupreviewswap", &priv->menu_swap_camera,
    "details_vbox",  &priv->details_vbox,
    "vcodec_encoding_label", &priv->vcodec_encoding_label,
    "acodec_encoding_label", &priv->acodec_encoding_label,
    "acodec_decoding_label", &priv->acodec_decoding_label,
    "vcodec_decoding_label", &priv->vcodec_decoding_label,
    "audio_remote_candidate_label", &priv->audio_remote_candidate_label,
    "audio_local_candidate_label", &priv->audio_local_candidate_label,
    "video_remote_candidate_label", &priv->video_remote_candidate_label,
    "video_local_candidate_label", &priv->video_local_candidate_label,
    "video_remote_candidate_info_img", &priv->video_remote_candidate_info_img,
    "video_local_candidate_info_img", &priv->video_local_candidate_info_img,
    "audio_remote_candidate_info_img", &priv->audio_remote_candidate_info_img,
    "audio_local_candidate_info_img", &priv->audio_local_candidate_info_img,
    NULL);
  g_free (filename);

  tpaw_builder_connect (gui, self,
    "hangup", "clicked", empathy_call_window_hangup_cb,
    "audiocall", "clicked", empathy_call_window_audio_call_cb,
    "videocall", "clicked", empathy_call_window_video_call_cb,
    "camera", "toggled", empathy_call_window_camera_toggled_cb,
    "dialpad", "toggled", empathy_call_window_dialpad_cb,
    "menufullscreen", "activate", empathy_call_window_fullscreen_cb,
    "menusettings", "activate", empathy_call_window_settings_cb,
    "menucontents", "activate", empathy_call_window_contents_cb,
    "menudebuggst", "activate", empathy_call_window_debug_gst_cb,
    "menudebugtp", "activate", empathy_call_window_debug_tp_cb,
    "menuabout", "activate", empathy_call_window_about_cb,
    "menupreviewdisable", "activate", empathy_call_window_disable_camera_cb,
    "menupreviewminimise", "activate", empathy_call_window_minimise_camera_cb,
    "menupreviewmaximise", "activate", empathy_call_window_maximise_camera_cb,
    "menupreviewswap", "activate", empathy_call_window_swap_camera_cb,
    NULL);

  empathy_set_css_provider (GTK_WIDGET (self));
  gtk_action_set_sensitive (priv->menu_fullscreen, FALSE);

  priv->camera_monitor = tpaw_camera_monitor_dup_singleton ();
  empathy_call_window_update_swap_camera (self);

  g_object_bind_property (priv->camera_monitor, "available",
      priv->camera_button, "sensitive",
      G_BINDING_SYNC_CREATE);

  g_signal_connect (priv->camera_monitor, "added",
      G_CALLBACK (empathy_call_window_camera_added_cb), self);
  g_signal_connect (priv->camera_monitor, "removed",
      G_CALLBACK (empathy_call_window_camera_removed_cb), self);

  g_signal_connect (priv->mic_button, "clicked",
      G_CALLBACK (mic_button_clicked), self);

  g_mutex_init (&priv->lock);

  gtk_container_add (GTK_CONTAINER (self), top_vbox);

  priv->content_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,
      CONTENT_HBOX_SPACING);
  gtk_box_pack_start (GTK_BOX (priv->pane), priv->content_hbox,
      TRUE, TRUE, 0);

  /* main contents remote avatar/video box */
  priv->video_layout = clutter_bin_layout_new (CLUTTER_BIN_ALIGNMENT_FILL,
      CLUTTER_BIN_ALIGNMENT_FILL);

  priv->video_box = clutter_box_new (priv->video_layout);

  priv->video_container = gtk_clutter_embed_new ();

  gtk_widget_set_size_request (priv->video_container,
      REMOTE_VIDEO_DEFAULT_WIDTH, REMOTE_VIDEO_DEFAULT_HEIGHT);

  /* Set the background black */
  clutter_stage_set_color (
      CLUTTER_STAGE (gtk_clutter_embed_get_stage (
          GTK_CLUTTER_EMBED (priv->video_container))),
      &black);

  clutter_container_add (
      CLUTTER_CONTAINER (gtk_clutter_embed_get_stage (
          GTK_CLUTTER_EMBED (priv->video_container))),
      priv->video_box,
      NULL);

  constraint = clutter_bind_constraint_new (
      gtk_clutter_embed_get_stage (GTK_CLUTTER_EMBED (priv->video_container)),
      CLUTTER_BIND_SIZE, 0);
  clutter_actor_add_constraint (priv->video_box, constraint);

  priv->remote_user_avatar_widget = gtk_image_new ();
  remote_avatar = gtk_clutter_actor_new_with_contents (
      priv->remote_user_avatar_widget);
  make_background_transparent (GTK_CLUTTER_ACTOR (remote_avatar));

  clutter_container_add_actor (CLUTTER_CONTAINER (priv->video_box),
      remote_avatar);

  /* create the overlay bin */
  priv->overlay_layout = clutter_bin_layout_new (CLUTTER_BIN_ALIGNMENT_CENTER,
    CLUTTER_BIN_ALIGNMENT_CENTER);
  priv->overlay_bin = clutter_actor_new ();
  clutter_actor_set_layout_manager (priv->overlay_bin, priv->overlay_layout);

  clutter_actor_set_margin (priv->overlay_bin, &overlay_margin);

  clutter_bin_layout_add (CLUTTER_BIN_LAYOUT (priv->video_layout),
      priv->overlay_bin,
      CLUTTER_BIN_ALIGNMENT_FILL, CLUTTER_BIN_ALIGNMENT_FILL);

  empathy_call_window_create_preview_rectangles (self);

  gtk_box_pack_start (GTK_BOX (priv->content_hbox),
      priv->video_container, TRUE, TRUE,
      CONTENT_HBOX_CHILDREN_PACKING_PADDING);

  create_pipeline (self);
  create_video_output_widget (self);
  create_audio_input (self);
  create_video_input (self);

  priv->floating_toolbar = gtk_clutter_actor_new ();
  clutter_actor_set_reactive (priv->floating_toolbar, TRUE);
  make_background_transparent (GTK_CLUTTER_ACTOR (priv->floating_toolbar));

  gtk_style_context_add_class (
      gtk_widget_get_style_context (GTK_WIDGET (priv->bottom_toolbar)),
      GTK_STYLE_CLASS_OSD);
  gtk_widget_reparent (priv->bottom_toolbar,
      gtk_clutter_actor_get_widget (GTK_CLUTTER_ACTOR (priv->floating_toolbar)));

  clutter_bin_layout_add (CLUTTER_BIN_LAYOUT (priv->overlay_layout),
      priv->floating_toolbar,
      CLUTTER_BIN_ALIGNMENT_CENTER, CLUTTER_BIN_ALIGNMENT_END);

  clutter_actor_raise_top (priv->floating_toolbar);

  /* Transitions for the floating toolbar */
  priv->transitions = clutter_state_new ();

  /* all transitions last for 2s */
  clutter_state_set_duration (priv->transitions, NULL, NULL, 2000);

  /* transition from any state to "fade-out" state */
  clutter_state_set (priv->transitions, NULL, "fade-out",
      priv->floating_toolbar,
      "opacity", CLUTTER_EASE_OUT_QUAD, 0,
      NULL);

  /* transition from any state to "fade-in" state */
  clutter_state_set (priv->transitions, NULL, "fade-in",
      priv->floating_toolbar,
      "opacity", CLUTTER_EASE_OUT_QUAD, 255,
      NULL);

  /* put the actor into the "fade-in" state with no animation */
  clutter_state_warp_to_state (priv->transitions, "fade-in");

  /* The call will be started as soon the pipeline is playing */
  priv->start_call_when_playing = TRUE;

  priv->dtmf_panel = empathy_dialpad_widget_new ();
  g_signal_connect (priv->dtmf_panel, "start-tone",
      G_CALLBACK (dtmf_start_tone_cb), self);

  gtk_box_pack_start (GTK_BOX (priv->pane), priv->dtmf_panel,
      FALSE, FALSE, 6);

  gtk_box_pack_start (GTK_BOX (priv->pane), priv->details_vbox,
      FALSE, FALSE, 0);

  gtk_widget_set_sensitive (priv->dtmf_panel, FALSE);

  gtk_widget_show_all (top_vbox);

  gtk_widget_hide (priv->dtmf_panel);
  gtk_widget_hide (priv->details_vbox);

  priv->fullscreen = empathy_call_window_fullscreen_new (self);

  empathy_call_window_fullscreen_set_video_widget (priv->fullscreen,
      priv->video_container);

  /* We hide the bottom toolbar after 3s of inactivity and show it
   * again on mouse movement */
  priv->inactivity_src = g_timeout_add_seconds (3,
      empathy_call_window_toolbar_timeout, self);

  g_signal_connect (G_OBJECT (priv->fullscreen->leave_fullscreen_button),
      "clicked", G_CALLBACK (empathy_call_window_fullscreen_cb), self);

  g_signal_connect (G_OBJECT (self), "realize",
    G_CALLBACK (empathy_call_window_realized_cb), self);

  g_signal_connect (G_OBJECT (self), "delete-event",
    G_CALLBACK (empathy_call_window_delete_cb), self);

  g_signal_connect (G_OBJECT (self), "window-state-event",
    G_CALLBACK (empathy_call_window_state_event_cb), self);

  g_signal_connect (G_OBJECT (self), "key-press-event",
      G_CALLBACK (empathy_call_window_key_press_cb), self);

  g_signal_connect (self, "motion-notify-event",
      G_CALLBACK (empathy_call_window_motion_notify_cb), self);

  g_object_ref (priv->ui_manager);
  g_object_unref (gui);

  priv->sound_mgr = empathy_sound_manager_dup_singleton ();
  priv->mic_menu = empathy_mic_menu_new (self);
  priv->camera_menu = empathy_camera_menu_new (self);

  empathy_call_window_show_hangup_button (self, TRUE);

  gtk_window_set_default_size (GTK_WINDOW (self), 580, 480);

  empathy_geometry_bind (GTK_WINDOW (self), "call-window");
  /* These signals are used to track the window position and save it
   * when the window is destroyed. We need to do this as we don't want
   * the window geometry to be saved with the dialpad taken into account. */
  g_signal_connect (self, "destroy",
      G_CALLBACK (empathy_call_window_destroyed_cb), self);
  g_signal_connect (self, "configure-event",
      G_CALLBACK (empathy_call_window_configure_event_cb), self);
  g_signal_connect (self, "window-state-event",
      G_CALLBACK (empathy_call_window_configure_event_cb), self);

  /* Don't display labels in both toolbars */
  gtk_toolbar_set_style (GTK_TOOLBAR (priv->toolbar), GTK_TOOLBAR_ICONS);
}

/* Instead of specifying a width and a height, we specify only one size. That's
   because we want a square avatar icon.  */
static void
init_contact_avatar_with_size (EmpathyContact *contact,
    GtkWidget *image_widget,
    gint size)
{
  GdkPixbuf *pixbuf_avatar = NULL;

  if (contact != NULL)
    {
      pixbuf_avatar = empathy_pixbuf_avatar_from_contact_scaled (contact,
        size, size);
    }

  if (pixbuf_avatar == NULL)
    {
      pixbuf_avatar = tpaw_pixbuf_from_icon_name_sized (
          TPAW_IMAGE_AVATAR_DEFAULT, size);
    }

  gtk_image_set_from_pixbuf (GTK_IMAGE (image_widget), pixbuf_avatar);

  if (pixbuf_avatar != NULL)
    g_object_unref (pixbuf_avatar);
}

static void
set_window_title (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  gchar *tmp;

  if (priv->contact != NULL)
    {
      /* translators: Call is a noun and %s is the contact name. This string
       * is used in the window title */
      tmp = g_strdup_printf (_("Call with %s"),
          empathy_contact_get_alias (priv->contact));
      gtk_window_set_title (GTK_WINDOW (self), tmp);
      g_free (tmp);
    }
  else
    {
      g_warning ("Unknown remote contact!");
    }
}

static void
set_remote_user_name (EmpathyCallWindow *self,
  EmpathyContact *contact)
{
  const gchar *alias = empathy_contact_get_alias (contact);
  const gchar *status = empathy_contact_get_status (contact);

  gtk_label_set_text (GTK_LABEL (self->priv->remote_user_name_toolbar), alias);

  if (status != NULL) {
    gchar *markup;

    markup = g_markup_printf_escaped ("<small>%s</small>", status);
    gtk_label_set_markup (GTK_LABEL (self->priv->remote_user_status_toolbar),
      markup);
    g_free (markup);
  } else {
    gtk_label_set_markup (GTK_LABEL (self->priv->remote_user_status_toolbar),
      "");
  }
}

static void
contact_name_changed_cb (EmpathyContact *contact,
    GParamSpec *pspec,
    EmpathyCallWindow *self)
{
  set_window_title (self);
  set_remote_user_name (self, contact);
}

static void
contact_presence_changed_cb (EmpathyContact *contact,
    GParamSpec *pspec,
    EmpathyCallWindow *self)
{
  set_remote_user_name (self, contact);
}

static void
contact_avatar_changed_cb (EmpathyContact *contact,
    GParamSpec *pspec,
    EmpathyCallWindow *self)
{
  int size;
  GtkAllocation allocation;
  GtkWidget *avatar_widget;

  avatar_widget = self->priv->remote_user_avatar_widget;

  gtk_widget_get_allocation (avatar_widget, &allocation);
  size = allocation.height;

  if (size == 0)
    {
      /* the widget is not allocated yet, set a default size */
      size = MIN (REMOTE_CONTACT_AVATAR_DEFAULT_HEIGHT,
          REMOTE_CONTACT_AVATAR_DEFAULT_WIDTH);
    }

  init_contact_avatar_with_size (contact, avatar_widget, size);

  avatar_widget = self->priv->remote_user_avatar_toolbar;

  gtk_widget_get_allocation (avatar_widget, &allocation);
  size = allocation.height;

  if (size == 0)
    {
      /* the widget is not allocated yet, set a default size */
      size = SMALL_TOOLBAR_SIZE;
    }

  init_contact_avatar_with_size (contact, avatar_widget, size);
}

static void
empathy_call_window_setup_avatars (EmpathyCallWindow *self,
    EmpathyCallHandler *handler)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  tp_g_signal_connect_object (priv->contact, "notify::name",
      G_CALLBACK (contact_name_changed_cb), self, 0);
  tp_g_signal_connect_object (priv->contact, "notify::avatar",
    G_CALLBACK (contact_avatar_changed_cb), self, 0);
  tp_g_signal_connect_object (priv->contact, "notify::presence",
      G_CALLBACK (contact_presence_changed_cb), self, 0);

  set_window_title (self);
  set_remote_user_name (self, priv->contact);

  init_contact_avatar_with_size (priv->contact,
      priv->remote_user_avatar_widget,
      MIN (REMOTE_CONTACT_AVATAR_DEFAULT_WIDTH,
          REMOTE_CONTACT_AVATAR_DEFAULT_HEIGHT));

  init_contact_avatar_with_size (priv->contact,
      priv->remote_user_avatar_toolbar,
      SMALL_TOOLBAR_SIZE);

  /* The remote avatar is shown by default and will be hidden when we receive
     video from the remote side. */
  clutter_actor_hide (priv->video_output);
  gtk_widget_show (priv->remote_user_avatar_widget);
}

static void
update_send_codec (EmpathyCallWindow *self,
    gboolean audio)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  FsCodec *codec;
  GtkWidget *widget;
  gchar *tmp;

  if (audio)
    {
      codec = empathy_call_handler_get_send_audio_codec (priv->handler);
      widget = priv->acodec_encoding_label;
    }
  else
    {
      codec = empathy_call_handler_get_send_video_codec (priv->handler);
      widget = priv->vcodec_encoding_label;
    }

  if (codec == NULL)
    return;

  tmp = g_strdup_printf ("%s/%u", codec->encoding_name, codec->clock_rate);
  gtk_label_set_text (GTK_LABEL (widget), tmp);
  g_free (tmp);
}

static void
send_audio_codec_notify_cb (GObject *object,
    GParamSpec *pspec,
    gpointer user_data)
{
  EmpathyCallWindow *self = user_data;

  update_send_codec (self, TRUE);
}

static void
send_video_codec_notify_cb (GObject *object,
    GParamSpec *pspec,
    gpointer user_data)
{
  EmpathyCallWindow *self = user_data;

  update_send_codec (self, FALSE);
}

static void
update_recv_codec (EmpathyCallWindow *self,
    gboolean audio)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GList *codecs, *l;
  GtkWidget *widget;
  GString *str = NULL;

  if (audio)
    {
      codecs = empathy_call_handler_get_recv_audio_codecs (priv->handler);
      widget = priv->acodec_decoding_label;
    }
  else
    {
      codecs = empathy_call_handler_get_recv_video_codecs (priv->handler);
      widget = priv->vcodec_decoding_label;
    }

  if (codecs == NULL)
    return;

  for (l = codecs; l != NULL; l = g_list_next (l))
    {
      FsCodec *codec = l->data;

      if (str == NULL)
        str = g_string_new (NULL);
      else
        g_string_append (str, ", ");

      g_string_append_printf (str, "%s/%u", codec->encoding_name,
          codec->clock_rate);
    }

  gtk_label_set_text (GTK_LABEL (widget), str->str);
  g_string_free (str, TRUE);
}

static void
recv_audio_codecs_notify_cb (GObject *object,
    GParamSpec *pspec,
    gpointer user_data)
{
  EmpathyCallWindow *self = user_data;

  update_recv_codec (self, TRUE);
}

static void
recv_video_codecs_notify_cb (GObject *object,
    GParamSpec *pspec,
    gpointer user_data)
{
  EmpathyCallWindow *self = user_data;

  update_recv_codec (self, FALSE);
}

static const gchar *
candidate_type_to_str (FsCandidate *candidate)
{
  switch (candidate->type)
    {
      case FS_CANDIDATE_TYPE_HOST:
        return "host";
      case FS_CANDIDATE_TYPE_SRFLX:
        return "server reflexive";
      case FS_CANDIDATE_TYPE_PRFLX:
        return "peer reflexive";
      case FS_CANDIDATE_TYPE_RELAY:
        return "relay";
      case FS_CANDIDATE_TYPE_MULTICAST:
        return "multicast";
    }

  return NULL;
}

static const gchar *
candidate_type_to_desc (FsCandidate *candidate)
{
  switch (candidate->type)
    {
      case FS_CANDIDATE_TYPE_HOST:
        return _("The IP address as seen by the machine");
      case FS_CANDIDATE_TYPE_SRFLX:
        return _("The IP address as seen by a server on the Internet");
      case FS_CANDIDATE_TYPE_PRFLX:
        return _("The IP address of the peer as seen by the other side");
      case FS_CANDIDATE_TYPE_RELAY:
        return _("The IP address of a relay server");
      case FS_CANDIDATE_TYPE_MULTICAST:
        return _("The IP address of the multicast group");
    }

  return NULL;
}

static void
update_candidat_widget (EmpathyCallWindow *self,
    GtkWidget *label,
    GtkWidget *img,
    FsCandidate *candidate)
{
  gchar *str;

  g_assert (candidate != NULL);
  str = g_strdup_printf ("%s %u (%s)", candidate->ip,
      candidate->port, candidate_type_to_str (candidate));

  gtk_label_set_text (GTK_LABEL (label), str);
  gtk_widget_set_tooltip_text (img, candidate_type_to_desc (candidate));

  g_free (str);
}

static void
candidates_changed_cb (GObject *object,
    FsMediaType type,
    EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  FsCandidate *candidate = NULL;

  if (type == FS_MEDIA_TYPE_VIDEO)
    {
      /* Update remote candidate */
      candidate = empathy_call_handler_get_video_remote_candidate (
          priv->handler);

      update_candidat_widget (self, priv->video_remote_candidate_label,
          priv->video_remote_candidate_info_img, candidate);

      /* Update local candidate */
      candidate = empathy_call_handler_get_video_local_candidate (
          priv->handler);

      update_candidat_widget (self, priv->video_local_candidate_label,
          priv->video_local_candidate_info_img, candidate);
    }
  else
    {
      /* Update remote candidate */
      candidate = empathy_call_handler_get_audio_remote_candidate (
          priv->handler);

      update_candidat_widget (self, priv->audio_remote_candidate_label,
          priv->audio_remote_candidate_info_img, candidate);

      /* Update local candidate */
      candidate = empathy_call_handler_get_audio_local_candidate (
          priv->handler);

      update_candidat_widget (self, priv->audio_local_candidate_label,
          priv->audio_local_candidate_info_img, candidate);
    }
}

static void
empathy_call_window_constructed (GObject *object)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (object);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  TpCallChannel *call;
  TpCallState state;

  g_assert (priv->handler != NULL);

  g_object_get (priv->handler, "call-channel", &call, NULL);
  state = tp_call_channel_get_state (call, NULL, NULL, NULL);
  priv->outgoing = (state == TP_CALL_STATE_PENDING_INITIATOR);
  tp_clear_object (&call);

  priv->contact = empathy_call_handler_get_contact (priv->handler);
  g_assert (priv->contact != NULL);
  g_object_ref (priv->contact);

  if (!empathy_contact_can_voip_video (priv->contact))
    {
      gtk_widget_set_sensitive (priv->video_call_button, FALSE);
      gtk_widget_set_sensitive (priv->camera_button, FALSE);
    }

  empathy_call_window_setup_avatars (self, priv->handler);
  empathy_call_window_set_state_connecting (self);

  if (!empathy_call_handler_has_initial_video (priv->handler))
    {
      gtk_toggle_button_set_active (
          GTK_TOGGLE_BUTTON (priv->camera_button), FALSE);
    }
  /* If call has InitialVideo, the preview will be started once the call has
   * been started (start_call()). */

  update_send_codec (self, TRUE);
  update_send_codec (self, FALSE);
  update_recv_codec (self, TRUE);
  update_recv_codec (self, FALSE);

  tp_g_signal_connect_object (priv->handler, "notify::send-audio-codec",
      G_CALLBACK (send_audio_codec_notify_cb), self, 0);
  tp_g_signal_connect_object (priv->handler, "notify::send-video-codec",
      G_CALLBACK (send_video_codec_notify_cb), self, 0);
  tp_g_signal_connect_object (priv->handler, "notify::recv-audio-codecs",
      G_CALLBACK (recv_audio_codecs_notify_cb), self, 0);
  tp_g_signal_connect_object (priv->handler, "notify::recv-video-codecs",
      G_CALLBACK (recv_video_codecs_notify_cb), self, 0);

  tp_g_signal_connect_object (priv->handler, "candidates-changed",
      G_CALLBACK (candidates_changed_cb), self, 0);
}

static void empathy_call_window_dispose (GObject *object);
static void empathy_call_window_finalize (GObject *object);

static void
empathy_call_window_set_property (GObject *object,
  guint property_id, const GValue *value, GParamSpec *pspec)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (object);

  switch (property_id)
    {
      case PROP_CALL_HANDLER:
        priv->handler = g_value_dup_object (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
empathy_call_window_get_property (GObject *object,
  guint property_id, GValue *value, GParamSpec *pspec)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (object);

  switch (property_id)
    {
      case PROP_CALL_HANDLER:
        g_value_set_object (value, priv->handler);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
empathy_call_window_class_init (
  EmpathyCallWindowClass *empathy_call_window_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (empathy_call_window_class);
  GParamSpec *param_spec;

  g_type_class_add_private (empathy_call_window_class,
    sizeof (EmpathyCallWindowPriv));

  object_class->constructed = empathy_call_window_constructed;
  object_class->set_property = empathy_call_window_set_property;
  object_class->get_property = empathy_call_window_get_property;

  object_class->dispose = empathy_call_window_dispose;
  object_class->finalize = empathy_call_window_finalize;

  param_spec = g_param_spec_object ("handler",
    "handler", "The call handler",
    EMPATHY_TYPE_CALL_HANDLER,
    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class,
    PROP_CALL_HANDLER, param_spec);

  signals[SIG_INHIBIT] = g_signal_new ("inhibit",
      G_OBJECT_CLASS_TYPE (empathy_call_window_class),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      1, G_TYPE_BOOLEAN);
}

void
empathy_call_window_dispose (GObject *object)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (object);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  if (priv->handler != NULL)
    {
      empathy_call_handler_stop_call (priv->handler);
      tp_clear_object (&priv->handler);
    }

  if (priv->bus_message_source_id != 0)
    {
      g_source_remove (priv->bus_message_source_id);
      priv->bus_message_source_id = 0;
    }

  if (priv->got_video_src > 0)
    {
      g_source_remove (priv->got_video_src);
      priv->got_video_src = 0;
    }

  if (priv->inactivity_src > 0)
    {
      g_source_remove (priv->inactivity_src);
      priv->inactivity_src = 0;
    }

  tp_clear_object (&priv->pipeline);
  tp_clear_object (&priv->video_input);
  tp_clear_object (&priv->audio_input);
  tp_clear_object (&priv->video_tee);
  tp_clear_object (&priv->ui_manager);
  tp_clear_object (&priv->fullscreen);
  tp_clear_object (&priv->camera_monitor);
  tp_clear_object (&priv->settings);
  tp_clear_object (&priv->sound_mgr);
  tp_clear_object (&priv->mic_menu);
  tp_clear_object (&priv->camera_menu);
  tp_clear_object (&priv->transitions);

  g_list_free_full (priv->notifiers, g_object_unref);

  if (priv->timer_id != 0)
    g_source_remove (priv->timer_id);
  priv->timer_id = 0;

  tp_clear_object (&priv->contact);

  G_OBJECT_CLASS (empathy_call_window_parent_class)->dispose (object);
}

static void
disconnect_video_output_motion_handler (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  if (priv->video_output_motion_handler_id != 0)
    {
      g_signal_handler_disconnect (G_OBJECT (priv->video_container),
          priv->video_output_motion_handler_id);
      priv->video_output_motion_handler_id = 0;
    }
}

void
empathy_call_window_finalize (GObject *object)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (object);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  disconnect_video_output_motion_handler (self);

  /* free any data held directly by the object here */
  g_mutex_clear (&priv->lock);

  g_timer_destroy (priv->timer);

  G_OBJECT_CLASS (empathy_call_window_parent_class)->finalize (object);
}


EmpathyCallWindow *
empathy_call_window_new (EmpathyCallHandler *handler)
{
  return EMPATHY_CALL_WINDOW (
    g_object_new (EMPATHY_TYPE_CALL_WINDOW, "handler", handler, NULL));
}

void
empathy_call_window_new_handler (EmpathyCallWindow *self,
    EmpathyCallHandler *handler,
    gboolean present,
    guint32 x11_time)
{
  g_return_if_fail (EMPATHY_IS_CALL_HANDLER (handler));

  if (present)
    tpaw_window_present_with_time (GTK_WINDOW (self), x11_time);

  if (self->priv->call_state == DISCONNECTED)
    {
      /* start a new call if one is not already in progress */
      tp_clear_object (&self->priv->handler);
      self->priv->handler = g_object_ref (handler);
      empathy_call_window_connect_handler (self);

      empathy_call_window_restart_call (self);
    }
}

static void
empathy_call_window_conference_added_cb (EmpathyCallHandler *handler,
  GstElement *conference, gpointer user_data)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (user_data);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  FsElementAddedNotifier *notifier;
  GKeyFile *keyfile;

  DEBUG ("Conference added");

  /* Add notifier to set the various element properties as needed */
  notifier = fs_element_added_notifier_new ();
  keyfile = fs_utils_get_default_element_properties (conference);

  if (keyfile != NULL)
    fs_element_added_notifier_set_properties_from_keyfile (notifier, keyfile);

  fs_element_added_notifier_add (notifier, GST_BIN (priv->pipeline));

  priv->notifiers = g_list_prepend (priv->notifiers, notifier);

  gst_bin_add (GST_BIN (priv->pipeline), conference);
  gst_element_set_state (conference, GST_STATE_PLAYING);
}

static void
empathy_call_window_add_notifier_remove (gpointer data, gpointer user_data)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (user_data);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  FsElementAddedNotifier *notifier = data;

  fs_element_added_notifier_remove (notifier, GST_BIN (priv->pipeline));
}

static void
empathy_call_window_conference_removed_cb (EmpathyCallHandler *handler,
  GstElement *conference, gpointer user_data)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (user_data);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  gst_bin_remove (GST_BIN (priv->pipeline), conference);
  gst_element_set_state (conference, GST_STATE_NULL);

  g_list_foreach (priv->notifiers,
    empathy_call_window_add_notifier_remove, user_data);

  g_list_free_full (priv->notifiers, g_object_unref);
  priv->notifiers = NULL;
}

static gboolean
empathy_call_window_reset_pipeline (EmpathyCallWindow *self)
{
  GstStateChangeReturn state_change_return;
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  if (priv->pipeline == NULL)
    return TRUE;

  if (priv->bus_message_source_id != 0)
    {
      g_source_remove (priv->bus_message_source_id);
      priv->bus_message_source_id = 0;
    }

  state_change_return = gst_element_set_state (priv->pipeline, GST_STATE_NULL);

  if (state_change_return == GST_STATE_CHANGE_SUCCESS ||
        state_change_return == GST_STATE_CHANGE_NO_PREROLL)
    {
      if (priv->pipeline != NULL)
        g_object_unref (priv->pipeline);
      priv->pipeline = NULL;

      if (priv->audio_output != NULL)
        g_object_unref (priv->audio_output);
      priv->audio_output = NULL;
      priv->audio_output_added = FALSE;

      if (priv->video_tee != NULL)
        g_object_unref (priv->video_tee);
      priv->video_tee = NULL;

      if (priv->video_preview != NULL)
        clutter_actor_destroy (priv->video_preview);
      priv->video_preview = NULL;

      /* If we destroy the preview while it's being dragged, we won't
       * get the ::drag-end signal, so manually destroy the clone */
      if (priv->drag_preview != NULL)
        {
          clutter_actor_destroy (priv->drag_preview);
          empathy_call_window_show_preview_rectangles (self, FALSE);
          priv->drag_preview = NULL;
        }

      priv->funnel = NULL;

      create_pipeline (self);
      /* Call will be started when user will hit the 'redial' button */
      priv->start_call_when_playing = FALSE;
      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);

      return TRUE;
    }
  else
    {
      g_message ("Error: could not destroy pipeline. Closing call window");
      gtk_widget_destroy (GTK_WIDGET (self));

      return FALSE;
    }
}

static void
reset_details_pane (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  gtk_label_set_text (GTK_LABEL (priv->vcodec_encoding_label), _("Unknown"));
  gtk_label_set_text (GTK_LABEL (priv->acodec_encoding_label), _("Unknown"));
  gtk_label_set_text (GTK_LABEL (priv->vcodec_decoding_label), _("Unknown"));
  gtk_label_set_text (GTK_LABEL (priv->acodec_decoding_label), _("Unknown"));
}

static gboolean
empathy_call_window_disconnected (EmpathyCallWindow *self,
    gboolean restart)
{
  gboolean could_disconnect = FALSE;
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  gboolean could_reset_pipeline;

  /* Leave full screen mode if needed */
  gtk_window_unfullscreen (GTK_WINDOW (self));

  g_signal_emit (self, signals[SIG_INHIBIT], 0, FALSE);

  gtk_action_set_sensitive (priv->menu_fullscreen, FALSE);
  gtk_widget_set_sensitive (priv->dtmf_panel, FALSE);

  /* Create the video input and then turn on the camera
   * menu, so that the active camera gets marked as such.
   */
  if (priv->video_input == NULL)
    create_video_input (self);
  empathy_camera_menu_set_sensitive (priv->camera_menu, TRUE);

  could_reset_pipeline = empathy_call_window_reset_pipeline (self);

  if (priv->call_state == CONNECTING)
      empathy_sound_manager_stop (priv->sound_mgr, EMPATHY_SOUND_PHONE_OUTGOING);

  if (priv->call_state != REDIALING)
    priv->call_state = DISCONNECTED;

  /* Show the toolbar */
  clutter_state_set_state (priv->transitions, "fade-in");

  if (could_reset_pipeline)
    {
      g_mutex_lock (&priv->lock);

      g_timer_stop (priv->timer);

      if (priv->timer_id != 0)
        g_source_remove (priv->timer_id);
      priv->timer_id = 0;

      g_mutex_unlock (&priv->lock);

      if (!restart)
        /* We are about to destroy the window, no need to update it or create
         * a video preview */
        return TRUE;

      empathy_call_window_status_message (self, _("Disconnected"));

      empathy_call_window_show_hangup_button (self, FALSE);

      /* Unsensitive the camera and mic button */
      gtk_widget_set_sensitive (priv->camera_button, FALSE);
      gtk_widget_set_sensitive (priv->mic_button, FALSE);

      /* Be sure that the mic button is enabled */
      empathy_audio_src_set_mute (
          EMPATHY_GST_AUDIO_SRC (self->priv->audio_input), FALSE);

      if (priv->camera_state == CAMERA_STATE_ON)
        {
          /* Restart the preview with the new pipeline. */
          display_video_preview (self, TRUE);
        }

      /* destroy the video output; it will be recreated when we'll redial */
      disconnect_video_output_motion_handler (self);
      if (priv->video_output != NULL)
        clutter_actor_destroy (priv->video_output);
      priv->video_output = NULL;
      if (priv->got_video_src > 0)
        {
          g_source_remove (priv->got_video_src);
          priv->got_video_src = 0;
        }

      gtk_widget_show (priv->remote_user_avatar_widget);

      reset_details_pane (self);

      priv->sending_video = FALSE;
      priv->call_started = FALSE;

      could_disconnect = TRUE;

      /* TODO: display the self avatar of the preview (depends if the "Always
       * Show Video Preview" is enabled or not) */
    }

  return could_disconnect;
}


static void
empathy_call_window_channel_closed_cb (EmpathyCallHandler *handler,
    gpointer user_data)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (user_data);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  if (empathy_call_window_disconnected (self, TRUE) &&
      priv->call_state == REDIALING)
      empathy_call_window_restart_call (self);
}

static gboolean
empathy_call_window_content_is_raw (TfContent *content)
{
  FsConference *conference;
  gboolean israw;

  g_object_get (content, "fs-conference", &conference, NULL);
  g_assert (conference != NULL);

  /* FIXME: Ugly hack, update when moving a packetization property into
   * farstream */
  israw = g_str_has_prefix (GST_OBJECT_NAME (conference), "fsrawconf");
  gst_object_unref (conference);

  return israw;
}

static gboolean
empathy_call_window_content_removed_cb (EmpathyCallHandler *handler,
    TfContent *content,
    EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  FsMediaType media_type;

  DEBUG ("removing content");

  g_object_get (content, "media-type", &media_type, NULL);

  /*
   * This assumes that there is only one video stream per channel...
   */

  if ((guint) media_type == FS_MEDIA_TYPE_VIDEO)
    {
      if (priv->funnel != NULL)
        {
          GstElement *output;

          output = priv->video_output_sink;

          gst_element_set_state (output, GST_STATE_NULL);
          gst_element_set_state (priv->funnel, GST_STATE_NULL);

          gst_bin_remove (GST_BIN (priv->pipeline), output);
          gst_bin_remove (GST_BIN (priv->pipeline), priv->funnel);
          priv->funnel = NULL;
        }
    }
  else if (media_type == FS_MEDIA_TYPE_AUDIO)
    {
      if (priv->audio_output != NULL)
        {
          gst_element_set_state (priv->audio_output, GST_STATE_NULL);

          if (priv->audio_output_added)
            gst_bin_remove (GST_BIN (priv->pipeline), priv->audio_output);
          priv->audio_output = NULL;
          priv->audio_output_added = FALSE;
        }
    }
  else
    {
      g_assert_not_reached ();
    }

  return TRUE;
}

static void
empathy_call_window_framerate_changed_cb (EmpathyCallHandler *handler,
    guint framerate,
    EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  DEBUG ("Framerate changed to %u", framerate);

  if (priv->video_input != NULL)
    empathy_video_src_set_framerate (priv->video_input, framerate);
}

static void
empathy_call_window_resolution_changed_cb (EmpathyCallHandler *handler,
    guint width,
    guint height,
    EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  DEBUG ("Resolution changed to %ux%u", width, height);

  if (priv->video_input != NULL)
    {
      empathy_video_src_set_resolution (priv->video_input, width, height);
    }
}

/* Called with global lock held */
static GstPad *
empathy_call_window_get_video_sink_pad (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GstPad *pad;
  GstElement *output;

  if (priv->funnel == NULL)
    {
      output = priv->video_output_sink;
      priv->funnel = gst_element_factory_make ("funnel", NULL);

      if (!priv->funnel)
        {
          g_warning ("Could not create video funnel");
          return NULL;
        }

      if (!gst_bin_add (GST_BIN (priv->pipeline), priv->funnel))
        {
          gst_object_unref (priv->funnel);
          priv->funnel = NULL;
          g_warning ("Could  not add funnel to pipeline");
          return NULL;
        }

      if (!gst_bin_add (GST_BIN (priv->pipeline), output))
        {
          g_warning ("Could not add the video output widget to the pipeline");
          goto error;
        }

      if (!gst_element_link (priv->funnel, output))
        {
          g_warning ("Could not link output sink to funnel");
          goto error_output_added;
        }

      if (gst_element_set_state (output, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        {
          g_warning ("Could not start video sink");
          goto error_output_added;
        }

      if (gst_element_set_state (priv->funnel, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        {
          g_warning ("Could not start funnel");
          goto error_output_added;
        }
    }
  pad = gst_element_get_request_pad (priv->funnel, "sink_%u");

  if (!pad)
    g_warning ("Could not get request pad from funnel");

  return pad;


 error_output_added:

  gst_element_set_locked_state (priv->funnel, TRUE);
  gst_element_set_locked_state (output, TRUE);

  gst_element_set_state (priv->funnel, GST_STATE_NULL);
  gst_element_set_state (output, GST_STATE_NULL);

  gst_bin_remove (GST_BIN (priv->pipeline), output);
  gst_element_set_locked_state (output, FALSE);

 error:

  gst_bin_remove (GST_BIN (priv->pipeline), priv->funnel);
  priv->funnel = NULL;

  return NULL;
}

/* Called with global lock held */
static GstPad *
empathy_call_window_get_audio_sink_pad (EmpathyCallWindow *self,
  TfContent *content)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GstPad *pad;
  GstPadTemplate *template;

  if (!priv->audio_output_added)
    {
      if (!gst_bin_add (GST_BIN (priv->pipeline), priv->audio_output))
        {
          g_warning ("Could not add audio sink to pipeline");
          g_object_unref (priv->audio_output);
          goto error_add_output;
        }

      if (gst_element_set_state (priv->audio_output, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        {
          g_warning ("Could not start audio sink");
          goto error;
        }
      priv->audio_output_added = TRUE;
    }

  template = gst_element_class_get_pad_template (
    GST_ELEMENT_GET_CLASS (priv->audio_output), "sink%d");

  pad = gst_element_request_pad (priv->audio_output,
    template, NULL, NULL);

  if (pad == NULL)
    {
      g_warning ("Could not get sink pad from sink");
      return NULL;
    }

  return pad;

error:
  gst_element_set_locked_state (priv->audio_output, TRUE);
  gst_element_set_state (priv->audio_output, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (priv->pipeline), priv->audio_output);
  priv->audio_output = NULL;

error_add_output:

  return NULL;
}

static gboolean
empathy_call_window_update_timer (gpointer user_data)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (user_data);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  const gchar *status;
  gchar *str;
  gdouble time_;

  time_ = g_timer_elapsed (priv->timer, NULL);

  if (priv->call_state == HELD)
    status = _("On hold");
  else if (priv->call_state == DISCONNECTED)
    status = _("Disconnected");
  else if (priv->muted)
    status = _("Mute");
  else
    status = _("Duration");

  /* Translators: 'status - minutes:seconds' the caller has been connected */
  str = g_strdup_printf (_("%s — %d:%02dm"),
      status,
      (int) time_ / 60, (int) time_ % 60);
  empathy_call_window_status_message (self, str);
  g_free (str);

  return TRUE;
}

enum
{
  EMP_RESPONSE_BALANCE
};

static void
on_error_infobar_response_cb (GtkInfoBar *info_bar,
    gint response_id,
    gpointer user_data)
{
  switch (response_id)
    {
      case GTK_RESPONSE_CLOSE:
        gtk_widget_destroy (GTK_WIDGET (info_bar));
        break;
      case EMP_RESPONSE_BALANCE:
        empathy_url_show (GTK_WIDGET (info_bar),
            g_object_get_data (G_OBJECT (info_bar), "uri"));
        break;
    }
}

static void
display_error (EmpathyCallWindow *self,
    const gchar *img,
    const gchar *title,
    const gchar *desc,
    const gchar *details,
    const gchar *button_text,
    const gchar *uri,
    gint button_response)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GtkWidget *info_bar;
  GtkWidget *content_area;
  GtkWidget *hbox;
  GtkWidget *vbox;
  GtkWidget *image;
  GtkWidget *label;
  gchar *txt;

  /* Create info bar */
  info_bar = gtk_info_bar_new ();

  if (button_text != NULL)
    {
      gtk_info_bar_add_button (GTK_INFO_BAR (info_bar),
          button_text, button_response);
      g_object_set_data_full (G_OBJECT (info_bar),
          "uri", g_strdup (uri), g_free);
    }

  gtk_info_bar_add_button (GTK_INFO_BAR (info_bar),
      GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE);

  gtk_info_bar_set_message_type (GTK_INFO_BAR (info_bar), GTK_MESSAGE_WARNING);

  content_area = gtk_info_bar_get_content_area (GTK_INFO_BAR (info_bar));

  /* hbox containing the image and the messages vbox */
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_container_add (GTK_CONTAINER (content_area), hbox);

  /* Add image */
  image = gtk_image_new_from_icon_name (img, GTK_ICON_SIZE_DIALOG);
  gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);

  /* vbox containing the main message and the details expander */
  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
  gtk_box_pack_start (GTK_BOX (hbox), vbox, TRUE, TRUE, 0);

  /* Add text */
  txt = g_strdup_printf ("<b>%s</b>\n%s", title, desc);

  label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (label), txt);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_misc_set_alignment (GTK_MISC (label), 0, 0);
  g_free (txt);

  gtk_box_pack_start (GTK_BOX (vbox), label, TRUE, TRUE, 0);

  /* Add details */
  if (details != NULL)
    {
      GtkWidget *expander;

      expander = gtk_expander_new (_("Technical Details"));

      txt = g_strdup_printf ("<i>%s</i>", details);

      label = gtk_label_new (NULL);
      gtk_label_set_markup (GTK_LABEL (label), txt);
      gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
      gtk_misc_set_alignment (GTK_MISC (label), 0, 0);
      g_free (txt);

      gtk_container_add (GTK_CONTAINER (expander), label);
      gtk_box_pack_start (GTK_BOX (vbox), expander, TRUE, TRUE, 0);
    }

  g_signal_connect (info_bar, "response",
      G_CALLBACK (on_error_infobar_response_cb), NULL);

  gtk_box_pack_start (GTK_BOX (priv->errors_vbox), info_bar,
      FALSE, FALSE, CONTENT_HBOX_CHILDREN_PACKING_PADDING);
  gtk_widget_show_all (info_bar);
}

#if 0
static gchar *
media_stream_error_to_txt (EmpathyCallWindow *self,
    TpCallChannel *call,
    gboolean audio,
    TpMediaStreamError error)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  const gchar *cm = NULL;
  gchar *url;
  gchar *result;

  switch (error)
    {
      case TP_MEDIA_STREAM_ERROR_CODEC_NEGOTIATION_FAILED:
        if (audio)
          return g_strdup_printf (
              _("%s's software does not understand any of the audio formats "
                "supported by your computer"),
            empathy_contact_get_alias (priv->contact));
        else
          return g_strdup_printf (
              _("%s's software does not understand any of the video formats "
                "supported by your computer"),
            empathy_contact_get_alias (priv->contact));

      case TP_MEDIA_STREAM_ERROR_CONNECTION_FAILED:
        return g_strdup_printf (
            _("Can't establish a connection to %s. "
              "One of you might be on a network that does not allow "
              "direct connections."),
          empathy_contact_get_alias (priv->contact));

      case TP_MEDIA_STREAM_ERROR_NETWORK_ERROR:
          return g_strdup (_("There was a failure on the network"));

      case TP_MEDIA_STREAM_ERROR_NO_CODECS:
        if (audio)
          return g_strdup (_("The audio formats necessary for this call "
                "are not installed on your computer"));
        else
          return g_strdup (_("The video formats necessary for this call "
                "are not installed on your computer"));

      case TP_MEDIA_STREAM_ERROR_INVALID_CM_BEHAVIOR:
        tp_connection_parse_object_path (
            tp_channel_get_connection (TP_CHANNEL (call)),
            NULL, &cm);

        url = g_strdup_printf ("http://bugs.freedesktop.org/enter_bug.cgi?"
            "product=Telepathy&amp;component=%s", cm);

        result = g_strdup_printf (
            _("Something unexpected happened in a Telepathy component. "
              "Please <a href=\"%s\">report this bug</a> and attach "
              "logs gathered from the 'Debug' window in the Help menu."), url);

        g_free (url);
        g_free (cm);
        return result;

      case TP_MEDIA_STREAM_ERROR_MEDIA_ERROR:
        return g_strdup (_("There was a failure in the call engine"));

      case TP_MEDIA_STREAM_ERROR_EOS:
        return g_strdup (_("The end of the stream was reached"));

      case TP_MEDIA_STREAM_ERROR_UNKNOWN:
      default:
        return NULL;
    }
}

static void
empathy_call_window_stream_error (EmpathyCallWindow *self,
    TpCallChannel *call,
    gboolean audio,
    guint code,
    const gchar *msg,
    const gchar *icon,
    const gchar *title)
{
  gchar *desc;

  desc = media_stream_error_to_txt (self, call, audio, code);
  if (desc == NULL)
    {
      /* No description, use the error message. That's not great as it's not
       * localized but it's better than nothing. */
      display_error (self, call, icon, title, msg, NULL);
    }
  else
    {
      display_error (self, call, icon, title, desc, msg);
      g_free (desc);
    }
}

static void
empathy_call_window_audio_stream_error (TpCallChannel *call,
    guint code,
    const gchar *msg,
    EmpathyCallWindow *self)
{
  empathy_call_window_stream_error (self, call, TRUE, code, msg,
      "gnome-stock-mic", _("Can't establish audio stream"));
}

static void
empathy_call_window_video_stream_error (TpCallChannel *call,
    guint code,
    const gchar *msg,
    EmpathyCallWindow *self)
{
  empathy_call_window_stream_error (self, call, FALSE, code, msg,
      "camera-web", _("Can't establish video stream"));
}
#endif

static void
show_balance_error (EmpathyCallWindow *self)
{
  TpChannel *call;
  TpConnection *conn;
  gchar *balance, *tmp;
  const gchar *uri, *currency;
  gint amount;
  guint scale;

  g_object_get (self->priv->handler,
      "call-channel", &call,
      NULL);

  conn = tp_channel_get_connection (call);
  g_object_unref (call);

  uri = tp_connection_get_balance_uri (conn);

  if (!tp_connection_get_balance (conn, &amount, &scale, &currency))
    {
      /* unknown balance */
      balance = g_strdup ("(--)");
    }
  else
    {
      char *money = empathy_format_currency (amount, scale, currency);

      balance = g_strdup_printf ("%s %s",
          currency, money);
      g_free (money);
    }

  tmp = g_strdup_printf (_("Your current balance is %s."), balance),

  display_error (self,
      NULL,
      _("Sorry, you don’t have enough credit for that call."),
      tmp, NULL,
      _("Top Up"),
      uri,
      EMP_RESPONSE_BALANCE);

  g_free (tmp);
  g_free (balance);
}

static void
empathy_call_window_state_changed_cb (EmpathyCallHandler *handler,
    TpCallState state,
    gchar *reason,
    EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  TpCallChannel *call;
  gboolean can_send_video;

  if (state == TP_CALL_STATE_ENDED)
    {
      DEBUG ("Call ended: %s", (reason != NULL && reason[0] != '\0') ? reason : "unspecified reason");
      empathy_call_window_disconnected (self, TRUE);
      if (!tp_strdiff (reason, TP_ERROR_STR_INSUFFICIENT_BALANCE))
          show_balance_error (self);
      return;
    }

  if (state != TP_CALL_STATE_ACCEPTED)
    return;

  if (priv->call_state == CONNECTED)
    return;

  g_timer_start (priv->timer);
  priv->call_state = CONNECTED;

  empathy_sound_manager_stop (priv->sound_mgr, EMPATHY_SOUND_PHONE_OUTGOING);

  can_send_video = priv->video_input != NULL &&
    empathy_contact_can_voip_video (priv->contact) &&
    tpaw_camera_monitor_get_available (priv->camera_monitor);

  g_object_get (priv->handler, "call-channel", &call, NULL);

  if (tp_call_channel_has_dtmf (call))
    gtk_widget_set_sensitive (priv->dtmf_panel, TRUE);

  if (priv->video_input == NULL)
    empathy_call_window_set_send_video (self, CAMERA_STATE_OFF);

  gtk_widget_set_sensitive (priv->camera_button, can_send_video);

  empathy_call_window_show_hangup_button (self, TRUE);

  gtk_widget_set_sensitive (priv->mic_button, TRUE);

  clutter_actor_hide (priv->video_output);
  gtk_widget_show (priv->remote_user_avatar_widget);

  g_object_unref (call);

  g_mutex_lock (&priv->lock);

  priv->timer_id = g_timeout_add_seconds (1,
    empathy_call_window_update_timer, self);

  g_mutex_unlock (&priv->lock);

  empathy_call_window_update_timer (self);

  gtk_action_set_sensitive (priv->menu_fullscreen, TRUE);
}

static gboolean
empathy_call_window_show_video_output_cb (gpointer user_data)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (user_data);

  if (self->priv->video_output != NULL)
    {
      gtk_widget_hide (self->priv->remote_user_avatar_widget);
      clutter_actor_show (self->priv->video_output);
      clutter_actor_raise_top (self->priv->overlay_bin);
    }

  return FALSE;
}

static gboolean
empathy_call_window_check_video_cb (gpointer data)
{
  EmpathyCallWindow *self = data;

  if (self->priv->got_video)
    {
      self->priv->got_video = FALSE;
      return TRUE;
    }

  /* No video in the last N seconds, display the remote avatar */
  empathy_call_window_show_video_output (self, FALSE);

  return TRUE;
}

/* Called from the streaming thread */
static GstPadProbeReturn
empathy_call_window_video_probe_cb (GstPad *pad,
    GstPadProbeInfo *info,
    gpointer user_data)
{
    EmpathyCallWindow *self = user_data;

  if (G_UNLIKELY (!self->priv->got_video))
    {
      /* show the remote video */
      g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
          empathy_call_window_show_video_output_cb,
          g_object_ref (self), g_object_unref);

      self->priv->got_video = TRUE;
    }

  return GST_PAD_PROBE_OK;
}

/* Called from the streaming thread */
static gboolean
empathy_call_window_src_added_cb (EmpathyCallHandler *handler,
  TfContent *content, GstPad *src, gpointer user_data)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (user_data);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  gboolean retval = FALSE;
  guint media_type;

  GstPad *pad;

  g_mutex_lock (&priv->lock);

  g_object_get (content, "media-type", &media_type, NULL);

  switch (media_type)
    {
      case TP_MEDIA_STREAM_TYPE_AUDIO:
        pad = empathy_call_window_get_audio_sink_pad (self, content);
        break;
      case TP_MEDIA_STREAM_TYPE_VIDEO:
        g_idle_add (empathy_call_window_show_video_output_cb, self);
        pad = empathy_call_window_get_video_sink_pad (self);

        gst_pad_add_probe (src,
            GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST,
            empathy_call_window_video_probe_cb, self, NULL);

        if (priv->got_video_src > 0)
          g_source_remove (priv->got_video_src);
        priv->got_video_src = g_timeout_add_seconds (1,
            empathy_call_window_check_video_cb, self);
        break;
      default:
        g_assert_not_reached ();
    }

  if (pad == NULL)
    goto out;

  if (GST_PAD_LINK_FAILED (gst_pad_link (src, pad)))
      g_warning ("Could not link %s sink pad",
          media_type == TP_MEDIA_STREAM_TYPE_AUDIO ? "audio" : "video");
  else
      retval = TRUE;

  gst_object_unref (pad);

 out:

  /* If no sink could be linked, try to add fakesink to prevent the whole call
   * aborting */

  if (!retval)
    {
      GstElement *fakesink = gst_element_factory_make ("fakesink", NULL);

      if (gst_bin_add (GST_BIN (priv->pipeline), fakesink))
        {
          GstPad *sinkpad = gst_element_get_static_pad (fakesink, "sink");
          if (gst_element_set_state (fakesink, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE ||
              GST_PAD_LINK_FAILED (gst_pad_link (src, sinkpad)))
            {
              gst_element_set_locked_state (fakesink, TRUE);
              gst_element_set_state (fakesink, GST_STATE_NULL);
              gst_bin_remove (GST_BIN (priv->pipeline), fakesink);
            }
          else
            {
              DEBUG ("Could not link real sink, linked fakesink instead");
            }
          gst_object_unref (sinkpad);
        }
      else
        {
          gst_object_unref (fakesink);
        }
    }


  g_mutex_unlock (&priv->lock);

  return TRUE;
}

static void
empathy_call_window_prepare_audio_output (EmpathyCallWindow *self,
  TfContent *content)
{
  EmpathyCallWindowPriv *priv = self->priv;

  g_assert (priv->audio_output_added == FALSE);
  g_assert (priv->audio_output == FALSE);

  priv->audio_output = empathy_audio_sink_new ();
  g_object_ref_sink (priv->audio_output);

  /* volume button to output volume linking */
  g_object_bind_property (priv->audio_output, "volume",
    priv->volume_button, "value",
    G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  g_object_bind_property_full (content, "requested-output-volume",
    priv->audio_output, "volume",
    G_BINDING_DEFAULT,
    audio_control_volume_to_element,
    element_volume_to_audio_control,
    NULL, NULL);

  /* Link volumes together, sync the current audio input volume property
    * back to farstream first */
  g_object_bind_property_full (priv->audio_output, "volume",
    content, "reported-output-volume",
    G_BINDING_SYNC_CREATE,
    element_volume_to_audio_control,
    audio_control_volume_to_element,
    NULL, NULL);

  /* For raw audio conferences assume that the producer of the raw data
   * has already processed it, so turn off any echo cancellation and any
   * other audio improvements that come with it */
  empathy_audio_sink_set_echo_cancel (
    EMPATHY_GST_AUDIO_SINK (priv->audio_output),
    !empathy_call_window_content_is_raw (content));
}


static gboolean
empathy_call_window_content_added_cb (EmpathyCallHandler *handler,
  TfContent *content, gpointer user_data)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (user_data);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GstPad *sink, *pad;
  FsMediaType media_type;
  gboolean retval = FALSE;

  g_object_get (content, "media-type", &media_type, "sink-pad", &sink, NULL);
  g_assert (sink != NULL);

  switch (media_type)
    {
      case FS_MEDIA_TYPE_AUDIO:

        /* For raw audio conferences assume that the receiver of the raw data
         * wants it unprocessed, so turn off any echo cancellation and any
         * other audio improvements that come with it */
        empathy_audio_src_set_echo_cancel (
          EMPATHY_GST_AUDIO_SRC (priv->audio_input),
          !empathy_call_window_content_is_raw (content));

        /* Link volumes together, sync the current audio input volume property
         * back to farstream first */
        g_object_bind_property_full (content, "requested-input-volume",
          priv->audio_input, "volume",
          G_BINDING_DEFAULT,
          audio_control_volume_to_element,
          element_volume_to_audio_control,
          NULL, NULL);

        g_object_bind_property_full (priv->audio_input, "volume",
          content, "reported-input-volume",
          G_BINDING_SYNC_CREATE,
          element_volume_to_audio_control,
          audio_control_volume_to_element,
          NULL, NULL);

        if (!gst_bin_add (GST_BIN (priv->pipeline), priv->audio_input))
          {
            g_warning ("Could not add audio source to pipeline");
            break;
          }

        pad = gst_element_get_static_pad (priv->audio_input, "src");
        if (!pad)
          {
            gst_bin_remove (GST_BIN (priv->pipeline), priv->audio_input);
            g_warning ("Could not get source pad from audio source");
            break;
          }

        if (GST_PAD_LINK_FAILED (gst_pad_link (pad, sink)))
          {
            gst_bin_remove (GST_BIN (priv->pipeline), priv->audio_input);
            gst_object_unref (pad);
            g_warning ("Could not link audio source to farsight");
            break;
          }
        gst_object_unref (pad);

        if (gst_element_set_state (priv->audio_input, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
          {
            g_warning ("Could not start audio source");
            gst_element_set_state (priv->audio_input, GST_STATE_NULL);
            gst_bin_remove (GST_BIN (priv->pipeline), priv->audio_input);
            break;
          }

        /* Prepare our audio output, not added yet though */
        empathy_call_window_prepare_audio_output (self, content);

        retval = TRUE;
        break;
      case FS_MEDIA_TYPE_VIDEO:
        if (priv->video_tee != NULL)
          {
            pad = gst_element_get_request_pad (priv->video_tee, "src_%u");
            if (GST_PAD_LINK_FAILED (gst_pad_link (pad, sink)))
              {
                g_warning ("Could not link video source input pipeline");
                break;
              }
            gst_object_unref (pad);
          }

        retval = TRUE;
        break;
      default:
        g_assert_not_reached ();
    }

  gst_object_unref (sink);
  return retval;
}

static void
empathy_call_window_remove_video_input (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GstElement *preview;

  disable_camera (self);

  DEBUG ("remove video input");
  preview = priv->video_preview_sink;

  gst_element_set_state (priv->video_input, GST_STATE_NULL);
  gst_element_set_state (priv->video_tee, GST_STATE_NULL);
  gst_element_set_state (preview, GST_STATE_NULL);

  gst_bin_remove_many (GST_BIN (priv->pipeline), priv->video_input,
    preview, NULL);

  g_object_unref (priv->video_input);
  priv->video_input = NULL;
  g_object_unref (priv->video_tee);
  priv->video_tee = NULL;
  clutter_actor_destroy (priv->video_preview);
  priv->video_preview = NULL;

  gtk_widget_set_sensitive (priv->camera_button, FALSE);
  empathy_camera_menu_set_sensitive (priv->camera_menu, FALSE);
}

static void
start_call (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);

  g_signal_emit (self, signals[SIG_INHIBIT], 0, TRUE);

  priv->call_started = TRUE;
  empathy_call_handler_start_call (priv->handler,
      gtk_get_current_event_time ());

  if (empathy_call_handler_has_initial_video (priv->handler))
    {
      TpCallChannel *call;
      TpSendingState s;

      g_object_get (priv->handler, "call-channel", &call, NULL);
      /* If the call channel isn't set yet we're requesting it, if we're
       * requesting it with initial video it should be PENDING_SEND when we get
       * it */
      if (call == NULL)
        s = TP_SENDING_STATE_PENDING_SEND;
      else
        s = empathy_call_channel_get_video_state (call);

      if (s == TP_SENDING_STATE_PENDING_SEND ||
          s == TP_SENDING_STATE_SENDING)
        {
          /* Enable 'send video' buttons and display the preview */
          gtk_toggle_button_set_active (
            GTK_TOGGLE_BUTTON (priv->camera_button), TRUE);
        }
      else
        {
          gtk_toggle_button_set_active (
            GTK_TOGGLE_BUTTON (priv->camera_button), FALSE);

          if (priv->video_preview == NULL)
            {
              create_video_preview (self);
              add_video_preview_to_pipeline (self);
            }
        }

      if (call != NULL)
        g_object_unref (call);
    }
}

static gboolean
empathy_call_window_bus_message (GstBus *bus, GstMessage *message,
  gpointer user_data)
{
  EmpathyCallWindow *self = EMPATHY_CALL_WINDOW (user_data);
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GstState newstate, pending;

  empathy_call_handler_bus_message (priv->handler, bus, message);

  switch (GST_MESSAGE_TYPE (message))
    {
      case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC (message) == GST_OBJECT (priv->video_input))
          {
            gst_message_parse_state_changed (message, NULL, &newstate, NULL);
          }
        if (GST_MESSAGE_SRC (message) == GST_OBJECT (priv->pipeline) &&
            !priv->call_started)
          {
            gst_message_parse_state_changed (message, NULL, &newstate, NULL);
            if (newstate == GST_STATE_PAUSED)
              {
                gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
                priv->pipeline_playing = TRUE;

                if (priv->start_call_when_playing)
                  start_call (self);
              }
          }
        if (priv->video_preview_sink != NULL &&
            GST_MESSAGE_SRC (message) == GST_OBJECT (priv->video_preview_sink))
          {
            gst_message_parse_state_changed (message, NULL, &newstate,
                &pending);

            if (newstate == GST_STATE_PLAYING &&
                pending == GST_STATE_VOID_PENDING)
              empathy_call_window_stop_camera_spinning (self);
          }
        break;
      case GST_MESSAGE_ERROR:
        {
          GError *error = NULL;
          GstElement *gst_error;
          gchar *debug;
          gchar *name;

          gst_message_parse_error (message, &error, &debug);
          gst_error = GST_ELEMENT (GST_MESSAGE_SRC (message));

          g_message ("Element error: %s -- %s\n", error->message, debug);

          name = gst_element_get_name (gst_error);
          if (g_str_has_prefix (name, VIDEO_INPUT_ERROR_PREFIX))
            {
              /* Remove the video input and continue */
              if (priv->video_input != NULL)
                empathy_call_window_remove_video_input (self);
              gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
            }
          else
            {
              empathy_call_window_disconnected (self, TRUE);
            }
          g_free (name);
          g_error_free (error);
          g_free (debug);
        }
      case GST_MESSAGE_UNKNOWN:
      case GST_MESSAGE_EOS:
      case GST_MESSAGE_WARNING:
      case GST_MESSAGE_INFO:
      case GST_MESSAGE_TAG:
      case GST_MESSAGE_BUFFERING:
      case GST_MESSAGE_STATE_DIRTY:
      case GST_MESSAGE_STEP_DONE:
      case GST_MESSAGE_CLOCK_PROVIDE:
      case GST_MESSAGE_CLOCK_LOST:
      case GST_MESSAGE_NEW_CLOCK:
      case GST_MESSAGE_STRUCTURE_CHANGE:
      case GST_MESSAGE_STREAM_STATUS:
      case GST_MESSAGE_APPLICATION:
      case GST_MESSAGE_ELEMENT:
      case GST_MESSAGE_SEGMENT_START:
      case GST_MESSAGE_SEGMENT_DONE:
      case GST_MESSAGE_DURATION:
      case GST_MESSAGE_ANY:
      default:
        break;
    }

  return TRUE;
}

static void
empathy_call_window_members_changed_cb (TpCallChannel *call,
    GHashTable *updates,
    GPtrArray *removed,
    TpCallStateReason *reason,
    EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  GHashTableIter iter;
  gpointer key, value;
  gboolean held = FALSE;

  g_hash_table_iter_init (&iter, updates);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (GPOINTER_TO_INT (value) & TP_CALL_MEMBER_FLAG_HELD)
        {
          /* This assumes this is a 1-1 call, otherwise one participant
           * putting the call on hold wouldn't mean the call is on hold
           * for everyone. */
          held = TRUE;
          break;
        }
    }

  if (held)
    priv->call_state = HELD;
  else if (priv->call_state == HELD)
    priv->call_state = CONNECTED;
}

static void
call_handler_notify_call_cb (EmpathyCallHandler *handler,
    GParamSpec *spec,
    EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  TpCallChannel *call;

  g_object_get (priv->handler, "call-channel", &call, NULL);
  if (call == NULL)
    return;

/* FIXME
  tp_g_signal_connect_object (call, "audio-stream-error",
      G_CALLBACK (empathy_call_window_audio_stream_error), self, 0);
  tp_g_signal_connect_object (call, "video-stream-error",
      G_CALLBACK (empathy_call_window_video_stream_error), self, 0);
*/

  tp_g_signal_connect_object (call, "members-changed",
      G_CALLBACK (empathy_call_window_members_changed_cb), self, 0);

  g_object_unref (call);
}

static void
empathy_call_window_connect_handler (EmpathyCallWindow *self)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (self);
  TpCallChannel *call;

  g_signal_connect (priv->handler, "state-changed",
    G_CALLBACK (empathy_call_window_state_changed_cb), self);
  g_signal_connect (priv->handler, "conference-added",
    G_CALLBACK (empathy_call_window_conference_added_cb), self);
  g_signal_connect (priv->handler, "conference-removed",
    G_CALLBACK (empathy_call_window_conference_removed_cb), self);
  g_signal_connect (priv->handler, "closed",
    G_CALLBACK (empathy_call_window_channel_closed_cb), self);
  g_signal_connect (priv->handler, "src-pad-added",
    G_CALLBACK (empathy_call_window_src_added_cb), self);
  g_signal_connect (priv->handler, "content-added",
    G_CALLBACK (empathy_call_window_content_added_cb), self);
  g_signal_connect (priv->handler, "content-removed",
    G_CALLBACK (empathy_call_window_content_removed_cb), self);

  /* We connect to ::call-channel unconditionally since we'll
   * get new channels if we hangup and redial or if we reuse the
   * call window. */
  g_signal_connect (priv->handler, "notify::call-channel",
    G_CALLBACK (call_handler_notify_call_cb), self);

  g_signal_connect (priv->handler, "framerate-changed",
    G_CALLBACK (empathy_call_window_framerate_changed_cb), self);
  g_signal_connect (priv->handler, "resolution-changed",
    G_CALLBACK (empathy_call_window_resolution_changed_cb), self);

  g_object_get (priv->handler, "call-channel", &call, NULL);
  if (call != NULL)
    {
      /* We won't get notify::call-channel for this channel, so
       * directly call the callback. */
      call_handler_notify_call_cb (priv->handler, NULL, self);
      g_object_unref (call);
    }
}

static void
empathy_call_window_realized_cb (GtkWidget *widget,
    EmpathyCallWindow *self)
{
  gint width;

  /* Make the hangup button twice as wide */
  width = gtk_widget_get_allocated_width (self->priv->hangup_button);
  gtk_widget_set_size_request (self->priv->hangup_button, width * 2, -1);

  empathy_call_window_connect_handler (self);

  gst_element_set_state (self->priv->pipeline, GST_STATE_PAUSED);
}

static gboolean
empathy_call_window_delete_cb (GtkWidget *widget, GdkEvent*event,
  EmpathyCallWindow *window)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);

  if (priv->pipeline != NULL)
    {
      if (priv->bus_message_source_id != 0)
        {
          g_source_remove (priv->bus_message_source_id);
          priv->bus_message_source_id = 0;
        }

      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
    }

  if (priv->call_state == CONNECTING)
    empathy_sound_manager_stop (priv->sound_mgr, EMPATHY_SOUND_PHONE_OUTGOING);

  return FALSE;
}

static void
show_controls (EmpathyCallWindow *window, gboolean set_fullscreen)
{
  GtkWidget *menu;
  EmpathyCallWindowPriv *priv = GET_PRIV (window);

  menu = gtk_ui_manager_get_widget (priv->ui_manager,
            "/menubar1");

  if (set_fullscreen)
    {
      gtk_widget_hide (priv->dtmf_panel);
      gtk_widget_hide (menu);
      gtk_widget_hide (priv->toolbar);
    }
  else
    {
      if (priv->dialpad_was_visible_before_fs)
        gtk_widget_show (priv->dtmf_panel);

      gtk_widget_show (menu);
      gtk_widget_show (priv->toolbar);

      gtk_window_resize (GTK_WINDOW (window), priv->original_width_before_fs,
          priv->original_height_before_fs);
    }
}

static void
show_borders (EmpathyCallWindow *window, gboolean set_fullscreen)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);

  gtk_box_set_spacing (GTK_BOX (priv->content_hbox),
      set_fullscreen ? 0 : CONTENT_HBOX_SPACING);

  if (priv->video_output != NULL)
    {
#if 0
      gtk_box_set_child_packing (GTK_BOX (priv->content_hbox),
          priv->video_output, TRUE, TRUE,
          set_fullscreen ? 0 : CONTENT_HBOX_CHILDREN_PACKING_PADDING,
          GTK_PACK_START);
#endif
    }
}

static gboolean
empathy_call_window_state_event_cb (GtkWidget *widget,
  GdkEventWindowState *event, EmpathyCallWindow *window)
{
  if (event->changed_mask & GDK_WINDOW_STATE_FULLSCREEN)
    {
      EmpathyCallWindowPriv *priv = GET_PRIV (window);
      gboolean set_fullscreen = event->new_window_state &
        GDK_WINDOW_STATE_FULLSCREEN;

      if (set_fullscreen)
        {
          gboolean dialpad_was_visible;
          GtkAllocation allocation;
          gint original_width, original_height;

          gtk_widget_get_allocation (GTK_WIDGET (window), &allocation);
          original_width = allocation.width;
          original_height = allocation.height;

          g_object_get (priv->dtmf_panel,
              "visible", &dialpad_was_visible,
              NULL);

          priv->dialpad_was_visible_before_fs = dialpad_was_visible;
          priv->original_width_before_fs = original_width;
          priv->original_height_before_fs = original_height;

          if (priv->video_output_motion_handler_id == 0 &&
                priv->video_output != NULL)
            {
              priv->video_output_motion_handler_id = g_signal_connect (
                  G_OBJECT (priv->video_container), "motion-notify-event",
                  G_CALLBACK (empathy_call_window_video_output_motion_notify),
                  window);
            }
        }
      else
        {
          disconnect_video_output_motion_handler (window);
        }

      empathy_call_window_fullscreen_set_fullscreen (priv->fullscreen,
          set_fullscreen);
      show_controls (window, set_fullscreen);
      show_borders (window, set_fullscreen);
      gtk_action_set_stock_id (priv->menu_fullscreen,
          (set_fullscreen ? "view-restore" : "view-fullscreen"));
      priv->is_fullscreen = set_fullscreen;
  }

  return FALSE;
}

static void
empathy_call_window_show_dialpad (EmpathyCallWindow *window,
    gboolean active)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);
  int w, h, dialpad_width;
  GtkAllocation allocation;

  gtk_widget_get_allocation (GTK_WIDGET (window), &allocation);
  w = allocation.width;
  h = allocation.height;

  gtk_widget_get_preferred_width (priv->dtmf_panel, &dialpad_width, NULL);

  if (active)
    {
      gtk_widget_show (priv->dtmf_panel);
      w += dialpad_width;
    }
  else
    {
      w -= dialpad_width;
      gtk_widget_hide (priv->dtmf_panel);
    }

  if (w > 0 && h > 0)
    gtk_window_resize (GTK_WINDOW (window), w, h);
}

static void
empathy_call_window_set_send_video (EmpathyCallWindow *window,
  CameraState state)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);
  TpCallChannel *call;

  priv->sending_video = (state == CAMERA_STATE_ON);

  if (state == CAMERA_STATE_ON)
    {
      /* When we start sending video, we want to show the video preview by
         default. */
      display_video_preview (window, TRUE);
    }
  else
    {
      display_video_preview (window, FALSE);
    }

  if (priv->call_state != CONNECTED)
    return;

  g_object_get (priv->handler, "call-channel", &call, NULL);
  DEBUG ("%s sending video", priv->sending_video ? "start": "stop");
  empathy_call_channel_send_video (call, priv->sending_video);
  g_object_unref (call);
}

static void
empathy_call_window_hangup_cb (gpointer object,
    EmpathyCallWindow *self)
{
  /* stopping the call will put it the ENDED state and
   * from state_changed_cb we'll reconfigure the window
   */
  empathy_call_handler_stop_call (self->priv->handler);
}

static void
empathy_call_window_restart_call (EmpathyCallWindow *window)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);

  /* Remove error info bars */
  gtk_container_forall (GTK_CONTAINER (priv->errors_vbox),
      (GtkCallback) gtk_widget_destroy, NULL);

  create_video_output_widget (window);
  priv->outgoing = TRUE;
  empathy_call_window_set_state_connecting (window);

  if (priv->pipeline_playing)
    start_call (window);
  else
    /* call will be started when the pipeline is ready */
    priv->start_call_when_playing = TRUE;

  empathy_call_window_setup_avatars (window, priv->handler);

  empathy_call_window_show_hangup_button (window, TRUE);
}

static void
empathy_call_window_dialpad_cb (GtkToggleToolButton *button,
    EmpathyCallWindow *window)
{
  gboolean active;

  active = gtk_toggle_tool_button_get_active (button);

  empathy_call_window_show_dialpad (window, active);
}

static void
empathy_call_window_fullscreen_cb (gpointer object,
                                   EmpathyCallWindow *window)
{
  empathy_call_window_fullscreen_toggle (window);
}

static void
empathy_call_window_fullscreen_toggle (EmpathyCallWindow *window)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);

  if (priv->is_fullscreen)
    gtk_window_unfullscreen (GTK_WINDOW (window));
  else
    gtk_window_fullscreen (GTK_WINDOW (window));
}

static gboolean
empathy_call_window_video_button_press_cb (GtkWidget *video_preview,
  GdkEventButton *event, EmpathyCallWindow *window)
{
  if (event->button == 3 && event->type == GDK_BUTTON_PRESS)
    {
      empathy_call_window_video_menu_popup (window, event->button);
      return TRUE;
    }

  return FALSE;
}

static gboolean
empathy_call_window_key_press_cb (GtkWidget *video_output,
  GdkEventKey *event, EmpathyCallWindow *window)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);
  gchar key;

  if (priv->is_fullscreen && event->keyval == GDK_KEY_Escape)
    {
      /* Since we are in fullscreen mode, toggling will bring us back to
         normal mode. */
      empathy_call_window_fullscreen_toggle (window);
      return TRUE;
    }

  key = gdk_keyval_to_unicode (event->keyval);
  switch (key)
    {
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
      case '*':
      case '#':
        break;
      default:
        return TRUE;
        break;
    }

  gtk_toggle_tool_button_set_active (
      GTK_TOGGLE_TOOL_BUTTON (priv->dialpad_button), TRUE);

  empathy_dialpad_widget_press_key (
      EMPATHY_DIALPAD_WIDGET (priv->dtmf_panel), key);

  return TRUE;
}

static gboolean
empathy_call_window_video_output_motion_notify (GtkWidget *widget,
    GdkEventMotion *event, EmpathyCallWindow *window)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);

  if (priv->is_fullscreen)
    {
      empathy_call_window_fullscreen_show_popup (priv->fullscreen);

      /* Show the bottom toolbar */
      empathy_call_window_motion_notify_cb (NULL, NULL, window);
      return TRUE;
    }
  return FALSE;
}

static void
empathy_call_window_video_menu_popup (EmpathyCallWindow *window,
  guint button)
{
  GtkWidget *menu;
  EmpathyCallWindowPriv *priv = GET_PRIV (window);

  menu = gtk_ui_manager_get_widget (priv->ui_manager,
            "/video-popup");
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL,
      button, gtk_get_current_event_time ());
  gtk_menu_shell_select_first (GTK_MENU_SHELL (menu), FALSE);
}

static void
empathy_call_window_status_message (EmpathyCallWindow *self,
  gchar *message)
{
  gtk_label_set_label (GTK_LABEL (self->priv->status_label), message);
}

GtkUIManager *
empathy_call_window_get_ui_manager (EmpathyCallWindow *window)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);

  return priv->ui_manager;
}

EmpathyGstAudioSrc *
empathy_call_window_get_audio_src (EmpathyCallWindow *window)
{
  EmpathyCallWindowPriv *priv = GET_PRIV (window);

  return (EmpathyGstAudioSrc *) priv->audio_input;
}

EmpathyGstVideoSrc *
empathy_call_window_get_video_src (EmpathyCallWindow *self)
{
  return EMPATHY_GST_VIDEO_SRC (self->priv->video_input);
}

void
empathy_call_window_change_webcam (EmpathyCallWindow *self,
    const gchar *device)
{
  EmpathyGstVideoSrc *video;
  gboolean running;

  /* Restart the camera only if it's already running */
  running = (self->priv->video_preview != NULL);
  video = empathy_call_window_get_video_src (self);

  if (running)
    empathy_call_window_play_camera (self, FALSE);

  empathy_video_src_change_device (video, device);

  if (running)
    empathy_call_window_play_camera (self, TRUE);
}
