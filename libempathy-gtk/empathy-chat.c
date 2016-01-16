/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2002-2007 Imendio AB
 * Copyright (C) 2007-2010 Collabora Ltd.
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301  USA
 *
 * Authors: Mikael Hallendal <micke@imendio.com>
 *          Richard Hult <richard@imendio.com>
 *          Martyn Russell <martyn@imendio.com>
 *          Geert-Jan Van den Bogaerde <geertjan@gnome.org>
 *          Xavier Claessens <xclaesse@gmail.com>
 */

/* for GCompletion */
#define GLIB_DISABLE_DEPRECATION_WARNINGS 1

#include "config.h"
#include "empathy-chat.h"

#include <glib/gi18n-lib.h>
#include <tp-account-widgets/tpaw-keyring.h>
#include <tp-account-widgets/tpaw-builder.h>
#include <tp-account-widgets/tpaw-utils.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-client-factory.h"
#include "empathy-gsettings.h"
#include "empathy-individual-information-dialog.h"
#include "empathy-individual-store-channel.h"
#include "empathy-individual-view.h"
#include "empathy-input-text-view.h"
#include "empathy-request-util.h"
#include "empathy-search-bar.h"
#include "empathy-spell.h"
#include "empathy-string-parser.h"
#include "empathy-theme-manager.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CHAT
#include "empathy-debug.h"

#define IS_ENTER(v) (v == GDK_KEY_Return || v == GDK_KEY_ISO_Enter || v == GDK_KEY_KP_Enter)
#define COMPOSING_STOP_TIMEOUT 5

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyChat)
struct _EmpathyChatPriv {
	EmpathyTpChat     *tp_chat;
	TpAccount         *account;
	gchar             *id;
	gchar             *name;
	gchar             *subject;
	EmpathyContact    *self_contact;
	EmpathyContact    *remote_contact;
	gboolean           show_contacts;

	GSettings         *gsettings_chat;
	GSettings         *gsettings_ui;

	TplLogManager     *log_manager;
	TplLogWalker      *log_walker;
	/* Are we watching for scrolling movements? */
	gboolean           watch_scroll;
	/* Maximum page size of the chat->view. */
	guint              max_page_size;
	/* The offset from the lower edge of the chat->view before it
	 * expanded to fit in the newly fetched logs. This is to
	 * restore the chat->view to the page it was on before the
	 * latest batch of logs were inserted. */
	guint              scroll_offset;

	TpAccountManager  *account_manager;
	GList             *input_history;
	GList             *input_history_current;
	GList             *compositors;
	GCompletion       *completion;
	guint              composing_stop_timeout_id;
	guint              block_events_timeout_id;
	TpHandleType       handle_type;
	gint               contacts_width;
	gboolean           has_input_vscroll;

	/* TRUE if spell checking is enabled, FALSE otherwise.
	 * This is to keep track of the last state of spell checking
	 * when it changes. */
	gboolean	   spell_checking_enabled;

	/* These store the signal handler ids for the enclosed text entry. */
	gulong		   insert_text_id;
	gulong		   delete_range_id;
	gulong		   notify_cursor_position_id;

	/* Source func ID for update_misspelled_words () */
	guint              update_misspelled_words_id;
	/* Source func ID for save_paned_pos_timeout () */
	guint              save_paned_pos_id;
	/* Source func ID for chat_contacts_visible_timeout_cb () */
	guint              contacts_visible_id;

	GtkWidget         *widget;
	GtkWidget         *hpaned;
	GtkWidget         *vbox_left;
	GtkWidget         *scrolled_window_chat;
	GtkWidget         *scrolled_window_input;
	GtkWidget         *scrolled_window_contacts;
	GtkWidget         *hbox_topic;
	GtkWidget         *expander_topic;
	GtkWidget         *label_topic;
	GtkWidget         *contact_list_view;
	GtkWidget         *info_bar_vbox;
	GtkWidget         *search_bar;

	guint              unread_messages;
	guint              unread_messages_when_offline;

	/* FIXME: retrieving_backlogs flag is a workaround for Bug#610994 and should
	 * be differently handled since it introduces another race condition, which
	 * is really hard to occur, but still possible.
	 *
	 * With the current workaround (which has the race above), we need to be
	 * sure to ACK any pending messages only when the retrieval of backlogs is
	 * finished, that's why using retrieving_backlogs flag.
	 * empathy_chat_messages_read () will check this variable and not ACK
	 * anything when TRUE. It will be set TRUE at chat_constructed () and set
	 * back to FALSE when the backlog has been retrieved and the pending
	 * messages actually showed to the user.
	 *
	 * Race condition introduced with this workaround:
	 * Scenario: a message is pending, the user is notified and selects the tab.
	 * the tab with a pending message is focused before the messages are properly
	 * shown (since the preparation of the window is slower AND async WRT the
	 * tab showing), which means the user won't see any new messages (rare but
	 * possible), if he/she will change tab focus before the messages are
	 * properly shown, the tab will be set as 'seen' and the user won't be
	 * notified again about the already notified pending messages when the
	 * messages in tab will be properly shown */
	gboolean           retrieving_backlogs;
	gboolean           sms_channel;

	/* we need to know whether populate-popup happened in response to
	 * the keyboard or the mouse. We can't ask GTK for the most recent
	 * event, because it will be a notify event. Instead we track it here */
	GdkEventType       most_recent_event_type;

	/* A regex matching our own current nickname in the room, or %NULL if
	 * !empathy_chat_is_room (). */
	GRegex            *highlight_regex;

	/* TRUE if empathy_chat_is_room () and there are unread highlighted messages.
	 * Cleared by empathy_chat_messages_read (). */
	gboolean           highlighted;
};

typedef struct {
	gchar *text; /* Original message that was specified
	              * upon entry creation. */
	gchar *modified_text; /* Message that was modified by user.
	                       * When no modifications were made, it is NULL */
} InputHistoryEntry;

enum {
	COMPOSING,
	NEW_MESSAGE,
	PART_COMMAND_ENTERED,
	LAST_SIGNAL
};

enum {
	PROP_0,
	PROP_TP_CHAT,
	PROP_ACCOUNT,
	PROP_ID,
	PROP_NAME,
	PROP_SUBJECT,
	PROP_REMOTE_CONTACT,
	PROP_SHOW_CONTACTS,
	PROP_SMS_CHANNEL,
	PROP_N_MESSAGES_SENDING,
	PROP_NB_UNREAD_MESSAGES,
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (EmpathyChat, empathy_chat, GTK_TYPE_BOX);

static gboolean chat_scrollable_connect (gpointer user_data);
static gboolean update_misspelled_words (gpointer data);

static void
chat_get_property (GObject    *object,
		   guint       param_id,
		   GValue     *value,
		   GParamSpec *pspec)
{
	EmpathyChat *chat = EMPATHY_CHAT (object);
	EmpathyChatPriv *priv = GET_PRIV (object);

	switch (param_id) {
	case PROP_TP_CHAT:
		g_value_set_object (value, priv->tp_chat);
		break;
	case PROP_ACCOUNT:
		g_value_set_object (value, priv->account);
		break;
	case PROP_NAME:
		g_value_take_string (value, empathy_chat_dup_name (chat));
		break;
	case PROP_ID:
		g_value_set_string (value, priv->id);
		break;
	case PROP_SUBJECT:
		g_value_set_string (value, priv->subject);
		break;
	case PROP_REMOTE_CONTACT:
		g_value_set_object (value, priv->remote_contact);
		break;
	case PROP_SHOW_CONTACTS:
		g_value_set_boolean (value, priv->show_contacts);
		break;
	case PROP_SMS_CHANNEL:
		g_value_set_boolean (value, priv->sms_channel);
		break;
	case PROP_N_MESSAGES_SENDING:
		g_value_set_uint (value,
			empathy_chat_get_n_messages_sending (chat));
		break;
	case PROP_NB_UNREAD_MESSAGES:
		g_value_set_uint (value,
			empathy_chat_get_nb_unread_messages (chat));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	};
}

static void
chat_set_property (GObject      *object,
		   guint         param_id,
		   const GValue *value,
		   GParamSpec   *pspec)
{
	EmpathyChat *chat = EMPATHY_CHAT (object);

	switch (param_id) {
	case PROP_TP_CHAT:
		empathy_chat_set_tp_chat (chat, EMPATHY_TP_CHAT (g_value_get_object (value)));
		break;
	case PROP_SHOW_CONTACTS:
		empathy_chat_set_show_contacts (chat, g_value_get_boolean (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	};
}

static void
account_reconnected (EmpathyChat *chat,
			TpAccount *account)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	DEBUG ("Account reconnected, request a new Text channel");

	/* FIXME: Ideally we should ask to handle ourself the channel so we can
	* report the error if any but this is blocked by
	* https://bugs.freedesktop.org/show_bug.cgi?id=13422 */
	switch (priv->handle_type) {
		case TP_HANDLE_TYPE_CONTACT:
			if (priv->sms_channel)
				empathy_sms_contact_id (
					account, priv->id,
					TP_USER_ACTION_TIME_NOT_USER_ACTION,
					NULL, NULL);
			else
				empathy_chat_with_contact_id (
					account, priv->id,
					TP_USER_ACTION_TIME_NOT_USER_ACTION,
					NULL, NULL);
			break;
		case TP_HANDLE_TYPE_ROOM:
			empathy_join_muc (account, priv->id,
				TP_USER_ACTION_TIME_NOT_USER_ACTION);
			break;
		case TP_HANDLE_TYPE_NONE:
		case TP_HANDLE_TYPE_LIST:
		case TP_HANDLE_TYPE_GROUP:
		default:
			g_assert_not_reached ();
			break;
	}

	g_object_unref (chat);
}

static void
chat_new_connection_cb (TpAccount   *account,
			guint        old_status,
			guint        new_status,
			guint        reason,
			gchar       *dbus_error_name,
			GHashTable  *details,
			EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	if (new_status != TP_CONNECTION_STATUS_CONNECTED)
		return;

	if (priv->tp_chat != NULL || account != priv->account ||
	    priv->handle_type == TP_HANDLE_TYPE_NONE ||
	    TPAW_STR_EMPTY (priv->id))
		return;

	g_object_ref (chat);

	account_reconnected (chat, account);
}

static void
chat_composing_remove_timeout (EmpathyChat *chat)
{
	EmpathyChatPriv *priv;

	priv = GET_PRIV (chat);

	if (priv->composing_stop_timeout_id) {
		g_source_remove (priv->composing_stop_timeout_id);
		priv->composing_stop_timeout_id = 0;
	}
}

static void
set_chate_state_cb (GObject *source,
		    GAsyncResult *result,
		    gpointer user_data)
{
	GError *error = NULL;

	if (!tp_text_channel_set_chat_state_finish (TP_TEXT_CHANNEL (source), result,
		&error)) {
		DEBUG ("Failed to set chat state: %s", error->message);
		g_error_free (error);
	}
}

static void
set_chat_state (EmpathyChat *self,
		TpChannelChatState state)
{
	EmpathyChatPriv *priv = GET_PRIV (self);

	if (!tp_proxy_has_interface_by_id (priv->tp_chat,
		TP_IFACE_QUARK_CHANNEL_INTERFACE_CHAT_STATE))
		return;

	tp_text_channel_set_chat_state_async (TP_TEXT_CHANNEL (priv->tp_chat), state,
		set_chate_state_cb, self);
}

static gboolean
chat_composing_stop_timeout_cb (EmpathyChat *chat)
{
	EmpathyChatPriv *priv;
	gboolean send_chat_states;

	priv = GET_PRIV (chat);

	priv->composing_stop_timeout_id = 0;
	send_chat_states = g_settings_get_boolean (priv->gsettings_chat,
	                                 EMPATHY_PREFS_CHAT_SEND_CHAT_STATES);
	if (!send_chat_states) {
		set_chat_state (chat, TP_CHANNEL_CHAT_STATE_ACTIVE);
	} else {
		set_chat_state (chat, TP_CHANNEL_CHAT_STATE_PAUSED);
	}

	return FALSE;
}

static void
chat_composing_start (EmpathyChat *chat)
{
	EmpathyChatPriv *priv;
	gboolean send_chat_states;

	priv = GET_PRIV (chat);

	send_chat_states = g_settings_get_boolean (priv->gsettings_chat,
	                                  EMPATHY_PREFS_CHAT_SEND_CHAT_STATES);
	if (!send_chat_states) {
		return;
	}

	if (priv->composing_stop_timeout_id) {
		/* Just restart the timeout */
		chat_composing_remove_timeout (chat);
	} else {
		set_chat_state (chat, TP_CHANNEL_CHAT_STATE_COMPOSING);
	}

	priv->composing_stop_timeout_id = g_timeout_add_seconds (
		COMPOSING_STOP_TIMEOUT,
		(GSourceFunc) chat_composing_stop_timeout_cb,
		chat);
}

static void
chat_composing_stop (EmpathyChat *chat)
{
	chat_composing_remove_timeout (chat);
	set_chat_state (chat, TP_CHANNEL_CHAT_STATE_ACTIVE);
}

static gint
chat_input_history_entry_cmp (InputHistoryEntry *entry,
                              const gchar *text)
{
	if (!tp_strdiff (entry->text, text)) {
		if (entry->modified_text != NULL) {
			/* Modified entry and single string cannot be equal. */
			return 1;
		}
		return 0;
	}
	return 1;
}

static InputHistoryEntry *
chat_input_history_entry_new_with_text (const gchar *text)
{
	InputHistoryEntry *entry;
	entry = g_slice_new0 (InputHistoryEntry);
	entry->text = g_strdup (text);

	return entry;
}

static void
chat_input_history_entry_free (InputHistoryEntry *entry)
{
	g_free (entry->text);
	g_free (entry->modified_text);
	g_slice_free (InputHistoryEntry, entry);
}

static void
chat_input_history_entry_revert (InputHistoryEntry *entry)
{
	g_free (entry->modified_text);
	entry->modified_text = NULL;
}

static void
chat_input_history_entry_update_text (InputHistoryEntry *entry,
                                      const gchar *text)
{
	gchar *old;

	if (!tp_strdiff (text, entry->text)) {
		g_free (entry->modified_text);
		entry->modified_text = NULL;
		return;
	}

	old = entry->modified_text;
	entry->modified_text = g_strdup (text);
	g_free (old);
}

static const gchar *
chat_input_history_entry_get_text (InputHistoryEntry *entry)
{
	if (entry == NULL) {
		return NULL;
	}

	if (entry->modified_text != NULL) {
		return entry->modified_text;
	}
	return entry->text;
}

static GList *
chat_input_history_remove_item (GList *list,
                                GList *item)
{
	list = g_list_remove_link (list, item);
	chat_input_history_entry_free (item->data);
	g_list_free_1 (item);
	return list;
}

static void
chat_input_history_revert (EmpathyChat *chat)
{
	EmpathyChatPriv   *priv;
	GList             *list;
	GList             *item1;
	GList             *item2;
	InputHistoryEntry *entry;

	priv = GET_PRIV (chat);
	list = priv->input_history;

	if (list == NULL) {
		DEBUG ("No input history");
		return;
	}

	/* Delete temporary entry */
	if (priv->input_history_current != NULL) {
		item1 = list;
		list = chat_input_history_remove_item (list, item1);
		if (priv->input_history_current == item1) {
			/* Removed temporary entry was current entry */
			priv->input_history = list;
			priv->input_history_current = NULL;
			return;
		}
	}
	else {
		/* There is no entry to revert */
		return;
	}

	/* Restore the current history entry to original value */
	item1 = priv->input_history_current;
	entry = item1->data;
	chat_input_history_entry_revert (entry);

	/* Remove restored entry if there is other occurance before this entry */
	item2 = g_list_find_custom (list, chat_input_history_entry_get_text (entry),
	                            (GCompareFunc) chat_input_history_entry_cmp);
	if (item2 != item1) {
		list = chat_input_history_remove_item (list, item1);
	}
	else {
		/* Remove other occurance of the restored entry */
		item2 = g_list_find_custom (item1->next,
		                            chat_input_history_entry_get_text (entry),
		                            (GCompareFunc) chat_input_history_entry_cmp);
		if (item2 != NULL) {
			list = chat_input_history_remove_item (list, item2);
		}
	}

	priv->input_history_current = NULL;
	priv->input_history = list;
}

static void
chat_input_history_add (EmpathyChat  *chat,
                        const gchar *str,
                        gboolean temporary)
{
	EmpathyChatPriv   *priv;
	GList             *list;
	GList             *item;
	InputHistoryEntry *entry;

	priv = GET_PRIV (chat);

	list = priv->input_history;

	/* Remove any other occurances of this entry, if not temporary */
	if (!temporary) {
		while ((item = g_list_find_custom (list, str,
		    (GCompareFunc) chat_input_history_entry_cmp)) != NULL) {
			list = chat_input_history_remove_item (list, item);
		}

		/* Trim the list to the last 10 items */
		while (g_list_length (list) > 10) {
			item = g_list_last (list);
			if (item != NULL) {
				list = chat_input_history_remove_item (list, item);
			}
		}
	}



	/* Add new entry */
	entry = chat_input_history_entry_new_with_text (str);
	list = g_list_prepend (list, entry);

	/* Set the list and the current item pointer */
	priv->input_history = list;
	if (temporary) {
		priv->input_history_current = list;
	}
	else {
		priv->input_history_current = NULL;
	}
}

static const gchar *
chat_input_history_get_next (EmpathyChat *chat)
{
	EmpathyChatPriv *priv;
	GList           *item;
	const gchar     *msg;

	priv = GET_PRIV (chat);

	if (priv->input_history == NULL) {
		DEBUG ("No input history, next entry is NULL");
		return NULL;
	}
	g_assert (priv->input_history_current != NULL);

	if ((item = g_list_next (priv->input_history_current)) == NULL)
	{
		item = priv->input_history_current;
	}

	msg = chat_input_history_entry_get_text (item->data);

	DEBUG ("Returning next entry: '%s'", msg);

	priv->input_history_current = item;

	return msg;
}

static const gchar *
chat_input_history_get_prev (EmpathyChat *chat)
{
	EmpathyChatPriv *priv;
	GList           *item;
	const gchar     *msg;

	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), NULL);

