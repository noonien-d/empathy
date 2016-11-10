/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * empathy-contact-search-dialog.c
 *
 * Copyright (C) 2010-2011 Collabora Ltd.
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
 * Authors:
 *     Danielle Madeley <danielle.madeley@collabora.co.uk>
 *     Emilio Pozuelo Monfort <emilio.pozuelo@collabora.co.uk>
 */

#include "config.h"
#include "empathy-contact-search-dialog.h"

#include <glib/gi18n-lib.h>

#include "empathy-account-chooser.h"
#include "empathy-cell-renderer-activatable.h"
#include "empathy-cell-renderer-text.h"
#include "empathy-client-factory.h"
#include "empathy-images.h"
#include "empathy-individual-information-dialog.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

#include "empathy-contact-search-dialog.h"

#define GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), EMPATHY_TYPE_CONTACT_SEARCH_DIALOG, EmpathyContactSearchDialogPrivate))

G_DEFINE_TYPE (EmpathyContactSearchDialog, empathy_contact_search_dialog, GTK_TYPE_DIALOG);

enum
{
   NAME_COLUMN,
   LOGIN_COLUMN,
   N_COLUMNS
};

enum {
   PAGE_SEARCH_RESULTS,
   PAGE_NO_MATCH,
   PAGE_NOT_SUPPORTED,
};

typedef struct _EmpathyContactSearchDialogPrivate EmpathyContactSearchDialogPrivate;
struct _EmpathyContactSearchDialogPrivate
{
  TpContactSearch *searcher;
  GtkListStore *store;

  GtkWidget *chooser;
  GtkWidget *notebook;
  GtkWidget *tree_view;
  GtkWidget *spinner;
  GtkWidget *add_button;
  GtkWidget *find_button;
  GtkWidget *search_entry;
  /* GtkWidget *server_entry; */
  GtkWidget *message;
  GtkWidget *message_window;
  GtkWidget *message_label;
};

static void
empathy_contact_search_dialog_dispose (GObject *self)
{
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);

  tp_clear_object (&priv->searcher);

  G_OBJECT_CLASS (empathy_contact_search_dialog_parent_class)->dispose (self);
}

static void
on_searcher_reset (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyContactSearchDialog *self = EMPATHY_CONTACT_SEARCH_DIALOG (user_data);
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);
  TpContactSearch *searcher = TP_CONTACT_SEARCH (source_object);
  GError *error = NULL;
  GHashTable *search;
  const gchar *search_criteria;

  tp_contact_search_reset_finish (searcher, result, &error);
  if (error != NULL)
    {
      DEBUG ("Failed to reset the TpContactSearch: %s", error->message);
      gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook),
          PAGE_NOT_SUPPORTED);

      g_error_free (error);
      return;
    }

  gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook),
      PAGE_SEARCH_RESULTS);

  search = g_hash_table_new (g_str_hash, g_str_equal);

  search_criteria = gtk_entry_get_text (GTK_ENTRY (priv->search_entry));

  if (tp_strv_contains (tp_contact_search_get_search_keys (searcher), ""))
    g_hash_table_insert (search, "", (gpointer) search_criteria);
  else
    g_hash_table_insert (search, "fn", (gpointer) search_criteria);

  gtk_list_store_clear (priv->store);
  tp_contact_search_start (priv->searcher, search);

  g_hash_table_unref (search);
}

static void
empathy_contact_search_dialog_do_search (EmpathyContactSearchDialog *self)
{
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);

  tp_contact_search_reset_async (priv->searcher,
      NULL, /* gtk_entry_get_text (GTK_ENTRY (priv->server_entry)), */
      0,
      on_searcher_reset,
      self);
}

static void
on_get_contact_factory_get_from_id_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  gchar *message = user_data;
  GError *error = NULL;
  EmpathyContact *contact;

  contact = empathy_client_factory_dup_contact_by_id_finish (
      EMPATHY_CLIENT_FACTORY (source), result, &error);
  if (contact == NULL)
    {
      g_warning ("Error while getting the contact: %s", error->message);
      g_error_free (error);
      goto out;
    }

  empathy_contact_add_to_contact_list (contact, message);
  g_object_unref (contact);

out:
  g_free (message);
}

