/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2007-2010 Collabora Ltd.
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
#include "empathy-individual-dialogs.h"

#include <glib/gi18n-lib.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-contact-widget.h"
#include "empathy-individual-manager.h"
#include "empathy-utils.h"

#define BULLET_POINT "\342\200\242"

static GtkWidget *new_individual_dialog = NULL;

/*
 *  New contact dialog
 */

static void
can_add_contact_to_account (TpAccount *account,
    EmpathyAccountChooserFilterResultCallback callback,
    gpointer callback_data,
    gpointer user_data)
{
  EmpathyIndividualManager *individual_manager;
  TpConnection *connection;
  gboolean result;

  connection = tp_account_get_connection (account);
  if (connection == NULL)
    {
      callback (FALSE, callback_data);
      return;
    }

  individual_manager = empathy_individual_manager_dup_singleton ();
  result = empathy_connection_can_add_personas (connection);
  g_object_unref (individual_manager);

  callback (result, callback_data);
}

static void
new_individual_response_cb (GtkDialog *dialog,
    gint response,
    GtkWidget *contact_widget)
{
  EmpathyIndividualManager *individual_manager;
  EmpathyContact *contact;

  individual_manager = empathy_individual_manager_dup_singleton ();
  contact = empathy_contact_widget_get_contact (contact_widget);

  if (contact && response == GTK_RESPONSE_OK)
    empathy_individual_manager_add_from_contact (individual_manager, contact);

  new_individual_dialog = NULL;
  gtk_widget_destroy (GTK_WIDGET (dialog));
  g_object_unref (individual_manager);
}

void
empathy_new_individual_dialog_show (GtkWindow *parent)
{
  empathy_new_individual_dialog_show_with_individual (parent, NULL);
}

void
empathy_new_individual_dialog_show_with_individual (GtkWindow *parent,
    FolksIndividual *individual)
{
  GtkWidget *dialog;
  GtkWidget *button;
  EmpathyContact *contact = NULL;
  GtkWidget *contact_widget;

  g_return_if_fail (individual == NULL || FOLKS_IS_INDIVIDUAL (individual));

  if (new_individual_dialog)
    {
      gtk_window_present (GTK_WINDOW (new_individual_dialog));
      return;
    }

  /* Create dialog */
  dialog = gtk_dialog_new ();
  gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);
  gtk_window_set_title (GTK_WINDOW (dialog), _("New Contact"));

  /* Cancel button */
  button = gtk_button_new_with_label (GTK_STOCK_CANCEL);
  gtk_button_set_use_stock (GTK_BUTTON (button), TRUE);
  gtk_dialog_add_action_widget (GTK_DIALOG (dialog), button,
      GTK_RESPONSE_CANCEL);
  gtk_widget_show (button);

  /* Add button */
  button = gtk_button_new_with_label (GTK_STOCK_ADD);
  gtk_button_set_use_stock (GTK_BUTTON (button), TRUE);
  gtk_dialog_add_action_widget (GTK_DIALOG (dialog), button, GTK_RESPONSE_OK);
  gtk_widget_show (button);

  /* Contact info widget */
  if (individual != NULL)
    contact = empathy_contact_dup_from_folks_individual (individual);

  contact_widget = empathy_contact_widget_new (contact);
  gtk_container_set_border_width (GTK_CONTAINER (contact_widget), 8);
  gtk_box_pack_start (
      GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
      contact_widget, TRUE, TRUE, 0);
  empathy_contact_widget_set_account_filter (contact_widget,
      can_add_contact_to_account, NULL);
  gtk_widget_show (contact_widget);

  new_individual_dialog = dialog;

  g_signal_connect (dialog, "response", G_CALLBACK (new_individual_response_cb),
      contact_widget);

  if (parent != NULL)
    gtk_window_set_transient_for (GTK_WINDOW (dialog), parent);

  gtk_widget_show (dialog);

  tp_clear_object (&contact);
}

static char *
contact_pretty_name (TpContact *contact)
{
  const char *alias = tp_contact_get_alias (contact);
  const char *identifier = tp_contact_get_identifier (contact);

  if (tp_strdiff (alias, identifier))
    return g_strdup_printf ("%s (%s)", alias, identifier);
  else
    return g_strdup (alias);
}

