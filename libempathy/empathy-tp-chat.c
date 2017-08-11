/*
 * Copyright (C) 2007-2012 Collabora Ltd.
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
 *
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 */

#include "config.h"
#include "empathy-tp-chat.h"

#include <tp-account-widgets/tpaw-utils.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-request-util.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_TP | EMPATHY_DEBUG_CHAT
#include "empathy-debug.h"

struct _EmpathyTpChatPrivate
{
  TpAccount *account;
  EmpathyContact *user;
  EmpathyContact *remote_contact;
  GList *members;
  /* Queue of messages signalled but not acked yet */
  GQueue *pending_messages_queue;

  /* Subject */
  gboolean supports_subject;
  gboolean can_set_subject;
  gchar *subject;
  gchar *subject_actor;

  /* Room config (for now, we only track the title and don't support
   * setting it) */
  gchar *title;

  gboolean can_upgrade_to_muc;

  GHashTable *messages_being_sent;

  /* GSimpleAsyncResult used when preparing EMPATHY_TP_CHAT_FEATURE_CORE */
  GSimpleAsyncResult *ready_result;
  gboolean preparing_password;
};

enum
{
  PROP_0,
  PROP_ACCOUNT,
  PROP_SELF_CONTACT,
  PROP_REMOTE_CONTACT,
  PROP_N_MESSAGES_SENDING,
  PROP_TITLE,
  PROP_SUBJECT,
};

enum
{
  MESSAGE_RECEIVED,
  MESSAGE_STATUS_UPDATE,
  SEND_ERROR,
  MESSAGE_ACKNOWLEDGED,
  SIG_MEMBER_RENAMED,
  SIG_MEMBERS_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (EmpathyTpChat, empathy_tp_chat, TP_TYPE_TEXT_CHANNEL)

static void
tp_chat_set_delivery_status (EmpathyTpChat *self,
    const gchar *token,
    EmpathyDeliveryStatus delivery_status)
{
  TpDeliveryReportingSupportFlags flags =
    tp_text_channel_get_delivery_reporting_support (
      TP_TEXT_CHANNEL (self));

  /* channel must support receiving failures and successes */
  if (!tp_str_empty (token) &&
      flags & TP_DELIVERY_REPORTING_SUPPORT_FLAG_RECEIVE_FAILURES &&
      flags & TP_DELIVERY_REPORTING_SUPPORT_FLAG_RECEIVE_SUCCESSES)
    {
      DEBUG ("Delivery status (%s) = %u", token, delivery_status);

      switch (delivery_status)
        {
          case EMPATHY_DELIVERY_STATUS_NONE:
            g_hash_table_remove (self->priv->messages_being_sent,
              token);
            break;

          default:
            g_hash_table_insert (self->priv->messages_being_sent,
              g_strdup (token),
              GUINT_TO_POINTER (delivery_status));
            break;
        }

    g_object_notify (G_OBJECT (self), "n-messages-sending");
  }
}

static void tp_chat_prepare_ready_async (TpProxy *proxy,
  const TpProxyFeature *feature,
  GAsyncReadyCallback callback,
  gpointer user_data);

static void
tp_chat_async_cb (TpChannel *proxy,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  if (error != NULL)
    DEBUG ("Error %s: %s", (gchar *) user_data, error->message);
}

static void
update_config_cb (TpChannel *proxy,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  if (error != NULL)
    DEBUG ("Failed to change config of the room: %s", error->message);
}

static void
create_conference_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  TpChannel *channel;
  GError *error = NULL;
  GHashTable *props;

  channel = tp_account_channel_request_create_and_observe_channel_finish (
      TP_ACCOUNT_CHANNEL_REQUEST (source), result, &error);
  if (channel == NULL)
    {
      DEBUG ("Failed to create conference channel: %s", error->message);
      g_error_free (error);
      return;
    }

  /* Make the channel more confidential as only people invited are supposed to
   * join it. */
  props = tp_asv_new (
      "Private", G_TYPE_BOOLEAN, TRUE,
      "InviteOnly", G_TYPE_BOOLEAN, TRUE,
      NULL);

  tp_cli_channel_interface_room_config_call_update_configuration (channel, -1,
      props, update_config_cb, NULL, NULL, NULL);

  g_object_unref (channel);
  g_hash_table_unref (props);
}

void
empathy_tp_chat_add (EmpathyTpChat *self,
    EmpathyContact *contact,
    const gchar *message)
{
  TpChannel *channel = (TpChannel *) self;

  if (tp_proxy_has_interface_by_id (self,
    TP_IFACE_QUARK_CHANNEL_INTERFACE_GROUP))
    {
      TpHandle handle;
      GArray handles = {(gchar *) &handle, 1};

      g_return_if_fail (EMPATHY_IS_CONTACT (contact));

      handle = empathy_contact_get_handle (contact);
      tp_cli_channel_interface_group_call_add_members (channel,
        -1, &handles, NULL, NULL, NULL, NULL, NULL);
    }
  else if (self->priv->can_upgrade_to_muc)
    {
      TpAccountChannelRequest *req;
      const gchar *channels[2] = { NULL, };
      const char *invitees[2] = { NULL, };
      TpAccount *account;

      invitees[0] = empathy_contact_get_id (contact);
      channels[0] = tp_proxy_get_object_path (self);

      account = empathy_tp_chat_get_account (self);

      req = tp_account_channel_request_new_text (account,
        TP_USER_ACTION_TIME_NOT_USER_ACTION);

      tp_account_channel_request_set_conference_initial_channels (req,
          channels);

      tp_account_channel_request_set_initial_invitee_ids (req, invitees);

      /* FIXME: InvitationMessage ? */

      /* Although this is a MUC, it's anonymous, so CreateChannel is
       * valid. */
      tp_account_channel_request_create_and_observe_channel_async (req,
          EMPATHY_CHAT_TP_BUS_NAME, NULL, create_conference_cb, NULL);

      g_object_unref (req);
    }
  else
    {
      g_warning ("Cannot add to this channel");
    }
}

