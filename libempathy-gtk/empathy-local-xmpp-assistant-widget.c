/*
 * empathy-local-xmpp-assistant-widget.h - Source for
 * EmpathyLocalXmppAssistantWidget
 *
 * Copyright (C) 2012 - Collabora Ltd.
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with This library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "empathy-local-xmpp-assistant-widget.h"

#include <glib/gi18n-lib.h>
#include <tp-account-widgets/tpaw-account-widget.h>
#include <tp-account-widgets/tpaw-pixbuf-utils.h>
#include <tp-account-widgets/tpaw-utils.h>

#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_ACCOUNT
#include "empathy-debug.h"

G_DEFINE_TYPE (EmpathyLocalXmppAssistantWidget,
    empathy_local_xmpp_assistant_widget, GTK_TYPE_GRID)

enum {
  SIG_VALID = 1,
  LAST_SIGNAL
};

static gulong signals[LAST_SIGNAL] = { 0, };

struct _EmpathyLocalXmppAssistantWidgetPrivate
{
  TpawAccountSettings  *settings;
};

static void
empathy_local_xmpp_assistant_widget_init (EmpathyLocalXmppAssistantWidget *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
      EMPATHY_TYPE_LOCAL_XMPP_ASSISTANT_WIDGET,
      EmpathyLocalXmppAssistantWidgetPrivate);
}

static void
handle_apply_cb (TpawAccountWidget *widget_object,
    gboolean is_valid,
    EmpathyLocalXmppAssistantWidget *self)
{
  g_signal_emit (self, signals[SIG_VALID], 0, is_valid);
}

static void
empathy_local_xmpp_assistant_widget_constructed (GObject *object)
{
  EmpathyLocalXmppAssistantWidget *self = (EmpathyLocalXmppAssistantWidget *)
    object;
  GtkWidget *w;
  GdkPixbuf *pix;
  TpawAccountWidget *account_widget;
  gchar *markup;

  G_OBJECT_CLASS (empathy_local_xmpp_assistant_widget_parent_class)->
    constructed (object);

  gtk_container_set_border_width (GTK_CONTAINER (self), 12);

  w = gtk_label_new (
      _("Empathy can automatically discover and chat with the people "
        "connected on the same network as you. "
        "If you want to use this feature, please check that the "
        "details below are correct."));
  gtk_misc_set_alignment (GTK_MISC (w), 0, 0.5);
  gtk_label_set_line_wrap (GTK_LABEL (w), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (w), 60);
  gtk_grid_attach (GTK_GRID (self), w, 0, 0, 1, 1);
  gtk_widget_show (w);

  pix = tpaw_pixbuf_from_icon_name_sized ("im-local-xmpp", 48);
  if (pix != NULL)
    {
      w = gtk_image_new_from_pixbuf (pix);
      gtk_grid_attach (GTK_GRID (self), w, 1, 0, 1, 1);
      gtk_widget_show (w);

      g_object_unref (pix);
    }

  self->priv->settings = tpaw_account_settings_new ("salut", "local-xmpp",
      NULL, _("People nearby"));

  account_widget = tpaw_account_widget_new_for_protocol (
      self->priv->settings, NULL, TRUE);
  tpaw_account_widget_hide_buttons (account_widget);

  g_signal_connect (account_widget, "handle-apply",
      G_CALLBACK (handle_apply_cb), self);

  gtk_grid_attach (GTK_GRID (self), GTK_WIDGET (account_widget), 0, 1, 2, 1);
  gtk_widget_show (GTK_WIDGET (account_widget));

  w = gtk_label_new (NULL);
  markup = g_strdup_printf (
      "<span size=\"small\">%s</span>",
      _("You can change these details later or disable this feature "
        "by choosing <span style=\"italic\">Edit → Accounts</span> "
        "in the Contact List."));
  gtk_label_set_markup (GTK_LABEL (w), markup);
  g_free (markup);
  gtk_misc_set_alignment (GTK_MISC (w), 0, 0.5);
  gtk_label_set_line_wrap (GTK_LABEL (w), TRUE);
  gtk_grid_attach (GTK_GRID (self), w, 0, 2, 2, 1);
  gtk_widget_show (w);
}

static void
empathy_local_xmpp_assistant_widget_dispose (GObject *object)
{
  EmpathyLocalXmppAssistantWidget *self = (EmpathyLocalXmppAssistantWidget *)
    object;

  g_clear_object (&self->priv->settings);

  G_OBJECT_CLASS (empathy_local_xmpp_assistant_widget_parent_class)->
    dispose (object);
}

static void
empathy_local_xmpp_assistant_widget_class_init (
    EmpathyLocalXmppAssistantWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = empathy_local_xmpp_assistant_widget_constructed;
  object_class->dispose = empathy_local_xmpp_assistant_widget_dispose;

  signals[SIG_VALID] =
      g_signal_new ("valid",
          G_TYPE_FROM_CLASS (klass),
          G_SIGNAL_RUN_LAST, 0, NULL, NULL,
          g_cclosure_marshal_generic,
          G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  g_type_class_add_private (object_class,
      sizeof (EmpathyLocalXmppAssistantWidgetPrivate));
}

GtkWidget *
empathy_local_xmpp_assistant_widget_new ()
{
  return g_object_new (EMPATHY_TYPE_LOCAL_XMPP_ASSISTANT_WIDGET,
      "row-spacing", 12,
      NULL);
}

static void
account_enabled_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccount *account = TP_ACCOUNT (source);
  GError *error = NULL;
  TpAccountManager *account_mgr;

  if (!tp_account_set_enabled_finish (account, result, &error))
    {
      DEBUG ("Failed to enable account: %s", error->message);
      g_error_free (error);
      return;
    }

  account_mgr = tp_account_manager_dup ();

  tpaw_connect_new_account (account, account_mgr);

  g_object_unref (account_mgr);
}

static void
apply_account_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  TpawAccountSettings *settings = TPAW_ACCOUNT_SETTINGS (source);
  TpAccount *account;
  GError *error = NULL;

  if (!tpaw_account_settings_apply_finish (settings, result, NULL, &error))
    {
      DEBUG ("Failed to create account: %s", error->message);
      g_error_free (error);
      return;
    }

  /* enable the newly created account */
  account = tpaw_account_settings_get_account (settings);
  tp_account_set_enabled_async (account, TRUE, account_enabled_cb, NULL);
}

void
empathy_local_xmpp_assistant_widget_create_account (
    EmpathyLocalXmppAssistantWidget *self)
{
  tpaw_account_settings_apply_async (self->priv->settings,
      apply_account_cb, NULL);
}

gboolean
empathy_local_xmpp_assistant_widget_should_create_account (
    TpAccountManager *manager)
{
  gboolean salut_created = FALSE;
  GList *accounts, *l;

  accounts = tp_account_manager_dup_valid_accounts (manager);

  for (l = accounts; l != NULL;  l = g_list_next (l))
    {
      TpAccount *account = TP_ACCOUNT (l->data);

      if (!tp_strdiff (tp_account_get_protocol_name (account), "local-xmpp"))
        {
          salut_created = TRUE;
          break;
        }
    }

  g_list_free_full (accounts, g_object_unref);

  return !salut_created;
}

gboolean
empathy_local_xmpp_assistant_widget_is_valid (
        EmpathyLocalXmppAssistantWidget *self)
{
  return tpaw_account_settings_is_valid (self->priv->settings);
}