	priv = GET_PRIV (chat);

	if (priv->input_history == NULL) {
		DEBUG ("No input history, previous entry is NULL");
		return NULL;
	}

	if (priv->input_history_current == NULL)
	{
		return NULL;
	}
	else if ((item = g_list_previous (priv->input_history_current)) == NULL)
	{
		item = priv->input_history_current;
	}

	msg = chat_input_history_entry_get_text (item->data);

	DEBUG ("Returning previous entry: '%s'", msg);

	priv->input_history_current = item;

	return msg;
}

static void
chat_input_history_update (EmpathyChat *chat,
                           GtkTextBuffer *buffer)
{
	EmpathyChatPriv      *priv;
	GtkTextIter           start, end;
	gchar                *text;
	InputHistoryEntry    *entry;

	priv = GET_PRIV (chat);

	gtk_text_buffer_get_bounds (buffer, &start, &end);
	text = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);

	if (priv->input_history_current == NULL) {
		/* Add the current text temporarily to the history */
		chat_input_history_add (chat, text, TRUE);
		g_free (text);
		return;
	}

	/* Save the changes in the history */
	entry = priv->input_history_current->data;
	if (tp_strdiff (chat_input_history_entry_get_text (entry), text)) {
		chat_input_history_entry_update_text (entry, text);
	}

	g_free (text);
}

typedef struct {
	EmpathyChat *chat;
	gchar *message;
} ChatCommandMsgData;

static void
chat_command_msg_cb (GObject *source,
			      GAsyncResult *result,
			      gpointer                  user_data)
{
	ChatCommandMsgData *data = user_data;
	GError *error = NULL;
	TpChannel *channel;

	channel = tp_account_channel_request_ensure_and_observe_channel_finish (
					TP_ACCOUNT_CHANNEL_REQUEST (source), result, &error);

	if (channel == NULL) {
		DEBUG ("Failed to get channel: %s", error->message);
		g_error_free (error);

		empathy_theme_adium_append_event (data->chat->view,
			_("Failed to open private chat"));
		goto OUT;
	}

	if (!TPAW_STR_EMPTY (data->message) && TP_IS_TEXT_CHANNEL (channel)) {
		TpTextChannel *text = (TpTextChannel *) channel;
		TpMessage *msg;

		msg = tp_client_message_new_text (TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL,
			data->message);

		tp_text_channel_send_message_async (text, msg, 0, NULL, NULL);

		g_object_unref (msg);
	}

	g_object_unref (channel);

OUT:
	g_free (data->message);
	g_slice_free (ChatCommandMsgData, data);
}

static gboolean
nick_command_supported (EmpathyChat *chat)
{
	EmpathyChatPriv * priv = GET_PRIV (chat);
	TpConnection    *connection;

	connection = tp_channel_get_connection (TP_CHANNEL (priv->tp_chat));
	return tp_proxy_has_interface_by_id (connection,
		TP_IFACE_QUARK_CONNECTION_INTERFACE_RENAMING);
}

static gboolean
part_command_supported (EmpathyChat *chat)
{
	EmpathyChatPriv * priv = GET_PRIV (chat);

	return tp_proxy_has_interface_by_id (priv->tp_chat,
			TP_IFACE_QUARK_CHANNEL_INTERFACE_GROUP);
}

static void
chat_command_clear (EmpathyChat *chat,
		    GStrv        strv)
{
	empathy_theme_adium_clear (chat->view);
}

static void
chat_command_topic (EmpathyChat *chat,
		    GStrv        strv)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	if (!empathy_tp_chat_supports_subject (priv->tp_chat)) {
		empathy_theme_adium_append_event (chat->view,
			_("Topic not supported on this conversation"));
		return;
	}

	if (!empathy_tp_chat_can_set_subject (priv->tp_chat)) {
		empathy_theme_adium_append_event (chat->view,
			_("You are not allowed to change the topic"));
		return;
	}

	empathy_tp_chat_set_subject (priv->tp_chat, strv[1]);
}

void
empathy_chat_join_muc (EmpathyChat *chat,
		   const gchar   *room)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	empathy_join_muc (priv->account, room,
		empathy_get_current_action_time ());
}

static void
chat_command_join (EmpathyChat *chat,
		   GStrv        strv)
{
	guint i = 0;

	GStrv rooms = g_strsplit_set (strv[1], ", ", -1);

	/* FIXME: Ideally we should ask to handle ourself the channel so we can
	* report the error if any but this is blocked by
	* https://bugs.freedesktop.org/show_bug.cgi?id=13422 */
	while (rooms[i] != NULL) {
		/* ignore empty strings */
		if (!TPAW_STR_EMPTY (rooms[i])) {
			empathy_chat_join_muc (chat, rooms[i]);
		}
		i++;
	}
	g_strfreev (rooms);
}

static void
chat_command_part (EmpathyChat *chat,
			   GStrv        strv)
{
	g_signal_emit (chat, signals[PART_COMMAND_ENTERED], 0, strv);
}

static void
chat_command_msg_internal (EmpathyChat *chat,
			   const gchar *contact_id,
			   const gchar *message)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	ChatCommandMsgData *data;
	TpAccountChannelRequest *req;

	req = tp_account_channel_request_new_text (priv->account,
		empathy_get_current_action_time ());

	tp_account_channel_request_set_target_id (req, TP_HANDLE_TYPE_CONTACT,
		contact_id);

	/* FIXME: We should probably search in members alias. But this
	 * is enough for IRC */
	data = g_slice_new (ChatCommandMsgData);
	data->chat = chat;
	data->message = g_strdup (message);

	tp_account_channel_request_ensure_and_observe_channel_async (req,
		EMPATHY_CHAT_TP_BUS_NAME, NULL, chat_command_msg_cb, data);

	g_object_unref (req);
}

static void
chat_command_query (EmpathyChat *chat,
		    GStrv        strv)
{
	/* If <message> part is not defined,
	 * strv[2] will be the terminal NULL */
	chat_command_msg_internal (chat, strv[1], strv[2]);
}

static void
chat_command_msg (EmpathyChat *chat,
		  GStrv        strv)
{
	chat_command_msg_internal (chat, strv[1], strv[2]);
}

static void
callback_for_request_rename (TpConnection *conn,
		  const GError *error,
		  gpointer user_data,
		  GObject *weak_object)
{
	if (error != NULL) {
		DEBUG ("Call to RequestRename method failed: %s",error->message);
	}
}

static void
chat_command_nick (EmpathyChat *chat,
		   GStrv        strv)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	TpConnection *conn;

	conn = tp_account_get_connection (priv->account);

	tp_cli_connection_interface_renaming_call_request_rename (conn, -1,
		strv[1], callback_for_request_rename, NULL, NULL, NULL);
}

static void
chat_command_me (EmpathyChat *chat,
		  GStrv        strv)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	TpMessage *message;
	TpTextChannel *channel;

	channel = (TpTextChannel *) (priv->tp_chat);

	if (!tp_text_channel_supports_message_type (channel,
			TP_CHANNEL_TEXT_MESSAGE_TYPE_ACTION)) {
		/* Action message are not supported, 'simulate' the action */
		gchar *tmp;

		/* The TpChat can't be ready if it doesn't have the self contact */
		g_assert (priv->self_contact != NULL);

		tmp = g_strdup_printf ("%s %s", empathy_contact_get_alias (priv->self_contact),
			strv[1]);
		message = tp_client_message_new_text (TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL,
			tmp);
		g_free (tmp);
	}
	else {
		message = tp_client_message_new_text (TP_CHANNEL_TEXT_MESSAGE_TYPE_ACTION,
			strv[1]);
	}

	empathy_tp_chat_send (priv->tp_chat, message);
	g_object_unref (message);
}

static void
chat_command_say (EmpathyChat *chat,
		  GStrv        strv)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	TpMessage *message;

	message = tp_client_message_new_text (TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL,
		strv[1]);
	empathy_tp_chat_send (priv->tp_chat, message);
	g_object_unref (message);
}

static void
whois_got_contact_cb (GObject *source,
		      GAsyncResult *result,
		      gpointer user_data)
{
	EmpathyChat *chat = user_data;
	EmpathyContact *contact;
	FolksIndividual *individual;

	contact = empathy_client_factory_dup_contact_by_id_finish (
		EMPATHY_CLIENT_FACTORY (source), result, NULL);

	if (contact == NULL) {
		empathy_theme_adium_append_event (chat->view, _("Invalid contact ID"));
		goto out;
	}

	individual = empathy_ensure_individual_from_tp_contact (
		empathy_contact_get_tp_contact (contact));
	empathy_display_individual_info (individual);

	g_object_unref (individual);
	g_object_unref (contact);

out:
	g_object_unref (chat);
}

static void
chat_command_whois (EmpathyChat *chat,
		    GStrv strv)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	TpConnection *conn;
	EmpathyClientFactory *factory;

	conn = tp_channel_get_connection ((TpChannel *) priv->tp_chat);
	factory = empathy_client_factory_dup ();

	/* Element 0 of 'strv' is "whois"; element 1 is the contact ID
	 * entered by the user (including spaces, if any). */
	empathy_client_factory_dup_contact_by_id_async (factory, conn, strv[1],
		whois_got_contact_cb, g_object_ref (chat));

	g_object_unref (factory);
}

static void
chat_command_whale (EmpathyChat *chat,
		    GStrv        strv)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	TpMessage *message;

	message = tp_client_message_new_text (TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL,
		"\n\n\n"
		"•_______________•");
	empathy_tp_chat_send (priv->tp_chat, message);
	g_object_unref (message);
}

static void
chat_command_babywhale (EmpathyChat *chat,
			GStrv        strv)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	TpMessage *message;

	message = tp_client_message_new_text (TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL,
		"\n"
		"•_____•");
	empathy_tp_chat_send (priv->tp_chat, message);
	g_object_unref (message);
}

static void
chat_command_inspector (EmpathyChat *chat,
		    GStrv        strv)
{
	if (EMPATHY_IS_THEME_ADIUM (chat->view)) {
		empathy_theme_adium_show_inspector (
			EMPATHY_THEME_ADIUM (chat->view));
	}
}

static void chat_command_help (EmpathyChat *chat, GStrv strv);

typedef void (*ChatCommandFunc) (EmpathyChat *chat, GStrv strv);

typedef struct {
	const gchar *prefix;
	guint min_parts;
	guint max_parts;
	ChatCommandFunc func;
	gboolean (*is_supported)(EmpathyChat *chat);
	const gchar *help;
} ChatCommandItem;

static ChatCommandItem commands[] = {
	{"clear", 1, 1, chat_command_clear, NULL,
	 N_("/clear: clear all messages from the current conversation")},

	{"topic", 2, 2, chat_command_topic, NULL,
	 N_("/topic <topic>: set the topic of the current conversation")},

	{"join", 2, 2, chat_command_join, NULL,
	 N_("/join <chat room ID>: join a new chat room")},

	{"j", 2, 2, chat_command_join, NULL,
	 N_("/j <chat room ID>: join a new chat room")},

	/* FIXME: https://bugzilla.gnome.org/show_bug.cgi?id=643295 */
	{"part", 1, 3, chat_command_part, part_command_supported,
	 N_("/part [<chat room ID>] [<reason>]: leave the chat room, "
	    "by default the current one")},

	{"query", 2, 3, chat_command_query, NULL,
	 N_("/query <contact ID> [<message>]: open a private chat")},

	{"msg", 3, 3, chat_command_msg, NULL,
	 N_("/msg <contact ID> <message>: open a private chat")},

	{"nick", 2, 2, chat_command_nick, nick_command_supported,
	 N_("/nick <nickname>: change your nickname on the current server")},

	{"me", 2, 2, chat_command_me, NULL,
	 N_("/me <message>: send an ACTION message to the current conversation")},

	{"say", 2, 2, chat_command_say, NULL,
	 N_("/say <message>: send <message> to the current conversation. "
	    "This is used to send a message starting with a '/'. For example: "
	    "\"/say /join is used to join a new chat room\"")},

	{"whois", 2, 2, chat_command_whois, NULL,
	 N_("/whois <contact ID>: display information about a contact")},

	{"help", 1, 2, chat_command_help, NULL,
	 N_("/help [<command>]: show all supported commands. "
	    "If <command> is defined, show its usage.")},

	{"inspector", 1, 1, chat_command_inspector, NULL, NULL},

	{"whale", 1, 1, chat_command_whale, NULL, NULL},
	{"babywhale", 1, 1, chat_command_babywhale, NULL, NULL},
};

static void
chat_command_show_help (EmpathyChat     *chat,
			ChatCommandItem *item)
{
	gchar *str;

	if (item->help == NULL) {
		return;
	}

	str = g_strdup_printf (_("Usage: %s"), _(item->help));
	empathy_theme_adium_append_event (chat->view, str);
	g_free (str);
}

static void
chat_command_help (EmpathyChat *chat,
		   GStrv        strv)
{
	guint i;

	/* If <command> part is not defined,
	 * strv[1] will be the terminal NULL */
	if (strv[1] == NULL) {
		for (i = 0; i < G_N_ELEMENTS (commands); i++) {
			if (commands[i].is_supported != NULL) {
				if (!commands[i].is_supported (chat)) {
					continue;
				}
			}
			if (commands[i].help == NULL) {
				continue;
			}
			empathy_theme_adium_append_event (chat->view,
				_(commands[i].help));
		}
		return;
	}

	for (i = 0; i < G_N_ELEMENTS (commands); i++) {
		if (g_ascii_strcasecmp (strv[1], commands[i].prefix) == 0) {
			if (commands[i].is_supported != NULL) {
				if (!commands[i].is_supported (chat)) {
					break;
				}
			}
			if (commands[i].help == NULL) {
				break;
			}
			chat_command_show_help (chat, &commands[i]);
			return;
		}
	}

	empathy_theme_adium_append_event (chat->view,
		_("Unknown command"));
}

static GStrv
chat_command_parse (const gchar *text, guint max_parts)
{
	GPtrArray *array;
	gchar *item;

	DEBUG ("Parse command, parts=%d text=\"%s\":", max_parts, text);

	array = g_ptr_array_sized_new (max_parts + 1);
	while (max_parts > 1) {
		const gchar *end;

		/* Skip white spaces */
		while (g_ascii_isspace (*text)) {
			text++;
		}

		/* Search the end of this part, until first space. */
		for (end = text; *end != '\0' && !g_ascii_isspace (*end); end++)
			/* Do nothing */;
		if (*end == '\0') {
			break;
		}

		item = g_strndup (text, end - text);
		g_ptr_array_add (array, item);
		DEBUG ("\tITEM: \"%s\"", item);

		text = end;
		max_parts--;
	}

	/* Append last part if not empty */
	item = g_strstrip (g_strdup (text));
	if (!TPAW_STR_EMPTY (item)) {
		g_ptr_array_add (array, item);
		DEBUG ("\tITEM: \"%s\"", item);
	} else {
		g_free (item);
	}

	/* Make the array NULL-terminated */
	g_ptr_array_add (array, NULL);

	return (GStrv) g_ptr_array_free (array, FALSE);
}

static gboolean
has_prefix_case (const gchar *s,
		  const gchar *prefix)
{
	return g_ascii_strncasecmp (s, prefix, strlen (prefix)) == 0;
}