GList *
empathy_tp_chat_get_members (EmpathyTpChat *self)
{
  GList *members = NULL;

  if (self->priv->members)
    {
      members = g_list_copy (self->priv->members);
      g_list_foreach (members, (GFunc) g_object_ref, NULL);
    }
  else
    {
      members = g_list_prepend (members, g_object_ref (self->priv->user));

      if (self->priv->remote_contact != NULL)
        members = g_list_prepend (members,
            g_object_ref (self->priv->remote_contact));
    }

  return members;
}

static void
check_ready (EmpathyTpChat *self)
{
  if (self->priv->ready_result == NULL)
    return;

  DEBUG ("Ready");

  g_simple_async_result_complete_in_idle (self->priv->ready_result);
  tp_clear_object (&self->priv->ready_result);
}

static void
tp_chat_build_message (EmpathyTpChat *self,
    TpMessage *msg,
    gboolean incoming)
{
  EmpathyMessage *message;
  TpContact *sender;

  message = empathy_message_new_from_tp_message (msg, incoming);
  /* FIXME: this is actually a lie for incoming messages. */
  empathy_message_set_receiver (message, self->priv->user);

  sender = tp_signalled_message_get_sender (msg);
  g_assert (sender != NULL);

  if (tp_contact_get_handle (sender) == 0)
    {
      empathy_message_set_sender (message, self->priv->user);
    }
  else
    {
      EmpathyContact *contact;

      contact = empathy_contact_dup_from_tp_contact (sender);

      empathy_message_set_sender (message, contact);

      g_object_unref (contact);
    }

  g_queue_push_tail (self->priv->pending_messages_queue, message);
  g_signal_emit (self, signals[MESSAGE_RECEIVED], 0, message);
}

static void
handle_delivery_report (EmpathyTpChat *self,
    TpMessage *message)
{
  TpDeliveryStatus delivery_status;
  const GHashTable *header;
  TpChannelTextSendError delivery_error;
  gboolean valid;
  GPtrArray *echo;
  const gchar *message_body = NULL;
  const gchar *delivery_dbus_error;
  const gchar *delivery_token = NULL;

  header = tp_message_peek (message, 0);
  if (header == NULL)
    goto out;

  delivery_token = tp_asv_get_string (header, "delivery-token");
  delivery_status = tp_asv_get_uint32 (header, "delivery-status", &valid);

  if (valid)
    g_signal_emit (self, signals[MESSAGE_STATUS_UPDATE], 0, delivery_token, delivery_status);

  if (!valid)
    {
      goto out;
    }
  else if (delivery_status == TP_DELIVERY_STATUS_ACCEPTED)
    {
      DEBUG ("Accepted %s", delivery_token);
      tp_chat_set_delivery_status (self, delivery_token,
        EMPATHY_DELIVERY_STATUS_ACCEPTED);
      goto out;
    }
  else if (delivery_status == TP_DELIVERY_STATUS_DELIVERED)
    {
      DEBUG ("Delivered %s", delivery_token);
      tp_chat_set_delivery_status (self, delivery_token,
        EMPATHY_DELIVERY_STATUS_NONE);
      goto out;
    }
  else if (delivery_status == TP_DELIVERY_STATUS_READ)
    {
      DEBUG ("Read %s", delivery_token);
      tp_chat_set_delivery_status (self, delivery_token,
        EMPATHY_DELIVERY_STATUS_NONE);
      goto out;
    }
  else if (delivery_status != TP_DELIVERY_STATUS_PERMANENTLY_FAILED &&
       delivery_status != TP_DELIVERY_STATUS_TEMPORARILY_FAILED)
    {
      goto out;
    }

  delivery_error = tp_asv_get_uint32 (header, "delivery-error", &valid);
  if (!valid)
    delivery_error = TP_CHANNEL_TEXT_SEND_ERROR_UNKNOWN;

  delivery_dbus_error = tp_asv_get_string (header, "delivery-dbus-error");

  /* TODO: ideally we should use tp-glib API giving us the echoed message as a
   * TpMessage. (fdo #35884) */
  echo = tp_asv_get_boxed (header, "delivery-echo",
    TP_ARRAY_TYPE_MESSAGE_PART_LIST);
  if (echo != NULL && echo->len >= 2)
    {
      const GHashTable *echo_body;

      echo_body = g_ptr_array_index (echo, 1);
      if (echo_body != NULL)
        message_body = tp_asv_get_string (echo_body, "content");
    }

  tp_chat_set_delivery_status (self, delivery_token,
      EMPATHY_DELIVERY_STATUS_NONE);
  g_signal_emit (self, signals[SEND_ERROR], 0, message_body,
      delivery_error, delivery_dbus_error);

out:
  tp_text_channel_ack_message_async (TP_TEXT_CHANNEL (self),
    message, NULL, NULL);
}

