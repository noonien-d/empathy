/*
 * Copyright (C) 2006-2007 Imendio AB
 * Copyright (C) 2007-2011 Collabora Ltd.
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
 * Authors: Martyn Russell <martyn@imendio.com>
 *          Xavier Claessens <xclaesse@gmail.com>
 */

#include "config.h"
#include "empathy-new-chatroom-dialog.h"

#include <glib/gi18n.h>
#include <tp-account-widgets/tpaw-builder.h>
#include <tp-account-widgets/tpaw-utils.h>

#include "empathy-account-chooser.h"
#include "empathy-gsettings.h"
#include "empathy-request-util.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

G_DEFINE_TYPE (EmpathyNewChatroomDialog, empathy_new_chatroom_dialog,
    GTK_TYPE_DIALOG)

struct _EmpathyNewChatroomDialogPriv
{
  TpRoomList *room_list;
  /* Currently selected account */
  TpAccount *account;
  /* Signal id of the "status-changed" signal connected on the currently
   * selected account */
  gulong status_changed_id;

  GtkWidget *table_grid;
  GtkWidget *label_account;
  GtkWidget *account_chooser;
  GtkWidget *label_server;
  GtkWidget *entry_server;
  GtkWidget *label_room;
  GtkWidget *entry_room;
  GtkWidget *expander_browse;
  GtkWidget *hbox_expander;
  GtkWidget *throbber;
  GtkWidget *treeview;
  GtkTreeModel *model;
  GtkWidget *button_join;
  GtkWidget *label_error_message;
  GtkWidget *viewport_error;

  GSettings *gsettings;
};

enum
{
  COL_NEED_PASSWORD,
  COL_INVITE_ONLY,
  COL_NAME,
  COL_ROOM,
  COL_MEMBERS,
  COL_MEMBERS_INT,
  COL_TOOLTIP,
  COL_COUNT
};

static EmpathyNewChatroomDialog *dialog_p = NULL;

static void
new_chatroom_dialog_store_last_account (GSettings *gsettings,
    EmpathyAccountChooser *account_chooser)
{
  TpAccount *account;
  const char *account_path;

  account = empathy_account_chooser_get_account (account_chooser);
  if (account == NULL)
    return;

  account_path = tp_proxy_get_object_path (account);
  DEBUG ("Storing account path '%s'", account_path);

  g_settings_set (gsettings, EMPATHY_PREFS_CHAT_ROOM_LAST_ACCOUNT,
      "o", account_path);
}

static void
new_chatroom_dialog_join (EmpathyNewChatroomDialog *self)
{
  EmpathyAccountChooser *account_chooser;
  TpAccount *account;
  const gchar *room;
  const gchar *server = NULL;
  gchar *room_name = NULL;

  room = gtk_entry_get_text (GTK_ENTRY (self->priv->entry_room));
  server = gtk_entry_get_text (GTK_ENTRY (self->priv->entry_server));

  account_chooser = EMPATHY_ACCOUNT_CHOOSER (self->priv->account_chooser);
  account = empathy_account_chooser_get_account (account_chooser);

  if (!TPAW_STR_EMPTY (server))
    room_name = g_strconcat (room, "@", server, NULL);
  else
    room_name = g_strdup (room);

  g_strstrip (room_name);

  DEBUG ("Requesting channel for '%s'", room_name);

  empathy_join_muc (account, room_name, empathy_get_current_action_time ());

  g_free (room_name);
}