static void
add_selected_contact (EmpathyContactSearchDialog *self)
{
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);
  GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->tree_view));
  TpConnection *conn;
  GtkTreeIter iter;
  GtkTreeModel *model;
  GtkTextBuffer *buffer;
  GtkTextIter start, end;
  gchar *message;
  gboolean sel;
  gchar *id;
  EmpathyClientFactory *factory;

  conn = empathy_account_chooser_get_connection (EMPATHY_ACCOUNT_CHOOSER (priv->chooser));

  sel = gtk_tree_selection_get_selected (selection, &model, &iter);
  g_return_if_fail (sel == TRUE);

  gtk_tree_model_get (model, &iter, LOGIN_COLUMN, &id, -1);

  DEBUG ("Requested to add contact: %s", id);

  buffer = gtk_text_view_get_buffer GTK_TEXT_VIEW (priv->message);
  gtk_text_buffer_get_start_iter (buffer, &start);
  gtk_text_buffer_get_end_iter (buffer, &end);
  message = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);

  factory = empathy_client_factory_dup ();

  empathy_client_factory_dup_contact_by_id_async (factory, conn, id,
      on_get_contact_factory_get_from_id_cb, message);

  g_object_unref (factory);

  /* Close the dialog */
  gtk_dialog_response (GTK_DIALOG (self), GTK_RESPONSE_CANCEL);
}

static void
empathy_contact_search_dialog_response (GtkDialog *self,
    gint response)
{
  switch (response)
    {
      case GTK_RESPONSE_APPLY:
        add_selected_contact (EMPATHY_CONTACT_SEARCH_DIALOG (self));
        break;
      default:
        gtk_widget_destroy (GTK_WIDGET (self));
        break;
    }
}

static void
empathy_contact_search_dialog_class_init (
    EmpathyContactSearchDialogClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkDialogClass *dialog_class = GTK_DIALOG_CLASS (klass);

  gobject_class->dispose = empathy_contact_search_dialog_dispose;

  dialog_class->response = empathy_contact_search_dialog_response;

  g_type_class_add_private (gobject_class,
      sizeof (EmpathyContactSearchDialogPrivate));
}

static void
_on_search_state_changed_cb (TpContactSearch *searcher,
    GParamSpec *pspec,
    gpointer user_data)
{
  EmpathyContactSearchDialog *self = EMPATHY_CONTACT_SEARCH_DIALOG (user_data);
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);
  TpChannelContactSearchState state;

  g_object_get (searcher, "state", &state, NULL);

  DEBUG ("new search status: %d", state);

  if (state == TP_CHANNEL_CONTACT_SEARCH_STATE_IN_PROGRESS)
    {
      gtk_widget_show (priv->spinner);
      gtk_spinner_start (GTK_SPINNER (priv->spinner));
    }
  else
    {
      gtk_widget_hide (priv->spinner);
      gtk_spinner_stop (GTK_SPINNER (priv->spinner));
    }

  if (state == TP_CHANNEL_CONTACT_SEARCH_STATE_NOT_STARTED
      || state == TP_CHANNEL_CONTACT_SEARCH_STATE_IN_PROGRESS)
    {
      gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook),
          PAGE_SEARCH_RESULTS);
    }
  else
    {
      GtkTreeIter help_iter;

      if (!gtk_tree_model_get_iter_first (GTK_TREE_MODEL (priv->store),
          &help_iter))
        {
          /* No results found, display a helpful message. */
          gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook),
              PAGE_NO_MATCH);
        }
    }
}

static void
_search_results_received (TpContactSearch *searcher,
    GList *results,
    EmpathyContactSearchDialog *self)
{
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);
  const TpContactInfoField *name;
  GList *l;

  for (l = results; l != NULL; l = l->next)
    {
      TpContactSearchResult *result = l->data;

      name = tp_contact_search_result_get_field (result, "fn");

      gtk_list_store_insert_with_values (priv->store, NULL, -1,
          NAME_COLUMN, name ? name->field_value[0] : NULL,
          LOGIN_COLUMN, tp_contact_search_result_get_identifier (result),
          -1);
    }
}