static void
handle_incoming_message (EmpathyTpChat *self,
    TpMessage *message,
    gboolean pending)
{
  gchar *message_body;

  if (tp_message_is_delivery_report (message))
    {
      handle_delivery_report (self, message);
      return;
    }

  message_body = tp_message_to_text (message, NULL);

  DEBUG ("Message %s (channel %s): %s",
    pending ? "pending" : "received",
    tp_proxy_get_object_path (self), message_body);

  if (message_body == NULL)
    {
      DEBUG ("Empty message with NonTextContent, ignoring and acking.");

      tp_text_channel_ack_message_async (TP_TEXT_CHANNEL (self),
        message, NULL, NULL);
      return;
    }

  tp_chat_build_message (self, message, TRUE);

  g_free (message_body);
}

static void
message_received_cb (TpTextChannel *channel,
    TpMessage *message,
    EmpathyTpChat *self)
{
  handle_incoming_message (self, message, FALSE);
}

static gboolean
find_pending_message_func (gconstpointer a,
    gconstpointer b)
{
  EmpathyMessage *msg = (EmpathyMessage *) a;
  TpMessage *message = (TpMessage *) b;

  if (empathy_message_get_tp_message (msg) == message)
    return 0;

  return -1;
}

static void
pending_message_removed_cb (TpTextChannel   *channel,
    TpMessage *message,
    EmpathyTpChat *self)
{
  GList *m;

  m = g_queue_find_custom (self->priv->pending_messages_queue, message,
      find_pending_message_func);

  if (m == NULL)
    return;

  g_signal_emit (self, signals[MESSAGE_ACKNOWLEDGED], 0, m->data);

  g_object_unref (m->data);
  g_queue_delete_link (self->priv->pending_messages_queue, m);
}

static void
message_sent_cb (TpTextChannel *channel,
    TpMessage *message,
    TpMessageSendingFlags flags,
    gchar *token,
    EmpathyTpChat *self)
{
  gchar *message_body;

  message_body = tp_message_to_text (message, NULL);

  DEBUG ("Message sent: %s", message_body);

  tp_chat_build_message (self, message, FALSE);

  g_free (message_body);
}

static TpChannelTextSendError
error_to_text_send_error (GError *error)
{
  if (error->domain != TP_ERROR)
    return TP_CHANNEL_TEXT_SEND_ERROR_UNKNOWN;

  switch (error->code)
    {
      case TP_ERROR_OFFLINE:
        return TP_CHANNEL_TEXT_SEND_ERROR_OFFLINE;
      case TP_ERROR_INVALID_HANDLE:
        return TP_CHANNEL_TEXT_SEND_ERROR_INVALID_CONTACT;
      case TP_ERROR_PERMISSION_DENIED:
        return TP_CHANNEL_TEXT_SEND_ERROR_PERMISSION_DENIED;
      case TP_ERROR_NOT_IMPLEMENTED:
        return TP_CHANNEL_TEXT_SEND_ERROR_NOT_IMPLEMENTED;
    }

  return TP_CHANNEL_TEXT_SEND_ERROR_UNKNOWN;
}

static void
message_send_cb (GObject *source,
     GAsyncResult *result,
     gpointer user_data)
{
  EmpathyTpChat *self = user_data;
  TpTextChannel *channel = (TpTextChannel *) source;
  gchar *token = NULL;
  GError *error = NULL;

  if (!tp_text_channel_send_message_finish (channel, result, &token, &error))
    {
      DEBUG ("Error: %s", error->message);

      /* FIXME: we should use the body of the message as first argument of the
       * signal but can't easily get it as we just get a user_data pointer. Once
       * we'll have rebased EmpathyTpChat on top of TpTextChannel we'll be able
       * to use the user_data pointer to pass the message and fix this. */
      g_signal_emit (self, signals[SEND_ERROR], 0,
               NULL, error_to_text_send_error (error), NULL);

      g_error_free (error);
    }

  tp_chat_set_delivery_status (self, token,
    EMPATHY_DELIVERY_STATUS_SENDING);
  g_free (token);
}

static void
list_pending_messages (EmpathyTpChat *self)
{
  GList *messages, *l;

  messages = tp_text_channel_dup_pending_messages (TP_TEXT_CHANNEL (self));

  for (l = messages; l != NULL; l = g_list_next (l))
    {
      TpMessage *message = l->data;

      handle_incoming_message (self, message, FALSE);
    }

  g_list_free_full (messages, g_object_unref);
}

static void
update_subject (EmpathyTpChat *self,
    GHashTable *properties)
{
  gboolean can_set, valid;
  const gchar *subject;

  can_set = tp_asv_get_boolean (properties, "CanSet", &valid);
  if (valid)
    self->priv->can_set_subject = can_set;

  subject = tp_asv_get_string (properties, "Subject");
  if (subject != NULL)
    {
      const gchar *actor;

      g_free (self->priv->subject);
      self->priv->subject = g_strdup (subject);

      /* If the actor is included with this update, use it;
       * otherwise, clear it to avoid showing stale information.
       * Why might it not be included? When you join an IRC channel,
       * you get a pair of messages: first, the current topic; next,
       * who set it, and when. Idle reports these in two separate
       * signals.
       */
      actor = tp_asv_get_string (properties, "Actor");
      g_free (self->priv->subject_actor);
      self->priv->subject_actor = g_strdup (actor);

      g_object_notify (G_OBJECT (self), "subject");
    }

  /* TODO: track Timestamp. */
}