static void
empathy_new_chatroom_dialog_response (GtkDialog *dialog,
    gint response)
{
  EmpathyNewChatroomDialog *self = EMPATHY_NEW_CHATROOM_DIALOG (dialog);

  if (response == GTK_RESPONSE_OK)
    {
      new_chatroom_dialog_join (self);
      new_chatroom_dialog_store_last_account (self->priv->gsettings,
          EMPATHY_ACCOUNT_CHOOSER (self->priv->account_chooser));
    }

  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
new_chatroom_dialog_model_add_columns (EmpathyNewChatroomDialog *self)
{
  GtkTreeView *view;
  GtkTreeViewColumn *column;
  GtkCellRenderer *cell;
  gint width, height;

  gtk_icon_size_lookup (GTK_ICON_SIZE_MENU, &width, &height);

  view = GTK_TREE_VIEW (self->priv->treeview);

  cell = gtk_cell_renderer_pixbuf_new ();
  g_object_set (cell,
      "width", width,
      "height", height,
      "stock-size", GTK_ICON_SIZE_MENU,
      NULL);

  column = gtk_tree_view_column_new_with_attributes (NULL, cell,
      "stock-id", COL_INVITE_ONLY, NULL);

  gtk_tree_view_column_set_sort_column_id (column, COL_INVITE_ONLY);
  gtk_tree_view_append_column (view, column);

  column = gtk_tree_view_column_new_with_attributes (NULL, cell,
      "stock-id", COL_NEED_PASSWORD, NULL);

  gtk_tree_view_column_set_sort_column_id (column, COL_NEED_PASSWORD);
  gtk_tree_view_append_column (view, column);

  cell = gtk_cell_renderer_text_new ();
  g_object_set (cell,
          "xpad", (guint) 4,
          "ypad", (guint) 2,
          "ellipsize", PANGO_ELLIPSIZE_END,
          NULL);

  column = gtk_tree_view_column_new_with_attributes (_("Chat Room"), cell,
      "text", COL_NAME, NULL);

  gtk_tree_view_column_set_sort_column_id (column, COL_NAME);
  gtk_tree_view_column_set_expand (column, TRUE);
  gtk_tree_view_append_column (view, column);

  cell = gtk_cell_renderer_text_new ();
  g_object_set (cell,
      "xpad", (guint) 4,
      "ypad", (guint) 2,
      "ellipsize", PANGO_ELLIPSIZE_END,
      "alignment", PANGO_ALIGN_RIGHT,
      NULL);

  column = gtk_tree_view_column_new_with_attributes (_("Members"), cell,
      "text", COL_MEMBERS, NULL);

  gtk_tree_view_column_set_sort_column_id (column, COL_MEMBERS_INT);
  gtk_tree_view_append_column (view, column);
}

static void
new_chatroom_dialog_model_row_activated_cb (GtkTreeView *tree_view,
    GtkTreePath *path,
    GtkTreeViewColumn *column,
    EmpathyNewChatroomDialog *self)
{
  gtk_widget_activate (self->priv->button_join);
}

static void
new_chatroom_dialog_model_selection_changed (GtkTreeSelection *selection,
    EmpathyNewChatroomDialog *self)
{
  GtkTreeModel *model;
  GtkTreeIter iter;
  gchar *room = NULL;
  gchar *server = NULL;

  if (!gtk_tree_selection_get_selected (selection, &model, &iter))
    return;

  gtk_tree_model_get (model, &iter, COL_ROOM, &room, -1);
  server = strstr (room, "@");
  if (server)
    {
      *server = '\0';
      server++;
    }

  gtk_entry_set_text (GTK_ENTRY (self->priv->entry_server),
      server ? server : "");
  gtk_entry_set_text (GTK_ENTRY (self->priv->entry_room), room ? room : "");

  g_free (room);
}

static void
new_chatroom_dialog_model_setup (EmpathyNewChatroomDialog *self)
{
  GtkTreeView *view;
  GtkListStore *store;
  GtkTreeSelection *selection;

  /* View */
  view = GTK_TREE_VIEW (self->priv->treeview);

  g_signal_connect (view, "row-activated",
      G_CALLBACK (new_chatroom_dialog_model_row_activated_cb), self);

  /* Store/Model */
  store = gtk_list_store_new (COL_COUNT,
      G_TYPE_STRING,       /* Need password */
      G_TYPE_STRING,       /* Invite only */
      G_TYPE_STRING,       /* Name */
      G_TYPE_STRING,       /* Room */
      G_TYPE_STRING,       /* Member count */
      G_TYPE_INT,          /* Member count int */
      G_TYPE_STRING);      /* Tool tip */

  self->priv->model = GTK_TREE_MODEL (store);
  gtk_tree_view_set_model (view, self->priv->model);
  gtk_tree_view_set_tooltip_column (view, COL_TOOLTIP);
  gtk_tree_view_set_search_column (view, COL_NAME);

  /* Selection */
  selection = gtk_tree_view_get_selection (view);
  gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store),
      COL_NAME, GTK_SORT_ASCENDING);

  g_signal_connect (selection, "changed",
      G_CALLBACK (new_chatroom_dialog_model_selection_changed), self);

  /* Columns */
  new_chatroom_dialog_model_add_columns (self);
}