/*
 * Block contact dialog
 */
gboolean
empathy_block_individual_dialog_show (GtkWindow *parent,
    FolksIndividual *individual,
    GdkPixbuf *avatar,
    gboolean *abusive)
{
  GtkWidget *dialog;
  GtkWidget *abusive_check = NULL;
  GeeSet *personas;
  GeeIterator *iter;
  GString *text = g_string_new ("");
  GString *blocked_str = g_string_new ("");
  GString *notblocked_str = g_string_new ("");
  guint npersonas_blocked = 0, npersonas_notblocked = 0;
  gboolean can_report_abuse = FALSE;
  int res;

  dialog = gtk_message_dialog_new (parent,
      GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
      _("Block %s?"),
      folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (individual)));

  if (avatar != NULL)
    {
      GtkWidget *image = gtk_image_new_from_pixbuf (avatar);
      gtk_message_dialog_set_image (GTK_MESSAGE_DIALOG (dialog), image);
      gtk_widget_show (image);
    }

  /* build a list of personas that support blocking */
  personas = folks_individual_get_personas (individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (gee_iterator_next (iter))
    {
      TpfPersona *persona = gee_iterator_get (iter);
      TpContact *contact;
      GString *s;
      char *str;
      TpConnection *conn;

      if (!TPF_IS_PERSONA (persona))
          goto while_finish;

      contact = tpf_persona_get_contact (persona);
      if (contact == NULL)
        goto while_finish;

      conn = tp_contact_get_connection (contact);

      if (tp_proxy_has_interface_by_id (conn,
            TP_IFACE_QUARK_CONNECTION_INTERFACE_CONTACT_BLOCKING))
        {
          s = blocked_str;
          npersonas_blocked++;
        }
      else
        {
          s = notblocked_str;
          npersonas_notblocked++;
        }

      if (tp_connection_can_report_abusive (conn))
        can_report_abuse = TRUE;

      str = contact_pretty_name (contact);
      g_string_append_printf (s, "\n " BULLET_POINT " %s", str);
      g_free (str);

while_finish:
      g_clear_object (&persona);
    }
  g_clear_object (&iter);

  g_string_append_printf (text,
      _("Are you sure you want to block “%s” from contacting you again?"),
      folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (individual)));

  if (npersonas_blocked > 0)
    g_string_append_printf (text, "\n\n%s\n%s",
        ngettext ("The following identity will be blocked:",
                  "The following identities will be blocked:",
                  npersonas_blocked),
        blocked_str->str);

  if (npersonas_notblocked > 0)
    g_string_append_printf (text, "\n\n%s\n%s",
        ngettext ("The following identity can not be blocked:",
                  "The following identities can not be blocked:",
                  npersonas_notblocked),
        notblocked_str->str);

  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
    "%s", text->str);

  gtk_dialog_add_buttons (GTK_DIALOG (dialog),
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      _("_Block"), GTK_RESPONSE_REJECT,
      NULL);

  if (can_report_abuse)
    {
      GtkWidget *vbox;

      vbox = gtk_message_dialog_get_message_area (GTK_MESSAGE_DIALOG (dialog));
      abusive_check = gtk_check_button_new_with_mnemonic (
          ngettext ("_Report this contact as abusive",
                    "_Report these contacts as abusive",
                    npersonas_blocked));

      gtk_box_pack_start (GTK_BOX (vbox), abusive_check, FALSE, TRUE, 0);
      gtk_widget_show (abusive_check);
    }

  g_string_free (text, TRUE);
  g_string_free (blocked_str, TRUE);
  g_string_free (notblocked_str, TRUE);

  res = gtk_dialog_run (GTK_DIALOG (dialog));

  if (abusive != NULL)
    {
      if (abusive_check != NULL)
        *abusive = gtk_toggle_button_get_active (
            GTK_TOGGLE_BUTTON (abusive_check));
      else
        *abusive = FALSE;
    }

  gtk_widget_destroy (dialog);

  return res == GTK_RESPONSE_REJECT;
}