static void
tp_chat_get_all_subject_cb (TpProxy *proxy,
    GHashTable *properties,
    const GError *error,
    gpointer user_data G_GNUC_UNUSED,
    GObject *chat)
{
  EmpathyTpChat *self = EMPATHY_TP_CHAT (chat);

  if (error != NULL)
    {
      DEBUG ("Error fetching subject: %s", error->message);
      return;
    }

  self->priv->supports_subject = TRUE;
  update_subject (self, properties);
}

static void
update_title (EmpathyTpChat *self,
    GHashTable *properties)
{
  const gchar *title = tp_asv_get_string (properties, "Title");

  if (title != NULL)
    {
      if (tp_str_empty (title))
        title = NULL;

      g_free (self->priv->title);
      self->priv->title = g_strdup (title);
      g_object_notify (G_OBJECT (self), "title");
    }
}

static void
tp_chat_get_all_room_config_cb (TpProxy *proxy,
    GHashTable *properties,
    const GError *error,
    gpointer user_data G_GNUC_UNUSED,
    GObject *chat)
{
  EmpathyTpChat *self = EMPATHY_TP_CHAT (chat);

  if (error)
    {
      DEBUG ("Error fetching room config: %s", error->message);
      return;
    }

  update_title (self, properties);
}

static void
tp_chat_dbus_properties_changed_cb (TpProxy *proxy,
    const gchar *interface_name,
    GHashTable *changed,
    const gchar **invalidated,
    gpointer user_data,
    GObject *chat)
{
  EmpathyTpChat *self = EMPATHY_TP_CHAT (chat);

  if (!tp_strdiff (interface_name, TP_IFACE_CHANNEL_INTERFACE_SUBJECT))
    update_subject (self, changed);

  if (!tp_strdiff (interface_name, TP_IFACE_CHANNEL_INTERFACE_ROOM_CONFIG))
    update_title (self, changed);
}

void
empathy_tp_chat_set_subject (EmpathyTpChat *self,
    const gchar *subject)
{
  tp_cli_channel_interface_subject_call_set_subject (TP_CHANNEL (self), -1,
      subject, tp_chat_async_cb, "while setting subject", NULL,
      G_OBJECT (self));
}

const gchar *
empathy_tp_chat_get_title (EmpathyTpChat *self)
{
  return self->priv->title;
}

gboolean
empathy_tp_chat_supports_subject (EmpathyTpChat *self)
{
  return self->priv->supports_subject;
}

gboolean
empathy_tp_chat_can_set_subject (EmpathyTpChat *self)
{
  return self->priv->can_set_subject;
}

const gchar *
empathy_tp_chat_get_subject (EmpathyTpChat *self)
{
  return self->priv->subject;
}

const gchar *
empathy_tp_chat_get_subject_actor (EmpathyTpChat *self)
{
  return self->priv->subject_actor;
}

static void
tp_chat_dispose (GObject *object)
{
  EmpathyTpChat *self = EMPATHY_TP_CHAT (object);

  tp_clear_object (&self->priv->remote_contact);
  tp_clear_object (&self->priv->user);

  g_queue_foreach (self->priv->pending_messages_queue,
    (GFunc) g_object_unref, NULL);
  g_queue_clear (self->priv->pending_messages_queue);

  tp_clear_object (&self->priv->ready_result);

  if (G_OBJECT_CLASS (empathy_tp_chat_parent_class)->dispose)
    G_OBJECT_CLASS (empathy_tp_chat_parent_class)->dispose (object);
}

static void
tp_chat_finalize (GObject *object)
{
  EmpathyTpChat *self = (EmpathyTpChat *) object;

  DEBUG ("Finalize: %p", object);

  g_queue_free (self->priv->pending_messages_queue);
  g_hash_table_unref (self->priv->messages_being_sent);

  g_free (self->priv->title);
  g_free (self->priv->subject);
  g_free (self->priv->subject_actor);

  G_OBJECT_CLASS (empathy_tp_chat_parent_class)->finalize (object);
}

static void
check_almost_ready (EmpathyTpChat *self)
{
  TpChannel *channel = (TpChannel *) self;

  if (self->priv->ready_result == NULL)
    return;

  if (self->priv->user == NULL)
    return;

  if (self->priv->preparing_password)
    return;

  /* We need either the members (room) or the remote contact (private chat).
   * If the chat is protected by a password we can't get these information so
   * consider the chat as ready so it can be presented to the user. */
  if (!tp_channel_password_needed (channel) && self->priv->members == NULL &&
      self->priv->remote_contact == NULL)
    return;

  g_assert (tp_proxy_is_prepared (self,
    TP_TEXT_CHANNEL_FEATURE_INCOMING_MESSAGES));

  tp_g_signal_connect_object (self, "message-received",
    G_CALLBACK (message_received_cb), self, 0);
  tp_g_signal_connect_object (self, "pending-message-removed",
    G_CALLBACK (pending_message_removed_cb), self, 0);

  list_pending_messages (self);

  tp_g_signal_connect_object (self, "message-sent",
    G_CALLBACK (message_sent_cb), self, 0);

  check_ready (self);
}