static void
update_join_button_sensitivity (EmpathyNewChatroomDialog *self)
{
  const gchar *room;
  const gchar *protocol;
  gboolean sensitive = FALSE;

  room = gtk_entry_get_text (GTK_ENTRY (self->priv->entry_room));
  protocol = tp_account_get_protocol_name (self->priv->account);
  if (TPAW_STR_EMPTY (room))
    goto out;

  if (!tp_strdiff (protocol, "irc") &&
      (!tp_strdiff (room, "#") || !tp_strdiff (room, "&")))
    goto out;

  if (self->priv->account == NULL)
    goto out;

  if (tp_account_get_connection_status (self->priv->account, NULL) !=
          TP_CONNECTION_STATUS_CONNECTED)
    goto out;

  sensitive = TRUE;

out:
  gtk_widget_set_sensitive (self->priv->button_join, sensitive);
}

static void
new_chatroom_dialog_update_widgets (EmpathyNewChatroomDialog *self)
{
  const gchar *protocol;

  if (self->priv->account == NULL)
    return;

  protocol = tp_account_get_protocol_name (self->priv->account);

  gtk_entry_set_text (GTK_ENTRY (self->priv->entry_server), "");

  /* hardcode here known protocols */
  if (strcmp (protocol, "jabber") == 0)
    gtk_widget_set_sensitive (self->priv->entry_server, TRUE);
  else if (strcmp (protocol, "local-xmpp") == 0)
    gtk_widget_set_sensitive (self->priv->entry_server, FALSE);
  else if (strcmp (protocol, "irc") == 0)
    gtk_widget_set_sensitive (self->priv->entry_server, FALSE);
  else
    gtk_widget_set_sensitive (self->priv->entry_server, TRUE);

  if (!tp_strdiff (protocol, "irc"))
    gtk_entry_set_text (GTK_ENTRY (self->priv->entry_room), "#");
  else
    gtk_entry_set_text (GTK_ENTRY (self->priv->entry_room), "");

  update_join_button_sensitivity (self);

  /* Final set up of the dialog */
  gtk_widget_grab_focus (self->priv->entry_room);
  gtk_editable_set_position (GTK_EDITABLE (self->priv->entry_room), -1);
}

static void
account_status_changed_cb (TpAccount *account,
    guint old_status,
    guint new_status,
    guint reason,
    gchar *dbus_error_name,
    GHashTable *details,
    EmpathyNewChatroomDialog *self)
{
  update_join_button_sensitivity (self);
}

static void
new_chatroom_dialog_select_last_account (GSettings *gsettings,
    EmpathyAccountChooser *account_chooser)
{
  const gchar *account_path;
  TpAccountManager *manager;
  TpSimpleClientFactory *factory;
  TpAccount *account;
  TpConnectionStatus status;

  account_path = g_settings_get_string (gsettings,
      EMPATHY_PREFS_CHAT_ROOM_LAST_ACCOUNT);
  DEBUG ("Selecting account path '%s'", account_path);

  manager =  tp_account_manager_dup ();
  factory = tp_proxy_get_factory (manager);
  account = tp_simple_client_factory_ensure_account (factory, account_path,
      NULL, NULL);

  if (account != NULL)
    {
      status = tp_account_get_connection_status (account, NULL);
      if (status == TP_CONNECTION_STATUS_CONNECTED)
        empathy_account_chooser_set_account (account_chooser, account);
      g_object_unref (account);
    }

  g_object_unref (manager);
}