static void
chat_send (EmpathyChat  *chat,
	   const gchar *msg)
{
	EmpathyChatPriv *priv;
	TpMessage  *message;
	guint            i;

	if (TPAW_STR_EMPTY (msg)) {
		return;
	}

	priv = GET_PRIV (chat);

	chat_input_history_add (chat, msg, FALSE);

	if (msg[0] == '/') {
		gboolean second_slash = FALSE;
		const gchar *iter = msg + 1;

		for (i = 0; i < G_N_ELEMENTS (commands); i++) {
			GStrv strv;
			guint strv_len;
			gchar c;

			if (!has_prefix_case (msg + 1, commands[i].prefix)) {
				continue;
			}
			c = *(msg + 1 + strlen (commands[i].prefix));
			if (c != '\0' && !g_ascii_isspace (c)) {
				continue;
			}
			if (commands[i].is_supported != NULL) {
				if (!commands[i].is_supported (chat)) {
					continue;
				}
			}

			/* We can't use g_strsplit here because it does
			 * not deal correctly if we have more than one space
			 * between args */
			strv = chat_command_parse (msg + 1, commands[i].max_parts);

			strv_len = g_strv_length (strv);
			if (strv_len < commands[i].min_parts ||
			    strv_len > commands[i].max_parts) {
				chat_command_show_help (chat, &commands[i]);
				g_strfreev (strv);
				return;
			}

			commands[i].func (chat, strv);
			g_strfreev (strv);
			return;
		}

		/* Also allow messages with two slashes before the
		 * first space, so it is possible to send a /unix/path.
		 * This heuristic is kind of crap. */
		while (*iter != '\0' && !g_ascii_isspace (*iter)) {
			if (*iter == '/') {
				second_slash = TRUE;
				break;
			}
			iter++;
		}

		if (!second_slash) {
			empathy_theme_adium_append_event (chat->view,
				_("Unknown command; see /help for the available"
				  " commands"));
			return;
		}
	}

	message = tp_client_message_new_text (TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL,
		msg);
	empathy_tp_chat_send (priv->tp_chat, message);
	g_object_unref (message);
}

static void
chat_input_text_view_send (EmpathyChat *chat)
{
	GtkTextBuffer  *buffer;
	GtkTextIter     start, end;
	gchar	       *msg;

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));

	gtk_text_buffer_get_bounds (buffer, &start, &end);
	msg = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);

	/* clear the input field */
	gtk_text_buffer_set_text (buffer, "", -1);
	/* delete input history modifications */
	chat_input_history_revert (chat);

	chat_send (chat, msg);
	g_free (msg);
}

static void
chat_state_changed_cb (EmpathyTpChat      *tp_chat,
		       TpContact     *tp_contact,
		       TpChannelChatState  state,
		       EmpathyChat        *chat)
{
	EmpathyChatPriv *priv;
	GList          *l;
	gboolean        was_composing;
	EmpathyContact *contact;

	priv = GET_PRIV (chat);

	contact = empathy_contact_dup_from_tp_contact (tp_contact);

	if (empathy_contact_is_user (contact)) {
		/* We don't care about our own chat state */
		goto out;
	}

	was_composing = (priv->compositors != NULL);

	/* Find the contact in the list. After that l is the list elem or NULL */
	for (l = priv->compositors; l; l = l->next) {
		if (contact == l->data) {
			break;
		}
	}

	switch (state) {
	case TP_CHANNEL_CHAT_STATE_GONE:
	case TP_CHANNEL_CHAT_STATE_INACTIVE:
	case TP_CHANNEL_CHAT_STATE_PAUSED:
	case TP_CHANNEL_CHAT_STATE_ACTIVE:
		/* Contact is not composing */
		if (l) {
			priv->compositors = g_list_remove_link (priv->compositors, l);
			g_object_unref (l->data);
			g_list_free1 (l);
		}
		break;
	case TP_CHANNEL_CHAT_STATE_COMPOSING:
		/* Contact is composing */
		if (!l) {
			priv->compositors = g_list_prepend (priv->compositors,
							    g_object_ref (contact));
		}
		break;
	default:
		g_assert_not_reached ();
	}

	DEBUG ("Was composing: %s now composing: %s",
		was_composing ? "yes" : "no",
		priv->compositors ? "yes" : "no");

	if ((was_composing && !priv->compositors) ||
	    (!was_composing && priv->compositors)) {
		/* Composing state changed */
		g_signal_emit (chat, signals[COMPOSING], 0,
			       priv->compositors != NULL);
	}

out:
	g_object_unref (contact);
}

static GRegex *
get_highlight_regex_for (const gchar *name)
{
	GRegex *regex;
	gchar *name_esc, *pattern;
	GError *error = NULL;

	name_esc = g_regex_escape_string (name, -1);
	pattern = g_strdup_printf ("\\b%s\\b", name_esc);
	regex = g_regex_new (pattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0,
		&error);

	if (regex == NULL) {
		DEBUG ("couldn't compile regex /%s/: %s", pattern,
			error->message);

		g_error_free (error);
	}

	g_free (pattern);
	g_free (name_esc);

	return regex;
}

/* Called when priv->self_contact changes, or priv->self_contact:alias changes.
 * Only connected if empathy_chat_is_room() is TRUE, for obvious-ish reasons.
 */
static void
chat_self_contact_alias_changed_cb (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	tp_clear_pointer (&priv->highlight_regex, g_regex_unref);

	if (priv->self_contact != NULL) {
		const gchar *alias = empathy_contact_get_alias (priv->self_contact);

		g_return_if_fail (alias != NULL);
		priv->highlight_regex = get_highlight_regex_for (alias);
	}
}

static gboolean
chat_should_highlight (EmpathyChat *chat,
	EmpathyMessage *message)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	const gchar   *msg;

	g_return_val_if_fail (EMPATHY_IS_MESSAGE (message), FALSE);

	if (!empathy_chat_is_room (chat)) {
		return FALSE;
	}

	if (!empathy_message_is_incoming (message)) {
		return FALSE;
	}

	msg = empathy_message_get_body (message);
	if (!msg) {
		return FALSE;
	}

	if (empathy_message_is_backlog (message)) {
		/* FIXME: Ideally we shouldn't highlight scrollback messages only if they
		 * have already been received by the user before (and so are in the logs) */
		return FALSE;
	}

	if (priv->highlight_regex == NULL) {
		return FALSE;
	}

	return g_regex_match (priv->highlight_regex, msg, 0, NULL);
}

static void
chat_message_received (EmpathyChat *chat,
	EmpathyMessage *message,
	gboolean pending)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	EmpathyContact  *sender;

	sender = empathy_message_get_sender (message);

	if (empathy_message_is_edit (message)) {
		DEBUG ("Editing message '%s' to '%s'",
			empathy_message_get_supersedes (message),
			empathy_message_get_body (message));

		empathy_theme_adium_edit_message (chat->view, message);
	} else {
		gboolean should_highlight = chat_should_highlight (chat, message);

		if (should_highlight) {
			priv->highlighted = TRUE;
		}

		DEBUG ("Appending new message '%s' from %s (%d)",
			empathy_message_get_token (message),
			empathy_contact_get_alias (sender),
			empathy_contact_get_handle (sender));

		empathy_theme_adium_append_message (chat->view, message, should_highlight);

		if (empathy_message_is_incoming (message)) {
			priv->unread_messages++;
			g_object_notify (G_OBJECT (chat), "nb-unread-messages");
		}

		g_signal_emit (chat, signals[NEW_MESSAGE], 0, message, pending,
			       should_highlight);
	}

	/* We received a message so the contact is no longer
	 * composing */
	chat_state_changed_cb (priv->tp_chat, empathy_contact_get_tp_contact (sender),
			       TP_CHANNEL_CHAT_STATE_ACTIVE,
			       chat);
}

static void
chat_message_received_cb (EmpathyTpChat  *tp_chat,
			  EmpathyMessage *message,
			  EmpathyChat    *chat)
{
	chat_message_received (chat, message, FALSE);
}

static void
chat_message_acknowledged_cb (EmpathyTpChat  *tp_chat,
			      EmpathyMessage *message,
			      EmpathyChat    *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	empathy_theme_adium_message_acknowledged (chat->view,
	    message);

	if (!empathy_message_is_edit (message)) {
		priv->unread_messages--;
		g_object_notify (G_OBJECT (chat), "nb-unread-messages");
	}
}

static void
append_balance_error (EmpathyChat *chat,
		      const gchar *message_body)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	TpConnection *conn = tp_channel_get_connection (TP_CHANNEL (priv->tp_chat));
	const gchar *uri = tp_connection_get_balance_uri (conn);
	const gchar *error = _("insufficient balance to send message");
	gchar *str, *str_markup = NULL;

	if (message_body != NULL) {
		str = g_strdup_printf (_("Error sending message '%s': %s"), message_body, error);
	} else {
		str = g_strdup_printf (_("Error sending message: %s"), error);
	}

	if (!tp_str_empty (uri)) {
		/* translators: error used when user doesn't have enough credit on his
		 * account to send the message. */
		gchar *markup_error = g_strdup_printf (_("insufficient balance to send message."
							 " <a href='%s'>Top up</a>."), uri);

		if (message_body != NULL) {
			gchar *escaped_body = g_markup_escape_text (message_body, -1);

			str_markup = g_strdup_printf (_("Error sending message '%s': %s"),
				escaped_body, markup_error);

			g_free (escaped_body);
		} else {
			str_markup = g_strdup_printf (_("Error sending message: %s"), markup_error);
		}

		g_free (markup_error);
	}

	if (str_markup != NULL)
		empathy_theme_adium_append_event_markup (chat->view, str_markup, str);
	else
		empathy_theme_adium_append_event (chat->view, str);

	g_free (str);
	g_free (str_markup);
}

static void
chat_send_error_cb (EmpathyTpChat          *tp_chat,
		    const gchar            *message_body,
		    TpChannelTextSendError  error_code,
		    const gchar            *dbus_error,
		    EmpathyChat            *chat)
{
	const gchar *error = NULL;
	gchar       *str;

	if (!tp_strdiff (dbus_error, TP_ERROR_STR_INSUFFICIENT_BALANCE)) {
		append_balance_error (chat, message_body);
		return;
	} else if (!tp_strdiff (dbus_error, TP_ERROR_STR_NOT_CAPABLE)) {
		error = _("not capable");
	}

	if (error == NULL) {
		/* if we didn't find a dbus-error, try the old error */
		switch (error_code) {
		case TP_CHANNEL_TEXT_SEND_ERROR_OFFLINE:
			error = _("offline");
			break;
		case TP_CHANNEL_TEXT_SEND_ERROR_INVALID_CONTACT:
			error = _("invalid contact");
			break;
		case TP_CHANNEL_TEXT_SEND_ERROR_PERMISSION_DENIED:
			error = _("permission denied");
			break;
		case TP_CHANNEL_TEXT_SEND_ERROR_TOO_LONG:
			error = _("too long message");
			break;
		case TP_CHANNEL_TEXT_SEND_ERROR_NOT_IMPLEMENTED:
			error = _("not implemented");
			break;
		case TP_CHANNEL_TEXT_SEND_ERROR_UNKNOWN:
		default:
			error = _("unknown");
			break;
		}
	}

	if (message_body != NULL) {
			str = g_strdup_printf (_("Error sending message '%s': %s"),
				message_body, error);
	}
	else {
			str = g_strdup_printf (_("Error sending message: %s"), error);
	}

	empathy_theme_adium_append_event (chat->view, str);
	g_free (str);
}

static void
chat_topic_label_size_allocate_cb (GtkLabel *label,
				   GtkAllocation *allocation,
				   EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	if (!gtk_label_get_line_wrap (label)) {
		if (pango_layout_is_ellipsized (gtk_label_get_layout (label)))
			gtk_widget_show (priv->expander_topic);
		else
			gtk_widget_hide (priv->expander_topic);

		return;
	}
}

static void
chat_topic_expander_activate_cb (GtkExpander *expander,
				 GParamSpec *param_spec,
				 EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	if (gtk_expander_get_expanded (expander)) {
		gtk_label_set_ellipsize (GTK_LABEL (priv->label_topic), PANGO_ELLIPSIZE_NONE);
		gtk_label_set_line_wrap (GTK_LABEL (priv->label_topic), TRUE);
	} else {
		gtk_label_set_ellipsize (GTK_LABEL (priv->label_topic), PANGO_ELLIPSIZE_END);
		gtk_label_set_line_wrap (GTK_LABEL (priv->label_topic), FALSE);
	}
}

static void
chat_subject_changed_cb (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

		g_free (priv->subject);
		priv->subject = g_strdup (empathy_tp_chat_get_subject (priv->tp_chat));
		g_object_notify (G_OBJECT (chat), "subject");

		if (TPAW_STR_EMPTY (priv->subject)) {
			gtk_widget_hide (priv->hbox_topic);
		} else {
			gchar *markup_topic;
			gchar *markup_text;

			markup_topic = tpaw_add_link_markup (priv->subject);
			markup_text = g_strdup_printf ("<span weight=\"bold\">%s</span> %s",
				_("Topic:"), markup_topic);

			gtk_label_set_markup (GTK_LABEL (priv->label_topic), markup_text);
			g_free (markup_text);
			g_free (markup_topic);

			gtk_widget_show (priv->hbox_topic);
		}
		if (priv->block_events_timeout_id == 0) {
			gchar *str = NULL;

			if (!TPAW_STR_EMPTY (priv->subject)) {
				const gchar *actor = empathy_tp_chat_get_subject_actor (priv->tp_chat);

				if (tp_str_empty (actor)) {
					str = g_strdup_printf (_("Topic set to: %s"), priv->subject);
				} else {
					str = g_strdup_printf (_("Topic set by %s to: %s"),
							       actor, priv->subject);
				}
			} else if (empathy_tp_chat_supports_subject (priv->tp_chat)) {
				/* No need to display this 'event' is no topic can be defined anyway */
				str = g_strdup (_("No topic defined"));
			}

			if (str != NULL) {
				empathy_theme_adium_append_event (EMPATHY_CHAT (chat)->view, str);
				g_free (str);
			}
		}
}

static void
chat_title_changed_cb (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

		g_free (priv->name);
		priv->name = g_strdup (empathy_tp_chat_get_title (priv->tp_chat));
		g_object_notify (G_OBJECT (chat), "name");
}

static gboolean
chat_input_text_get_word_from_iter (GtkTextIter   *iter,
                                    GtkTextIter   *start,
                                    GtkTextIter   *end)
{
	GtkTextIter word_start = *iter;
	GtkTextIter word_end = *iter;
	GtkTextIter tmp;

	if (gtk_text_iter_inside_word (&word_end) &&
			!gtk_text_iter_ends_word (&word_end)) {
		gtk_text_iter_forward_word_end (&word_end);
	}

	tmp = word_end;

	if (gtk_text_iter_get_char (&tmp) == '\'') {
		gtk_text_iter_forward_char (&tmp);

		if (g_unichar_isalpha (gtk_text_iter_get_char (&tmp))) {
			gtk_text_iter_forward_word_end (&word_end);
		}
	}


	if (gtk_text_iter_inside_word (&word_start) ||
			gtk_text_iter_ends_word (&word_start)) {
		if (!gtk_text_iter_starts_word (&word_start) ||
				gtk_text_iter_equal (&word_start, &word_end)) {
			gtk_text_iter_backward_word_start (&word_start);
		}

		tmp = word_start;
		gtk_text_iter_backward_char (&tmp);

		if (gtk_text_iter_get_char (&tmp) == '\'') {
			gtk_text_iter_backward_char (&tmp);

			if (g_unichar_isalpha (gtk_text_iter_get_char (&tmp))) {
				gtk_text_iter_backward_word_start (&word_start);
			}
		}
	}

	*start = word_start;
	*end = word_end;
	return TRUE;
}

static void
chat_input_text_buffer_insert_text_cb (GtkTextBuffer *buffer,
                                       GtkTextIter   *location,
                                       gchar         *text,
                                       gint           len,
                                       EmpathyChat   *chat)
{
	GtkTextIter iter, pos;

	/* Remove all misspelled tags in the inserted text.
	 * This happens when text is inserted within a misspelled word. */
	gtk_text_buffer_get_iter_at_offset (buffer, &iter,
					    gtk_text_iter_get_offset (location) - len);
	gtk_text_buffer_remove_tag_by_name (buffer, "misspelled",
					    &iter, location);

	gtk_text_buffer_get_iter_at_mark (buffer, &pos, gtk_text_buffer_get_insert (buffer));

	do {
		GtkTextIter start, end;
		gchar *str;

		if (!chat_input_text_get_word_from_iter (&iter, &start, &end))
			continue;

		str = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);

		if (gtk_text_iter_in_range (&pos, &start, &end) ||
				gtk_text_iter_equal (&pos, &end) ||
				empathy_spell_check (str)) {
			gtk_text_buffer_remove_tag_by_name (buffer, "misspelled", &start, &end);
		} else {
			gtk_text_buffer_apply_tag_by_name (buffer, "misspelled", &start, &end);
		}

		g_free (str);

	} while (gtk_text_iter_forward_word_end (&iter) &&
		 gtk_text_iter_compare (&iter, location) <= 0);
}

static void
chat_input_text_buffer_delete_range_cb (GtkTextBuffer *buffer,
                                        GtkTextIter   *start,
                                        GtkTextIter   *end,
                                        EmpathyChat   *chat)
{
	GtkTextIter word_start, word_end;

	if (chat_input_text_get_word_from_iter (start, &word_start, &word_end)) {
		gtk_text_buffer_remove_tag_by_name (buffer, "misspelled",
						    &word_start, &word_end);
	}
}