static void
add_members_contact (EmpathyTpChat *self,
    GPtrArray *contacts)
{
  guint i;

  for (i = 0; i < contacts->len; i++)
    {
      EmpathyContact *contact;

      contact = empathy_contact_dup_from_tp_contact (g_ptr_array_index (
            contacts, i));

      self->priv->members = g_list_prepend (self->priv->members, contact);

      g_signal_emit (self, signals[SIG_MEMBERS_CHANGED], 0,
                 contact, NULL, 0, NULL, TRUE);
    }

  check_almost_ready (self);
}

static void
remove_member (EmpathyTpChat *self,
    EmpathyContact *contact)
{
  GList *l;

  for (l = self->priv->members; l; l = l->next)
    {
      EmpathyContact *c = l->data;

      if (contact == c)
        {
          self->priv->members = g_list_delete_link (self->priv->members, l);
          g_object_unref (c);
          break;
        }
    }
}

static void
contact_renamed (EmpathyTpChat *self,
    TpContact *old_contact,
    TpContact *new_contact,
    TpChannelGroupChangeReason reason,
    const gchar *message)
{
  EmpathyContact *old = NULL, *new = NULL;

  old = empathy_contact_dup_from_tp_contact (old_contact);
  new = empathy_contact_dup_from_tp_contact (new_contact);

  self->priv->members = g_list_prepend (self->priv->members, new);

  if (old != NULL)
    {
      remove_member (self, old);

      g_signal_emit (self, signals[SIG_MEMBER_RENAMED], 0, old, new,
          reason, message);
      g_object_unref (old);
    }

  if (self->priv->user == old)
    {
      /* We change our nick */
      tp_clear_object (&self->priv->user);
      self->priv->user = g_object_ref (new);
      g_object_notify (G_OBJECT (self), "self-contact");
    }

  check_almost_ready (self);
}

static void
tp_chat_group_contacts_changed_cb (TpChannel *channel,
    GPtrArray *added,
    GPtrArray *removed,
    GPtrArray *local_pending,
    GPtrArray *remote_pending,
    TpContact *actor,
    GHashTable *details,
    EmpathyTpChat *self)
{
  EmpathyContact *actor_contact = NULL;
  guint i;
  TpChannelGroupChangeReason reason;
  const gchar *message;

  reason = tp_asv_get_uint32 (details, "change-reason", NULL);
  message = tp_asv_get_string (details, "message");

  /* Contact renamed */
  if (reason == TP_CHANNEL_GROUP_CHANGE_REASON_RENAMED)
    {
      /* there can only be a single 'added' and a single 'removed' handle */
      if (removed->len != 1 || added->len != 1)
        {
          g_warning ("RENAMED with %u added, %u removed (expected 1, 1)",
            added->len, removed->len);
          return;
        }

      contact_renamed (self, g_ptr_array_index (removed, 0),
          g_ptr_array_index (added, 0), reason, message);
      return;
    }

  if (actor != NULL)
    {
      actor_contact = empathy_contact_dup_from_tp_contact (actor);

      if (actor_contact == NULL)
        {
          /* FIXME: handle this a tad more gracefully: perhaps
           * the actor was a server op. We could use the
           * contact-ids detail of MembersChangedDetailed.
           */
          DEBUG ("actor %s not a channel member",
              tp_contact_get_identifier (actor));
        }
    }

  /* Remove contacts that are not members anymore */
  for (i = 0; i < removed->len; i++)
    {
      TpContact *tp_contact = g_ptr_array_index (removed, i);
      EmpathyContact *contact;

      contact = empathy_contact_dup_from_tp_contact (tp_contact);

      if (contact != NULL)
        {
          remove_member (self, contact);

          g_signal_emit (self, signals[SIG_MEMBERS_CHANGED], 0,
                     contact, actor_contact, reason, message, FALSE);
          g_object_unref (contact);
        }
    }

  if (added->len > 0)
    {
      add_members_contact (self, added);
    }

  if (actor_contact != NULL)
    g_object_unref (actor_contact);
}

static void
create_remote_contact (EmpathyTpChat *self,
    TpContact *contact)
{
  self->priv->remote_contact = empathy_contact_dup_from_tp_contact (contact);
  g_object_notify (G_OBJECT (self), "remote-contact");

  check_almost_ready (self);
}

static void
create_self_contact (EmpathyTpChat *self,
    TpContact *contact)
{
  self->priv->user = empathy_contact_dup_from_tp_contact (contact);
  empathy_contact_set_is_user (self->priv->user, TRUE);
  g_object_notify (G_OBJECT (self), "self-contact");
  check_almost_ready (self);
}

static void
tp_chat_get_property (GObject *object,
    guint param_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyTpChat *self = EMPATHY_TP_CHAT (object);

  switch (param_id)
    {
      case PROP_SELF_CONTACT:
        g_value_set_object (value, self->priv->user);
        break;
      case PROP_REMOTE_CONTACT:
        g_value_set_object (value, self->priv->remote_contact);
        break;
      case PROP_N_MESSAGES_SENDING:
        g_value_set_uint (value,
          g_hash_table_size (self->priv->messages_being_sent));
        break;
      case PROP_TITLE:
        g_value_set_string (value,
          empathy_tp_chat_get_title (self));
        break;
      case PROP_SUBJECT:
        g_value_set_string (value,
          empathy_tp_chat_get_subject (self));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        break;
    };
}

enum {
  FEAT_READY,
  N_FEAT
};

