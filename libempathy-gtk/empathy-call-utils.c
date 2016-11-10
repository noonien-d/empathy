/*
 * Copyright (C) 2011 Collabora Ltd.
 *
 * The code contained in this file is free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either version
 * 2.1 of the License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this code; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Authors: Emilio Pozuelo Monfort <emilio.pozuelo@collabora.co.uk>
 */

#include "config.h"
#include "empathy-call-utils.h"

#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-request-util.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

static const gchar *
get_error_display_message (GError *error)
{
  if (error->domain != TP_ERROR)
    return _("There was an error starting the call");

  switch (error->code)
    {
      case TP_ERROR_NETWORK_ERROR:
        return _("Network error");
      case TP_ERROR_NOT_CAPABLE:
        return _("The specified contact doesn’t support calls");
      case TP_ERROR_OFFLINE:
        return _("The specified contact is offline");
      case TP_ERROR_INVALID_HANDLE:
        return _("The specified contact is not valid");
      case TP_ERROR_EMERGENCY_CALLS_NOT_SUPPORTED:
        return _("Emergency calls are not supported on this protocol");
      case TP_ERROR_INSUFFICIENT_BALANCE:
        return _("You don’t have enough credit in order to place this call");
    }

  return _("There was an error starting the call");
}

static void
show_call_error (GError *error)
{
  GtkWidget *dialog;

  dialog = gtk_message_dialog_new (NULL, 0,
      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
      "%s", get_error_display_message (error));

  g_signal_connect_swapped (dialog, "response",
      G_CALLBACK (gtk_widget_destroy),
      dialog);

  gtk_widget_show (dialog);
}

static void
create_call_channel_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GError *error = NULL;

  if (tp_account_channel_request_create_channel_finish (
      TP_ACCOUNT_CHANNEL_REQUEST (source), result, &error))
    return;

  DEBUG ("Failed to create Call channel: %s", error->message);

  show_call_error (error);
}

TpAccountChannelRequest *
empathy_call_create_call_request (TpAccount *account,
    const gchar *contact,
    gboolean initial_video,
    gint64 timestamp)
{
  TpAccountChannelRequest *call_req;

  if (initial_video)
    call_req = tp_account_channel_request_new_audio_video_call (account,
        timestamp);
  else
    call_req = tp_account_channel_request_new_audio_call (account, timestamp);

  tp_account_channel_request_set_target_id (call_req, TP_HANDLE_TYPE_CONTACT,
      contact);

  return call_req;
}

void
empathy_call_new_with_streams (const gchar *contact,
    TpAccount *account,
    gboolean initial_video,
    gint64 timestamp)
{
  TpAccountChannelRequest *call_req;

  call_req = empathy_call_create_call_request (account, contact, initial_video,
      timestamp);

  tp_account_channel_request_create_channel_async (call_req,
      EMPATHY_CALL_TP_BUS_NAME, NULL, create_call_channel_cb, NULL);

  g_object_unref (call_req);
}

/* Copied from telepathy-yell call-channel.c */
void
empathy_call_channel_send_video (TpCallChannel *self,
    gboolean send)
{
  GPtrArray *contents;
  gboolean found = FALSE;
  guint i;

  g_return_if_fail (TP_IS_CALL_CHANNEL (self));

  /* Loop over all the contents, if some of them a video set all their
   * streams to sending, otherwise request a video channel in case we want to
   * sent */
  contents = tp_call_channel_get_contents (self);
  for (i = 0 ; i < contents->len ; i++)
    {
      TpCallContent *content = g_ptr_array_index (contents, i);

      if (tp_call_content_get_media_type (content) ==
              TP_MEDIA_STREAM_TYPE_VIDEO)
        {
          GPtrArray *streams;
          guint j;

          found = TRUE;
          streams = tp_call_content_get_streams (content);
          for (j = 0; j < streams->len; j++)
            {
              TpCallStream *stream = g_ptr_array_index (streams, j);

              tp_call_stream_set_sending_async (stream, send, NULL, NULL);
            }
        }
    }

  if (send && !found)
    {
      tp_call_channel_add_content_async (self, "video",
          TP_MEDIA_STREAM_TYPE_VIDEO, TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL,
          NULL, NULL);
    }
}

/* Copied from telepathy-yell call-channel.c */
TpSendingState
empathy_call_channel_get_video_state (TpCallChannel *self)
{
  TpSendingState result = TP_SENDING_STATE_NONE;
  GPtrArray *contents;
  guint i;

  g_return_val_if_fail (TP_IS_CALL_CHANNEL (self), TP_SENDING_STATE_NONE);

  contents = tp_call_channel_get_contents (self);
  for (i = 0 ; i < contents->len ; i++)
    {
      TpCallContent *content = g_ptr_array_index (contents, i);

      if (tp_call_content_get_media_type (content) ==
              TP_MEDIA_STREAM_TYPE_VIDEO)
        {
          GPtrArray *streams;
          guint j;

          streams = tp_call_content_get_streams (content);
          for (j = 0; j < streams->len; j++)
            {
              TpCallStream *stream = g_ptr_array_index (streams, j);
              TpSendingState state;

              state = tp_call_stream_get_local_sending_state (stream);
              if (state != TP_SENDING_STATE_PENDING_STOP_SENDING &&
                  state > result)
                result = state;
            }
        }
    }

  return result;
}