static void
new_chatroom_dialog_account_ready_cb (EmpathyAccountChooser *combobox,
    EmpathyNewChatroomDialog *self)
{
  new_chatroom_dialog_select_last_account (self->priv->gsettings, combobox);
}

static void
listing_failed_cb (TpRoomList *room_list,
    GError *error,
    EmpathyNewChatroomDialog *self)
{
  gtk_label_set_text (GTK_LABEL (self->priv->label_error_message),
      _("Failed to list rooms"));
  gtk_widget_show_all (self->priv->viewport_error);
  gtk_widget_set_sensitive (self->priv->treeview, FALSE);
}

static void
new_chatroom_dialog_got_room_cb (TpRoomList *room_list,
    TpRoomInfo *room,
    EmpathyNewChatroomDialog *self)
{
  GtkListStore *store;
  gchar *members;
  gchar *tooltip;
  const gchar *need_password;
  const gchar *invite_only;
  gchar *tmp;

  if (tp_str_empty (tp_room_info_get_handle_name (room)))
    {
      DEBUG ("Room handle name is empty - Broken CM");
      return;
    }

  DEBUG ("New room listed: %s (%s)", tp_room_info_get_name (room),
      tp_room_info_get_handle_name (room));

  /* Add to model */
  store = GTK_LIST_STORE (self->priv->model);
  members = g_strdup_printf ("%d", tp_room_info_get_members_count (
        room, NULL));
  tmp = g_strdup_printf ("<b>%s</b>", tp_room_info_get_name (room));

  tooltip = g_strdup_printf (
      /* Translators: Room/Join's roomlist tooltip. Parameters are a channel name,
      yes/no, yes/no and a number. */
      _("%s\nInvite required: %s\nPassword required: %s\nMembers: %s"),
      tmp,
      tp_room_info_get_invite_only (room, NULL) ? _("Yes") : _("No"),
      tp_room_info_get_requires_password (room, NULL) ? _("Yes") : _("No"),
      members);
  g_free (tmp);

  invite_only = (tp_room_info_get_invite_only (room, NULL) ?
    GTK_STOCK_INDEX : NULL);
  need_password = (tp_room_info_get_requires_password (room, NULL) ?
    GTK_STOCK_DIALOG_AUTHENTICATION : NULL);

  gtk_list_store_insert_with_values (store, NULL, -1,
      COL_NEED_PASSWORD, need_password,
      COL_INVITE_ONLY, invite_only,
      COL_NAME, tp_room_info_get_name (room),
      COL_ROOM, tp_room_info_get_handle_name (room),
      COL_MEMBERS, members,
      COL_MEMBERS_INT, tp_room_info_get_members_count (room, NULL),
      COL_TOOLTIP, tooltip,
      -1);

  g_free (members);
  g_free (tooltip);
}

static void
new_chatroom_dialog_listing_cb (TpRoomList *room_list,
    GParamSpec *spec,
    EmpathyNewChatroomDialog *self)
{
  /* Update the throbber */
  if (tp_room_list_is_listing (room_list))
    {
      gtk_spinner_start (GTK_SPINNER (self->priv->throbber));
      gtk_widget_show (self->priv->throbber);
    }
  else
    {
      gtk_spinner_stop (GTK_SPINNER (self->priv->throbber));
      gtk_widget_hide (self->priv->throbber);
    }
}

static void
new_chatroom_dialog_model_clear (EmpathyNewChatroomDialog *self)
{
  GtkListStore *store;

  store = GTK_LIST_STORE (self->priv->model);
  gtk_list_store_clear (store);
}