static const TpProxyFeature *
tp_chat_list_features (TpProxyClass *cls G_GNUC_UNUSED)
{
  static TpProxyFeature features[N_FEAT + 1] = { { 0 } };
  static GQuark need[3] = {0, 0, 0};

  if (G_LIKELY (features[0].name != 0))
    return features;

  features[FEAT_READY].name = EMPATHY_TP_CHAT_FEATURE_READY;
  need[0] = TP_TEXT_CHANNEL_FEATURE_INCOMING_MESSAGES;
  need[1] = TP_CHANNEL_FEATURE_CONTACTS;
  features[FEAT_READY].depends_on = need;
  features[FEAT_READY].prepare_async =
    tp_chat_prepare_ready_async;

  /* assert that the terminator at the end is there */
  g_assert (features[N_FEAT].name == 0);

  return features;
}

static void
empathy_tp_chat_class_init (EmpathyTpChatClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  TpProxyClass *proxy_class = TP_PROXY_CLASS (klass);

  object_class->dispose = tp_chat_dispose;
  object_class->finalize = tp_chat_finalize;
  object_class->get_property = tp_chat_get_property;

  proxy_class->list_features = tp_chat_list_features;

  /**
   * EmpathyTpChat:self-contact:
   *
   * Not to be confused with TpChannel:group-self-contact.
   */
  g_object_class_install_property (object_class, PROP_SELF_CONTACT,
      g_param_spec_object ("self-contact", "The local contact",
        "The EmpathyContact for the local user on this channel",
        EMPATHY_TYPE_CONTACT,
        G_PARAM_READABLE));

  g_object_class_install_property (object_class, PROP_REMOTE_CONTACT,
      g_param_spec_object ("remote-contact", "The remote contact",
        "The remote contact if there is no group iface on the channel",
        EMPATHY_TYPE_CONTACT,
        G_PARAM_READABLE));

  g_object_class_install_property (object_class, PROP_N_MESSAGES_SENDING,
      g_param_spec_uint ("n-messages-sending", "Num Messages Sending",
        "The number of messages being sent",
        0, G_MAXUINT, 0,
        G_PARAM_READABLE));

  g_object_class_install_property (object_class, PROP_TITLE,
      g_param_spec_string ("title", "Title",
        "A human-readable name for the room, if any",
        NULL,
        G_PARAM_READABLE |
        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_SUBJECT,
      g_param_spec_string ("subject", "Subject",
        "The room's current subject, if any",
        NULL,
        G_PARAM_READABLE |
        G_PARAM_STATIC_STRINGS));

  /* Signals */
  signals[MESSAGE_RECEIVED] = g_signal_new ("message-received-empathy",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL,
      g_cclosure_marshal_generic,
      G_TYPE_NONE,
      1, EMPATHY_TYPE_MESSAGE);

  signals[MESSAGE_STATUS_UPDATE] = g_signal_new ("message-status-update",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL,
      g_cclosure_marshal_generic,
      G_TYPE_NONE,
      2, G_TYPE_STRING, G_TYPE_UINT);

  signals[SEND_ERROR] = g_signal_new ("send-error",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL,
      g_cclosure_marshal_generic,
      G_TYPE_NONE,
      3, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING);

  signals[MESSAGE_ACKNOWLEDGED] = g_signal_new ("message-acknowledged",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL,
      g_cclosure_marshal_generic,
      G_TYPE_NONE,
      1, EMPATHY_TYPE_MESSAGE);

  signals[SIG_MEMBER_RENAMED] = g_signal_new ("member-renamed",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      4, EMPATHY_TYPE_CONTACT, EMPATHY_TYPE_CONTACT,
      G_TYPE_UINT, G_TYPE_STRING);

  signals[SIG_MEMBERS_CHANGED] = g_signal_new ("members-changed",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      5, EMPATHY_TYPE_CONTACT, EMPATHY_TYPE_CONTACT,
      G_TYPE_UINT, G_TYPE_STRING, G_TYPE_BOOLEAN);

  g_type_class_add_private (object_class, sizeof (EmpathyTpChatPrivate));
}

static void
empathy_tp_chat_init (EmpathyTpChat *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, EMPATHY_TYPE_TP_CHAT,
      EmpathyTpChatPrivate);

  self->priv->pending_messages_queue = g_queue_new ();
  self->priv->messages_being_sent = g_hash_table_new_full (
      g_str_hash, g_str_equal, g_free, NULL);
}

EmpathyTpChat *
empathy_tp_chat_new (TpSimpleClientFactory *factory,
    TpConnection *conn,
    const gchar *object_path,
    const GHashTable *immutable_properties)
{
  g_return_val_if_fail (TP_IS_CONNECTION (conn), NULL);
  g_return_val_if_fail (immutable_properties != NULL, NULL);

  return g_object_new (EMPATHY_TYPE_TP_CHAT,
      "factory", factory,
       "connection", conn,
       "dbus-daemon", tp_proxy_get_dbus_daemon (conn),
       "bus-name", tp_proxy_get_bus_name (conn),
       "object-path", object_path,
       "channel-properties", immutable_properties,
       NULL);
}

const gchar *
empathy_tp_chat_get_id (EmpathyTpChat *self)
{
  const gchar *id;

  g_return_val_if_fail (EMPATHY_IS_TP_CHAT (self), NULL);

  id = tp_channel_get_identifier ((TpChannel *) self);
  if (!TPAW_STR_EMPTY (id))
    return id;
  else if (self->priv->remote_contact)
    return empathy_contact_get_id (self->priv->remote_contact);
  else
    return NULL;

}