static void
on_searcher_created (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyContactSearchDialog *self;
  EmpathyContactSearchDialogPrivate *priv;
  GError *error = NULL;

  if (EMPATHY_IS_CONTACT_SEARCH_DIALOG (user_data) == FALSE)
    /* This happens if the dialog is closed before the callback is called */
    return;

  self = EMPATHY_CONTACT_SEARCH_DIALOG (user_data);
  priv = GET_PRIVATE (self);

  priv->searcher = tp_contact_search_new_finish (result, &error);
  if (error != NULL)
    {
      DEBUG ("Failed to create a TpContactSearch: %s", error->message);
      gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook),
          PAGE_NOT_SUPPORTED);

      g_error_free (error);
      return;
    }

  gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook),
      PAGE_SEARCH_RESULTS);

  g_signal_connect (priv->searcher, "search-results-received",
      G_CALLBACK (_search_results_received), self);
  g_signal_connect (priv->searcher, "notify::state",
      G_CALLBACK (_on_search_state_changed_cb), self);

  gtk_widget_set_sensitive (priv->find_button, TRUE);
  gtk_widget_set_sensitive (priv->search_entry, TRUE);
}

static void
on_selection_changed (GtkTreeSelection *selection,
    gpointer user_data)
{
  EmpathyContactSearchDialog *self;
  EmpathyContactSearchDialogPrivate *priv;
  gboolean sel;

  self = EMPATHY_CONTACT_SEARCH_DIALOG (user_data);
  priv = GET_PRIVATE (self);
  sel = gtk_tree_selection_get_selected (selection, NULL, NULL);

  gtk_widget_set_sensitive (priv->add_button, sel);
}

static void
check_request_message_available (EmpathyContactSearchDialog *self,
    TpConnection *conn)
{
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);

  gtk_widget_set_visible (priv->message_window,
      tp_connection_get_can_change_contact_list (conn));
  gtk_widget_set_visible (priv->message_label,
      tp_connection_get_can_change_contact_list (conn));
}

static void
_account_chooser_changed (EmpathyAccountChooser *chooser,
    EmpathyContactSearchDialog *self)
{
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);
  TpAccount *account = empathy_account_chooser_get_account (chooser);
  TpConnection *conn = empathy_account_chooser_get_connection (chooser);
  TpCapabilities *caps = tp_connection_get_capabilities (conn);
  gboolean can_cs, can_set_limit, can_set_server;

  can_cs = tp_capabilities_supports_contact_search (caps,
      &can_set_limit, &can_set_server);
  DEBUG ("The server supports cs|limit|server: %s|%s|%s",
      can_cs ? "yes" : "no",
      can_set_limit ? "yes" : "no",
      can_set_server ? "yes" : "no");

  /* gtk_widget_set_sensitive (priv->server_entry, can_set_server); */
  gtk_widget_set_sensitive (priv->find_button, FALSE);
  gtk_widget_set_sensitive (priv->search_entry, FALSE);

  DEBUG ("New account is %s", tp_proxy_get_object_path (account));

  tp_clear_object (&priv->searcher);
  tp_contact_search_new_async (account,
      NULL, /* gtk_entry_get_text (GTK_ENTRY (priv->server_entry)), */
      0,
      on_searcher_created, self);

  /* Make the request message textview sensitive if it can be used */
  check_request_message_available (self, conn);
}

static void
_on_button_search_clicked (GtkWidget *widget,
    EmpathyContactSearchDialog *self)
{
  empathy_contact_search_dialog_do_search (self);
}

#if 0
static void
on_server_changed_cb (GtkEditable *editable,
    gpointer user_data)
{
  EmpathyContactSearchDialog *self = EMPATHY_CONTACT_SEARCH_DIALOG (user_data);
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);

  g_return_if_fail (priv->searcher != NULL);

  tp_contact_search_reset_async (priv->searcher,
      gtk_entry_get_text (GTK_ENTRY (editable)),
      0,
      on_searcher_reset,
      self);
}
#endif

static void
empathy_account_chooser_filter_supports_contact_search (
    TpAccount *account,
    EmpathyAccountChooserFilterResultCallback callback,
    gpointer callback_data,
    gpointer user_data)
{
  TpConnection *connection;
  gboolean supported = FALSE;
  TpCapabilities *caps;

  connection = tp_account_get_connection (account);
  if (connection == NULL)
      goto out;

  caps = tp_connection_get_capabilities (connection);
  if (caps == NULL)
      goto out;

  supported = tp_capabilities_supports_contact_search (caps, NULL, NULL);

out:
  callback (supported, callback_data);
}

static void
contact_search_dialog_row_activated_cb (GtkTreeView *tv,
    GtkTreePath *path,
    GtkTreeViewColumn *column,
    EmpathyContactSearchDialog *self)
{
  /* just emit the same response as the Add Button */
  gtk_dialog_response (GTK_DIALOG (self), GTK_RESPONSE_APPLY);
}