static void
chat_input_text_buffer_changed_cb (GtkTextBuffer *buffer,
                                   EmpathyChat    *chat)
{
	if (gtk_text_buffer_get_char_count (buffer) == 0) {
		chat_composing_stop (chat);
	} else {
		chat_composing_start (chat);
	}
}

static void
chat_input_text_buffer_notify_cursor_position_cb (GtkTextBuffer *buffer,
                                                  GParamSpec    *pspec,
                                                  EmpathyChat    *chat)
{
	GtkTextIter pos;
	GtkTextIter prev_pos;
	GtkTextIter word_start;
	GtkTextIter word_end;
	GtkTextMark *mark;
	gchar *str;

	mark = gtk_text_buffer_get_mark (buffer, "previous-cursor-position");

	gtk_text_buffer_get_iter_at_mark (buffer, &pos,
					  gtk_text_buffer_get_insert (buffer));
	gtk_text_buffer_get_iter_at_mark (buffer, &prev_pos, mark);

	if (!chat_input_text_get_word_from_iter (&prev_pos, &word_start, &word_end))
		goto out;

	if (!gtk_text_iter_in_range (&pos, &word_start, &word_end) &&
			!gtk_text_iter_equal (&pos, &word_end)) {
		str = gtk_text_buffer_get_text (buffer,
					&word_start, &word_end, FALSE);

		if (!empathy_spell_check (str)) {
			gtk_text_buffer_apply_tag_by_name (buffer,
					"misspelled", &word_start, &word_end);
		} else {
			gtk_text_buffer_remove_tag_by_name (buffer,
					"misspelled", &word_start, &word_end);
		}

		g_free (str);
	}

out:
	gtk_text_buffer_move_mark (buffer, mark, &pos);
}

static gboolean
empathy_isspace_cb (gunichar c,
		 gpointer data)
{
	return g_unichar_isspace (c);
}

static gboolean
chat_input_key_press_event_cb (GtkWidget   *widget,
			       GdkEventKey *event,
			       EmpathyChat *chat)
{
	EmpathyChatPriv *priv;
	GtkAdjustment  *adj;
	gdouble         val;
	GtkWidget      *text_view_sw;

	priv = GET_PRIV (chat);

	priv->most_recent_event_type = event->type;

	/* Catch ctrl+up/down so we can traverse messages we sent */
	if ((event->state & GDK_CONTROL_MASK) &&
	    (event->keyval == GDK_KEY_Up ||
	     event->keyval == GDK_KEY_Down)) {
		GtkTextBuffer *buffer;
		const gchar   *str;

		buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));
		chat_input_history_update (chat, buffer);

		if (event->keyval == GDK_KEY_Up) {
			str = chat_input_history_get_next (chat);
		} else {
			str = chat_input_history_get_prev (chat);
		}

		g_signal_handlers_block_by_func (buffer,
						 chat_input_text_buffer_changed_cb,
						 chat);
		gtk_text_buffer_set_text (buffer, str ? str : "", -1);
		g_signal_handlers_unblock_by_func (buffer,
						   chat_input_text_buffer_changed_cb,
						   chat);

		return TRUE;
	}

	/* Catch enter but not ctrl/shift-enter */
	if (IS_ENTER (event->keyval) &&
	    !(event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) {
		GtkTextView *view;

		/* This is to make sure that kinput2 gets the enter. And if
		 * it's handled there we shouldn't send on it. This is because
		 * kinput2 uses Enter to commit letters. See:
		 * http://bugzilla.redhat.com/bugzilla/show_bug.cgi?id=104299
		 */

		view = GTK_TEXT_VIEW (chat->input_text_view);
		if (gtk_text_view_im_context_filter_keypress (view, event)) {
			gtk_text_view_reset_im_context (view);
			return TRUE;
		}

		chat_input_text_view_send (chat);
		return TRUE;
	}

	text_view_sw = gtk_widget_get_parent (GTK_WIDGET (chat->view));

	if (IS_ENTER (event->keyval) &&
	    (event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) {
		/* Newline for shift/control-enter. */
		return FALSE;
	}
	if (!(event->state & GDK_CONTROL_MASK) &&
	    event->keyval == GDK_KEY_Page_Up) {
		adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (text_view_sw));
		gtk_adjustment_set_value (adj, gtk_adjustment_get_value (adj) - gtk_adjustment_get_page_size (adj));
		return TRUE;
	}
	if ((event->state & GDK_CONTROL_MASK) != GDK_CONTROL_MASK &&
	    event->keyval == GDK_KEY_Page_Down) {
		adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (text_view_sw));
		val = MIN (gtk_adjustment_get_value (adj) + gtk_adjustment_get_page_size (adj),
			   gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
		gtk_adjustment_set_value (adj, val);
		return TRUE;
	}
	if (event->keyval == GDK_KEY_Escape) {
		empathy_search_bar_hide (EMPATHY_SEARCH_BAR (priv->search_bar));
	}
	if (!(event->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) &&
	    event->keyval == GDK_KEY_Tab) {
		GtkTextBuffer *buffer;
		GtkTextIter    start, current;
		gchar         *nick, *completed;
		GList         *list, *completed_list;
		gboolean       is_start_of_buffer;

		buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (EMPATHY_CHAT (chat)->input_text_view));
		gtk_text_buffer_get_iter_at_mark (buffer, &current, gtk_text_buffer_get_insert (buffer));

		/* Get the start of the nick to complete. */
		gtk_text_buffer_get_iter_at_mark (buffer, &start, gtk_text_buffer_get_insert (buffer));
		if (gtk_text_iter_backward_find_char (&start, &empathy_isspace_cb, NULL, NULL)) {
			gtk_text_iter_set_offset (&start, gtk_text_iter_get_offset (&start) + 1);
		}
		is_start_of_buffer = gtk_text_iter_is_start (&start);

		list = empathy_tp_chat_get_members (priv->tp_chat);
		g_completion_add_items (priv->completion, list);

		nick = gtk_text_buffer_get_text (buffer, &start, &current, FALSE);
		completed_list = g_completion_complete (priv->completion,
							nick,
							&completed);

		g_free (nick);

		if (completed) {
			guint        len;
			const gchar *text;
			GString     *message = NULL;
			GList       *l;

			gtk_text_buffer_delete (buffer, &start, &current);

			len = g_list_length (completed_list);

			if (len == 1) {
				/* If we only have one hit, use that text
				 * instead of the text in completed since the
				 * completed text will use the typed string
				 * which might be cased all wrong.
				 * Fixes #120876
				 * */
				text = empathy_contact_get_alias (completed_list->data);
			} else {
				text = completed;

				/* Print all hits to the scrollback view, so the
				 * user knows what possibilities he has.
				 * Fixes #599779
				 * */
				 message = g_string_new ("");
				 for (l = completed_list; l != NULL; l = l->next) {
					g_string_append (message, empathy_contact_get_alias (l->data));
					g_string_append (message, " - ");
				 }
				 empathy_theme_adium_append_event (chat->view, message->str);
				 g_string_free (message, TRUE);
			}

			gtk_text_buffer_insert_at_cursor (buffer, text, strlen (text));

			if (len == 1 && is_start_of_buffer) {
			    gchar *complete_char;

			    complete_char = g_settings_get_string (
				    priv->gsettings_chat,
				    EMPATHY_PREFS_CHAT_NICK_COMPLETION_CHAR);

			    if (complete_char != NULL) {
				gtk_text_buffer_insert_at_cursor (buffer,
								  complete_char,
								  strlen (complete_char));
				gtk_text_buffer_insert_at_cursor (buffer, " ", 1);
				g_free (complete_char);
			    }
			}

			g_free (completed);
		}

		g_completion_clear_items (priv->completion);

		g_list_foreach (list, (GFunc) g_object_unref, NULL);
		g_list_free (list);

		return TRUE;
	}

	return FALSE;
}

static gboolean
chat_text_view_focus_in_event_cb (GtkWidget  *widget,
				  GdkEvent   *event,
				  EmpathyChat *chat)
{
	gtk_widget_grab_focus (chat->input_text_view);

	return TRUE;
}

static void
chat_input_realize_cb (GtkWidget   *widget,
		       EmpathyChat *chat)
{
	DEBUG ("Setting focus to the input text view");
	if (gtk_widget_is_sensitive (widget)) {
		gtk_widget_grab_focus (widget);
	}
}

static void
chat_input_has_focus_notify_cb (GtkWidget   *widget,
				GParamSpec  *pspec,
				EmpathyChat *chat)
{
	empathy_theme_adium_focus_toggled (chat->view, gtk_widget_has_focus (widget));
}

void
empathy_chat_insert_smiley (GtkTextBuffer *buffer,
				EmpathySmiley        *smiley)
{
	gtk_text_buffer_insert_at_cursor (buffer, smiley->str, -1);
}

static void
chat_insert_smiley_activate_cb (EmpathySmileyManager *manager,
				EmpathySmiley        *smiley,
				gpointer              user_data)
{
	EmpathyChat   *chat = EMPATHY_CHAT (user_data);
	GtkTextBuffer *buffer;

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));

	empathy_chat_insert_smiley (buffer, smiley);
}

typedef struct {
	EmpathyChat  *chat;
	gchar       *word;

	GtkTextIter  start;
	GtkTextIter  end;
} EmpathyChatSpell;

static EmpathyChatSpell *
chat_spell_new (EmpathyChat  *chat,
		const gchar *word,
		GtkTextIter  start,
		GtkTextIter  end)
{
	EmpathyChatSpell *chat_spell;

	chat_spell = g_slice_new0 (EmpathyChatSpell);

	chat_spell->chat = g_object_ref (chat);
	chat_spell->word = g_strdup (word);
	chat_spell->start = start;
	chat_spell->end = end;

	return chat_spell;
}

static void
chat_spell_free (EmpathyChatSpell *chat_spell)
{
	g_object_unref (chat_spell->chat);
	g_free (chat_spell->word);
	g_slice_free (EmpathyChatSpell, chat_spell);
}

static void
chat_spelling_menu_activate_cb (GtkMenuItem     *menu_item,
				                EmpathyChatSpell *chat_spell)
{
    empathy_chat_correct_word (chat_spell->chat,
                               &(chat_spell->start),
                               &(chat_spell->end),
                               gtk_menu_item_get_label (menu_item));
}


static GtkWidget *
chat_spelling_build_suggestions_menu (const gchar *code,
				      EmpathyChatSpell *chat_spell)
{
	GList     *suggestions, *l;
	GtkWidget *menu, *menu_item;

	suggestions = empathy_spell_get_suggestions (code, chat_spell->word);
	if (suggestions == NULL)
		return NULL;

	menu = gtk_menu_new ();
	for (l = suggestions; l; l = l->next) {
		menu_item = gtk_menu_item_new_with_label (l->data);
		g_signal_connect (G_OBJECT (menu_item), "activate",
				  G_CALLBACK (chat_spelling_menu_activate_cb),
				  chat_spell);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), menu_item);
	}
	empathy_spell_free_suggestions (suggestions);

	gtk_widget_show_all (menu);

	return menu;
}

static GtkWidget *
chat_spelling_build_menu (EmpathyChatSpell *chat_spell)
{
	GtkWidget *menu, *submenu, *item;
	GList     *codes, *l;

	codes = empathy_spell_get_enabled_language_codes ();
	g_assert (codes != NULL);

	if (g_list_length (codes) > 1) {
		menu = gtk_menu_new ();

		for (l = codes; l; l = l->next) {
			const gchar *code, *name;

			code = l->data;
			name = empathy_spell_get_language_name (code);
			if (!name)
				continue;

			item = gtk_image_menu_item_new_with_label (name);

			submenu = chat_spelling_build_suggestions_menu (
					code, chat_spell);
			if (submenu == NULL)
				gtk_widget_set_sensitive (item, FALSE);
			else
				gtk_menu_item_set_submenu (GTK_MENU_ITEM (item),
							   submenu);
			gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);
		}
	} else {
		menu = chat_spelling_build_suggestions_menu (codes->data,
							     chat_spell);
		if (menu == NULL) {
			menu = gtk_menu_new ();
			item = gtk_menu_item_new_with_label (_("(No Suggestions)"));
			gtk_widget_set_sensitive (item, FALSE);
			gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
		}
	}
	g_list_free (codes);

	gtk_widget_show_all (menu);

	return menu;
}

typedef struct {
	EmpathyChat  *chat;
	gchar        *word;
	gchar        *code;
} EmpathyChatWord;

static EmpathyChatWord *
chat_word_new (EmpathyChat  *chat,
		const gchar  *word,
		const gchar  *code)
{
	EmpathyChatWord *chat_word;

	chat_word = g_slice_new0 (EmpathyChatWord);

	chat_word->chat = g_object_ref (chat);
	chat_word->word = g_strdup (word);
	chat_word->code = g_strdup (code);

	return chat_word;
}

static void
chat_word_free (EmpathyChatWord *chat_word)
{
	g_object_unref (chat_word->chat);
	g_free (chat_word->word);
	g_free (chat_word->code);
	g_slice_free (EmpathyChatWord, chat_word);
}

static void
chat_add_to_dictionary_activate_cb (GtkMenuItem     *menu_item,
				    EmpathyChatWord *chat_word)
{
	EmpathyChatPriv *priv = GET_PRIV (chat_word->chat);

	empathy_spell_add_to_dictionary (chat_word->code,
					 chat_word->word);
	priv->update_misspelled_words_id =
		g_idle_add (update_misspelled_words, chat_word->chat);
}

static GtkWidget *
chat_spelling_build_add_to_dictionary_item (EmpathyChatSpell *chat_spell)
{
	GtkWidget       *menu, *item, *lang_item, *image;
	GList           *codes, *l;
	gchar           *label;
	const gchar     *code, *name;
	EmpathyChatWord *chat_word;

	codes = empathy_spell_get_enabled_language_codes ();
	g_assert (codes != NULL);
	if (g_list_length (codes) > 1) {
		/* translators: %s is the selected word */
		label = g_strdup_printf (_("Add '%s' to Dictionary"),
					chat_spell->word);
		item = gtk_image_menu_item_new_with_mnemonic (label);
		g_free (label);
		image = gtk_image_new_from_icon_name (GTK_STOCK_ADD,
						      GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item),
					       image);

		menu = gtk_menu_new ();

		for (l = codes; l; l = l->next) {
			code = l->data;
			name = empathy_spell_get_language_name (code);
			if (name == NULL)
				continue;

			lang_item = gtk_image_menu_item_new_with_label (name);

			chat_word= chat_word_new (chat_spell->chat,
						  chat_spell->word, code);
			g_object_set_data_full (G_OBJECT (lang_item),
				"chat-word", chat_word,
				(GDestroyNotify) chat_word_free);

			g_signal_connect (G_OBJECT (lang_item), "activate",
				G_CALLBACK (chat_add_to_dictionary_activate_cb),
				chat_word);
			gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), lang_item);
		}
		gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), menu);
	} else {
		code = codes->data;
		name = empathy_spell_get_language_name (code);
		g_assert (name != NULL);
		/* translators: first %s is the selected word,
		 * second %s is the language name of the target dictionary */
		label = g_strdup_printf (_("Add '%s' to %s Dictionary"),
					chat_spell->word, name);
		item = gtk_image_menu_item_new_with_mnemonic (label);
		g_free (label);
		image = gtk_image_new_from_icon_name (GTK_STOCK_ADD,
						      GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);

		chat_word = chat_word_new (chat_spell->chat, chat_spell->word,
					   code);
		g_object_set_data_full (G_OBJECT (item), "chat-word", chat_word,
					(GDestroyNotify) chat_word_free);

		g_signal_connect (G_OBJECT (item), "activate",
				  G_CALLBACK (chat_add_to_dictionary_activate_cb),
				  chat_word);
	}
	g_list_free (codes);

	gtk_widget_show_all (item);

	return item;
}

static void
chat_text_send_cb (GtkMenuItem *menuitem,
		   EmpathyChat *chat)
{
	chat_input_text_view_send (chat);
}

static gboolean
chat_input_button_press_event_cb (GtkTextView    *view,
				  GdkEventButton *event,
				  EmpathyChat    *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	priv->most_recent_event_type = event->type;

	return FALSE;
}