EmpathyContact *
empathy_tp_chat_get_remote_contact (EmpathyTpChat *self)
{
  g_return_val_if_fail (EMPATHY_IS_TP_CHAT (self), NULL);

  return self->priv->remote_contact;
}

TpAccount *
empathy_tp_chat_get_account (EmpathyTpChat *self)
{
  TpConnection *connection;

  g_return_val_if_fail (EMPATHY_IS_TP_CHAT (self), NULL);

  connection = tp_channel_get_connection (TP_CHANNEL (self));

  return tp_connection_get_account (connection);
}

void
empathy_tp_chat_send (EmpathyTpChat *self,
          TpMessage *message)
{
  gchar *message_body;

  g_return_if_fail (EMPATHY_IS_TP_CHAT (self));
  g_return_if_fail (TP_IS_CLIENT_MESSAGE (message));

  message_body = tp_message_to_text (message, NULL);

  DEBUG ("Sending message: %s", message_body);

  tp_text_channel_send_message_async (TP_TEXT_CHANNEL (self),
    message, TP_MESSAGE_SENDING_FLAG_REPORT_DELIVERY | TP_MESSAGE_SENDING_FLAG_REPORT_READ,
    message_send_cb, self);

  g_free (message_body);
}

const GList *
empathy_tp_chat_get_pending_messages (EmpathyTpChat *self)
{
  g_return_val_if_fail (EMPATHY_IS_TP_CHAT (self), NULL);

  return self->priv->pending_messages_queue->head;
}

void
empathy_tp_chat_acknowledge_message (EmpathyTpChat *self,
    EmpathyMessage *message)
{
  TpMessage *tp_msg;

  g_return_if_fail (EMPATHY_IS_TP_CHAT (self));

  if (!empathy_message_is_incoming (message))
    return;

  tp_msg = empathy_message_get_tp_message (message);
  tp_text_channel_ack_message_async (TP_TEXT_CHANNEL (self),
             tp_msg, NULL, NULL);
}

/**
 * empathy_tp_chat_can_add_contact:
 *
 * Returns: %TRUE if empathy_contact_list_add() will work for this channel.
 * That is if this chat is a 1-to-1 channel that can be upgraded to
 * a MUC using the Conference interface or if the channel is a MUC.
 */
gboolean
empathy_tp_chat_can_add_contact (EmpathyTpChat *self)
{
  g_return_val_if_fail (EMPATHY_IS_TP_CHAT (self), FALSE);

  return self->priv->can_upgrade_to_muc ||
    tp_proxy_has_interface_by_id (self,
      TP_IFACE_QUARK_CHANNEL_INTERFACE_GROUP);;
}

static void
tp_channel_leave_async_cb (GObject *source_object,
        GAsyncResult *res,
        gpointer user_data)
{
  GError *error = NULL;

  if (!tp_channel_leave_finish (TP_CHANNEL (source_object), res, &error))
    {
      DEBUG ("Could not leave channel properly: (%s); closing the channel",
        error->message);
      g_error_free (error);
    }
}

void
empathy_tp_chat_leave (EmpathyTpChat *self,
    const gchar *message)
{
  TpChannel *channel = (TpChannel *) self;

  DEBUG ("Leaving channel %s with message \"%s\"",
    tp_channel_get_identifier (channel), message);

  tp_channel_leave_async (channel, TP_CHANNEL_GROUP_CHANGE_REASON_NONE,
    message, tp_channel_leave_async_cb, self);
}

gboolean
empathy_tp_chat_is_invited (EmpathyTpChat *self,
    TpContact **inviter)
{
  TpContact *self_contact;
  TpChannel *channel = TP_CHANNEL (self);

  if (!tp_proxy_has_interface (self, TP_IFACE_CHANNEL_INTERFACE_GROUP))
    return FALSE;

  self_contact = tp_channel_group_get_self_contact (channel);
  if (self_contact == NULL)
    return FALSE;

  return tp_channel_group_get_local_pending_contact_info (channel,
      self_contact, inviter, NULL, NULL);
}

TpChannelChatState
empathy_tp_chat_get_chat_state (EmpathyTpChat *self,
    EmpathyContact *contact)
{
  return tp_text_channel_get_chat_state ((TpTextChannel *) self,
    empathy_contact_get_tp_contact (contact));
}

EmpathyContact *
empathy_tp_chat_get_self_contact (EmpathyTpChat *self)
{
  g_return_val_if_fail (EMPATHY_IS_TP_CHAT (self), NULL);

  return self->priv->user;
}

GQuark
empathy_tp_chat_get_feature_ready (void)
{
  return g_quark_from_static_string ("empathy-tp-chat-feature-ready");
}

static void
password_feature_prepare_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyTpChat *self = user_data;
  GError *error = NULL;

  if (!tp_proxy_prepare_finish (source, result, &error))
    {
      DEBUG ("Failed to prepare Password: %s", error->message);
      g_error_free (error);
    }

  self->priv->preparing_password = FALSE;

  check_almost_ready (self);
}