static void
on_profile_button_got_contact_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GError *error = NULL;
  EmpathyContact *contact;
  FolksIndividual *individual;

  contact = empathy_client_factory_dup_contact_by_id_finish (
        EMPATHY_CLIENT_FACTORY (source), result, &error);
  if (contact == NULL)
    {
      g_warning ("Error while getting the contact: %s", error->message);
      g_error_free (error);
      return;
    }

  individual = empathy_ensure_individual_from_tp_contact (
    empathy_contact_get_tp_contact (contact));

  empathy_display_individual_info (individual);

  g_object_unref (contact);
  g_object_unref (individual);
}

static void
on_profile_button_clicked_cb (EmpathyCellRendererActivatable *cell,
    const gchar *path_string,
    EmpathyContactSearchDialog *self)
{
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);
  TpConnection *conn;
  GtkTreeIter iter;
  GtkTreeModel *model;
  gboolean valid;
  gchar *id;
  EmpathyClientFactory *factory;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->tree_view));

  conn = empathy_account_chooser_get_connection (
      EMPATHY_ACCOUNT_CHOOSER (priv->chooser));

  valid = gtk_tree_model_get_iter_from_string (model, &iter, path_string);
  g_return_if_fail (valid == TRUE);

  gtk_tree_model_get (model, &iter, LOGIN_COLUMN, &id, -1);

  DEBUG ("Requested to show profile for contact: %s", id);

  factory = empathy_client_factory_dup ();

  empathy_client_factory_dup_contact_by_id_async (factory, conn, id,
      on_profile_button_got_contact_cb, self);

  g_object_unref (factory);
}

static void
append_message_page (EmpathyContactSearchDialog *self,
    const gchar *message)
{
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);
  GtkWidget *label;
  gchar *tmp;

  label = gtk_label_new (NULL);
  tmp = g_strdup_printf ("<b><span size='xx-large'>%s</span></b>", message);
  gtk_label_set_markup (GTK_LABEL (label), tmp);
  g_free (tmp);

  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);

  gtk_notebook_append_page (GTK_NOTEBOOK (priv->notebook), label, NULL);
}

static void
empathy_contact_search_dialog_init (EmpathyContactSearchDialog *self)
{
  EmpathyContactSearchDialogPrivate *priv = GET_PRIVATE (self);
  GtkWidget *vbox, *hbox, *scrolled_window, *label;
  GtkCellRenderer *cell;
  GtkTreeViewColumn *col;
  GtkTreeSelection *selection;
  GtkSizeGroup *size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

  /* Title */
  gtk_window_set_title (GTK_WINDOW (self), _("Search contacts"));

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 6);

  /* Account chooser */
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  label = gtk_label_new (_("Account:"));
  gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, TRUE, 0);
  gtk_size_group_add_widget (size_group, label);

  priv->chooser = empathy_account_chooser_new ();
  empathy_account_chooser_set_filter (EMPATHY_ACCOUNT_CHOOSER (priv->chooser),
      empathy_account_chooser_filter_supports_contact_search, NULL);
  gtk_box_pack_start (GTK_BOX (hbox), priv->chooser, TRUE, TRUE, 0);
  g_signal_connect (priv->chooser, "changed",
      G_CALLBACK (_account_chooser_changed), self);

  gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, TRUE, 0);

#if 0
  /* Server entry */
  priv->server_entry = gtk_entry_new ();
  gtk_box_pack_start (GTK_BOX (vbox), priv->server_entry, FALSE, TRUE, 6);
  g_signal_connect (GTK_EDITABLE (priv->server_entry), "changed",
      G_CALLBACK (on_server_changed_cb), self);