static void
new_chatroom_dialog_browse_start (EmpathyNewChatroomDialog *self)
{
  new_chatroom_dialog_model_clear (self);

  if (self->priv->room_list != NULL)
    tp_room_list_start (self->priv->room_list);
}

static void
new_room_list_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyNewChatroomDialog *self = user_data;
  TpRoomList *room_list;
  GError *error = NULL;

  room_list = tp_room_list_new_finish (result, &error);
  if (room_list == NULL)
    {
      DEBUG ("Failed to create TpRoomList: %s\n", error->message);
      g_error_free (error);
      return;
    }

  if (tp_room_list_get_account (room_list) != self->priv->account)
    {
      /* Account changed while we were creating this TpRoomList */
      g_object_unref (room_list);
      return;
    }

  self->priv->room_list = room_list;

  tp_g_signal_connect_object (self->priv->room_list, "got-room",
      G_CALLBACK (new_chatroom_dialog_got_room_cb), self, 0);
  tp_g_signal_connect_object (self->priv->room_list, "failed",
      G_CALLBACK (listing_failed_cb), self, 0);
  tp_g_signal_connect_object (self->priv->room_list, "notify::listing",
      G_CALLBACK (new_chatroom_dialog_listing_cb), self, 0);

  if (gtk_expander_get_expanded (GTK_EXPANDER (self->priv->expander_browse)))
    {
      gtk_widget_hide (self->priv->viewport_error);
      gtk_widget_set_sensitive (self->priv->treeview, TRUE);
      new_chatroom_dialog_browse_start (self);
    }

  if (tp_room_list_is_listing (self->priv->room_list))
    {
      gtk_spinner_start (GTK_SPINNER (self->priv->throbber));
      gtk_widget_show (self->priv->throbber);
    }

  gtk_widget_set_sensitive (self->priv->expander_browse, TRUE);

  new_chatroom_dialog_update_widgets (self);
}

static void
new_chatroom_dialog_account_changed_cb (GtkComboBox *combobox,
    EmpathyNewChatroomDialog *self)
{
  EmpathyAccountChooser *account_chooser;
  TpConnection *connection;
  TpCapabilities *caps;

  g_clear_object (&self->priv->room_list);

  gtk_spinner_stop (GTK_SPINNER (self->priv->throbber));
  gtk_widget_hide (self->priv->throbber);
  new_chatroom_dialog_model_clear (self);

  if (self->priv->account != NULL)
    {
      g_signal_handler_disconnect (self->priv->account,
          self->priv->status_changed_id);
      g_object_unref (self->priv->account);
    }

  account_chooser = EMPATHY_ACCOUNT_CHOOSER (self->priv->account_chooser);
  self->priv->account = empathy_account_chooser_dup_account (account_chooser);
  connection = empathy_account_chooser_get_connection (account_chooser);
  if (self->priv->account == NULL)
    goto out;

  self->priv->status_changed_id = g_signal_connect (self->priv->account,
      "status-changed", G_CALLBACK (account_status_changed_cb), self);

  /* empathy_account_chooser_filter_supports_chatrooms ensures that the
  * account has a connection and CAPABILITIES has been prepared. */
  g_assert (connection != NULL);
  g_assert (tp_proxy_is_prepared (connection,
    TP_CONNECTION_FEATURE_CAPABILITIES));
  caps = tp_connection_get_capabilities (connection);

  if (tp_capabilities_supports_room_list (caps, NULL))
    {
      /* Roomlist channels are supported */
      tp_room_list_new_async (self->priv->account, NULL, new_room_list_cb,
          self);
    }

  gtk_widget_set_sensitive (self->priv->expander_browse, FALSE);

out:
  new_chatroom_dialog_update_widgets (self);
}

static void
new_chatroom_dialog_button_close_error_clicked_cb   (GtkButton *button,
    EmpathyNewChatroomDialog *self)
{
  gtk_widget_hide (self->priv->viewport_error);
}