static void
chat_input_populate_popup_cb (GtkTextView *view,
			      GtkMenu     *menu,
			      EmpathyChat *chat)
{
	EmpathyChatPriv      *priv = GET_PRIV (chat);
	GtkTextBuffer        *buffer;
	GtkTextTagTable      *table;
	GtkTextTag           *tag;
	gint                  x, y;
	GtkTextIter           iter, start, end;
	GtkWidget            *item;
	gchar                *str = NULL;
	EmpathyChatSpell     *chat_spell;
	GtkWidget            *spell_menu;
	GtkWidget            *spell_item;
	EmpathySmileyManager *smiley_manager;
	GtkWidget            *smiley_menu;
	GtkWidget            *image;

	buffer = gtk_text_view_get_buffer (view);

	/* Add the emoticon menu. */
	item = gtk_separator_menu_item_new ();
	gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);
	gtk_widget_show (item);

	item = gtk_image_menu_item_new_with_mnemonic (_("Insert Smiley"));
	image = gtk_image_new_from_icon_name ("face-smile",
					      GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
	gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);
	gtk_widget_show (item);

	smiley_manager = empathy_smiley_manager_dup_singleton ();
	smiley_menu = empathy_smiley_menu_new (smiley_manager,
					       chat_insert_smiley_activate_cb,
					       chat);
	gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), smiley_menu);
	g_object_unref (smiley_manager);

	/* Add the Send menu item. */
	gtk_text_buffer_get_bounds (buffer, &start, &end);
	str = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
	if (!TPAW_STR_EMPTY (str)) {
		item = gtk_menu_item_new_with_mnemonic (_("_Send"));
		g_signal_connect (G_OBJECT (item), "activate",
				  G_CALLBACK (chat_text_send_cb), chat);
		gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);
		gtk_widget_show (item);
	}
	str = NULL;

	/* Add the spell check menu item. */
	table = gtk_text_buffer_get_tag_table (buffer);
	tag = gtk_text_tag_table_lookup (table, "misspelled");

	switch (priv->most_recent_event_type) {
	    case GDK_BUTTON_PRESS:
		/* get the location from the pointer */
		gdk_window_get_device_position (gtk_widget_get_window (GTK_WIDGET (view)),
			gdk_device_manager_get_client_pointer (gdk_display_get_device_manager (
				gtk_widget_get_display (GTK_WIDGET (view)))), &x, &y, NULL);

		gtk_text_view_window_to_buffer_coords (GTK_TEXT_VIEW (view),
						       GTK_TEXT_WINDOW_WIDGET,
						       x, y,
						       &x, &y);
		gtk_text_view_get_iter_at_location (GTK_TEXT_VIEW (view),
						    &iter, x, y);
		break;

	    default:
		g_warn_if_reached ();
		/* assume the KEY_PRESS case */

	    case GDK_KEY_PRESS:
		/* get the location from the cursor */
		gtk_text_buffer_get_iter_at_mark (buffer, &iter,
				gtk_text_buffer_get_insert (buffer));
		break;

	}

	start = end = iter;
	if (gtk_text_iter_backward_to_tag_toggle (&start, tag) &&
	    gtk_text_iter_forward_to_tag_toggle (&end, tag)) {

		str = gtk_text_buffer_get_text (buffer,
						&start, &end, FALSE);
	}
	if (!TPAW_STR_EMPTY (str)) {
		chat_spell = chat_spell_new (chat, str, start, end);
		g_object_set_data_full (G_OBJECT (menu),
					"chat-spell", chat_spell,
					(GDestroyNotify) chat_spell_free);

		item = gtk_separator_menu_item_new ();
		gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);
		gtk_widget_show (item);

		/* Spelling suggestions */
		item = gtk_image_menu_item_new_with_mnemonic (_("_Spelling Suggestions"));
		image = gtk_image_new_from_icon_name (GTK_STOCK_SPELL_CHECK,
						      GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);

		spell_menu = chat_spelling_build_menu (chat_spell);
		gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), spell_menu);


		spell_item = gtk_separator_menu_item_new ();
		gtk_menu_shell_append (GTK_MENU_SHELL (spell_menu), spell_item);
		gtk_widget_show (spell_item);

		/* Add to dictionary */
		spell_item = chat_spelling_build_add_to_dictionary_item (chat_spell);

		gtk_menu_shell_append (GTK_MENU_SHELL (spell_menu), spell_item);
		gtk_widget_show (spell_item);

		gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);
		gtk_widget_show (item);
	}
}


static gboolean
chat_log_filter (TplEvent *event,
		 gpointer user_data)
{
	EmpathyChat *chat = EMPATHY_CHAT (user_data);
	EmpathyChatPriv *priv = GET_PRIV (chat);
	EmpathyMessage *message;
	const GList *pending;
	bool retval = FALSE;

	g_return_val_if_fail (TPL_IS_EVENT (event), FALSE);
	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), FALSE);

	pending = empathy_tp_chat_get_pending_messages (priv->tp_chat);
	message = empathy_message_from_tpl_log_event (event);

	for (; pending; pending = g_list_next (pending)) {
		if (empathy_message_equal (message, pending->data))
			goto out;
	}

	retval = TRUE;

out:
	g_object_unref (message);
	return retval;
}

static void
show_pending_messages (EmpathyChat *chat) {
	EmpathyChatPriv *priv = GET_PRIV (chat);
	const GList *messages, *l;

	g_return_if_fail (EMPATHY_IS_CHAT (chat));
	g_return_if_fail (chat->view != NULL);
	g_return_if_fail (priv->tp_chat != NULL);

	messages = empathy_tp_chat_get_pending_messages (priv->tp_chat);

	for (l = messages; l != NULL ; l = g_list_next (l)) {
		EmpathyMessage *message = EMPATHY_MESSAGE (l->data);
		chat_message_received (chat, message, TRUE);
	}
}

static gboolean
chat_scrollable_set_value (gpointer user_data)
{
	EmpathyChat *chat = EMPATHY_CHAT (user_data);
	EmpathyChatPriv *priv = GET_PRIV (chat);
	GtkAdjustment *adjustment;
	guint upper;

	adjustment = gtk_scrollable_get_vadjustment (
	    GTK_SCROLLABLE (chat->view));

	/* Set the chat->view's adjustment back to the value it had
	 * before it grew as a result of new logs being inserted.
	 */
	upper  = (guint) gtk_adjustment_get_upper (adjustment);
	gtk_adjustment_set_value (adjustment, upper - priv->scroll_offset);

	return G_SOURCE_REMOVE;
}

static void
got_filtered_messages_cb (GObject *walker,
		GAsyncResult *result,
		gpointer user_data)
{
	GList *l;
	GList *messages;
	EmpathyChat *chat = EMPATHY_CHAT (user_data);
	EmpathyChatPriv *priv = GET_PRIV (chat);
	GError *error = NULL;

	if (!tpl_log_walker_get_events_finish (TPL_LOG_WALKER (walker),
		result, &messages, &error)) {
		DEBUG ("%s. Aborting.", error->message);
		empathy_theme_adium_append_event (chat->view,
			_("Failed to retrieve recent logs"));
		g_error_free (error);
		goto out;
	}

	for (l = g_list_last (messages); l; l = g_list_previous (l)) {
		EmpathyMessage *message;

		g_assert (TPL_IS_EVENT (l->data));

		message = empathy_message_from_tpl_log_event (l->data);
		g_object_unref (l->data);

		if (empathy_message_is_edit (message)) {
			/* this is an edited message, create a synthetic event
			 * using the supersedes token and
			 * original-message-sent timestamp, that we can then
			 * replace */
			EmpathyMessage *syn_msg = g_object_new (
				EMPATHY_TYPE_MESSAGE,
				"body", "",
				"token", empathy_message_get_supersedes (message),
				"type", empathy_message_get_tptype (message),
				"timestamp", empathy_message_get_original_timestamp (message),
				"incoming", empathy_message_is_incoming (message),
				"is-backlog", TRUE,
				"receiver", empathy_message_get_receiver (message),
				"sender", empathy_message_get_sender (message),
				NULL);

			empathy_theme_adium_prepend_message (chat->view, syn_msg,
							  chat_should_highlight (chat, syn_msg));
			empathy_theme_adium_edit_message (chat->view, message);

			g_object_unref (syn_msg);
		} else {
			/* append the latest message */
			empathy_theme_adium_prepend_message (chat->view, message,
							  chat_should_highlight (chat, message));
		}

		g_object_unref (message);
	}
	g_list_free (messages);

out:
	/* FIXME: See Bug#610994, we are forcing the ACK of the queue. See comments
	 * about it in EmpathyChatPriv definition */
	priv->retrieving_backlogs = FALSE;
	empathy_chat_messages_read (chat);

	/* Turn back on scrolling */
	empathy_theme_adium_scroll (chat->view, TRUE);

	/* We start watching the scrolling movements only after the first
	 * batch of logs have been fetched. Otherwise, if the
	 * chat->view's page size is too small the scrollbar might hit
	 * the upper edge and trigger another batch of logs to be
	 * fetched.
	 */
	if (G_UNLIKELY (!priv->watch_scroll &&
			!tpl_log_walker_is_end (priv->log_walker))) {
		priv->watch_scroll = TRUE;
		g_idle_add_full (G_PRIORITY_LOW, chat_scrollable_connect,
		    g_object_ref (chat), g_object_unref);
	}
	else {
		GtkAdjustment *adjustment;
		guint upper;
		guint value;

		/* The chat->view's adjustment won't change unless we
		 * return to the main loop. Save the current offset
		 * from the lower edge (or the upper value of the
		 * adjustment) so that we can restore it later once the
		 * adjustment grows.
		 */
		adjustment = gtk_scrollable_get_vadjustment (
		    GTK_SCROLLABLE (chat->view));
		upper = (guint) gtk_adjustment_get_upper (adjustment);
		value = (guint) gtk_adjustment_get_value (adjustment);
		priv->scroll_offset = upper - value;

		g_idle_add_full (G_PRIORITY_LOW, chat_scrollable_set_value,
		    g_object_ref (chat), g_object_unref);
	}

	g_object_unref (chat);
}

static gboolean
chat_add_logs (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	if (!priv->id) {
		return G_SOURCE_REMOVE;
	}

	/* Turn off scrolling temporarily */
	empathy_theme_adium_scroll (chat->view, FALSE);

	tpl_log_walker_get_events_async (priv->log_walker, 5,
	    got_filtered_messages_cb, g_object_ref (chat));

	return G_SOURCE_REMOVE;
}

static void
chat_schedule_logs (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	if (priv->retrieving_backlogs)
		return;

	priv->retrieving_backlogs = TRUE;
	g_timeout_add_full (G_PRIORITY_LOW, 500, /* ms */
	    (GSourceFunc) chat_add_logs, g_object_ref (chat), g_object_unref);
}

static void
chat_view_adjustment_changed_cb (GtkAdjustment *adjustment,
				 gpointer user_data)
{
	EmpathyChat *chat = EMPATHY_CHAT (user_data);
	EmpathyChatPriv *priv = GET_PRIV (chat);
	guint page_size;

	if (tpl_log_walker_is_end (priv->log_walker)) {
		g_signal_handlers_disconnect_by_func (adjustment,
		    chat_view_adjustment_changed_cb, user_data);
		return;
	}

	page_size = (guint) gtk_adjustment_get_page_size (adjustment);
	if (page_size <= priv->max_page_size)
		return;

	/* We need to fetch more logs if the page size of the view
	 * increases, so that there is no empty space at the top.
	 */
	if (G_LIKELY (priv->max_page_size != 0))
		chat_schedule_logs (chat);

	priv->max_page_size = page_size;
}

static void
chat_view_adjustment_value_changed_cb (GtkAdjustment *adjustment,
				       gpointer user_data)
{
	EmpathyChat *chat = EMPATHY_CHAT (user_data);
	EmpathyChatPriv *priv = GET_PRIV (chat);
	guint lower;
	guint value;

	if (tpl_log_walker_is_end (priv->log_walker)) {
		g_signal_handlers_disconnect_by_func (adjustment,
		    chat_view_adjustment_value_changed_cb, user_data);
		return;
	}

	lower = (guint) gtk_adjustment_get_lower (adjustment);
	value = (guint) gtk_adjustment_get_value (adjustment);
	if (value != lower)
		return;

	/* Request for more logs to be fetched if the user hit the
	 * upper edge of the chat->view.
	 */
	chat_schedule_logs (chat);
}

static gboolean
chat_scrollable_connect (gpointer user_data)
{
	EmpathyChat *chat = EMPATHY_CHAT (user_data);
	GtkAdjustment *adjustment;

	adjustment = gtk_scrollable_get_vadjustment (
	    GTK_SCROLLABLE (chat->view));

	g_signal_connect (adjustment, "changed",
	    G_CALLBACK (chat_view_adjustment_changed_cb), chat);
	g_signal_connect (adjustment, "value-changed",
	    G_CALLBACK (chat_view_adjustment_value_changed_cb), chat);

	return G_SOURCE_REMOVE;
}

static gint
chat_contacts_completion_func (const gchar *s1,
			       const gchar *s2,
			       gsize        n)
{
	gchar *tmp, *nick1, *nick2;
	gint   ret;

	if (s1 == s2) {
		return 0;
	}
	if (!s1 || !s2) {
		return s1 ? -1 : +1;
	}

	tmp = g_utf8_normalize (s1, -1, G_NORMALIZE_DEFAULT);
	nick1 = g_utf8_casefold (tmp, -1);
	g_free (tmp);

	tmp = g_utf8_normalize (s2, -1, G_NORMALIZE_DEFAULT);
	nick2 = g_utf8_casefold (tmp, -1);
	g_free (tmp);

	ret = strncmp (nick1, nick2, n);

	g_free (nick1);
	g_free (nick2);

	return ret;
}

static gchar *
build_part_message (guint           reason,
		    const gchar    *name,
		    EmpathyContact *actor,
		    const gchar    *message)
{
	GString *s = g_string_new ("");
	const gchar *actor_name = NULL;

	if (actor != NULL) {
		actor_name = empathy_contact_get_alias (actor);
	}

	/* Having an actor only really makes sense for a few actions... */
	switch (reason) {
	case TP_CHANNEL_GROUP_CHANGE_REASON_OFFLINE:
		g_string_append_printf (s, _("%s has disconnected"), name);
		break;
	case TP_CHANNEL_GROUP_CHANGE_REASON_KICKED:
		if (actor_name != NULL) {
			/* translators: reverse the order of these arguments
			 * if the kicked should come before the kicker in your locale.
			 */
			g_string_append_printf (s, _("%1$s was kicked by %2$s"),
				name, actor_name);
		} else {
			g_string_append_printf (s, _("%s was kicked"), name);
		}
		break;
	case TP_CHANNEL_GROUP_CHANGE_REASON_BANNED:
		if (actor_name != NULL) {
			/* translators: reverse the order of these arguments
			 * if the banned should come before the banner in your locale.
			 */
			g_string_append_printf (s, _("%1$s was banned by %2$s"),
				name, actor_name);
		} else {
			g_string_append_printf (s, _("%s was banned"), name);
		}
		break;
	default:
		g_string_append_printf (s, _("%s has left the room"), name);
	}

	if (!TPAW_STR_EMPTY (message)) {
		/* Note to translators: this string is appended to
		 * notifications like "foo has left the room", with the message
		 * given by the user living the room. If this poses a problem,
		 * please let us know. :-)
		 */
		g_string_append_printf (s, _(" (%s)"), message);
	}

	return g_string_free (s, FALSE);
}

static void
chat_members_changed_cb (EmpathyTpChat  *tp_chat,
			 EmpathyContact *contact,
			 EmpathyContact *actor,
			 guint           reason,
			 gchar          *message,
			 gboolean        is_member,
			 EmpathyChat    *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	const gchar *name = empathy_contact_get_alias (contact);
	gchar *str;

	g_return_if_fail (TP_CHANNEL_GROUP_CHANGE_REASON_RENAMED != reason);

	if (priv->block_events_timeout_id != 0)
		return;

	if (is_member) {
		str = g_strdup_printf (_("%s has joined the room"),
				       name);
	} else {
		str = build_part_message (reason, name, actor, message);
	}

	empathy_theme_adium_append_event (chat->view, str);
	g_free (str);
}

static void
chat_member_renamed_cb (EmpathyTpChat  *tp_chat,
			 EmpathyContact *old_contact,
			 EmpathyContact *new_contact,
			 guint           reason,
			 gchar          *message,
			 EmpathyChat    *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	g_return_if_fail (TP_CHANNEL_GROUP_CHANGE_REASON_RENAMED == reason);

	if (priv->block_events_timeout_id == 0) {
		gchar *str;

		str = g_strdup_printf (_("%s is now known as %s"),
				       empathy_contact_get_alias (old_contact),
				       empathy_contact_get_alias (new_contact));
		empathy_theme_adium_append_event (chat->view, str);
		g_free (str);
	}

}

static gboolean
chat_contacts_visible_timeout_cb (gpointer chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	/* Relax the size request */
	gtk_widget_set_size_request (priv->vbox_left, -1, -1);

	/* Set the position of the slider. This must be done here because
	 * GtkPaned need to know its size allocation and it will be settled only
	 * after the gtk_window_resize () tough effect. */
	if (priv->contacts_width > 0) {
		gtk_paned_set_position (GTK_PANED (priv->hpaned),
					priv->contacts_width);
	}

	priv->contacts_visible_id = 0;

	return FALSE;
}