#endif

  /* Search input */
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  label = gtk_label_new (_("Search: "));
  gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, TRUE, 0);
  gtk_size_group_add_widget (size_group, label);

  priv->search_entry = gtk_entry_new ();
  gtk_box_pack_start (GTK_BOX (hbox), priv->search_entry, TRUE, TRUE, 0);
  g_signal_connect (priv->search_entry, "activate",
      G_CALLBACK (_on_button_search_clicked), self);

  priv->find_button = gtk_button_new_from_stock (GTK_STOCK_FIND);
  g_signal_connect (priv->find_button, "clicked",
      G_CALLBACK (_on_button_search_clicked), self);
  gtk_box_pack_end (GTK_BOX (hbox), priv->find_button, FALSE, TRUE, 0);

  priv->spinner = gtk_spinner_new ();
  gtk_box_pack_end (GTK_BOX (hbox), priv->spinner, FALSE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, TRUE, 0);

  /* Search results */
  priv->store = gtk_list_store_new (N_COLUMNS,
                                    G_TYPE_STRING,  /* Name */
                                    G_TYPE_STRING); /* Login */

  priv->tree_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (priv->store));
  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->tree_view));
  gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);

  g_signal_connect (priv->tree_view, "row-activated",
      G_CALLBACK (contact_search_dialog_row_activated_cb), self);
  g_signal_connect (selection, "changed",
      G_CALLBACK (on_selection_changed), self);

  gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (priv->tree_view), FALSE);

  col = gtk_tree_view_column_new ();

  cell = empathy_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (col, cell, TRUE);
  /* EmpathyCellRendererText displays "name" above and "status" below.
   * We want the login above since it'll always be available, and the
   * name below since we won't always have it. */
  gtk_tree_view_column_add_attribute (col, cell,
      "name", LOGIN_COLUMN);
  gtk_tree_view_column_add_attribute (col, cell,
      "status", NAME_COLUMN);

  cell = empathy_cell_renderer_activatable_new ();
  gtk_tree_view_column_pack_end (col, cell, FALSE);
  g_object_set (cell, "stock-id", EMPATHY_IMAGE_CONTACT_INFORMATION, NULL);
  g_signal_connect (cell, "path-activated",
      G_CALLBACK (on_profile_button_clicked_cb), self);

  gtk_tree_view_append_column (GTK_TREE_VIEW (priv->tree_view), col);

  gtk_dialog_add_button (GTK_DIALOG (self),
      GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE);

  priv->add_button = gtk_dialog_add_button (GTK_DIALOG (self),
      _("_Add Contact"), GTK_RESPONSE_APPLY);
  gtk_widget_set_sensitive (priv->add_button, FALSE);
  gtk_button_set_image (GTK_BUTTON (priv->add_button),
      gtk_image_new_from_stock (GTK_STOCK_ADD, GTK_ICON_SIZE_BUTTON));

  /* Pack the dialog */
  priv->notebook = gtk_notebook_new ();
  gtk_notebook_set_show_tabs (GTK_NOTEBOOK (priv->notebook), FALSE);
  g_object_set (priv->notebook, "margin", 6, NULL);

  scrolled_window = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_window),
      GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_container_add (GTK_CONTAINER (scrolled_window), priv->tree_view);

  gtk_notebook_append_page (GTK_NOTEBOOK (priv->notebook), scrolled_window,
      NULL);

  append_message_page (self, _("No contacts found"));
  append_message_page (self, _("Contact search is not supported on this account"));

  gtk_box_pack_start (GTK_BOX (vbox), priv->notebook, TRUE, TRUE, 3);

  /* Request message textview */
  priv->message_label = gtk_label_new (
       _("Your message introducing yourself:"));
  gtk_misc_set_alignment (GTK_MISC (priv->message_label), 0, 0.5);

  priv->message = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (priv->message),
      GTK_WRAP_WORD_CHAR);
  gtk_text_buffer_set_text (
      gtk_text_view_get_buffer (GTK_TEXT_VIEW (priv->message)),
      _("Please let me see when you’re online. Thanks!"), -1);

  priv->message_window = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_shadow_type (
      GTK_SCROLLED_WINDOW (priv->message_window),
      GTK_SHADOW_ETCHED_IN);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (priv->message_window),
      GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_container_add (GTK_CONTAINER (priv->message_window), priv->message);

  gtk_box_pack_start (GTK_BOX (vbox), priv->message_label, FALSE, TRUE, 3);
  gtk_box_pack_start (GTK_BOX (vbox), priv->message_window, FALSE, TRUE, 3);

  gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (
          GTK_DIALOG (self))), vbox, TRUE, TRUE, 0);

  gtk_window_set_default_size (GTK_WINDOW (self), 200, 400);
  gtk_widget_show_all (vbox);
  gtk_widget_hide (priv->spinner);
  g_object_unref (size_group);
}

GtkWidget *
empathy_contact_search_dialog_new (GtkWindow *parent)
{
  GtkWidget *self;

  g_return_val_if_fail (parent == NULL || GTK_IS_WINDOW (parent), NULL);

  self = g_object_new (EMPATHY_TYPE_CONTACT_SEARCH_DIALOG, NULL);

  if (parent != NULL)
    {
      gtk_window_set_transient_for (GTK_WINDOW (self), parent);
    }

  return self;
}