static void
new_chatroom_dialog_entry_changed_cb (GtkWidget *entry,
    EmpathyNewChatroomDialog *self)
{
  if (entry == self->priv->entry_room)
    {
      update_join_button_sensitivity (self);

      /* FIXME: Select the room in the list */
    }
}

static void
new_chatroom_dialog_entry_server_activate_cb (GtkWidget *widget,
    EmpathyNewChatroomDialog *self)
{
  new_chatroom_dialog_browse_start (self);
}

static void
new_chatroom_dialog_expander_browse_activate_cb (GtkWidget *widget,
    EmpathyNewChatroomDialog *self)
{
  gboolean expanded;

  expanded = gtk_expander_get_expanded (GTK_EXPANDER (widget));
  if (expanded)
    {
      gtk_window_set_resizable (GTK_WINDOW (self), FALSE);
    }
  else
    {
      gtk_widget_hide (self->priv->viewport_error);
      gtk_widget_set_sensitive (self->priv->treeview, TRUE);
      new_chatroom_dialog_browse_start (self);
      gtk_window_set_resizable (GTK_WINDOW (self), TRUE);
    }
}

static gboolean
new_chatroom_dialog_entry_server_focus_out_cb (GtkWidget *widget,
    GdkEventFocus *event,
    EmpathyNewChatroomDialog *self)
{
  gboolean expanded;

  expanded = gtk_expander_get_expanded (
      GTK_EXPANDER (self->priv->expander_browse));
  if (expanded)
    new_chatroom_dialog_browse_start (self);

  return FALSE;
}

static GObject *
empathy_new_chatroom_dialog_constructor (GType type,
    guint n_props,
    GObjectConstructParam *props)
{
  GObject *retval;

  if (dialog_p)
    {
      retval = G_OBJECT (dialog_p);
      g_object_ref (retval);
    }
  else
    {
      retval = G_OBJECT_CLASS (
          empathy_new_chatroom_dialog_parent_class)->constructor (type,
            n_props, props);

      dialog_p = EMPATHY_NEW_CHATROOM_DIALOG (retval);
      g_object_add_weak_pointer (retval, (gpointer) &dialog_p);
    }

  return retval;
}

GtkWidget *
empathy_new_chatroom_dialog_show (GtkWindow *parent)
{
  GtkWidget *dialog;

  dialog = g_object_new (EMPATHY_TYPE_NEW_CHATROOM_DIALOG, NULL);

  if (parent != NULL)
    {
      gtk_window_set_transient_for (GTK_WINDOW (dialog),
          GTK_WINDOW (parent));
    }

  gtk_window_present (GTK_WINDOW (dialog));
  return dialog;
}

static void
empathy_new_chatroom_dialog_dispose (GObject *object)
{
  EmpathyNewChatroomDialog *self = EMPATHY_NEW_CHATROOM_DIALOG (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_new_chatroom_dialog_parent_class)->dispose;

  g_clear_object (&self->priv->room_list);
  g_clear_object (&self->priv->model);

  if (self->priv->account != NULL)
    {
      g_signal_handler_disconnect (self->priv->account,
          self->priv->status_changed_id);
      g_clear_object (&self->priv->account);
    }

  g_clear_object (&self->priv->gsettings);

  if (chain_up != NULL)
    chain_up (object);
}

static void
empathy_new_chatroom_dialog_class_init (EmpathyNewChatroomDialogClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GtkDialogClass *dialog_class = GTK_DIALOG_CLASS (klass);

  oclass->constructor = empathy_new_chatroom_dialog_constructor;
  oclass->dispose = empathy_new_chatroom_dialog_dispose;

  dialog_class->response = empathy_new_chatroom_dialog_response;

  g_type_class_add_private (klass, sizeof (EmpathyNewChatroomDialogPriv));
}