static void
chat_update_contacts_visibility (EmpathyChat *chat,
			 gboolean show)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	if (!priv->scrolled_window_contacts) {
		return;
	}

	if (priv->remote_contact != NULL) {
		show = FALSE;
	}

	if (show && priv->contact_list_view == NULL) {
		EmpathyIndividualStore *store;
		gint                     min_width;
		GtkAllocation            allocation;

		/* We are adding the contact list to the chat, we don't want the
		 * chat view to become too small. If the chat view is already
		 * smaller than 250 make sure that size won't change. If the
		 * chat view is bigger the contact list will take some space on
		 * it but we make sure the chat view don't become smaller than
		 * 250. Relax the size request once the resize is done */
		gtk_widget_get_allocation (priv->vbox_left, &allocation);
		min_width = MIN (allocation.width, 250);
		gtk_widget_set_size_request (priv->vbox_left, min_width, -1);

		/* There is no way to know when the window resize will happen
		 * since it is WM's decision. Let's hope it won't be longer. */
		if (priv->contacts_visible_id != 0)
			g_source_remove (priv->contacts_visible_id);
		priv->contacts_visible_id = g_timeout_add (500,
			chat_contacts_visible_timeout_cb, chat);

		store = EMPATHY_INDIVIDUAL_STORE (
				empathy_individual_store_channel_new ((TpChannel *) priv->tp_chat));

		empathy_individual_store_set_show_groups (store, FALSE);

		priv->contact_list_view = GTK_WIDGET (empathy_individual_view_new (store,
			EMPATHY_INDIVIDUAL_VIEW_FEATURE_INDIVIDUAL_TOOLTIP,
			EMPATHY_INDIVIDUAL_FEATURE_ADD_CONTACT |
			EMPATHY_INDIVIDUAL_FEATURE_CHAT |
			EMPATHY_INDIVIDUAL_FEATURE_CALL |
			EMPATHY_INDIVIDUAL_FEATURE_LOG |
			EMPATHY_INDIVIDUAL_FEATURE_INFO));

		empathy_individual_view_set_show_offline (
			EMPATHY_INDIVIDUAL_VIEW (priv->contact_list_view), TRUE);
		empathy_individual_view_set_show_uninteresting (
			EMPATHY_INDIVIDUAL_VIEW (priv->contact_list_view), TRUE);

		gtk_container_add (GTK_CONTAINER (priv->scrolled_window_contacts),
				   priv->contact_list_view);

		gtk_widget_show (priv->contact_list_view);
		gtk_widget_show (priv->scrolled_window_contacts);
		g_object_unref (store);
	} else if (!show) {
		priv->contacts_width = gtk_paned_get_position (GTK_PANED (priv->hpaned));
		gtk_widget_hide (priv->scrolled_window_contacts);
		if (priv->contact_list_view != NULL) {
			gtk_widget_destroy (priv->contact_list_view);
			priv->contact_list_view = NULL;
		}
	}
}

void
empathy_chat_set_show_contacts (EmpathyChat *chat,
				gboolean     show)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	priv->show_contacts = show;

	chat_update_contacts_visibility (chat, show);

	g_object_notify (G_OBJECT (chat), "show-contacts");
}

static void
chat_self_contact_changed_cb (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	if (priv->self_contact != NULL) {
		g_signal_handlers_disconnect_by_func (priv->self_contact,
						      chat_self_contact_alias_changed_cb,
						      chat);
	}
	g_clear_object (&priv->self_contact);

	priv->self_contact = empathy_tp_chat_get_self_contact (priv->tp_chat);
	if (priv->self_contact != NULL) {
		g_object_ref (priv->self_contact);

		if (empathy_chat_is_room (chat)) {
			g_signal_connect_swapped (priv->self_contact, "notify::alias",
					  G_CALLBACK (chat_self_contact_alias_changed_cb),
					  chat);
		}
	}

	chat_self_contact_alias_changed_cb (chat);
}

static void
chat_remote_contact_changed_cb (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	if (priv->remote_contact != NULL) {
		g_object_unref (priv->remote_contact);
		priv->remote_contact = NULL;
	}

	g_free (priv->id);

	priv->id = g_strdup (empathy_tp_chat_get_id (priv->tp_chat));
	priv->remote_contact = empathy_tp_chat_get_remote_contact (priv->tp_chat);
	if (priv->remote_contact != NULL) {
		g_object_ref (priv->remote_contact);
		priv->handle_type = TP_HANDLE_TYPE_CONTACT;
	}
	else if (priv->tp_chat != NULL) {
		tp_channel_get_handle ((TpChannel *) priv->tp_chat, &priv->handle_type);
	}

	chat_update_contacts_visibility (chat, priv->show_contacts);

	g_object_notify (G_OBJECT (chat), "remote-contact");
	g_object_notify (G_OBJECT (chat), "id");
}

static void
chat_invalidated_cb (EmpathyTpChat *tp_chat,
		 guint domain,
		 gint code,
		 gchar *message,
		 EmpathyChat   *chat)
{
	EmpathyChatPriv *priv;

	priv = GET_PRIV (chat);

	if (!priv->tp_chat) {
		return;
	}

	chat_composing_remove_timeout (chat);
	g_object_unref (priv->tp_chat);
	priv->tp_chat = NULL;
	g_object_notify (G_OBJECT (chat), "tp-chat");

	empathy_theme_adium_append_event (chat->view, _("Disconnected"));
	gtk_widget_set_sensitive (chat->input_text_view, FALSE);

	chat_update_contacts_visibility (chat, FALSE);

	priv->unread_messages_when_offline = priv->unread_messages;
}

static gboolean
update_misspelled_words (gpointer data)
{
	EmpathyChat *chat = EMPATHY_CHAT (data);
	EmpathyChatPriv *priv = GET_PRIV (chat);
	GtkTextBuffer *buffer;
	GtkTextIter iter;
	gint length;

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));

	gtk_text_buffer_get_end_iter (buffer, &iter);
	length = gtk_text_iter_get_offset (&iter);
	chat_input_text_buffer_insert_text_cb (buffer, &iter,
					       NULL, length, chat);

	priv->update_misspelled_words_id = 0;

	return FALSE;
}

static void
conf_spell_checking_cb (GSettings *gsettings_chat,
			const gchar *key,
			gpointer user_data)
{
	EmpathyChat *chat = EMPATHY_CHAT (user_data);
	EmpathyChatPriv *priv = GET_PRIV (chat);
	gboolean spell_checker;
	GtkTextBuffer *buffer;

	if (strcmp (key, EMPATHY_PREFS_CHAT_SPELL_CHECKER_ENABLED) != 0)
		return;

	spell_checker = g_settings_get_boolean (gsettings_chat,
			EMPATHY_PREFS_CHAT_SPELL_CHECKER_ENABLED);

	if (!empathy_spell_supported ()) {
		spell_checker = FALSE;
	}

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));

	if (spell_checker == priv->spell_checking_enabled) {
		if (spell_checker) {
			/* Possibly changed dictionaries,
			 * update misspelled words. Need to do so in idle
			 * so the spell checker is updated. */
			priv->update_misspelled_words_id =
				g_idle_add (update_misspelled_words, chat);
		}

		return;
	}

	if (spell_checker) {
		GtkTextIter iter;

		priv->notify_cursor_position_id = tp_g_signal_connect_object  (
				buffer, "notify::cursor-position",
				G_CALLBACK (chat_input_text_buffer_notify_cursor_position_cb),
				chat, 0);
		priv->insert_text_id = tp_g_signal_connect_object  (
				buffer, "insert-text",
				G_CALLBACK (chat_input_text_buffer_insert_text_cb),
				chat, G_CONNECT_AFTER);
		priv->delete_range_id = tp_g_signal_connect_object  (
				buffer, "delete-range",
				G_CALLBACK (chat_input_text_buffer_delete_range_cb),
				chat, G_CONNECT_AFTER);

		gtk_text_buffer_create_tag (buffer, "misspelled",
					    "underline", PANGO_UNDERLINE_ERROR,
					    NULL);

		gtk_text_buffer_get_iter_at_mark (buffer, &iter,
	                                          gtk_text_buffer_get_insert (buffer));
		gtk_text_buffer_create_mark (buffer, "previous-cursor-position",
					     &iter, TRUE);

		/* Mark misspelled words in the existing buffer.
		 * Need to do so in idle so the spell checker is updated. */
		priv->update_misspelled_words_id =
			g_idle_add (update_misspelled_words, chat);
	} else {
		GtkTextTagTable *table;
		GtkTextTag *tag;

		g_signal_handler_disconnect (buffer, priv->notify_cursor_position_id);
		priv->notify_cursor_position_id = 0;
		g_signal_handler_disconnect (buffer, priv->insert_text_id);
		priv->insert_text_id = 0;
		g_signal_handler_disconnect (buffer, priv->delete_range_id);
		priv->delete_range_id = 0;

		table = gtk_text_buffer_get_tag_table (buffer);
		tag = gtk_text_tag_table_lookup (table, "misspelled");
		gtk_text_tag_table_remove (table, tag);

		gtk_text_buffer_delete_mark_by_name (buffer,
						     "previous-cursor-position");
	}

	priv->spell_checking_enabled = spell_checker;
}

static gboolean
save_paned_pos_timeout (gpointer data)
{
	EmpathyChat *self = data;
	gint hpaned_pos;

	self->priv->save_paned_pos_id = 0;

	hpaned_pos = gtk_paned_get_position (GTK_PANED (self->priv->hpaned));

	g_settings_set_int (self->priv->gsettings_ui,
			    EMPATHY_PREFS_UI_CHAT_WINDOW_PANED_POS,
			    hpaned_pos);

	return FALSE;
}

static gboolean
chat_hpaned_pos_changed_cb (GtkWidget* hpaned,
		GParamSpec *spec,
		gpointer user_data)
{
	EmpathyChat *chat = EMPATHY_CHAT (user_data);

	if (chat->priv->save_paned_pos_id != 0)
		g_source_remove (chat->priv->save_paned_pos_id);

	chat->priv->save_paned_pos_id = g_timeout_add_seconds (1,
		save_paned_pos_timeout, chat);

	return TRUE;
}

static void
chat_create_ui (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	GtkBuilder      *gui;
 	GList           *list = NULL;
	gchar           *filename;
	GtkTextBuffer   *buffer;
	EmpathyThemeManager *theme_mgr;

	filename = empathy_file_lookup ("empathy-chat.ui",
					"libempathy-gtk");
	gui = tpaw_builder_get_file (filename,
					"chat_widget", &priv->widget,
					"hpaned", &priv->hpaned,
					"vbox_left", &priv->vbox_left,
					"scrolled_window_chat", &priv->scrolled_window_chat,
					"scrolled_window_input", &priv->scrolled_window_input,
					"hbox_topic", &priv->hbox_topic,
					"expander_topic", &priv->expander_topic,
					"label_topic", &priv->label_topic,
					"scrolled_window_contacts", &priv->scrolled_window_contacts,
					"info_bar_vbox", &priv->info_bar_vbox,
					NULL);

	tpaw_builder_connect (gui, chat,
		"expander_topic", "notify::expanded", chat_topic_expander_activate_cb,
		"label_topic", "size-allocate", chat_topic_label_size_allocate_cb,
		NULL);

	g_free (filename);

	/* Add message view. */
	theme_mgr = empathy_theme_manager_dup_singleton ();
	chat->view = empathy_theme_manager_create_view (theme_mgr);
	g_object_unref (theme_mgr);
	/* If this is a GtkTextView, it's set as a drag destination for text/plain
	   and other types, even though it's non-editable and doesn't accept any
	   drags.  This steals drag motion for anything inside the scrollbars,
	   making drag destinations on chat windows far less useful.
	 */
	gtk_drag_dest_unset (GTK_WIDGET (chat->view));
	g_signal_connect (chat->view, "focus_in_event",
			  G_CALLBACK (chat_text_view_focus_in_event_cb),
			  chat);
	gtk_container_add (GTK_CONTAINER (priv->scrolled_window_chat),
			   GTK_WIDGET (chat->view));
	gtk_widget_show (GTK_WIDGET (chat->view));

	/* Add input GtkTextView */
	chat->input_text_view = empathy_input_text_view_new ();
	g_signal_connect (chat->input_text_view, "notify::has-focus",
			  G_CALLBACK (chat_input_has_focus_notify_cb),
			  chat);
	g_signal_connect (chat->input_text_view, "key-press-event",
			  G_CALLBACK (chat_input_key_press_event_cb),
			  chat);
	g_signal_connect (chat->input_text_view, "realize",
			  G_CALLBACK (chat_input_realize_cb),
			  chat);
	g_signal_connect (chat->input_text_view, "button-press-event",
			  G_CALLBACK (chat_input_button_press_event_cb),
			  chat);
	g_signal_connect (chat->input_text_view, "populate-popup",
			  G_CALLBACK (chat_input_populate_popup_cb),
			  chat);
	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));
	tp_g_signal_connect_object  (buffer, "changed",
			  G_CALLBACK (chat_input_text_buffer_changed_cb),
			  chat, 0);
	tp_g_signal_connect_object (priv->gsettings_chat,
			"changed::" EMPATHY_PREFS_CHAT_SPELL_CHECKER_ENABLED,
			G_CALLBACK (conf_spell_checking_cb), chat, 0);
	conf_spell_checking_cb (priv->gsettings_chat,
				EMPATHY_PREFS_CHAT_SPELL_CHECKER_ENABLED, chat);
	gtk_container_add (GTK_CONTAINER (priv->scrolled_window_input),
			   chat->input_text_view);
	gtk_widget_show (chat->input_text_view);

	/* Add the (invisible) search bar */
	priv->search_bar = empathy_search_bar_new (chat->view);
	gtk_box_pack_start (GTK_BOX(priv->vbox_left),
	                    priv->search_bar,
	                    FALSE, FALSE, 0);
	gtk_box_reorder_child (GTK_BOX(priv->vbox_left), priv->search_bar, 1);

	/* Initialy hide the topic, will be shown if not empty */
	gtk_widget_hide (priv->hbox_topic);

	g_signal_connect (priv->hpaned, "notify::position",
			  G_CALLBACK (chat_hpaned_pos_changed_cb),
			  chat);

	/* Set widget focus order */
	list = g_list_append (NULL, priv->search_bar);
	list = g_list_append (list, priv->scrolled_window_input);
	gtk_container_set_focus_chain (GTK_CONTAINER (priv->vbox_left), list);
	g_list_free (list);

	list = g_list_append (NULL, priv->vbox_left);
	list = g_list_append (list, priv->scrolled_window_contacts);
	gtk_container_set_focus_chain (GTK_CONTAINER (priv->hpaned), list);
	g_list_free (list);

	list = g_list_append (NULL, priv->hpaned);
	list = g_list_append (list, priv->hbox_topic);
	gtk_container_set_focus_chain (GTK_CONTAINER (priv->widget), list);
	g_list_free (list);

	/* Add the main widget in the chat widget */
	gtk_box_pack_start (GTK_BOX (chat), priv->widget, TRUE, TRUE, 0);
	g_object_unref (gui);
}

static void
chat_finalize (GObject *object)
{
	EmpathyChat     *chat;
	EmpathyChatPriv *priv;

	chat = EMPATHY_CHAT (object);
	priv = GET_PRIV (chat);

	DEBUG ("Finalized: %p", object);

	if (priv->update_misspelled_words_id != 0)
		g_source_remove (priv->update_misspelled_words_id);

	if (priv->save_paned_pos_id != 0)
		g_source_remove (priv->save_paned_pos_id);

	if (priv->contacts_visible_id != 0)
		g_source_remove (priv->contacts_visible_id);

	g_object_unref (priv->gsettings_chat);
	g_object_unref (priv->gsettings_ui);

	g_list_foreach (priv->input_history, (GFunc) chat_input_history_entry_free, NULL);
	g_list_free (priv->input_history);

	g_list_foreach (priv->compositors, (GFunc) g_object_unref, NULL);
	g_list_free (priv->compositors);

	chat_composing_remove_timeout (chat);

	g_object_unref (priv->account_manager);
	g_object_unref (priv->log_manager);
	g_object_unref (priv->log_walker);

	if (priv->tp_chat) {
		g_signal_handlers_disconnect_by_func (priv->tp_chat,
			chat_invalidated_cb, chat);
		g_signal_handlers_disconnect_by_func (priv->tp_chat,
			chat_message_received_cb, chat);
		g_signal_handlers_disconnect_by_func (priv->tp_chat,
			chat_message_acknowledged_cb, chat);
		g_signal_handlers_disconnect_by_func (priv->tp_chat,
			chat_send_error_cb, chat);
		g_signal_handlers_disconnect_by_func (priv->tp_chat,
			chat_state_changed_cb, chat);
		g_signal_handlers_disconnect_by_func (priv->tp_chat,
			chat_members_changed_cb, chat);
		g_signal_handlers_disconnect_by_func (priv->tp_chat,
			chat_self_contact_changed_cb, chat);
		g_signal_handlers_disconnect_by_func (priv->tp_chat,
			chat_remote_contact_changed_cb, chat);
		g_signal_handlers_disconnect_by_func (priv->tp_chat,
			chat_title_changed_cb, chat);
		g_signal_handlers_disconnect_by_func (priv->tp_chat,
			chat_subject_changed_cb, chat);
		empathy_tp_chat_leave (priv->tp_chat, "");
		g_object_unref (priv->tp_chat);
	}
	if (priv->account) {
		g_object_unref (priv->account);
	}
	if (priv->self_contact) {
		g_signal_handlers_disconnect_by_func (priv->self_contact,
						      chat_self_contact_alias_changed_cb,
						      chat);
		g_object_unref (priv->self_contact);
	}
	if (priv->remote_contact) {
		g_object_unref (priv->remote_contact);
	}

	if (priv->block_events_timeout_id) {
		g_source_remove (priv->block_events_timeout_id);
	}

	g_free (priv->id);
	g_free (priv->name);
	g_free (priv->subject);
	g_completion_free (priv->completion);

	tp_clear_pointer (&priv->highlight_regex, g_regex_unref);

	G_OBJECT_CLASS (empathy_chat_parent_class)->finalize (object);
}

