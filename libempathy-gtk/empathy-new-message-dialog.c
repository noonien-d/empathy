/*
 * Copyright (C) 2007-2008 Collabora Ltd.
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
#include "empathy-new-message-dialog.h"

#include <glib/gi18n-lib.h>

#include "empathy-request-util.h"
#include "empathy-contact-chooser.h"
#include "empathy-ui-utils.h"
#include "empathy-images.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CONTACT
#include "empathy-debug.h"

static EmpathyNewMessageDialog *dialog_singleton = NULL;

G_DEFINE_TYPE(EmpathyNewMessageDialog, empathy_new_message_dialog,
    GTK_TYPE_DIALOG)

struct _EmpathyNewMessageDialogPriv {
  GtkWidget *chooser;
  GtkWidget *button_chat;
  GtkWidget *button_sms;
};

/**
 * SECTION:empathy-new-message-dialog
 * @title: EmpathyNewMessageDialog
 * @short_description: A dialog to show a new message
 * @include: libempathy-gtk/empathy-new-message-dialog.h
 *
 * #EmpathyNewMessageDialog is a dialog which allows a text chat
 * to be started with any contact on any enabled account.
 */

enum
{
  EMP_NEW_MESSAGE_TEXT,
  EMP_NEW_MESSAGE_SMS,
};

static const gchar *
get_error_display_message (GError *error)
{
  if (error->domain != TP_ERROR)
    goto out;

  switch (error->code)
    {
      case TP_ERROR_NETWORK_ERROR:
        return _("Network error");
      case TP_ERROR_OFFLINE:
        return _("The contact is offline");
      case TP_ERROR_INVALID_HANDLE:
        return _("The specified contact is either invalid or unknown");
      case TP_ERROR_NOT_CAPABLE:
        return _("The contact does not support this kind of conversation");
      case TP_ERROR_NOT_IMPLEMENTED:
        return _("The requested functionality is not implemented "
                 "for this protocol");
      case TP_ERROR_INVALID_ARGUMENT:
        /* Not very user friendly to say 'invalid arguments' */
        break;
      case TP_ERROR_NOT_AVAILABLE:
        return _("Could not start a conversation with the given contact");
      case TP_ERROR_CHANNEL_BANNED:
        return _("You are banned from this channel");
      case TP_ERROR_CHANNEL_FULL:
        return _("This channel is full");
      case TP_ERROR_CHANNEL_INVITE_ONLY:
        return _("You must be invited to join this channel");
      case TP_ERROR_DISCONNECTED:
        return _("Can’t proceed while disconnected");
      case TP_ERROR_PERMISSION_DENIED:
        return _("Permission denied");
      default:
        DEBUG ("Unhandled error code: %d", error->code);
    }

out:
  return _("There was an error starting the conversation");
}

static void
show_chat_error (GError *error,
    GtkWindow *parent)
{
  GtkWidget *dialog;

  dialog = gtk_message_dialog_new (parent, GTK_DIALOG_MODAL,
      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
      "%s",
      get_error_display_message (error));

  g_signal_connect_swapped (dialog, "response",
      G_CALLBACK (gtk_widget_destroy),
      dialog);

  gtk_widget_show (dialog);
}

static void
ensure_text_channel_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GError *error = NULL;

  if (!tp_account_channel_request_ensure_channel_finish (
        TP_ACCOUNT_CHANNEL_REQUEST (source), result, &error))
    {
      DEBUG ("Failed to ensure text channel: %s", error->message);
      show_chat_error (error, user_data);
      g_error_free (error);
    }
}