static void
empathy_new_chatroom_dialog_init (EmpathyNewChatroomDialog *self)
{
  GtkBuilder *gui;
  GtkSizeGroup *size_group;
  gchar *filename;
  GtkWidget *vbox, *content;

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_NEW_CHATROOM_DIALOG, EmpathyNewChatroomDialogPriv);

  filename = empathy_file_lookup ("empathy-new-chatroom-dialog.ui", "src");
  gui = tpaw_builder_get_file (filename,
      "vbox_new_chatroom", &vbox,
      "table_grid", &self->priv->table_grid,
      "label_account", &self->priv->label_account,
      "label_server", &self->priv->label_server,
      "label_room", &self->priv->label_room,
      "entry_server", &self->priv->entry_server,
      "entry_room", &self->priv->entry_room,
      "treeview", &self->priv->treeview,
      "expander_browse", &self->priv->expander_browse,
      "hbox_expander", &self->priv->hbox_expander,
      "label_error_message", &self->priv->label_error_message,
      "viewport_error", &self->priv->viewport_error,
      NULL);
  g_free (filename);

  tpaw_builder_connect (gui, self,
      "entry_server", "changed", new_chatroom_dialog_entry_changed_cb,
      "entry_server", "activate", new_chatroom_dialog_entry_server_activate_cb,
      "entry_server", "focus-out-event",
          new_chatroom_dialog_entry_server_focus_out_cb,
      "entry_room", "changed", new_chatroom_dialog_entry_changed_cb,
      "expander_browse", "activate",
          new_chatroom_dialog_expander_browse_activate_cb,
      "button_close_error", "clicked",
          new_chatroom_dialog_button_close_error_clicked_cb,
      NULL);

  /* Create dialog */
  content = gtk_dialog_get_content_area (GTK_DIALOG (self));
  gtk_container_add (GTK_CONTAINER (content), vbox);

  gtk_dialog_add_button (GTK_DIALOG (self), GTK_STOCK_CANCEL,
      GTK_RESPONSE_CANCEL);
  self->priv->button_join = gtk_dialog_add_button (GTK_DIALOG (self),
      _("Join"), GTK_RESPONSE_OK);

  gtk_dialog_set_default_response (GTK_DIALOG (self), GTK_RESPONSE_OK);

  gtk_window_set_title (GTK_WINDOW (self), _("Join Room"));
  gtk_window_set_role (GTK_WINDOW (self), "join_new_chatroom");

  g_object_unref (gui);

  /* Label alignment */
  size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

  gtk_size_group_add_widget (size_group, self->priv->label_account);
  gtk_size_group_add_widget (size_group, self->priv->label_server);
  gtk_size_group_add_widget (size_group, self->priv->label_room);

  g_object_unref (size_group);

  /* Set up chatrooms treeview */
  new_chatroom_dialog_model_setup (self);

  /* Add throbber */
  self->priv->throbber = gtk_spinner_new ();
  gtk_box_pack_start (GTK_BOX (self->priv->hbox_expander), self->priv->throbber,
    TRUE, TRUE, 0);

  self->priv->gsettings = g_settings_new (EMPATHY_PREFS_CHAT_SCHEMA);

  /* Account chooser for custom */
  self->priv->account_chooser = empathy_account_chooser_new ();
  empathy_account_chooser_set_filter (
      EMPATHY_ACCOUNT_CHOOSER (self->priv->account_chooser),
      empathy_account_chooser_filter_supports_chatrooms, NULL);
  gtk_grid_attach (GTK_GRID (self->priv->table_grid),
      self->priv->account_chooser, 1, 0, 1, 1);
  gtk_widget_show (self->priv->account_chooser);

  g_signal_connect (EMPATHY_ACCOUNT_CHOOSER (self->priv->account_chooser),
      "ready", G_CALLBACK (new_chatroom_dialog_account_ready_cb), self);
  g_signal_connect (GTK_COMBO_BOX (self->priv->account_chooser), "changed",
      G_CALLBACK (new_chatroom_dialog_account_changed_cb), self);
  new_chatroom_dialog_account_changed_cb (
      GTK_COMBO_BOX (self->priv->account_chooser), self);
}