static void
chat_constructed (GObject *object)
{
	EmpathyChat *chat = EMPATHY_CHAT (object);
	EmpathyChatPriv *priv = GET_PRIV (chat);
	TplEntity *target;

	if (priv->tp_chat != NULL) {
		TpChannel *channel = TP_CHANNEL (priv->tp_chat);
		TpConnection *conn = tp_channel_get_connection (channel);
		gboolean supports_avatars =
			tp_proxy_has_interface_by_id (conn,
						      TP_IFACE_QUARK_CONNECTION_INTERFACE_AVATARS);

		empathy_theme_adium_set_show_avatars (chat->view,
						    supports_avatars);
	}

	/* Add messages from last conversations. Backlog messages are always
	 * prepended and pending messages are appended, so we can do both
	 * independently. Hacks like we previously had for bug #603980 are no
	 * longer needed. Pending messages are handled within
	 * empathy_chat_set_tp_chat() so we don't have to care about them here.
	 */
	if (priv->handle_type == TP_HANDLE_TYPE_ROOM)
		target = tpl_entity_new_from_room_id (priv->id);
	else
		target = tpl_entity_new (priv->id, TPL_ENTITY_CONTACT, NULL, NULL);

	priv->log_walker = tpl_log_manager_walk_filtered_events (priv->log_manager, priv->account, target,
								 TPL_EVENT_MASK_TEXT, chat_log_filter, chat);
	g_object_unref (target);

	if (priv->handle_type != TP_HANDLE_TYPE_ROOM) {
		chat_add_logs (chat);
	}
}

static void
empathy_chat_class_init (EmpathyChatClass *klass)
{
	GObjectClass   *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = chat_finalize;
	object_class->get_property = chat_get_property;
	object_class->set_property = chat_set_property;
	object_class->constructed = chat_constructed;

	g_object_class_install_property (object_class,
					 PROP_TP_CHAT,
					 g_param_spec_object ("tp-chat",
							      "Empathy tp chat",
							      "The tp chat object",
							      EMPATHY_TYPE_TP_CHAT,
							      G_PARAM_CONSTRUCT |
							      G_PARAM_READWRITE |
							      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
					 PROP_ACCOUNT,
					 g_param_spec_object ("account",
							      "Account of the chat",
							      "The account of the chat",
							      TP_TYPE_ACCOUNT,
							      G_PARAM_READABLE |
							      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
					 PROP_ID,
					 g_param_spec_string ("id",
							      "Chat's id",
							      "The id of the chat",
							      NULL,
							      G_PARAM_READABLE |
							      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
					 PROP_NAME,
					 g_param_spec_string ("name",
							      "Chat's name",
							      "The name of the chat",
							      NULL,
							      G_PARAM_READABLE |
							      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
					 PROP_SUBJECT,
					 g_param_spec_string ("subject",
							      "Chat's subject",
							      "The subject or topic of the chat",
							      NULL,
							      G_PARAM_READABLE |
							      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
					 PROP_REMOTE_CONTACT,
					 g_param_spec_object ("remote-contact",
							      "The remote contact",
							      "The remote contact is any",
							      EMPATHY_TYPE_CONTACT,
							      G_PARAM_READABLE |
							      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
					 PROP_SHOW_CONTACTS,
					 g_param_spec_boolean ("show-contacts",
							       "Contacts' visibility",
							       "The visibility of the contacts' list",
							       TRUE,
							       G_PARAM_READWRITE |
							       G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class,
					 PROP_SMS_CHANNEL,
					 g_param_spec_boolean ("sms-channel",
						 	       "SMS Channel",
							       "TRUE if this channel is for sending SMSes",
							       FALSE,
							       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class,
					 PROP_N_MESSAGES_SENDING,
					 g_param_spec_uint ("n-messages-sending",
						 	    "Num Messages Sending",
							    "The number of messages being sent",
							    0, G_MAXUINT, 0,
							    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class,
					 PROP_NB_UNREAD_MESSAGES,
					 g_param_spec_uint ("nb-unread-messages",
							    "Num Unread Messages",
							    "The number of unread messages",
							    0, G_MAXUINT, 0,
							    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

	signals[COMPOSING] =
		g_signal_new ("composing",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      0,
			      NULL, NULL,
			      g_cclosure_marshal_generic,
			      G_TYPE_NONE,
			      1, G_TYPE_BOOLEAN);

	/**
	 * EmpathyChat::new-message:
	 * @self: the #EmpathyChat
	 * @message: the new message
	 * @pending: whether the message was in the pending queue when @self
	 *  was created
	 * @should_highlight: %TRUE if the message mentions the local user
	 */
	signals[NEW_MESSAGE] =
		g_signal_new ("new-message",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      0,
			      NULL, NULL,
			      g_cclosure_marshal_generic,
			      G_TYPE_NONE,
			      3, EMPATHY_TYPE_MESSAGE, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);

	signals[PART_COMMAND_ENTERED] =
			g_signal_new ("part-command-entered",
				  G_OBJECT_CLASS_TYPE (object_class),
				  G_SIGNAL_RUN_LAST,
				  0,
				  NULL, NULL,
				  g_cclosure_marshal_generic,
				  G_TYPE_NONE,
				  1, G_TYPE_STRV);

	g_type_class_add_private (object_class, sizeof (EmpathyChatPriv));
}

static gboolean
chat_block_events_timeout_cb (gpointer data)
{
	EmpathyChatPriv *priv = GET_PRIV (data);

	priv->block_events_timeout_id = 0;

	return FALSE;
}

static void
account_manager_prepared_cb (GObject *source_object,
			     GAsyncResult *result,
			     gpointer user_data)
{
	GList *accounts, *l;
	TpAccountManager *account_manager = TP_ACCOUNT_MANAGER (source_object);
	EmpathyChat *chat = user_data;
	GError *error = NULL;

	if (!tp_proxy_prepare_finish (account_manager, result, &error)) {
		DEBUG ("Failed to prepare the account manager: %s", error->message);
		g_error_free (error);
		return;
	}

	accounts = tp_account_manager_dup_valid_accounts (account_manager);

	for (l = accounts; l != NULL; l = l->next) {
		TpAccount *account = l->data;
		tp_g_signal_connect_object (account, "status-changed",
					     G_CALLBACK (chat_new_connection_cb),
					     chat, 0);
	}

	g_list_free_full (accounts, g_object_unref);
}

static void
empathy_chat_init (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = G_TYPE_INSTANCE_GET_PRIVATE (chat,
		EMPATHY_TYPE_CHAT, EmpathyChatPriv);

	chat->priv = priv;
	priv->log_manager = tpl_log_manager_dup_singleton ();
	priv->gsettings_chat = g_settings_new (EMPATHY_PREFS_CHAT_SCHEMA);
	priv->gsettings_ui = g_settings_new (EMPATHY_PREFS_UI_SCHEMA);

	priv->contacts_width = g_settings_get_int (priv->gsettings_ui,
		EMPATHY_PREFS_UI_CHAT_WINDOW_PANED_POS);
	priv->input_history = NULL;
	priv->input_history_current = NULL;
	priv->account_manager = tp_account_manager_dup ();

	tp_proxy_prepare_async (priv->account_manager, NULL,
					  account_manager_prepared_cb, chat);

	priv->show_contacts = g_settings_get_boolean (priv->gsettings_chat,
			EMPATHY_PREFS_CHAT_SHOW_CONTACTS_IN_ROOMS);

	/* Block events for some time to avoid having "has come online" or
	 * "joined" messages. */
	priv->block_events_timeout_id =
		g_timeout_add_seconds (1, chat_block_events_timeout_cb, chat);

	/* Add nick name completion */
	priv->completion = g_completion_new ((GCompletionFunc) empathy_contact_get_alias);
	g_completion_set_compare (priv->completion, chat_contacts_completion_func);

	/* Create UI early so by the time empathy_chat_set_tp_chat() is called
	 * (construct property) the view will already exists to receive pending
	 * messages. */
	chat_create_ui (chat);
}

EmpathyChat *
empathy_chat_new (EmpathyTpChat *tp_chat)
{
	return g_object_new (EMPATHY_TYPE_CHAT, "tp-chat", tp_chat, NULL);
}

EmpathyTpChat *
empathy_chat_get_tp_chat (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), NULL);

	return priv->tp_chat;
}

typedef struct
{
	EmpathyChat *self;
	GtkWidget *info_bar;
	gulong response_id;
	GtkWidget *button;
	GtkWidget *label;
	GtkWidget *entry;
	GtkWidget *spinner;
	gchar *password;
} PasswordData;

static void
passwd_remember_button_cb (GtkButton *button,
			  PasswordData *data)
{
	gtk_info_bar_response (GTK_INFO_BAR (data->info_bar), GTK_RESPONSE_OK);
}

static void
passwd_not_now_button_cb (GtkButton *button,
			  PasswordData *data)
{
	gtk_info_bar_response (GTK_INFO_BAR (data->info_bar), GTK_RESPONSE_NO);
}

static void
remember_password_infobar_response_cb (GtkWidget *info_bar,
				       gint response_id,
				       PasswordData *data)
{
	EmpathyChatPriv *priv = GET_PRIV (data->self);

	if (response_id == GTK_RESPONSE_OK) {
		DEBUG ("Saving room password");
		tpaw_keyring_set_room_password_async (priv->account,
							 empathy_tp_chat_get_id (priv->tp_chat),
							 data->password,
							 NULL, NULL);
	}

	gtk_widget_destroy (info_bar);
	g_free (data->password);
	g_slice_free (PasswordData, data);
}

static void
chat_prompt_to_save_password (EmpathyChat *self,
			      PasswordData *data)
{
	GtkWidget *content_area;
	GtkWidget *hbox;
	GtkWidget *image;
	GtkWidget *label;
	GtkWidget *alig;
	GtkWidget *button;

	/* save the password in case it needs to be saved */
	data->password = g_strdup (gtk_entry_get_text (GTK_ENTRY (data->entry)));

	/* Remove all previous widgets */
	content_area = gtk_info_bar_get_content_area (GTK_INFO_BAR (data->info_bar));
	gtk_container_forall (GTK_CONTAINER (content_area),
			      (GtkCallback) gtk_widget_destroy, NULL);
	data->button = NULL;
	data->label = NULL;
	data->entry = NULL;
	data->spinner = NULL;

	gtk_info_bar_set_message_type (GTK_INFO_BAR (data->info_bar),
				       GTK_MESSAGE_QUESTION);

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 5);
	gtk_box_pack_start (GTK_BOX (content_area), hbox, TRUE, TRUE, 0);

	/* Add image */
	image = gtk_image_new_from_stock (GTK_STOCK_DIALOG_AUTHENTICATION,
					  GTK_ICON_SIZE_DIALOG);
	gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);

	/* Add message */
	label = gtk_label_new (_("Would you like to store this password?"));
	gtk_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 0);

	/* Add 'Remember' button */
	alig = gtk_alignment_new (0, 0.5, 1, 0);

	button = gtk_button_new_with_label (_("Remember"));
	gtk_container_add (GTK_CONTAINER (alig), button);
	gtk_box_pack_start (GTK_BOX (hbox), alig, FALSE, FALSE, 0);

	g_signal_connect (button, "clicked", G_CALLBACK (passwd_remember_button_cb),
			  data);

	/* Add 'Not now' button */
	alig = gtk_alignment_new (0, 0.5, 1, 0);

	button = gtk_button_new_with_label (_("Not now"));
	gtk_container_add (GTK_CONTAINER (alig), button);
	gtk_box_pack_start (GTK_BOX (hbox), alig, FALSE, FALSE, 0);

	g_signal_connect (button, "clicked", G_CALLBACK (passwd_not_now_button_cb),
			  data);

	/* go! */
	g_signal_handler_disconnect (data->info_bar, data->response_id);
	g_signal_connect (data->info_bar, "response",
			  G_CALLBACK (remember_password_infobar_response_cb), data);

	gtk_widget_show_all (data->info_bar);
}

static void
provide_password_cb (GObject *tp_chat,
		     GAsyncResult *res,
		     gpointer user_data)
{
	PasswordData *data = user_data;
	EmpathyChat *self = data->self;
	EmpathyChatPriv *priv = GET_PRIV (self);
	GError *error = NULL;

	if (!tp_channel_provide_password_finish (TP_CHANNEL (tp_chat), res,
						      &error)) {
		DEBUG ("error: %s", error->message);
		/* FIXME: what should we do if that's another error? Close the channel?
		 * Display the raw D-Bus error to the user isn't very useful */
		if (g_error_matches (error, TP_ERROR, TP_ERROR_AUTHENTICATION_FAILED)) {
			/* entry */
			gtk_entry_set_text (GTK_ENTRY (data->entry), "");
			gtk_widget_set_sensitive (data->entry, TRUE);
			gtk_widget_grab_focus (data->entry);

			/* info bar */
			gtk_info_bar_set_message_type (
			    GTK_INFO_BAR (data->info_bar),
			    GTK_MESSAGE_ERROR);

			/* button */
			gtk_widget_set_sensitive (data->button, TRUE);
			gtk_button_set_label (GTK_BUTTON (data->button),
			    _("Retry"));

			/* label */
			gtk_label_set_text (GTK_LABEL (data->label),
			    _("Wrong password; please try again:"));

			/* spinner */
			gtk_spinner_stop (GTK_SPINNER (data->spinner));
			gtk_widget_hide (data->spinner);
		}
		g_error_free (error);
		return;
	}

    /* ask whether they want to save the password */
    chat_prompt_to_save_password (self, data);

	/* Room joined */
	gtk_widget_set_sensitive (priv->hpaned, TRUE);
	gtk_widget_set_sensitive (self->input_text_view, TRUE);
	gtk_widget_grab_focus (self->input_text_view);
}

static void
password_infobar_response_cb (GtkWidget *info_bar,
			      gint response_id,
			      PasswordData *data)
{
	EmpathyChatPriv *priv = GET_PRIV (data->self);
	const gchar *password;

	if (response_id != GTK_RESPONSE_OK) {
		gtk_widget_destroy (info_bar);
		g_slice_free (PasswordData, data);
		return;
	}

	password = gtk_entry_get_text (GTK_ENTRY (data->entry));

	tp_channel_provide_password_async (TP_CHANNEL (priv->tp_chat), password,
						provide_password_cb, data);

	gtk_widget_set_sensitive (data->button, FALSE);
	gtk_widget_set_sensitive (data->entry, FALSE);

	gtk_spinner_start (GTK_SPINNER (data->spinner));
	gtk_widget_show (data->spinner);
}

static void
password_entry_activate_cb (GtkWidget *entry,
			  PasswordData *data)
{
	gtk_info_bar_response (GTK_INFO_BAR (data->info_bar), GTK_RESPONSE_OK);
}

static void
passwd_join_button_cb (GtkButton *button,
			  PasswordData *data)
{
	gtk_info_bar_response (GTK_INFO_BAR (data->info_bar), GTK_RESPONSE_OK);
}

static void
clear_icon_released_cb (GtkEntry *entry,
			GtkEntryIconPosition icon_pos,
			GdkEvent *event,
			PasswordData *data)
{
	gtk_entry_set_text (entry, "");
}

static void
password_entry_changed_cb (GtkEditable *entry,
			   PasswordData *data)
{
	const gchar *str;

	str = gtk_entry_get_text (GTK_ENTRY (entry));

	gtk_entry_set_icon_sensitive (GTK_ENTRY (entry),
	    GTK_ENTRY_ICON_SECONDARY, !TPAW_STR_EMPTY (str));
}

static void
infobar_chat_invalidated_cb (TpProxy       *proxy,
			   guint          domain,
			   gint           code,
			   gchar         *message,
			   gpointer       password_infobar)
{
	/* Destroy the password infobar whenever a channel is invalidated
	 * so we don't have multiple infobars when the MUC is rejoined */
	gtk_widget_destroy (GTK_WIDGET (password_infobar));
}

static void
display_password_info_bar (EmpathyChat *self)
{
	EmpathyChatPriv *priv = GET_PRIV (self);
	GtkWidget *info_bar;
	GtkWidget *content_area;
	GtkWidget *hbox;
	GtkWidget *image;
	GtkWidget *label;
	GtkWidget *entry;
	GtkWidget *alig;
	GtkWidget *button;
	GtkWidget *spinner;
	PasswordData *data;

	data = g_slice_new0 (PasswordData);

	info_bar = gtk_info_bar_new ();
	gtk_info_bar_set_message_type (GTK_INFO_BAR (info_bar),
	    GTK_MESSAGE_QUESTION);

	content_area = gtk_info_bar_get_content_area (GTK_INFO_BAR (info_bar));

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 5);
	gtk_box_pack_start (GTK_BOX (content_area), hbox, TRUE, TRUE, 0);

	/* Add image */
	image = gtk_image_new_from_stock (GTK_STOCK_DIALOG_AUTHENTICATION,
					  GTK_ICON_SIZE_DIALOG);
	gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);

	/* Add message */
	label = gtk_label_new (_("This room is protected by a password:"));
	gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);

	/* Add password entry */
	entry = gtk_entry_new ();
	gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
	gtk_box_pack_start (GTK_BOX (hbox), entry, TRUE, TRUE, 0);

	gtk_entry_set_icon_from_stock (GTK_ENTRY (entry),
				       GTK_ENTRY_ICON_SECONDARY, GTK_STOCK_CLEAR);
	gtk_entry_set_icon_sensitive (GTK_ENTRY (entry),
				      GTK_ENTRY_ICON_SECONDARY, FALSE);

	g_signal_connect (entry, "icon-release",
			  G_CALLBACK (clear_icon_released_cb), data);
	g_signal_connect (entry, "changed",
			  G_CALLBACK (password_entry_changed_cb), data);

	g_signal_connect (entry, "activate",
			  G_CALLBACK (password_entry_activate_cb), data);

	/* Focus the password entry once it's realized */
	g_signal_connect (entry, "realize", G_CALLBACK (gtk_widget_grab_focus), NULL);

	/* Add 'Join' button */
	alig = gtk_alignment_new (0, 0.5, 1, 0);

	button = gtk_button_new_with_label (_("Join"));
	gtk_container_add (GTK_CONTAINER (alig), button);
	gtk_box_pack_start (GTK_BOX (hbox), alig, FALSE, FALSE, 0);

	g_signal_connect (button, "clicked", G_CALLBACK (passwd_join_button_cb),
			  data);

	/* Add spinner */
	spinner = gtk_spinner_new ();
	gtk_box_pack_end (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);

	/* Save some data for messing around with later */
	data->self = self;
	data->info_bar = info_bar;
	data->button = button;
	data->label = label;
	data->entry = entry;
	data->spinner = spinner;

	gtk_box_pack_start (GTK_BOX (priv->info_bar_vbox), info_bar,
			    TRUE, TRUE, 3);
	gtk_widget_show_all (hbox);

	tp_g_signal_connect_object (priv->tp_chat,
				  "invalidated", G_CALLBACK (infobar_chat_invalidated_cb),
				  info_bar, 0);

	data->response_id = g_signal_connect (info_bar, "response",
					      G_CALLBACK (password_infobar_response_cb), data);

	gtk_widget_show_all (info_bar);
	/* ... but hide the spinner */
	gtk_widget_hide (spinner);

	/* prevent the user from typing anything */
	gtk_widget_set_sensitive (self->input_text_view, FALSE);
}

static void
provide_saved_password_cb (GObject *tp_chat,
			   GAsyncResult *res,
			   gpointer user_data)
{
	EmpathyChat *self = user_data;
	EmpathyChatPriv *priv = GET_PRIV (self);
	GError *error = NULL;

	if (!tp_channel_provide_password_finish (TP_CHANNEL (tp_chat), res,
						      &error)) {
		DEBUG ("error: %s", error->message);
		/* FIXME: what should we do if that's another error? Close the channel?
		 * Display the raw D-Bus error to the user isn't very useful */
		if (g_error_matches (error, TP_ERROR, TP_ERROR_AUTHENTICATION_FAILED)) {
			display_password_info_bar (self);
			gtk_widget_set_sensitive (priv->hpaned, FALSE);
		}
		g_error_free (error);
		return;
	}

	/* Room joined */
	gtk_widget_set_sensitive (priv->hpaned, TRUE);
	gtk_widget_grab_focus (self->input_text_view);
}

static void
chat_room_got_password_cb (GObject *source,
			   GAsyncResult *result,
			   gpointer user_data)
{
	EmpathyChat *self = user_data;
	EmpathyChatPriv *priv = GET_PRIV (self);
	const gchar *password;
	GError *error = NULL;

	password = tpaw_keyring_get_room_password_finish (priv->account,
	    result, &error);

	if (error != NULL) {
		DEBUG ("Couldn't get room password: %s\n", error->message);
		g_clear_error (&error);

		display_password_info_bar (self);
		gtk_widget_set_sensitive (priv->hpaned, FALSE);
		return;
	}

	tp_channel_provide_password_async (TP_CHANNEL (priv->tp_chat), password,
						provide_saved_password_cb, self);
}

static void
chat_password_needed_changed_cb (EmpathyChat *self)
{
	EmpathyChatPriv *priv = GET_PRIV (self);

	if (tp_channel_password_needed (TP_CHANNEL (priv->tp_chat))) {
		tpaw_keyring_get_room_password_async (priv->account,
							 empathy_tp_chat_get_id (priv->tp_chat),
							 chat_room_got_password_cb, self);
	}
}

static void
chat_sms_channel_changed_cb (EmpathyChat *self)
{
	EmpathyChatPriv *priv = GET_PRIV (self);

	priv->sms_channel = tp_text_channel_is_sms_channel (
		(TpTextChannel *) priv->tp_chat);
	g_object_notify (G_OBJECT (self), "sms-channel");
}

static void
chat_n_messages_sending_changed_cb (EmpathyChat *self)
{
	g_object_notify (G_OBJECT (self), "n-messages-sending");
}

void
empathy_chat_set_tp_chat (EmpathyChat   *chat,
			  EmpathyTpChat *tp_chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	g_return_if_fail (EMPATHY_IS_CHAT (chat));
	g_return_if_fail (EMPATHY_IS_TP_CHAT (tp_chat));

	if (priv->tp_chat) {
		return;
	}

	if (priv->account) {
		g_object_unref (priv->account);
	}

	priv->tp_chat = g_object_ref (tp_chat);
	priv->account = g_object_ref (empathy_tp_chat_get_account (priv->tp_chat));

	g_signal_connect (tp_chat, "invalidated",
			  G_CALLBACK (chat_invalidated_cb),
			  chat);
	g_signal_connect (tp_chat, "message-received-empathy",
			  G_CALLBACK (chat_message_received_cb),
			  chat);
	g_signal_connect (tp_chat, "message_acknowledged",
			  G_CALLBACK (chat_message_acknowledged_cb),
			  chat);
	g_signal_connect (tp_chat, "send-error",
			  G_CALLBACK (chat_send_error_cb),
			  chat);
	g_signal_connect (tp_chat, "contact-chat-state-changed",
			  G_CALLBACK (chat_state_changed_cb),
			  chat);
	g_signal_connect (tp_chat, "members-changed",
			  G_CALLBACK (chat_members_changed_cb),
			  chat);
	g_signal_connect (tp_chat, "member-renamed",
			  G_CALLBACK (chat_member_renamed_cb),
			  chat);
	g_signal_connect_swapped (tp_chat, "notify::self-contact",
				  G_CALLBACK (chat_self_contact_changed_cb),
				  chat);
	g_signal_connect_swapped (tp_chat, "notify::remote-contact",
				  G_CALLBACK (chat_remote_contact_changed_cb),
				  chat);
	g_signal_connect_swapped (tp_chat, "notify::password-needed",
				  G_CALLBACK (chat_password_needed_changed_cb),
				  chat);
	g_signal_connect_swapped (tp_chat, "notify::is-sms-channel",
				  G_CALLBACK (chat_sms_channel_changed_cb),
				  chat);
	g_signal_connect_swapped (tp_chat, "notify::n-messages-sending",
				  G_CALLBACK (chat_n_messages_sending_changed_cb),
				  chat);
	g_signal_connect_swapped (tp_chat, "notify::title",
				  G_CALLBACK (chat_title_changed_cb),
				  chat);
	g_signal_connect_swapped (tp_chat, "notify::subject",
				  G_CALLBACK (chat_subject_changed_cb),
				  chat);

	/* Get initial value of properties */
	chat_sms_channel_changed_cb (chat);
	chat_self_contact_changed_cb (chat);
	chat_remote_contact_changed_cb (chat);
	chat_title_changed_cb (chat);
	chat_subject_changed_cb (chat);

	if (chat->input_text_view) {
		gtk_widget_set_sensitive (chat->input_text_view, TRUE);
		if (priv->block_events_timeout_id == 0) {
			empathy_theme_adium_append_event (chat->view, _("Connected"));
		}
	}

	g_object_notify (G_OBJECT (chat), "tp-chat");
	g_object_notify (G_OBJECT (chat), "id");
	g_object_notify (G_OBJECT (chat), "account");

	show_pending_messages (chat);

	/* check if a password is needed */
	chat_password_needed_changed_cb (chat);
}

TpAccount *
empathy_chat_get_account (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), NULL);

	return priv->account;
}

const gchar *
empathy_chat_get_id (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), NULL);

	return priv->id;
}