static void
continue_preparing (EmpathyTpChat *self)
{
  TpChannel *channel = (TpChannel *) self;
  TpConnection *connection;
  gboolean listen_for_dbus_properties_changed = FALSE;

  connection = tp_channel_get_connection (channel);

  if (tp_proxy_has_interface_by_id (self,
        TP_IFACE_QUARK_CHANNEL_INTERFACE_PASSWORD))
    {
      /* The password feature can't be a hard dep on our own feature has we
       * depend on it only if the channel implements the
       * Password interface.
       */
      GQuark features[] = { TP_CHANNEL_FEATURE_PASSWORD , 0 };

      self->priv->preparing_password = TRUE;

      tp_proxy_prepare_async (self, features, password_feature_prepare_cb,
          self);
    }

  if (tp_proxy_has_interface_by_id (self,
            TP_IFACE_QUARK_CHANNEL_INTERFACE_GROUP))
    {
      GPtrArray *contacts;
      TpContact *contact;

      /* Get self contact from the group's self handle */
      contact = tp_channel_group_get_self_contact (channel);
      create_self_contact (self, contact);

      /* Get initial member contacts */
      contacts = tp_channel_group_dup_members_contacts (channel);
      add_members_contact (self, contacts);
      g_ptr_array_unref (contacts);

      self->priv->can_upgrade_to_muc = FALSE;

      tp_g_signal_connect_object (self, "group-contacts-changed",
        G_CALLBACK (tp_chat_group_contacts_changed_cb), self, 0);
    }
  else
    {
      TpCapabilities *caps;
      GVariant *classes, *class;
      GVariantIter iter;
      TpContact *contact;

      /* Get the self contact from the connection's self handle */
      contact = tp_connection_get_self_contact (connection);
      create_self_contact (self, contact);

      /* Get the remote contact */
      contact = tp_channel_get_target_contact (channel);
      create_remote_contact (self, contact);

      caps = tp_connection_get_capabilities (connection);
      g_assert (caps != NULL);

      classes = tp_capabilities_dup_channel_classes_variant (caps);

      g_variant_iter_init (&iter, classes);
      while ((class = g_variant_iter_next_value (&iter)))
        {
          GVariant *fixed, *allowed;
          const gchar *chan_type = NULL;

          fixed = g_variant_get_child_value (class, 0);
          allowed = g_variant_get_child_value (class, 1);

          g_variant_lookup (fixed, TP_PROP_CHANNEL_CHANNEL_TYPE, "&s",
              &chan_type);
          if (!tp_strdiff (chan_type, TP_IFACE_CHANNEL_TYPE_TEXT))
            {
              const gchar **oprops;

              oprops = g_variant_get_strv (allowed, NULL);

              if (tp_strv_contains (oprops,
                    TP_PROP_CHANNEL_INTERFACE_CONFERENCE_INITIAL_CHANNELS))
                {
                  self->priv->can_upgrade_to_muc = TRUE;
                }

              g_free (oprops);
            }

          g_variant_unref (class);
          g_variant_unref (fixed);
          g_variant_unref (allowed);

          if (self->priv->can_upgrade_to_muc)
            break;
        }

      g_variant_unref (classes);
    }

  if (tp_proxy_has_interface_by_id (self,
            TP_IFACE_QUARK_CHANNEL_INTERFACE_SUBJECT))
    {
      tp_cli_dbus_properties_call_get_all (channel, -1,
                   TP_IFACE_CHANNEL_INTERFACE_SUBJECT,
                   tp_chat_get_all_subject_cb,
                   NULL, NULL,
                   G_OBJECT (self));
      listen_for_dbus_properties_changed = TRUE;
    }

  if (tp_proxy_has_interface_by_id (self,
            TP_IFACE_QUARK_CHANNEL_INTERFACE_ROOM_CONFIG))
    {
      tp_cli_dbus_properties_call_get_all (channel, -1,
                   TP_IFACE_CHANNEL_INTERFACE_ROOM_CONFIG,
                   tp_chat_get_all_room_config_cb,
                   NULL, NULL,
                   G_OBJECT (self));
      listen_for_dbus_properties_changed = TRUE;
    }

  if (listen_for_dbus_properties_changed)
    {
      tp_cli_dbus_properties_connect_to_properties_changed (channel,
                        tp_chat_dbus_properties_changed_cb,
                        NULL, NULL,
                        G_OBJECT (self), NULL);
    }
}

static void
conn_connected_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyTpChat *self = user_data;
  GError *error = NULL;

  if (!tp_proxy_prepare_finish (source, result, &error))
    {
      DEBUG ("Failed to prepare Connected: %s", error->message);
      g_simple_async_result_take_error (self->priv->ready_result, error);
      g_simple_async_result_complete (self->priv->ready_result);
      tp_clear_object (&self->priv->ready_result);
      return;
    }

  continue_preparing (self);
}

static void
tp_chat_prepare_ready_async (TpProxy *proxy,
  const TpProxyFeature *feature,
  GAsyncReadyCallback callback,
  gpointer user_data)
{
  EmpathyTpChat *self = (EmpathyTpChat *) proxy;
  TpChannel *channel = (TpChannel *) proxy;
  TpConnection *connection;
  GQuark features[] = { TP_CONNECTION_FEATURE_CONNECTED, 0 };

  g_assert (self->priv->ready_result == NULL);

  self->priv->ready_result = g_simple_async_result_new (G_OBJECT (self),
    callback, user_data, tp_chat_prepare_ready_async);

  connection = tp_channel_get_connection (channel);

  /* First we have to make sure that TP_CONNECTION_FEATURE_CONNECTED is
   * prepared as we rely on TpConnection::self-contact
   * in continue_preparing().
   *
   * It would be nice if tp-glib could do this for us: fdo#59126 */
  tp_proxy_prepare_async (connection, features, conn_connected_cb, proxy);
}
