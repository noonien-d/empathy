/*
 * Copyright (C) 2012 Collabora Ltd.
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
 * Authors: Guillaume Desmottes <guillaume.desmottes@collabora.com>
 */

#include "config.h"
#include "empathy-subscription-dialog.h"

#include <glib/gi18n-lib.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-individual-widget.h"
#include "empathy-utils.h"

G_DEFINE_TYPE (EmpathySubscriptionDialog, empathy_subscription_dialog, GTK_TYPE_MESSAGE_DIALOG)

enum
{
  PROP_INDIVIDUAL = 1,
  PROP_MESSAGE,
  N_PROPS
};

/*
enum
{
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];
*/

struct _EmpathySubscriptionDialogPriv
{
  FolksIndividual *individual;
  gchar *message;
};

static void
empathy_subscription_dialog_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathySubscriptionDialog *self = EMPATHY_SUBSCRIPTION_DIALOG (object);

  switch (property_id)
    {
      case PROP_INDIVIDUAL:
        g_value_set_object (value, self->priv->individual);
        break;
      case PROP_MESSAGE:
        g_value_set_string (value, self->priv->message);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
empathy_subscription_dialog_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathySubscriptionDialog *self = EMPATHY_SUBSCRIPTION_DIALOG (object);

  switch (property_id)
    {
      case PROP_INDIVIDUAL:
        g_assert (self->priv->individual == NULL); /* construct only */
        self->priv->individual = g_value_dup_object (value);
        break;
      case PROP_MESSAGE:
        g_assert (self->priv->message == NULL); /* construct only */
        self->priv->message = g_value_dup_string (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

/**
 * block_contact_dialog_show:
 * @parent: the parent of this dialog (or %NULL)
 * @contact: the contact for this dialog
 * @abusive: a pointer to store the value of the abusive contact check box
 *  (or %NULL)
 *
 * Returns: %TRUE if the user wishes to block the contact
 */
static gboolean
block_contact_dialog_show (GtkWindow *parent,
    EmpathyContact *contact,
    GdkPixbuf *avatar,
    gboolean *abusive)
{
  GtkWidget *dialog;
  GtkWidget *abusive_check = NULL;
  int res;
  TpConnection *conn;

  dialog = gtk_message_dialog_new (parent, GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, _("Block %s?"),
      empathy_contact_get_alias (contact));

  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
      _("Are you sure you want to block “%s” from contacting you again?"),
      empathy_contact_get_alias (contact));

  gtk_dialog_add_buttons (GTK_DIALOG (dialog),
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      _("_Block"), GTK_RESPONSE_REJECT,
      NULL);

  if (avatar != NULL)
    {
      GtkWidget *image = gtk_image_new_from_pixbuf (avatar);
      gtk_message_dialog_set_image (GTK_MESSAGE_DIALOG (dialog), image);
      gtk_widget_show (image);
    }

  conn = empathy_contact_get_connection (contact);

  /* ask the user if they want to also report the contact as abusive */
  if (tp_connection_can_report_abusive (conn))
    {
      GtkWidget *vbox;

      vbox = gtk_message_dialog_get_message_area (
          GTK_MESSAGE_DIALOG (dialog));
      abusive_check = gtk_check_button_new_with_mnemonic (
          _("_Report this contact as abusive"));

      gtk_box_pack_start (GTK_BOX (vbox), abusive_check,
              FALSE, TRUE, 0);
      gtk_widget_show (abusive_check);
    }

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

static void
response_cb (GtkDialog *dialog,
    gint reponse,
    EmpathySubscriptionDialog *self)
{
  EmpathyContact *contact;

  contact = empathy_contact_dup_from_folks_individual (self->priv->individual);

  if (reponse == GTK_RESPONSE_YES)
    {
      empathy_contact_add_to_contact_list (contact, "");
    }
  else if (reponse == GTK_RESPONSE_NO)
    {
      empathy_contact_remove_from_contact_list (contact);
    }
  else if (reponse == GTK_RESPONSE_REJECT)
    {
      gboolean abusive;

      /* confirm the blocking */
      if (block_contact_dialog_show (GTK_WINDOW (dialog), contact,
            NULL, &abusive))
        {
          TpContact *tp_contact;

          empathy_contact_remove_from_contact_list (contact);

          tp_contact = empathy_contact_get_tp_contact (contact);

          tp_contact_block_async (tp_contact, abusive, NULL, NULL);
        }
      else
        {
          /* if they don't confirm, return back to the
           * first dialog */
          return;
        }
    }

  gtk_widget_destroy (GTK_WIDGET (self));
}

static void
empathy_subscription_dialog_constructed (GObject *object)
{
  EmpathySubscriptionDialog *self = EMPATHY_SUBSCRIPTION_DIALOG (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_subscription_dialog_parent_class)->constructed;
  GtkWidget *content;
  GtkWidget *individual_widget;
  const gchar *alias;
  gchar *tmp;
  EmpathyContact *contact;
  TpConnection *conn;

  if (chain_up != NULL)
    chain_up (object);

  g_assert (self->priv->individual != NULL);

  gtk_window_set_title (GTK_WINDOW (self), _("Subscription Request"));

  alias = folks_alias_details_get_alias (
      FOLKS_ALIAS_DETAILS (self->priv->individual));

  tmp = g_strdup_printf (
      _("%s would like permission to see when you are online"), alias);

  /* Why is there no gtk_message_dialog_set_text()? */
  g_object_set (self, "text", tmp, NULL);
  g_free (tmp);

  if (self->priv->message != NULL)
    {
      tmp = g_strdup_printf ("<i>%s</i>", self->priv->message);
      gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (self),
          "%s", tmp);
      g_free (tmp);
    }

  /* Individual widget */
  individual_widget = empathy_individual_widget_new (self->priv->individual,
      EMPATHY_INDIVIDUAL_WIDGET_EDIT_ALIAS |
      EMPATHY_INDIVIDUAL_WIDGET_EDIT_GROUPS |
      EMPATHY_INDIVIDUAL_WIDGET_SHOW_DETAILS);

  gtk_container_set_border_width (GTK_CONTAINER (individual_widget), 8);

  content = gtk_dialog_get_content_area (GTK_DIALOG (self));

  gtk_box_pack_start (GTK_BOX (content), individual_widget, TRUE, TRUE, 0);
  gtk_widget_show (individual_widget);

  /* Add 'Block' button if supported */
  contact = empathy_contact_dup_from_folks_individual (self->priv->individual);
  conn = empathy_contact_get_connection (contact);

  if (tp_proxy_has_interface_by_id (conn,
        TP_IFACE_QUARK_CONNECTION_INTERFACE_CONTACT_BLOCKING))
    {
      gtk_dialog_add_button (GTK_DIALOG (self),
          _("_Block"), GTK_RESPONSE_REJECT);
    }

  g_object_unref (contact);

  gtk_dialog_add_buttons (GTK_DIALOG (self),
      _("_Decline"), GTK_RESPONSE_NO,
      _("_Accept"), GTK_RESPONSE_YES,
      NULL);

  g_signal_connect (self, "response",
      G_CALLBACK (response_cb), self);
}

static void
empathy_subscription_dialog_dispose (GObject *object)
{
  EmpathySubscriptionDialog *self = EMPATHY_SUBSCRIPTION_DIALOG (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_subscription_dialog_parent_class)->dispose;

  g_clear_object (&self->priv->individual);

  if (chain_up != NULL)
    chain_up (object);
}

static void
empathy_subscription_dialog_finalize (GObject *object)
{
  EmpathySubscriptionDialog *self = EMPATHY_SUBSCRIPTION_DIALOG (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_subscription_dialog_parent_class)->finalize;

  g_free (self->priv->message);

  if (chain_up != NULL)
    chain_up (object);
}

static void
empathy_subscription_dialog_class_init (
    EmpathySubscriptionDialogClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GParamSpec *spec;

  oclass->get_property = empathy_subscription_dialog_get_property;
  oclass->set_property = empathy_subscription_dialog_set_property;
  oclass->constructed = empathy_subscription_dialog_constructed;
  oclass->dispose = empathy_subscription_dialog_dispose;
  oclass->finalize = empathy_subscription_dialog_finalize;

  spec = g_param_spec_object ("individual", "individual",
      "FolksIndividual",
      FOLKS_TYPE_INDIVIDUAL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_INDIVIDUAL, spec);

  spec = g_param_spec_string ("message", "message",
      "Message",
      NULL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_MESSAGE, spec);

  g_type_class_add_private (klass, sizeof (EmpathySubscriptionDialogPriv));
}

static void
empathy_subscription_dialog_init (EmpathySubscriptionDialog *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_SUBSCRIPTION_DIALOG, EmpathySubscriptionDialogPriv);
}

GtkWidget *
empathy_subscription_dialog_new (FolksIndividual *individual,
    const gchar *message)
{
  return g_object_new (EMPATHY_TYPE_SUBSCRIPTION_DIALOG,
      "individual", individual,
      "message", message,
      "message-type", GTK_MESSAGE_QUESTION,
      NULL);
}