static void
empathy_new_message_dialog_response (GtkDialog *dialog,
    int response_id)
{
  EmpathyNewMessageDialog *self = (EmpathyNewMessageDialog *) dialog;
  FolksIndividual *individual = NULL;
  EmpathyContact *contact = NULL;

  if (response_id < EMP_NEW_MESSAGE_TEXT)
    goto out;

  individual = empathy_contact_chooser_dup_selected (
      EMPATHY_CONTACT_CHOOSER (self->priv->chooser));
  if (individual == NULL)
    goto out;

  switch (response_id)
    {
      case EMP_NEW_MESSAGE_TEXT:
        contact = empathy_contact_dup_best_for_action (individual,
            EMPATHY_ACTION_CHAT);
        g_return_if_fail (contact != NULL);

        empathy_chat_with_contact_id (empathy_contact_get_account (contact),
            empathy_contact_get_id (contact),
            empathy_get_current_action_time (),
            ensure_text_channel_cb,
            gtk_widget_get_parent_window (GTK_WIDGET (dialog)));
        break;

      case EMP_NEW_MESSAGE_SMS:
        contact = empathy_contact_dup_best_for_action (individual,
            EMPATHY_ACTION_SMS);
        g_return_if_fail (contact != NULL);

        empathy_sms_contact_id (empathy_contact_get_account (contact),
            empathy_contact_get_id (contact),
            empathy_get_current_action_time (),
            ensure_text_channel_cb,
            gtk_widget_get_parent_window (GTK_WIDGET (dialog)));
        break;

      default:
        g_warn_if_reached ();
    }

out:
  tp_clear_object (&individual);
  tp_clear_object (&contact);
  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static GObject *
empathy_new_message_dialog_constructor (GType type,
    guint n_props,
    GObjectConstructParam *props)
{
  GObject *retval;

  if (dialog_singleton)
    {
      retval = G_OBJECT (dialog_singleton);
      g_object_ref (retval);
    }
  else
    {
      retval = G_OBJECT_CLASS (
      empathy_new_message_dialog_parent_class)->constructor (type,
        n_props, props);

      dialog_singleton = EMPATHY_NEW_MESSAGE_DIALOG (retval);
      g_object_add_weak_pointer (retval, (gpointer) &dialog_singleton);
    }

  return retval;
}

static gboolean
individual_supports_action (FolksIndividual *individual,
    EmpathyActionType action)
{
  EmpathyContact *contact;

  contact = empathy_contact_dup_best_for_action (individual, action);
  if (contact == NULL)
    return FALSE;

  g_object_unref (contact);
  return TRUE;
}

static gboolean
filter_individual (EmpathyContactChooser *chooser,
    FolksIndividual *individual,
    gboolean is_online,
    gboolean searching,
    gpointer user_data)
{
  return individual_supports_action (individual, EMPATHY_ACTION_CHAT) ||
    individual_supports_action (individual, EMPATHY_ACTION_SMS);
}

static void
selection_changed_cb (GtkWidget *chooser,
    FolksIndividual *selected,
    EmpathyNewMessageDialog *self)
{
  gboolean can_chat, can_sms;

  if (selected == NULL)
    {
      can_chat = can_sms = FALSE;
    }
  else
    {
      can_chat = individual_supports_action (selected, EMPATHY_ACTION_CHAT);
      can_sms = individual_supports_action (selected, EMPATHY_ACTION_SMS);
    }

  gtk_widget_set_sensitive (self->priv->button_chat, can_chat);
#if 0
  gtk_widget_set_sensitive (self->priv->button_sms, can_sms);
#endif
}

static void
selection_activate_cb (GtkWidget *chooser,
    EmpathyNewMessageDialog *self)
{
  gtk_dialog_response (GTK_DIALOG (self), EMP_NEW_MESSAGE_TEXT);
}

static void
empathy_new_message_dialog_init (EmpathyNewMessageDialog *self)
{
  GtkWidget *image;
  GtkWidget *content;

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_NEW_MESSAGE_DIALOG, EmpathyNewMessageDialogPriv);

  content = gtk_dialog_get_content_area (GTK_DIALOG (self));

  /* contact chooser */
  self->priv->chooser = empathy_contact_chooser_new ();

  empathy_contact_chooser_set_filter_func (
      EMPATHY_CONTACT_CHOOSER (self->priv->chooser), filter_individual, self);

  gtk_box_pack_start (GTK_BOX (content), self->priv->chooser, TRUE, TRUE, 0);
  gtk_widget_show (self->priv->chooser);

  g_signal_connect (self->priv->chooser, "selection-changed",
      G_CALLBACK (selection_changed_cb), self);
  g_signal_connect (self->priv->chooser, "activate",
      G_CALLBACK (selection_activate_cb), self);

  /* close button */
  gtk_dialog_add_button (GTK_DIALOG (self),
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);

#if 0
  /* add SMS button */
  self->priv->button_sms = gtk_button_new_with_mnemonic (_("_SMS"));
  image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_SMS,
      GTK_ICON_SIZE_BUTTON);
  gtk_button_set_image (GTK_BUTTON (self->priv->button_sms), image);
#endif

  /* add chat button */
  self->priv->button_chat = gtk_button_new_with_mnemonic (_("_Done"));
  image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_NEW_MESSAGE,
      GTK_ICON_SIZE_BUTTON);
  gtk_button_set_image (GTK_BUTTON (self->priv->button_chat), image);

#if 0
  gtk_dialog_add_action_widget (GTK_DIALOG (self), self->priv->button_sms,
      EMP_NEW_MESSAGE_SMS);
  gtk_widget_show (self->priv->button_sms);
#endif

  gtk_dialog_add_action_widget (GTK_DIALOG (self), self->priv->button_chat,
      EMP_NEW_MESSAGE_TEXT);
  gtk_widget_show (self->priv->button_chat);

  /* Tweak the dialog */
  gtk_window_set_title (GTK_WINDOW (self), _("New Conversation"));
  gtk_window_set_role (GTK_WINDOW (self), "new_message");

  /* Set a default height so a few contacts are displayed */
  gtk_window_set_default_size (GTK_WINDOW (self), -1, 600);

  gtk_widget_set_sensitive (self->priv->button_chat, FALSE);
#if 0
  gtk_widget_set_sensitive (self->priv->button_sms, FALSE);
#endif
}

static void
empathy_new_message_dialog_class_init (
  EmpathyNewMessageDialogClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GtkDialogClass *dialog_class = GTK_DIALOG_CLASS (class);

  object_class->constructor = empathy_new_message_dialog_constructor;

  dialog_class->response = empathy_new_message_dialog_response;

  g_type_class_add_private (class, sizeof (EmpathyNewMessageDialogPriv));
}

/**
 * empathy_new_message_dialog_new:
 * @parent: parent #GtkWindow of the dialog
 *
 * Create a new #EmpathyNewMessageDialog it.
 *
 * Return value: the new #EmpathyNewMessageDialog
 */
GtkWidget *
empathy_new_message_dialog_show (GtkWindow *parent)
{
  GtkWidget *dialog;

  dialog = g_object_new (EMPATHY_TYPE_NEW_MESSAGE_DIALOG,
                  "use-header-bar", 1,
                  NULL);

  if (parent)
    {
      gtk_window_set_transient_for (GTK_WINDOW (dialog),
          GTK_WINDOW (parent));
    }

  gtk_widget_show (dialog);
  return dialog;
}