gchar *
empathy_chat_dup_name (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	const gchar *ret;

	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), NULL);

	ret = priv->name;

	if (!ret && priv->remote_contact) {
		ret = empathy_contact_get_alias (priv->remote_contact);
	}

	if (!ret)
		ret = priv->id;

	if (!ret)
		ret = _("Conversation");

	if (priv->sms_channel)
		/* Translators: this string is a something like
		 * "Escher Cat (SMS)" */
		return g_strdup_printf (_("%s (SMS)"), ret);
	else
		return g_strdup (ret);
}

const gchar *
empathy_chat_get_subject (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), NULL);

	return priv->subject;
}

EmpathyContact *
empathy_chat_get_remote_contact (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), NULL);

	return priv->remote_contact;
}

GtkWidget *
empathy_chat_get_contact_menu (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	GtkWidget       *menu = NULL;
	FolksIndividual *individual;
	TpContact *contact;

	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), NULL);

	if (priv->remote_contact == NULL)
		return NULL;

	contact = empathy_contact_get_tp_contact (priv->remote_contact);
	if (contact == NULL)
		return NULL;

	individual = empathy_ensure_individual_from_tp_contact (contact);

	if (individual == NULL)
		return NULL;

	menu = empathy_individual_menu_new (individual, NULL,
					 EMPATHY_INDIVIDUAL_FEATURE_CALL |
					 EMPATHY_INDIVIDUAL_FEATURE_LOG |
					 EMPATHY_INDIVIDUAL_FEATURE_INFO |
					 EMPATHY_INDIVIDUAL_FEATURE_BLOCK, NULL);

	g_object_unref (individual);

	return menu;
}

void
empathy_chat_clear (EmpathyChat *chat)
{
	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	empathy_theme_adium_clear (chat->view);
}

void
empathy_chat_scroll_down (EmpathyChat *chat)
{
	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	empathy_theme_adium_scroll_down (chat->view);
}

void
empathy_chat_cut (EmpathyChat *chat)
{
	GtkTextBuffer *buffer;

	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));
	if (gtk_text_buffer_get_has_selection (buffer)) {
		GtkClipboard *clipboard;

		clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);

		gtk_text_buffer_cut_clipboard (buffer, clipboard, TRUE);
	}
}

static gboolean
copy_from_chat_view (EmpathyChat *chat)
{
	if (!empathy_theme_adium_get_has_selection (chat->view))
		return FALSE;

	empathy_theme_adium_copy_clipboard (chat->view);
	return TRUE;
}

static gboolean
copy_from_input (EmpathyChat *chat)
{
	GtkTextBuffer *buffer;
	GtkClipboard *clipboard;

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));
	if (!gtk_text_buffer_get_has_selection (buffer))
		return FALSE;

	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
	gtk_text_buffer_copy_clipboard (buffer, clipboard);
	return TRUE;
}

static gboolean
copy_from_topic (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);
	gint start_offset;
	gint end_offset;
	gchar *start;
	gchar *end;
	gchar *selection;
	const gchar *topic;
	GtkClipboard *clipboard;

	if (!gtk_label_get_selection_bounds (GTK_LABEL (priv->label_topic),
						       &start_offset,
						       &end_offset))
		return FALSE;

	topic = gtk_label_get_text (GTK_LABEL (priv->label_topic));
	start = g_utf8_offset_to_pointer (topic, start_offset);
	end = g_utf8_offset_to_pointer (topic, end_offset);
	selection = g_strndup (start, end - start);

	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_set_text (clipboard, selection, -1);

	g_free (selection);
	return TRUE;
}

void
empathy_chat_copy (EmpathyChat *chat)
{
	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	if (copy_from_chat_view (chat))
		return;

	if (copy_from_input (chat))
		return;

	copy_from_topic (chat);
}

void
empathy_chat_paste (EmpathyChat *chat)
{
	GtkTextBuffer *buffer;
	GtkClipboard  *clipboard;
	EmpathyChatPriv *priv;

	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	priv = GET_PRIV (chat);

	if (gtk_widget_get_visible (priv->search_bar)) {
		empathy_search_bar_paste_clipboard (EMPATHY_SEARCH_BAR (priv->search_bar));
		return;
	}

	if (priv->tp_chat == NULL ||
	    !gtk_widget_is_sensitive (chat->input_text_view))
		return;

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));
	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);

	gtk_text_buffer_paste_clipboard (buffer, clipboard, NULL, TRUE);
}

void
empathy_chat_find (EmpathyChat *chat)
{
	EmpathyChatPriv *priv;

	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	priv = GET_PRIV (chat);

	empathy_search_bar_show (EMPATHY_SEARCH_BAR (priv->search_bar));
}

void
empathy_chat_correct_word (EmpathyChat  *chat,
			  GtkTextIter *start,
			  GtkTextIter *end,
			  const gchar *new_word)
{
	GtkTextBuffer *buffer;

	g_return_if_fail (chat != NULL);
	g_return_if_fail (new_word != NULL);

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));

	gtk_text_buffer_delete (buffer, start, end);
	gtk_text_buffer_insert (buffer, start,
				new_word,
				-1);
}

gboolean
empathy_chat_is_room (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), FALSE);

	return (priv->handle_type == TP_HANDLE_TYPE_ROOM);
}

gboolean
empathy_chat_is_highlighted (EmpathyChat *chat)
{
	EmpathyChatPriv *priv = GET_PRIV (chat);

	g_return_val_if_fail (EMPATHY_IS_CHAT (chat), FALSE);

	return priv->highlighted;
}

guint
empathy_chat_get_nb_unread_messages (EmpathyChat *self)
{
	EmpathyChatPriv *priv = GET_PRIV (self);

	g_return_val_if_fail (EMPATHY_IS_CHAT (self), 0);

	return priv->unread_messages;
}

/* called when the messages have been read by user */
void
empathy_chat_messages_read (EmpathyChat *self)
{
	EmpathyChatPriv *priv = GET_PRIV (self);

	g_return_if_fail (EMPATHY_IS_CHAT (self));

	/* FIXME: See Bug#610994, See comments about it in EmpathyChatPriv
	 * definition. If we are still retrieving the backlogs, do not ACK */
	if (priv->retrieving_backlogs)
		return;

	if (priv->tp_chat != NULL) {
		tp_text_channel_ack_all_pending_messages_async (
			TP_TEXT_CHANNEL (priv->tp_chat), NULL, NULL);
	}

	priv->highlighted = FALSE;

	if (priv->unread_messages_when_offline > 0) {
		/* We can't ack those as the connection has gone away so just consider
		* them as read. */
		priv->unread_messages -= priv->unread_messages_when_offline;
		g_object_notify (G_OBJECT (self), "nb-unread-messages");
		priv->unread_messages_when_offline = 0;
	}
}

/* Return TRUE if on of the contacts in this chat is composing */
gboolean
empathy_chat_is_composing (EmpathyChat *chat)
{
  return chat->priv->compositors != NULL;
}

gboolean
empathy_chat_is_sms_channel (EmpathyChat *self)
{
	EmpathyChatPriv *priv = GET_PRIV (self);

	g_return_val_if_fail (EMPATHY_IS_CHAT (self), 0);

	return priv->sms_channel;
}

guint
empathy_chat_get_n_messages_sending (EmpathyChat *self)
{
	EmpathyChatPriv *priv;

	g_return_val_if_fail (EMPATHY_IS_CHAT (self), 0);

	priv = GET_PRIV (self);

	if (priv->tp_chat == NULL) {
		return 0;
	} else {
		guint n_messages;

		g_object_get (priv->tp_chat,
			"n-messages-sending", &n_messages,
			NULL);

		return n_messages;
	}
}

gchar *
empathy_chat_dup_text (EmpathyChat *self)
{
	GtkTextBuffer *buffer;
	GtkTextIter     start, end;

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->input_text_view));

	gtk_text_buffer_get_bounds (buffer, &start, &end);
	return gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
}

void
empathy_chat_set_text (EmpathyChat *self,
		       const gchar *text)
{
	GtkTextBuffer *buffer;

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->input_text_view));

	gtk_text_buffer_set_text (buffer, text, -1);
}
