/*
 * Copyright (C) 2005-2007 Imendio AB
 * Copyright (C) 2007-2009 Collabora Ltd.
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
 *          Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
 *          Jonathan Tellier <jonathan.tellier@gmail.com>
 *          Danielle Madeley <danielle.madeley@collabora.co.uk>
 */

#include "config.h"
#include "empathy-accounts-dialog.h"

#include <glib/gi18n-lib.h>
#include <tp-account-widgets/tpaw-account-widget.h>
#include <tp-account-widgets/tpaw-builder.h>
#include <tp-account-widgets/tpaw-user-info.h>
#include <tp-account-widgets/tpaw-pixbuf-utils.h>
#include <tp-account-widgets/tpaw-utils.h>

#include "empathy-accounts-common.h"
#include "empathy-import-dialog.h"
#include "empathy-import-utils.h"
#include "empathy-local-xmpp-assistant-widget.h"
#include "empathy-new-account-dialog.h"
#include "empathy-pkg-kit.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_ACCOUNT
#include "empathy-debug.h"

/* Flashing delay for icons (milliseconds). */
#define FLASH_TIMEOUT 500

/* The primary text of the dialog shown to the user when he is about to lose
 * unsaved changes */
#define PENDING_CHANGES_QUESTION_PRIMARY_TEXT \
  _("There are unsaved modifications to your %.50s account.")
/* The primary text of the dialog shown to the user when he is about to lose
 * an unsaved new account */
#define UNSAVED_NEW_ACCOUNT_QUESTION_PRIMARY_TEXT \
  _("Your new account has not been saved yet.")

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyAccountsDialog)
G_DEFINE_TYPE (EmpathyAccountsDialog, empathy_accounts_dialog, GTK_TYPE_DIALOG);

enum
{
  NOTEBOOK_PAGE_ACCOUNT = 0,
  NOTEBOOK_PAGE_LOADING,
  NOTEBOOK_PAGE_NO_PROTOCOL
};

typedef struct {
  GtkWidget *alignment_settings;
  GtkWidget *alignment_infobar;

  GtkWidget *vbox_details;
  GtkWidget *infobar;
  GtkWidget *label_status;
  GtkWidget *image_status;
  GtkWidget *throbber;
  GtkWidget *enabled_switch;

  GtkWidget *treeview;
  GtkCellRenderer *name_renderer;

  GtkWidget *button_add;
  GtkWidget *button_remove;
  GtkWidget *button_import;

  GtkWidget *image_type;
  GtkWidget *label_name;
  GtkWidget *label_type;
  GtkWidget *dialog_content;
  GtkWidget *user_info;

  GtkWidget *notebook_account;
  GtkWidget *spinner;
  gboolean loading;

  /* We have to keep a weak reference on the actual TpawAccountWidget, not
   * just its GtkWidget. It is the only reliable source we can query to know if
   * there are any unsaved changes to the currently selected account. We can't
   * look at the account settings because it does not contain everything that
   * can be changed using the TpawAccountWidget. For instance, it does not
   * contain the state of the "Enabled" checkbox.
   *
   * Even if we create it ourself, we just get a weak ref and not a strong one
   * as TpawAccountWidget unrefs itself when the GtkWidget is destroyed.
   * That's kinda ugly; cf bgo #640417.
   *
   * */
  TpawAccountWidget *setting_widget;

  gboolean  connecting_show;
  guint connecting_id;

  gulong  settings_ready_id;
  TpawAccountSettings *settings_ready;

  TpAccountManager *account_manager;
  TpawConnectionManagers *cms;
  GNetworkMonitor *connectivity;

  GtkWindow *parent_window;
  TpAccount *initial_selection;

  /* Those are needed when changing the selected row. When a user selects
   * another account and there are unsaved changes on the currently selected
   * one, a confirmation message box is presented to him. Since his answer
   * is retrieved asynchronously, we keep some information as member of the
   * EmpathyAccountsDialog object. */
  gboolean force_change_row;
  GtkTreeRowReference *destination_row;

  GHashTable *icons_cache;
} EmpathyAccountsDialogPriv;

enum {
  COL_NAME,
  COL_STATUS,
  COL_ACCOUNT,
  COL_ACCOUNT_SETTINGS,
  COL_COUNT
};

enum {
  PROP_PARENT = 1
};

static TpawAccountSettings * accounts_dialog_model_get_selected_settings (
    EmpathyAccountsDialog *dialog);

static void accounts_dialog_model_select_first (EmpathyAccountsDialog *dialog);

static void accounts_dialog_update_settings (EmpathyAccountsDialog *dialog,
    TpawAccountSettings *settings);

static void accounts_dialog_model_set_selected (EmpathyAccountsDialog *dialog,
    TpAccount *account);

static void accounts_dialog_connection_changed_cb (TpAccount *account,
    guint old_status,
    guint current,
    guint reason,
    gchar *dbus_error_name,
    GHashTable *details,
    EmpathyAccountsDialog *dialog);

static void accounts_dialog_presence_changed_cb (TpAccount *account,
    guint presence,
    gchar *status,
    gchar *status_message,
    EmpathyAccountsDialog *dialog);

static void accounts_dialog_model_selection_changed (
    GtkTreeSelection *selection,
    EmpathyAccountsDialog *dialog);

static gboolean accounts_dialog_has_pending_change (
    EmpathyAccountsDialog *dialog, TpAccount **account);

static void
accounts_dialog_status_infobar_set_message (EmpathyAccountsDialog *dialog,
    const gchar *message)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  gchar *message_markup;

  message_markup = g_markup_printf_escaped ("<i>%s</i>", message);
  gtk_label_set_markup (GTK_LABEL (priv->label_status), message_markup);

  gtk_widget_set_tooltip_text (priv->label_status, message);

  g_free (message_markup);
}

static void
accounts_dialog_enable_account_cb (GObject *object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccount *account = TP_ACCOUNT (object);
  gboolean enable = GPOINTER_TO_UINT (user_data);
  GError *error = NULL;
  TpAccountManager *am;

  if (!tp_account_set_enabled_finish (account, result, &error))
    {
      DEBUG ("Could not enable the account: %s", error->message);
      g_error_free (error);
      return;
    }

  /* tp_account_is_enabled() is not updated yet at this point */
  if (enable)
    {
      am = tp_account_manager_dup ();

      tpaw_connect_new_account (account, am);
      g_object_unref (am);
    }
}

static void
enable_and_connect_account (TpAccount *account,
    gboolean enable)
{
  tp_account_set_enabled_async (account, enable,
      accounts_dialog_enable_account_cb, GUINT_TO_POINTER (enable));
}

static void
accounts_dialog_enable_switch_active_cb (GtkSwitch *sw,
    GParamSpec *spec,
    EmpathyAccountsDialog *dialog)
{
  TpawAccountSettings *settings;
  TpAccount *account;
  gboolean enable;

  settings = accounts_dialog_model_get_selected_settings (dialog);
  if (settings == NULL)
    return;

  account = tpaw_account_settings_get_account (settings);
  if (account == NULL)
    return;

  enable = gtk_switch_get_active (sw);

  enable_and_connect_account (account, enable);
}

static void
install_haze_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GError *error = NULL;

  if (!empathy_pkg_kit_install_packages_finish  (result, &error))
    {
      DEBUG ("Failed to install telepathy-haze: %s", error->message);

      g_error_free (error);
    }
}

static gboolean
account_is_selected (EmpathyAccountsDialog *dialog,
    TpAccount *account)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  GtkTreeSelection *selection;
  GtkTreeModel *model;
  GtkTreeIter iter;
  TpAccount *selected_account;

  if (account == NULL)
    return FALSE;

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->treeview));
  if (selection == NULL)
    return FALSE;

  if (!gtk_tree_selection_get_selected (selection, &model, &iter))
    return FALSE;

  gtk_tree_model_get (model, &iter, COL_ACCOUNT, &selected_account, -1);

  if (selected_account != NULL)
    g_object_unref (selected_account);

  return account == selected_account;
}

static gboolean
account_can_be_enabled (TpAccount *account)
{
  TpStorageRestrictionFlags storage_restrictions;

  storage_restrictions = tp_account_get_storage_restrictions (account);
  if (storage_restrictions & TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_ENABLED)
    return FALSE;

  /* Butterfly accounts shouldn't be used any more */
  if (!tp_strdiff (tp_account_get_cm_name (account),
        "butterfly"))
    return FALSE;

  return TRUE;
}

static void
accounts_dialog_update_status_infobar (EmpathyAccountsDialog *dialog,
    TpAccount *account)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  gchar                     *status_message = NULL;
  guint                     status;
  guint                     reason;
  guint                     presence;
  gboolean                  account_enabled;
  gboolean                  creating_account;
  gboolean display_switch = TRUE;

  /* do not update the infobar when the account is not selected */
  if (!account_is_selected (dialog, account))
    return;

  if (account != NULL)
    {
      gchar *text;

      status = tp_account_get_connection_status (account, &reason);
      presence = tp_account_get_current_presence (account, NULL, &status_message);
      account_enabled = tp_account_is_enabled (account);
      creating_account = FALSE;

      if (status == TP_CONNECTION_STATUS_CONNECTED &&
          (presence == TP_CONNECTION_PRESENCE_TYPE_OFFLINE ||
           presence == TP_CONNECTION_PRESENCE_TYPE_UNSET))
        /* If presence is Unset (CM doesn't implement SimplePresence) but we
         * are connected, consider ourself as Available.
         * We also check Offline because of this MC5 bug: fd.o #26060 */
        presence = TP_CONNECTION_PRESENCE_TYPE_AVAILABLE;

      /* set presence to offline if account is disabled
       * (else no icon is shown in infobar)*/
      if (!account_enabled)
        presence = TP_CONNECTION_PRESENCE_TYPE_OFFLINE;

      display_switch = account_can_be_enabled (account);

      text = g_markup_printf_escaped ("<b>%.50s</b>",
          tp_account_get_display_name (account));
      gtk_label_set_markup (GTK_LABEL (priv->label_name), text);
      g_free (text);
    }
  else
    {
      status = TP_CONNECTION_STATUS_DISCONNECTED;
      presence = TP_CONNECTION_PRESENCE_TYPE_OFFLINE;
      account_enabled = FALSE;
      creating_account = TRUE;
    }

  gtk_image_set_from_icon_name (GTK_IMAGE (priv->image_status),
      empathy_icon_name_for_presence (presence), GTK_ICON_SIZE_SMALL_TOOLBAR);

  /* update the enabled switch */
  g_signal_handlers_block_by_func (priv->enabled_switch,
      accounts_dialog_enable_switch_active_cb, dialog);
  gtk_switch_set_active (GTK_SWITCH (priv->enabled_switch),
      account_enabled);
  g_signal_handlers_unblock_by_func (priv->enabled_switch,
      accounts_dialog_enable_switch_active_cb, dialog);

  /* Display the Enable switch if account supports it */
  gtk_widget_set_visible (priv->enabled_switch, display_switch);

  if (account_enabled)
    {
      switch (status)
        {
          case TP_CONNECTION_STATUS_CONNECTING:
            accounts_dialog_status_infobar_set_message (dialog,
                _("Connecting…"));
            gtk_info_bar_set_message_type (GTK_INFO_BAR (priv->infobar),
                GTK_MESSAGE_INFO);

            gtk_spinner_start (GTK_SPINNER (priv->throbber));
            gtk_widget_show (priv->throbber);
            gtk_widget_hide (priv->image_status);
            break;
          case TP_CONNECTION_STATUS_CONNECTED:
            if (g_strcmp0 (status_message, "") == 0)
              {
                gchar *message;

                message = g_strdup_printf ("%s",
                    empathy_presence_get_default_message (presence));

                accounts_dialog_status_infobar_set_message (dialog, message);
                g_free (message);
              }
            else
              {
                gchar *message;

                message = g_strdup_printf ("%s — %s",
                    empathy_presence_get_default_message (presence),
                    status_message);

                accounts_dialog_status_infobar_set_message (dialog, message);
                g_free (message);
              }
            gtk_info_bar_set_message_type (GTK_INFO_BAR (priv->infobar),
                GTK_MESSAGE_INFO);

            gtk_widget_show (priv->image_status);
            gtk_widget_hide (priv->throbber);
            break;
          case TP_CONNECTION_STATUS_DISCONNECTED:
            if (reason == TP_CONNECTION_STATUS_REASON_REQUESTED)
              {
                gchar *message;

                message = g_strdup_printf (_("Offline — %s"),
                    empathy_account_get_error_message (account, NULL));
                gtk_info_bar_set_message_type (GTK_INFO_BAR (priv->infobar),
                    GTK_MESSAGE_WARNING);

                accounts_dialog_status_infobar_set_message (dialog, message);
                g_free (message);
              }
            else
              {
                gchar *message;

                message = g_strdup_printf (_("Disconnected — %s"),
                    empathy_account_get_error_message (account, NULL));
                gtk_info_bar_set_message_type (GTK_INFO_BAR (priv->infobar),
                    GTK_MESSAGE_ERROR);

                accounts_dialog_status_infobar_set_message (dialog, message);
                g_free (message);
              }

            if (!g_network_monitor_get_network_available (priv->connectivity))
               accounts_dialog_status_infobar_set_message (dialog,
                    _("Offline — No Network Connection"));

            gtk_spinner_stop (GTK_SPINNER (priv->throbber));
            gtk_widget_show (priv->image_status);
            gtk_widget_hide (priv->throbber);
            break;
          default:
            accounts_dialog_status_infobar_set_message (dialog, _("Unknown Status"));
            gtk_info_bar_set_message_type (GTK_INFO_BAR (priv->infobar),
                GTK_MESSAGE_WARNING);

            gtk_spinner_stop (GTK_SPINNER (priv->throbber));
            gtk_widget_hide (priv->image_status);
            gtk_widget_hide (priv->throbber);
        }
    }
  else
    {
      if (!tp_strdiff (tp_account_get_cm_name (account),
            "butterfly"))
        {
          const gchar *packages[] = { "telepathy-haze", NULL };

          accounts_dialog_status_infobar_set_message (dialog,
              _("This account has been disabled because it relies on an old, "
                "unsupported backend. Please install telepathy-haze and "
                "restart your session to migrate the account."));

          empathy_pkg_kit_install_packages_async (0, packages, NULL, NULL,
              install_haze_cb, NULL);
        }
      else
        {
          accounts_dialog_status_infobar_set_message (dialog,
              _("Offline — Account Disabled"));
        }

      gtk_info_bar_set_message_type (GTK_INFO_BAR (priv->infobar),
          GTK_MESSAGE_WARNING);
      gtk_spinner_stop (GTK_SPINNER (priv->throbber));
      gtk_widget_show (priv->image_status);
      gtk_widget_hide (priv->throbber);
    }

  gtk_widget_show (priv->label_status);

  if (!creating_account)
    gtk_widget_show (priv->infobar);
  else
    gtk_widget_hide (priv->infobar);

  g_free (status_message);
}

void
empathy_account_dialog_cancel (EmpathyAccountsDialog *dialog)
{
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeSelection *selection;
  GtkTreeIter iter;
  TpawAccountSettings *settings;
  TpAccount *account;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  view = GTK_TREE_VIEW (priv->treeview);
  selection = gtk_tree_view_get_selection (view);

  if (!gtk_tree_selection_get_selected (selection, &model, &iter))
    return;

  gtk_tree_model_get (model, &iter,
      COL_ACCOUNT_SETTINGS, &settings,
      COL_ACCOUNT, &account, -1);

  tpaw_account_widget_discard_pending_changes (priv->setting_widget);

  if (account == NULL)
    {
      /* We were creating an account. We remove the selected row */
      gtk_list_store_remove (GTK_LIST_STORE (model), &iter);
    }
  else
    {
      /* We were modifying an account. We discard the changes by reloading the
       * settings and the UI. */
      accounts_dialog_update_settings (dialog, settings);
      g_object_unref (account);
    }

  gtk_widget_set_sensitive (priv->treeview, TRUE);
  gtk_widget_set_sensitive (priv->button_add, TRUE);
  gtk_widget_set_sensitive (priv->button_remove, TRUE);
  gtk_widget_set_sensitive (priv->button_import, TRUE);

  if (settings != NULL)
    g_object_unref (settings);
}

static void
empathy_account_dialog_widget_cancelled_cb (
    TpawAccountWidget *widget_object,
    EmpathyAccountsDialog *dialog)
{
  empathy_account_dialog_cancel (dialog);
}

static gboolean
accounts_dialog_has_valid_accounts (EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  GtkTreeModel *model;
  GtkTreeIter iter;
  gboolean creating;

  g_object_get (priv->setting_widget,
      "creating-account", &creating, NULL);

  if (!creating)
    return TRUE;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview));

  if (gtk_tree_model_get_iter_first (model, &iter))
    return gtk_tree_model_iter_next (model, &iter);

  return FALSE;
}

static void
account_dialog_create_edit_params_dialog (EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  TpawAccountSettings *settings;
  GtkWidget *subdialog, *content_area, *align;

  settings = accounts_dialog_model_get_selected_settings (dialog);
  if (settings == NULL)
    return;

  subdialog = gtk_dialog_new_with_buttons (_("Edit Connection Parameters"),
      GTK_WINDOW (dialog),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      NULL, NULL);

  gtk_window_set_resizable (GTK_WINDOW (subdialog), FALSE);

  priv->setting_widget = (TpawAccountWidget *)
    tpaw_account_widget_new_for_protocol (settings, NULL, FALSE);

  g_object_add_weak_pointer (G_OBJECT (priv->setting_widget),
      (gpointer *) &priv->setting_widget);

  if (accounts_dialog_has_valid_accounts (dialog))
    tpaw_account_widget_set_other_accounts_exist (
        priv->setting_widget, TRUE);

  g_signal_connect (priv->setting_widget, "cancelled",
          G_CALLBACK (empathy_account_dialog_widget_cancelled_cb), dialog);

  g_signal_connect_swapped (priv->setting_widget, "close",
      G_CALLBACK (gtk_widget_destroy), subdialog);

  content_area = gtk_dialog_get_content_area (GTK_DIALOG (subdialog));

  align = gtk_alignment_new (0.5, 0.5, 1, 1);
  gtk_alignment_set_padding (GTK_ALIGNMENT (align), 6, 0, 6, 6);

  gtk_container_add (GTK_CONTAINER (align), GTK_WIDGET (priv->setting_widget));
  gtk_box_pack_start (GTK_BOX (content_area), align, TRUE, TRUE, 0);

  gtk_widget_show (GTK_WIDGET (priv->setting_widget));
  gtk_widget_show (align);
  gtk_widget_show (subdialog);
}

static void
use_external_storage_provider (EmpathyAccountsDialog *self,
    TpAccount *account)
{
  const gchar *provider;

  provider = tp_account_get_storage_provider (account);
  if (!tp_strdiff (provider, "com.meego.libsocialweb"))
    {
      empathy_launch_external_app ("gnome-control-center.desktop",
          "bisho.desktop", NULL);
      return;
    }
  else if (!tp_strdiff (provider, EMPATHY_GOA_PROVIDER))
    {
      empathy_launch_external_app ("gnome-online-accounts-panel.desktop",
          NULL, NULL);
      return;
    }
  else if (!tp_strdiff (provider, EMPATHY_UOA_PROVIDER))
    {
      empathy_launch_external_app ("unity-credentials-panel.desktop",
          NULL, NULL);
      return;
    }
  else
    {
      DEBUG ("Don't know how to handle %s", provider);
      return;
    }
}

static void
account_dialow_show_edit_params_dialog (EmpathyAccountsDialog *dialog,
    GtkButton *button)
{
  TpawAccountSettings *settings;
  TpAccount *account;
  TpStorageRestrictionFlags storage_restrictions;

  settings = accounts_dialog_model_get_selected_settings (dialog);
  if (settings == NULL)
    return;

  account = tpaw_account_settings_get_account (settings);
  g_return_if_fail (account != NULL);

  storage_restrictions = tp_account_get_storage_restrictions (account);

  /* Empathy can only edit accounts without the Cannot_Set_Parameters flag */
  if (storage_restrictions & TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_PARAMETERS)
    {
      DEBUG ("Account is provided by an external storage provider");

      use_external_storage_provider (dialog, account);
    }
  else
    {
      account_dialog_create_edit_params_dialog (dialog);
    }
}

static void
account_dialog_create_dialog_content (EmpathyAccountsDialog *dialog,
    TpawAccountSettings *settings)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  const gchar *icon_name;
  TpAccount *account;
  GtkWidget *bbox, *button;
  GtkWidget *alig;

  account = tpaw_account_settings_get_account (settings);

  priv->dialog_content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_add (GTK_CONTAINER (priv->alignment_settings),
      priv->dialog_content);
  gtk_widget_show (priv->dialog_content);

  alig = gtk_alignment_new (0.5, 0, 1, 1);
  priv->user_info = tpaw_user_info_new (account);
  gtk_container_add (GTK_CONTAINER (alig), priv->user_info);
  gtk_box_pack_start (GTK_BOX (priv->dialog_content), alig, TRUE, TRUE, 0);
  gtk_widget_show (alig);
  gtk_widget_show (priv->user_info);

  bbox = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_button_box_set_layout (GTK_BUTTON_BOX (bbox), GTK_BUTTONBOX_END);
  gtk_box_pack_end (GTK_BOX (priv->dialog_content), bbox, FALSE, TRUE, 0);
  gtk_widget_show (bbox);

  button = gtk_button_new_with_mnemonic (_("_Edit Connection Parameters…"));
  gtk_box_pack_start (GTK_BOX (bbox), button, FALSE, TRUE, 0);
  gtk_widget_show (button);
  g_signal_connect_swapped (button, "clicked",
      G_CALLBACK (account_dialow_show_edit_params_dialog), dialog);

  icon_name = tpaw_account_settings_get_icon_name (settings);

  if (!gtk_icon_theme_has_icon (gtk_icon_theme_get_default (),
          icon_name))
    /* show the default icon; keep this in sync with the default
     * one in empathy-accounts-dialog.ui.
     */
    icon_name = GTK_STOCK_CUT;

  gtk_image_set_from_icon_name (GTK_IMAGE (priv->image_type),
      icon_name, GTK_ICON_SIZE_DIALOG);
  gtk_widget_set_tooltip_text (priv->image_type,
      tpaw_protocol_name_to_display_name
      (tpaw_account_settings_get_protocol (settings)));
  gtk_widget_show (priv->image_type);

  accounts_dialog_update_status_infobar (dialog, account);
}

static void
account_dialog_settings_ready_cb (TpawAccountSettings *settings,
    GParamSpec *spec,
    EmpathyAccountsDialog *dialog)
{
  if (tpaw_account_settings_is_ready (settings))
    account_dialog_create_dialog_content (dialog, settings);
}

static void
accounts_dialog_model_select_first (EmpathyAccountsDialog *dialog)
{
  GtkTreeView      *view;
  GtkTreeModel     *model;
  GtkTreeSelection *selection;
  GtkTreeIter       iter;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  /* select first */
  view = GTK_TREE_VIEW (priv->treeview);
  model = gtk_tree_view_get_model (view);

  if (gtk_tree_model_get_iter_first (model, &iter))
    {
      selection = gtk_tree_view_get_selection (view);
      gtk_tree_selection_select_iter (selection, &iter);
    }
  else
    {
      accounts_dialog_update_settings (dialog, NULL);
    }
}

static gboolean
accounts_dialog_has_pending_change (EmpathyAccountsDialog *dialog,
    TpAccount **account)
{
  GtkTreeIter iter;
  GtkTreeModel *model;
  GtkTreeSelection *selection;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->treeview));

  if (gtk_tree_selection_get_selected (selection, &model, &iter))
    gtk_tree_model_get (model, &iter, COL_ACCOUNT, account, -1);

  return priv->setting_widget != NULL
      && tpaw_account_widget_contains_pending_changes (
          priv->setting_widget);
}

static void
accounts_dialog_show_question_dialog (EmpathyAccountsDialog *dialog,
    const gchar *primary_text,
    const gchar *secondary_text,
    GCallback response_callback,
    gpointer user_data,
    const gchar *first_button_text,
    ...)
{
  va_list button_args;
  GtkWidget *message_dialog;
  const gchar *button_text;

  message_dialog = gtk_message_dialog_new (GTK_WINDOW (dialog),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_NONE,
      "%s", primary_text);

  gtk_message_dialog_format_secondary_text (
      GTK_MESSAGE_DIALOG (message_dialog), "%s", secondary_text);

  va_start (button_args, first_button_text);
  for (button_text = first_button_text;
       button_text;
       button_text = va_arg (button_args, const gchar *))
    {
      gint response_id;
      response_id = va_arg (button_args, gint);

      gtk_dialog_add_button (GTK_DIALOG (message_dialog), button_text,
          response_id);
    }
  va_end (button_args);

  g_signal_connect (message_dialog, "response", response_callback, user_data);

  gtk_widget_show (message_dialog);
}

static gchar *
get_dialog_primary_text (TpAccount *account)
{
  if (account != NULL)
    {
      /* Existing account */
      return g_strdup_printf (PENDING_CHANGES_QUESTION_PRIMARY_TEXT,
          tp_account_get_display_name (account));
    }
  else
    {
      /* Newly created account */
      return g_strdup (UNSAVED_NEW_ACCOUNT_QUESTION_PRIMARY_TEXT);
    }
}

static void
accounts_dialog_button_add_clicked_cb (GtkWidget *button,
    EmpathyAccountsDialog *self)
{
  GtkWidget *dialog;
  gint response;

  dialog = empathy_new_account_dialog_new (GTK_WINDOW (self));
  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

  response = gtk_dialog_run (GTK_DIALOG (dialog));

  if (response == GTK_RESPONSE_APPLY)
    {
      TpawAccountSettings *settings;
      TpAccount *account;

      settings = empathy_new_account_dialog_get_settings (
          EMPATHY_NEW_ACCOUNT_DIALOG (dialog));

      /* The newly created account has already been added by
       * accounts_dialog_account_validity_changed_cb so we just
       * have to select it. */
      account = tpaw_account_settings_get_account (settings);
      accounts_dialog_model_set_selected (self, account);
    }

  gtk_widget_destroy (dialog);
}

static void
accounts_dialog_update_settings (EmpathyAccountsDialog *dialog,
    TpawAccountSettings *settings)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  if (priv->settings_ready != NULL)
    {
      g_signal_handler_disconnect (priv->settings_ready,
          priv->settings_ready_id);
      priv->settings_ready = NULL;
      priv->settings_ready_id = 0;
    }

  if (!settings)
    {
      GtkTreeView  *view;
      GtkTreeModel *model;
      GtkTreeSelection *selection;

      view = GTK_TREE_VIEW (priv->treeview);
      model = gtk_tree_view_get_model (view);
      selection = gtk_tree_view_get_selection (view);

      if (gtk_tree_model_iter_n_children (model, NULL) > 0)
        {
          /* We have configured accounts, select the first one if there
           * is no other account selected already. */
          if (!gtk_tree_selection_get_selected (selection, NULL, NULL))
            accounts_dialog_model_select_first (dialog);

          return;
        }

      /* No account selected */
      gtk_widget_hide (priv->vbox_details);
      gtk_widget_set_sensitive (priv->button_add, TRUE);
      gtk_widget_set_sensitive (priv->button_remove, FALSE);

      gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook_account),
          NOTEBOOK_PAGE_NO_PROTOCOL);
      return;
    }

  /* We have an account selected, destroy old settings and create a new
   * one for the account selected */
  gtk_widget_show (priv->vbox_details);

  if (priv->user_info != NULL)
    {
      tpaw_user_info_apply_async ((TpawUserInfo *) priv->user_info,
          NULL, NULL);
      priv->user_info = NULL;
    }
  if (priv->dialog_content)
    {
      gtk_widget_destroy (priv->dialog_content);
      priv->dialog_content = NULL;
    }

  if (tpaw_account_settings_is_ready (settings))
    {
      account_dialog_create_dialog_content (dialog, settings);
    }
  else
    {
      priv->settings_ready = settings;
      priv->settings_ready_id =
        g_signal_connect (settings, "notify::ready",
            G_CALLBACK (account_dialog_settings_ready_cb), dialog);
    }

}

static void
accounts_dialog_name_editing_started_cb (GtkCellRenderer *renderer,
    GtkCellEditable *editable,
    gchar *path,
    EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  if (priv->connecting_id)
    g_source_remove (priv->connecting_id);

  DEBUG ("Editing account name started; stopping flashing");
}

static const gchar *
get_status_icon_for_account (EmpathyAccountsDialog *self,
    TpAccount *account)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (self);
  TpConnectionStatus status;
  TpConnectionStatusReason reason;
  TpConnectionPresenceType presence;

  if (account == NULL)
    return empathy_icon_name_for_presence (TP_CONNECTION_PRESENCE_TYPE_OFFLINE);

  if (!tp_account_is_enabled (account))
    return empathy_icon_name_for_presence (TP_CONNECTION_PRESENCE_TYPE_OFFLINE);

  status = tp_account_get_connection_status (account, &reason);

  if (status == TP_CONNECTION_STATUS_DISCONNECTED)
    {
      if (reason != TP_CONNECTION_STATUS_REASON_REQUESTED)
        /* An error occured */
        return GTK_STOCK_DIALOG_ERROR;

      presence = TP_CONNECTION_PRESENCE_TYPE_OFFLINE;
    }
  else if (status == TP_CONNECTION_STATUS_CONNECTING)
    {
      /* Account is connecting. Display a blinking account alternating between
       * the offline icon and the requested presence. */
      if (priv->connecting_show)
        presence = tp_account_get_requested_presence (account, NULL, NULL);
      else
        presence = TP_CONNECTION_PRESENCE_TYPE_OFFLINE;
    }
  else
    {
      /* status == TP_CONNECTION_STATUS_CONNECTED */
      presence = tp_account_get_current_presence (account, NULL, NULL);

      /* If presence is Unset (CM doesn't implement SimplePresence),
       * display the 'available' icon.
       * We also check Offline because of this MC5 bug: fd.o #26060 */
      if (presence == TP_CONNECTION_PRESENCE_TYPE_OFFLINE ||
          presence == TP_CONNECTION_PRESENCE_TYPE_UNSET)
        presence = TP_CONNECTION_PRESENCE_TYPE_AVAILABLE;
    }

  return empathy_icon_name_for_presence (presence);
}

static GdkPixbuf *
ensure_icon (EmpathyAccountsDialog *self,
    const gchar *icon_name)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (self);
  GdkPixbuf *pixbuf;

  pixbuf = g_hash_table_lookup (priv->icons_cache, icon_name);
  if (pixbuf == NULL)
    {
      pixbuf = tpaw_pixbuf_from_icon_name (icon_name, GTK_ICON_SIZE_BUTTON);

      if (pixbuf == NULL)
        return NULL;

      g_hash_table_insert (priv->icons_cache, g_strdup (icon_name),
          pixbuf);
    }

  return g_object_ref (pixbuf);
}

static void
accounts_dialog_model_status_pixbuf_data_func (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *model,
    GtkTreeIter *iter,
    EmpathyAccountsDialog *dialog)
{
  TpAccount *account;
  const gchar *icon_name;
  GdkPixbuf *pixbuf;

  gtk_tree_model_get (model, iter, COL_ACCOUNT, &account, -1);

  icon_name = get_status_icon_for_account (dialog, account);
  pixbuf = ensure_icon (dialog, icon_name);

  g_object_set (cell,
      "pixbuf", pixbuf,
      NULL);

  if (account != NULL)
    g_object_unref (account);

  if (pixbuf != NULL)
    g_object_unref (pixbuf);
}

static void
accounts_dialog_model_protocol_pixbuf_data_func (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *model,
    GtkTreeIter *iter,
    EmpathyAccountsDialog *dialog)
{
  TpawAccountSettings  *settings;
  gchar              *icon_name;
  GdkPixbuf          *pixbuf;
  TpConnectionStatus  status;

  gtk_tree_model_get (model, iter,
      COL_STATUS, &status,
      COL_ACCOUNT_SETTINGS, &settings,
      -1);

  icon_name = tpaw_account_settings_get_icon_name (settings);
  pixbuf = ensure_icon (dialog, icon_name);

  g_object_set (cell,
      "visible", TRUE,
      "pixbuf", pixbuf,
      NULL);

  g_object_unref (settings);

  if (pixbuf)
    g_object_unref (pixbuf);
}

static gboolean
accounts_dialog_row_changed_foreach (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    gpointer user_data)
{
  TpAccount *account;

  gtk_tree_model_get (model, iter, COL_ACCOUNT, &account, -1);

  if (account == NULL)
    return FALSE;

  if (tp_account_get_connection_status (account, NULL) ==
      TP_CONNECTION_STATUS_CONNECTING)
    {
      /* Only update the row where we have a connecting account as that's the
       * ones having a blinking icon. */
      gtk_tree_model_row_changed (model, path, iter);
    }

  g_object_unref (account);
  return FALSE;
}

static gboolean
accounts_dialog_flash_connecting_cb (EmpathyAccountsDialog *dialog)
{
  GtkTreeView  *view;
  GtkTreeModel *model;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  priv->connecting_show = !priv->connecting_show;

  view = GTK_TREE_VIEW (priv->treeview);
  model = gtk_tree_view_get_model (view);

  gtk_tree_model_foreach (model, accounts_dialog_row_changed_foreach, NULL);

  return TRUE;
}

static void
accounts_dialog_name_edited_cb (GtkCellRendererText *renderer,
    gchar *path,
    gchar *new_text,
    EmpathyAccountsDialog *dialog)
{
  TpawAccountSettings    *settings;
  GtkTreeModel *model;
  GtkTreePath  *treepath;
  GtkTreeIter   iter;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  gboolean connecting;

  empathy_account_manager_get_accounts_connected (&connecting);

  if (connecting)
    {
      priv->connecting_id = g_timeout_add (FLASH_TIMEOUT,
          (GSourceFunc) accounts_dialog_flash_connecting_cb,
          dialog);
    }

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview));
  treepath = gtk_tree_path_new_from_string (path);
  gtk_tree_model_get_iter (model, &iter, treepath);
  gtk_tree_model_get (model, &iter,
      COL_ACCOUNT_SETTINGS, &settings,
      -1);
  gtk_list_store_set (GTK_LIST_STORE (model), &iter,
      COL_NAME, new_text,
      -1);
  gtk_tree_path_free (treepath);

  tpaw_account_settings_set_display_name_async (settings, new_text,
      NULL, NULL);
  g_object_set (settings, "display-name-overridden", TRUE, NULL);
  g_object_unref (settings);
}

static void
accounts_dialog_delete_account_response_cb (GtkDialog *message_dialog,
  gint response_id,
  gpointer user_data)
{
  TpAccount *account;
  GtkTreeModel *model;
  GtkTreeIter iter;
  GtkTreeSelection *selection;
  EmpathyAccountsDialog *account_dialog = EMPATHY_ACCOUNTS_DIALOG (user_data);
  EmpathyAccountsDialogPriv *priv = GET_PRIV (account_dialog);

  if (response_id == GTK_RESPONSE_YES)
    {
      selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->treeview));

      if (!gtk_tree_selection_get_selected (selection, &model, &iter))
        return;

      gtk_tree_model_get (model, &iter, COL_ACCOUNT, &account, -1);

      if (account != NULL)
        {
          tp_account_remove_async (account, NULL, NULL);
          g_object_unref (account);
          account = NULL;
        }

      /* No need to call accounts_dialog_model_selection_changed while
       * removing as we are going to call accounts_dialog_model_select_first
       * right after which will update the selection. */
      g_signal_handlers_block_by_func (selection,
          accounts_dialog_model_selection_changed, account_dialog);

      gtk_list_store_remove (GTK_LIST_STORE (model), &iter);

      g_signal_handlers_unblock_by_func (selection,
          accounts_dialog_model_selection_changed, account_dialog);

      accounts_dialog_model_select_first (account_dialog);
    }

  gtk_widget_destroy (GTK_WIDGET (message_dialog));
}

static void
accounts_dialog_remove_account_iter (EmpathyAccountsDialog *dialog,
    GtkTreeIter *iter)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  TpAccount *account;
  GtkTreeModel *model;
  gchar *question_dialog_primary_text;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview));

  gtk_tree_model_get (model, iter, COL_ACCOUNT, &account, -1);

  if (account == NULL || !tp_account_is_valid (account))
    {
      if (account != NULL)
        g_object_unref (account);
      gtk_list_store_remove (GTK_LIST_STORE (model), iter);
      accounts_dialog_model_select_first (dialog);
      return;
    }

  question_dialog_primary_text = g_strdup_printf (
      _("Do you want to remove %.50s from your computer?"),
      tp_account_get_display_name (account));

  accounts_dialog_show_question_dialog (dialog, question_dialog_primary_text,
      _("This will not remove your account on the server."),
      G_CALLBACK (accounts_dialog_delete_account_response_cb),
      dialog,
      GTK_STOCK_CANCEL, GTK_RESPONSE_NO,
      GTK_STOCK_REMOVE, GTK_RESPONSE_YES, NULL);

  g_free (question_dialog_primary_text);
  g_object_unref (account);
}

static void
accounts_dialog_button_remove_clicked_cb (GtkWidget *button,
    EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  GtkTreeView  *view;
  GtkTreeSelection *selection;
  GtkTreeIter iter;

  view = GTK_TREE_VIEW (priv->treeview);
  selection = gtk_tree_view_get_selection (view);
  if (!gtk_tree_selection_get_selected (selection, NULL, &iter))
      return;

  accounts_dialog_remove_account_iter (dialog, &iter);
}

static void
accounts_dialog_model_add_columns (EmpathyAccountsDialog *dialog)
{
  GtkTreeView       *view;
  GtkTreeViewColumn *column;
  GtkCellRenderer   *cell;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  view = GTK_TREE_VIEW (priv->treeview);
  gtk_tree_view_set_headers_visible (view, FALSE);

  /* Account column */
  column = gtk_tree_view_column_new ();
  gtk_tree_view_column_set_expand (column, TRUE);
  gtk_tree_view_append_column (view, column);

  /* Status icon renderer */
  cell = gtk_cell_renderer_pixbuf_new ();
  gtk_tree_view_column_pack_start (column, cell, FALSE);
  gtk_tree_view_column_set_cell_data_func (column, cell,
      (GtkTreeCellDataFunc)
      accounts_dialog_model_status_pixbuf_data_func,
      dialog,
      NULL);

  /* Protocol icon renderer */
  cell = gtk_cell_renderer_pixbuf_new ();
  gtk_tree_view_column_pack_start (column, cell, FALSE);
  gtk_tree_view_column_set_cell_data_func (column, cell,
      (GtkTreeCellDataFunc)
      accounts_dialog_model_protocol_pixbuf_data_func,
      dialog,
      NULL);

  /* Name renderer */
  priv->name_renderer = gtk_cell_renderer_text_new ();
  g_object_set (priv->name_renderer,
      "ellipsize", PANGO_ELLIPSIZE_END,
      "width-chars", 25,
      "editable", TRUE,
      NULL);
  gtk_tree_view_column_pack_start (column, priv->name_renderer, TRUE);
  gtk_tree_view_column_add_attribute (column, priv->name_renderer,
      "text", COL_NAME);
  g_signal_connect (priv->name_renderer, "edited",
      G_CALLBACK (accounts_dialog_name_edited_cb),
      dialog);
  g_signal_connect (priv->name_renderer, "editing-started",
      G_CALLBACK (accounts_dialog_name_editing_started_cb),
      dialog);
  g_object_set (priv->name_renderer, "ypad", 4, NULL);
}

static TpawAccountSettings *
accounts_dialog_model_get_selected_settings (EmpathyAccountsDialog *dialog)
{
  GtkTreeView      *view;
  GtkTreeModel     *model;
  GtkTreeSelection *selection;
  GtkTreeIter       iter;
  TpawAccountSettings   *settings;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  view = GTK_TREE_VIEW (priv->treeview);
  selection = gtk_tree_view_get_selection (view);

  if (!gtk_tree_selection_get_selected (selection, &model, &iter))
    return NULL;

  gtk_tree_model_get (model, &iter,
      COL_ACCOUNT_SETTINGS, &settings, -1);

  return settings;
}

static void
accounts_dialog_model_selection_changed (GtkTreeSelection *selection,
    EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  TpawAccountSettings *settings;
  GtkTreeModel *model;
  GtkTreeIter   iter;
  gboolean      is_selection;
  gboolean creating = FALSE;

  is_selection = gtk_tree_selection_get_selected (selection, &model, &iter);

  settings = accounts_dialog_model_get_selected_settings (dialog);
  accounts_dialog_update_settings (dialog, settings);

  if (settings != NULL)
    g_object_unref (settings);

  if (priv->setting_widget != NULL)
    {
      g_object_get (priv->setting_widget,
          "creating-account", &creating, NULL);
    }

  /* Update remove button sensitivity */
  gtk_widget_set_sensitive (priv->button_remove, is_selection && !creating &&
      !priv->loading);
}

static void
accounts_dialog_selection_change_response_cb (GtkDialog *message_dialog,
  gint response_id,
  gpointer *user_data)
{
  EmpathyAccountsDialog *dialog = EMPATHY_ACCOUNTS_DIALOG (user_data);
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  gtk_widget_destroy (GTK_WIDGET (message_dialog));

    if (response_id == GTK_RESPONSE_YES && priv->destination_row != NULL)
      {
        /* The user wants to lose unsaved changes to the currently selected
         * account and select another account. We discard the changes and
         * select the other account. */
        GtkTreePath *path;
        GtkTreeSelection *selection;

        priv->force_change_row = TRUE;
        tpaw_account_widget_discard_pending_changes (
            priv->setting_widget);

        path = gtk_tree_row_reference_get_path (priv->destination_row);
        selection = gtk_tree_view_get_selection (
            GTK_TREE_VIEW (priv->treeview));

        if (path != NULL)
          {
            /* This will trigger a call to
             * accounts_dialog_account_selection_change() */
            gtk_tree_selection_select_path (selection, path);
            gtk_tree_path_free (path);
          }

        gtk_tree_row_reference_free (priv->destination_row);
      }
    else
      {
        priv->force_change_row = FALSE;
      }
}

static gboolean
accounts_dialog_account_selection_change (GtkTreeSelection *selection,
    GtkTreeModel *model,
    GtkTreePath *path,
    gboolean path_currently_selected,
    gpointer data)
{
  TpAccount *account = NULL;
  EmpathyAccountsDialog *dialog = EMPATHY_ACCOUNTS_DIALOG (data);
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  gboolean ret;

  if (priv->force_change_row)
    {
      /* We came back here because the user wants to discard changes to his
       * modified account. The changes have already been discarded so we
       * just change the selected row. */
      priv->force_change_row = FALSE;
      return TRUE;
    }

  if (accounts_dialog_has_pending_change (dialog, &account))
    {
      /* The currently selected account has some unsaved changes. We ask
       * the user if he really wants to lose his changes and select another
       * account */
      gchar *question_dialog_primary_text = get_dialog_primary_text (account);
      priv->destination_row = gtk_tree_row_reference_new (model, path);

      accounts_dialog_show_question_dialog (dialog,
          question_dialog_primary_text,
          _("You are about to select another account, which will discard\n"
              "your changes. Are you sure you want to proceed?"),
          G_CALLBACK (accounts_dialog_selection_change_response_cb),
          dialog,
          GTK_STOCK_CANCEL, GTK_RESPONSE_NO,
          GTK_STOCK_DISCARD, GTK_RESPONSE_YES, NULL);

      g_free (question_dialog_primary_text);
      ret = FALSE;
    }
  else
    {
      ret = TRUE;
    }

  if (account != NULL)
    g_object_unref (account);

  return ret;
}

static void
accounts_dialog_model_setup (EmpathyAccountsDialog *dialog)
{
  GtkListStore     *store;
  GtkTreeSelection *selection;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  store = gtk_list_store_new (COL_COUNT,
      G_TYPE_STRING,         /* name */
      G_TYPE_UINT,           /* status */
      TP_TYPE_ACCOUNT,   /* account */
      TPAW_TYPE_ACCOUNT_SETTINGS); /* settings */

  gtk_tree_view_set_model (GTK_TREE_VIEW (priv->treeview),
      GTK_TREE_MODEL (store));

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->treeview));
  gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);
  gtk_tree_selection_set_select_function (selection,
      accounts_dialog_account_selection_change, dialog, NULL);

  g_signal_connect (selection, "changed",
      G_CALLBACK (accounts_dialog_model_selection_changed),
      dialog);

  gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store),
      COL_NAME, GTK_SORT_ASCENDING);

  accounts_dialog_model_add_columns (dialog);

  g_object_unref (store);
}

static gboolean
accounts_dialog_get_account_iter (EmpathyAccountsDialog *dialog,
    TpAccount *account,
    GtkTreeIter *iter)
{
  GtkTreeView      *view;
  GtkTreeModel     *model;
  gboolean          ok;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  /* Update the status in the model */
  view = GTK_TREE_VIEW (priv->treeview);
  model = gtk_tree_view_get_model (view);

  for (ok = gtk_tree_model_get_iter_first (model, iter);
       ok;
       ok = gtk_tree_model_iter_next (model, iter))
    {
      TpAccount *this_account;
      gboolean   equal;

      gtk_tree_model_get (model, iter,
          COL_ACCOUNT, &this_account,
          -1);

      equal = (this_account == account);
      g_object_unref (this_account);

      if (equal)
        return TRUE;
    }

  return FALSE;
}

static void
select_and_scroll_to_iter (EmpathyAccountsDialog *dialog,
    GtkTreeIter *iter)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  GtkTreeSelection *selection;
  GtkTreePath *path;
  GtkTreeModel *model;

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->treeview));

  gtk_tree_selection_select_iter (selection, iter);

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview));
  path = gtk_tree_model_get_path (model, iter);

  gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (priv->treeview), path, NULL,
      TRUE, 0, 0.5);

  gtk_tree_path_free (path);
}

static void
accounts_dialog_model_set_selected (EmpathyAccountsDialog *dialog,
    TpAccount *account)
{
  GtkTreeIter       iter;

  if (accounts_dialog_get_account_iter (dialog, account, &iter))
    select_and_scroll_to_iter (dialog, &iter);
}

static void
accounts_dialog_treeview_enabled_cb (GtkMenuItem *item,
    TpAccount *account)
{
  gboolean enabled;

  enabled = tp_account_is_enabled (account);

  enable_and_connect_account (account, !enabled);
}

static void
accounts_dialog_treeview_rename_cb (GtkMenuItem *item,
    EmpathyAccountsDialog *self)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (self);
  GtkTreePath *path;
  GtkTreeIter iter;
  GtkTreeSelection *selection;
  GtkTreeModel *model;

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->treeview));
  if (!gtk_tree_selection_get_selected (selection, &model, &iter))
    return;
  path = gtk_tree_model_get_path (model, &iter);

  g_object_set (G_OBJECT (priv->name_renderer), "editable", TRUE, NULL);

  gtk_widget_grab_focus (GTK_WIDGET (priv->treeview));
  gtk_tree_view_set_cursor (GTK_TREE_VIEW (priv->treeview), path,
      gtk_tree_view_get_column (GTK_TREE_VIEW (priv->treeview), 0), TRUE);

  gtk_tree_path_free (path);
}

static gboolean
accounts_dialog_treeview_button_press_event_cb (GtkTreeView *view,
    GdkEventButton *event,
    EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  TpAccount *account = NULL;
  GtkTreeModel *model = NULL;
  GtkTreePath *path = NULL;
  GtkTreeIter iter;
  GtkWidget *menu;
  GtkWidget *item;

  /* ignore multiple clicks */
  if (event->type != GDK_BUTTON_PRESS)
    return TRUE;

  if (event->button != 3)
    goto finally;

  /* Selection is not yet set, so we have to get account from event position */
  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview));
  if (!gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (priv->treeview),
      event->x, event->y, &path, NULL, NULL, NULL))
    goto finally;

  if (!gtk_tree_model_get_iter (model, &iter, path))
    goto finally;

  gtk_tree_model_get (model, &iter, COL_ACCOUNT, &account, -1);

  /* Create the menu */
  menu = empathy_context_menu_new (GTK_WIDGET (view));

  /* Menu item: to enabled/disable the account */
  item = gtk_check_menu_item_new_with_mnemonic (_("_Enabled"));

  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

  if (account_can_be_enabled (account))
    {
      gboolean active;

      active = tp_account_is_enabled (account);
      gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item),
          active);

      tp_g_signal_connect_object (item, "activate",
          G_CALLBACK (accounts_dialog_treeview_enabled_cb), account, 0);
    }
  else
    {
      gtk_widget_set_sensitive (item, FALSE);
    }

  gtk_widget_show (item);

  /* Menu item: Rename */
  item = gtk_menu_item_new_with_mnemonic (_("Rename"));
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

  tp_g_signal_connect_object (item, "activate",
      G_CALLBACK (accounts_dialog_treeview_rename_cb), dialog, 0);

  gtk_widget_show (item);

  /* FIXME: Add here presence items, to be able to set per-account presence */

  /* Popup menu */
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL,
      event->button, event->time);

finally:
  tp_clear_object (&account);
  gtk_tree_path_free (path);

  return FALSE;
}

static void
reload_account_widget (EmpathyAccountsDialog *self)
{
  TpawAccountSettings *settings;

  settings = accounts_dialog_model_get_selected_settings (self);
  accounts_dialog_update_settings (self, settings);
}

static void
accounts_dialog_connection_changed_cb (TpAccount *account,
    guint old_status,
    guint current,
    guint reason,
    gchar *dbus_error_name,
    GHashTable *details,
    EmpathyAccountsDialog *dialog)
{
  GtkTreeModel *model;
  GtkTreeIter   iter;
  gboolean      found;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  /* Update the status-infobar in the details view */
  accounts_dialog_update_status_infobar (dialog, account);

  /* Update the status in the model */
  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview));

  if (accounts_dialog_get_account_iter (dialog, account, &iter))
    {
      GtkTreePath *path;

      gtk_list_store_set (GTK_LIST_STORE (model), &iter,
          COL_STATUS, current,
          -1);

      path = gtk_tree_model_get_path (model, &iter);
      gtk_tree_model_row_changed (model, path, &iter);
      gtk_tree_path_free (path);
    }

  empathy_account_manager_get_accounts_connected (&found);

  if (!found && priv->connecting_id)
    {
      g_source_remove (priv->connecting_id);
      priv->connecting_id = 0;
    }

  if (found && !priv->connecting_id)
    priv->connecting_id = g_timeout_add (FLASH_TIMEOUT,
        (GSourceFunc) accounts_dialog_flash_connecting_cb,
        dialog);
}

static void
update_account_in_treeview (EmpathyAccountsDialog *self,
    TpAccount *account)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (self);
  GtkTreeIter iter;
  GtkTreeModel *model;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview));
  if (accounts_dialog_get_account_iter (self, account, &iter))
    {
      GtkTreePath *path;

      path = gtk_tree_model_get_path (model, &iter);
      gtk_tree_model_row_changed (model, path, &iter);
      gtk_tree_path_free (path);
    }
}

static void
accounts_dialog_presence_changed_cb (TpAccount *account,
    guint presence,
    gchar *status,
    gchar *status_message,
    EmpathyAccountsDialog *dialog)
{
  /* Update the status-infobar in the details view */
  accounts_dialog_update_status_infobar (dialog, account);

  update_account_in_treeview (dialog, account);
}

static void
accounts_dialog_account_display_name_changed_cb (TpAccount *account,
  GParamSpec *pspec,
  EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  GtkTreeModel *model;
  GtkTreeIter iter;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview));
  if (accounts_dialog_get_account_iter (dialog, account, &iter))
    {
      gtk_list_store_set (GTK_LIST_STORE (model), &iter,
          COL_NAME, tp_account_get_display_name (account),
          -1);
    }

  accounts_dialog_update_status_infobar (dialog, account);
}

static void
conn_prepare_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyAccountsDialog *self = user_data;

  reload_account_widget (self);
}

static void
accounts_dialog_notify_connection_cb (TpAccount *account,
    GParamSpec *spec,
    EmpathyAccountsDialog *self)
{
  TpConnection *conn;
  if (!account_is_selected (self, account))
    return;

  conn = tp_account_get_connection (account);
  if (conn == NULL)
    {
      reload_account_widget (self);
    }
  else
    {
      /* Wait for this feature so TpConnection will have fetch the
       * self handle */
      GQuark features[] = { TP_CONNECTION_FEATURE_CONNECTED, 0 };

      tp_proxy_prepare_async (conn, features, conn_prepare_cb, self);
    }
}

static void
accounts_dialog_add_account (EmpathyAccountsDialog *dialog,
    TpAccount *account)
{
  TpawAccountSettings *settings;
  GtkTreeModel       *model;
  GtkTreeIter         iter;
  TpConnectionStatus  status;
  const gchar        *name;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  gboolean selected = FALSE;
  GtkTreeSelection *selection;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview));
  status = tp_account_get_connection_status (account, NULL);
  name = tp_account_get_display_name (account);

  settings = tpaw_account_settings_new_for_account (account);

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->treeview));

  if (!accounts_dialog_get_account_iter (dialog, account, &iter))
    {
      /* Select the account if it's the first added */
      if (gtk_tree_selection_count_selected_rows (selection) == 0)
        selected = TRUE;

      gtk_list_store_insert_with_values (GTK_LIST_STORE (model), &iter, -1,
          COL_NAME, name,
          COL_STATUS, status,
          COL_ACCOUNT, account,
          COL_ACCOUNT_SETTINGS, settings,
          -1);
    }
  else
    {
      selected = gtk_tree_selection_iter_is_selected (selection, &iter);

      gtk_list_store_set (GTK_LIST_STORE (model), &iter,
          COL_NAME, name,
          COL_STATUS, status,
          COL_ACCOUNT, account,
          COL_ACCOUNT_SETTINGS, settings,
          -1);
    }

  if (selected)
    {
      /* We just modified the selected account. Its display name may have been
       * changed and so it's place in the treeview. Scroll to it so it stays
       * visible. */
      select_and_scroll_to_iter (dialog, &iter);
    }

  accounts_dialog_connection_changed_cb (account,
      0,
      status,
      TP_CONNECTION_STATUS_REASON_NONE_SPECIFIED,
      NULL,
      NULL,
      dialog);

  tp_g_signal_connect_object (account, "notify::display-name",
      G_CALLBACK (accounts_dialog_account_display_name_changed_cb),
      dialog, 0);

  tp_g_signal_connect_object (account, "status-changed",
      G_CALLBACK (accounts_dialog_connection_changed_cb), dialog, 0);
  tp_g_signal_connect_object (account, "presence-changed",
      G_CALLBACK (accounts_dialog_presence_changed_cb), dialog, 0);
  tp_g_signal_connect_object (account, "notify::connection",
      G_CALLBACK (accounts_dialog_notify_connection_cb), dialog, 0);

  g_object_unref (settings);
}

static void
accounts_dialog_account_validity_changed_cb (TpAccountManager *manager,
    TpAccount *account,
    gboolean valid,
    EmpathyAccountsDialog *dialog)
{
  accounts_dialog_add_account (dialog, account);
}

static void
accounts_dialog_accounts_model_row_inserted_cb (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  if (priv->setting_widget != NULL &&
      accounts_dialog_has_valid_accounts (dialog))
    {
      tpaw_account_widget_set_other_accounts_exist (
          priv->setting_widget, TRUE);
    }
}

static void
accounts_dialog_accounts_model_row_deleted_cb (GtkTreeModel *model,
    GtkTreePath *path,
    EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  if (priv->setting_widget != NULL &&
      !accounts_dialog_has_valid_accounts (dialog))
    {
      tpaw_account_widget_set_other_accounts_exist (
          priv->setting_widget, FALSE);
    }
}

static void
accounts_dialog_account_removed_cb (TpAccountManager *manager,
    TpAccount *account,
    EmpathyAccountsDialog *dialog)
{
  GtkTreeIter iter;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  if (accounts_dialog_get_account_iter (dialog, account, &iter))
    {
      gtk_list_store_remove (GTK_LIST_STORE (
          gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview))), &iter);
    }
}

static void
enable_or_disable_account (EmpathyAccountsDialog *dialog,
    TpAccount *account,
    gboolean enabled)
{
  /* Update the status-infobar in the details view */
  accounts_dialog_update_status_infobar (dialog, account);

  DEBUG ("Account %s is now %s",
      tp_account_get_display_name (account),
      enabled ? "enabled" : "disabled");
}

static void
accounts_dialog_account_disabled_cb (TpAccountManager *manager,
    TpAccount *account,
    EmpathyAccountsDialog *dialog)
{
  enable_or_disable_account (dialog, account, FALSE);
  update_account_in_treeview (dialog, account);
}

static void
accounts_dialog_account_enabled_cb (TpAccountManager *manager,
    TpAccount *account,
    EmpathyAccountsDialog *dialog)
{
  enable_or_disable_account (dialog, account, TRUE);
}

static GtkWidget *
display_import_dialog (EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  GtkWidget *import_dialog;

  import_dialog = empathy_import_dialog_new (GTK_WINDOW (dialog),
      FALSE, priv->cms);
  gtk_widget_show (import_dialog);

  return import_dialog;
}

static void
accounts_dialog_button_import_clicked_cb (GtkWidget *button,
    EmpathyAccountsDialog *dialog)
{
  display_import_dialog (dialog);
}

static void
accounts_dialog_close_response_cb (GtkDialog *message_dialog,
  gint response_id,
  gpointer user_data)
{
  GtkWidget *account_dialog = GTK_WIDGET (user_data);

  gtk_widget_destroy (GTK_WIDGET (message_dialog));

  if (response_id == GTK_RESPONSE_YES)
    gtk_widget_destroy (account_dialog);
}

static gboolean
accounts_dialog_delete_event_cb (GtkWidget *widget,
    GdkEvent *event,
    EmpathyAccountsDialog *dialog)
{
  /* we maunally handle responses to delete events */
  return TRUE;
}

static void
accounts_dialog_set_selected_account (EmpathyAccountsDialog *dialog,
    TpAccount *account)
{
  GtkTreeIter       iter;

  if (accounts_dialog_get_account_iter (dialog, account, &iter))
    select_and_scroll_to_iter (dialog, &iter);
}

static void
salut_valid_cb (GtkWidget *widget,
    gboolean valid,
    GtkWidget *button)
{
  gtk_widget_set_sensitive (button, valid);
}

static void
maybe_show_salut_dialog (EmpathyAccountsDialog *self)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (self);
  GtkWidget *dialog, *widget, *content, *button;
  gint response;

  if (!empathy_local_xmpp_assistant_widget_should_create_account (
        priv->account_manager))
    return;

  widget = empathy_local_xmpp_assistant_widget_new ();
  gtk_widget_show (widget);

  dialog = gtk_dialog_new ();

  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

  gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Skip"),
      GTK_RESPONSE_NO);

  button = gtk_dialog_add_button (GTK_DIALOG (dialog),
      _("_Connect"), GTK_RESPONSE_YES);
  gtk_widget_set_sensitive (button,
      empathy_local_xmpp_assistant_widget_is_valid (
        EMPATHY_LOCAL_XMPP_ASSISTANT_WIDGET (widget)));

  g_signal_connect (widget, "valid", G_CALLBACK (salut_valid_cb),
      button);

  content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  gtk_box_pack_start (GTK_BOX (content), widget, TRUE, TRUE, 0);

  response = gtk_dialog_run (GTK_DIALOG (dialog));

  if (response == GTK_RESPONSE_YES)
    {
      empathy_local_xmpp_assistant_widget_create_account (
          EMPATHY_LOCAL_XMPP_ASSISTANT_WIDGET (widget));
    }

  gtk_widget_destroy (dialog);
}

static void
import_dialog_response_cb (GtkDialog *dialog,
    gint response_id,
    EmpathyAccountsDialog *self)
{
  maybe_show_salut_dialog (self);
}

static void
maybe_show_import_dialog (EmpathyAccountsDialog *self)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (self);
  GtkWidget *dialog;

  if (empathy_accounts_has_non_salut_accounts (priv->account_manager))
    return;

  if (!empathy_import_accounts_to_import ())
    {
      maybe_show_salut_dialog (self);
      return;
    }

  dialog = display_import_dialog (self);

  tp_g_signal_connect_object (dialog, "response",
      G_CALLBACK (import_dialog_response_cb), self, 0);
}

static void
finished_loading (EmpathyAccountsDialog *self)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (self);
  GtkTreeSelection *selection;
  gboolean has_selected;

  priv->loading = FALSE;

  gtk_widget_set_sensitive (priv->button_add, TRUE);
  gtk_widget_set_sensitive (priv->button_import, TRUE);
  gtk_widget_set_sensitive (priv->treeview, TRUE);

  /* Sensitive the remove button if there is an account selected */
  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->treeview));
  has_selected = gtk_tree_selection_get_selected (selection, NULL, NULL);
  gtk_widget_set_sensitive (priv->button_remove, has_selected);

  gtk_spinner_stop (GTK_SPINNER (priv->spinner));
  gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook_account),
      NOTEBOOK_PAGE_ACCOUNT);

  maybe_show_import_dialog (self);
}

static void
accounts_dialog_cms_prepare_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  TpawConnectionManagers *cms = TPAW_CONNECTION_MANAGERS (source);
  EmpathyAccountsDialog *dialog = user_data;
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  if (!tpaw_connection_managers_prepare_finish (cms, result, NULL))
    goto out;

  /* No need to update the settings if we are already preparing one */
  if (priv->settings_ready == NULL)
    accounts_dialog_update_settings (dialog, NULL);

  if (priv->initial_selection != NULL)
    {
      accounts_dialog_set_selected_account (dialog, priv->initial_selection);
      g_object_unref (priv->initial_selection);
      priv->initial_selection = NULL;
    }

out:
  finished_loading (dialog);
}

static void
accounts_dialog_accounts_setup (EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  GList *accounts, *l;

  g_signal_connect (priv->account_manager, "account-validity-changed",
      G_CALLBACK (accounts_dialog_account_validity_changed_cb),
      dialog);
  g_signal_connect (priv->account_manager, "account-removed",
      G_CALLBACK (accounts_dialog_account_removed_cb),
      dialog);
  g_signal_connect (priv->account_manager, "account-enabled",
      G_CALLBACK (accounts_dialog_account_enabled_cb),
      dialog);
  g_signal_connect (priv->account_manager, "account-disabled",
      G_CALLBACK (accounts_dialog_account_disabled_cb),
      dialog);

  /* Add existing accounts */
  accounts = tp_account_manager_dup_valid_accounts (priv->account_manager);
  for (l = accounts; l; l = l->next)
    {
      accounts_dialog_add_account (dialog, l->data);
    }
  g_list_free_full (accounts, g_object_unref);

  priv->cms = tpaw_connection_managers_dup_singleton ();

  tpaw_connection_managers_prepare_async (priv->cms,
      accounts_dialog_cms_prepare_cb, dialog);

  accounts_dialog_model_select_first (dialog);
}

static void
accounts_dialog_manager_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *manager = TP_ACCOUNT_MANAGER (source_object);
  GError *error = NULL;

  if (!tp_proxy_prepare_finish (manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  accounts_dialog_accounts_setup (user_data);
}

static void
dialog_response_cb (GtkWidget *widget,
    gint response_id,
    gpointer user_data)
{
  EmpathyAccountsDialog *dialog = EMPATHY_ACCOUNTS_DIALOG (widget);

  if (response_id == GTK_RESPONSE_HELP)
    {
      empathy_url_show (widget, "help:empathy/accounts-window");
    }
  else if (response_id == GTK_RESPONSE_CLOSE ||
      response_id == GTK_RESPONSE_DELETE_EVENT)
    {
      TpAccount *account = NULL;

      if (accounts_dialog_has_pending_change (dialog, &account))
        {
          gchar *question_dialog_primary_text = get_dialog_primary_text (
              account);

          accounts_dialog_show_question_dialog (dialog,
              question_dialog_primary_text,
              _("You are about to close the window, which will discard\n"
                  "your changes. Are you sure you want to proceed?"),
              G_CALLBACK (accounts_dialog_close_response_cb),
              widget,
              GTK_STOCK_CANCEL, GTK_RESPONSE_NO,
              GTK_STOCK_DISCARD, GTK_RESPONSE_YES, NULL);

          g_free (question_dialog_primary_text);
        }
      else
        {
          gtk_widget_destroy (widget);
        }

      if (account != NULL)
        g_object_unref (account);
    }
}

static void
accounts_dialog_build_ui (EmpathyAccountsDialog *dialog)
{
  GtkWidget *top_hbox;
  GtkBuilder                   *gui;
  gchar                        *filename;
  EmpathyAccountsDialogPriv    *priv = GET_PRIV (dialog);
  GtkWidget *content_area, *action_area;
  GtkWidget *grid, *hbox;
  GtkWidget *alig;
  GtkWidget *sw, *toolbar;
  GtkStyleContext *context;

  filename = empathy_file_lookup ("empathy-accounts-dialog.ui", "src");

  gui = tpaw_builder_get_file (filename,
      "accounts_dialog_hbox", &top_hbox,
      "vbox_details", &priv->vbox_details,
      "alignment_settings", &priv->alignment_settings,
      "alignment_infobar", &priv->alignment_infobar,
      "treeview", &priv->treeview,
      "button_add", &priv->button_add,
      "button_remove", &priv->button_remove,
      "button_import", &priv->button_import,
      "notebook_account", &priv->notebook_account,
      "alignment_loading", &alig,
      "accounts_sw", &sw,
      "add_remove_toolbar", &toolbar,
      NULL);
  g_free (filename);

  tpaw_builder_connect (gui, dialog,
      "button_add", "clicked", accounts_dialog_button_add_clicked_cb,
      "button_remove", "clicked", accounts_dialog_button_remove_clicked_cb,
      "button_import", "clicked", accounts_dialog_button_import_clicked_cb,
      "treeview", "button-press-event",
         accounts_dialog_treeview_button_press_event_cb,
      NULL);

  content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

  gtk_box_pack_start (GTK_BOX (content_area), top_hbox, TRUE, TRUE, 0);

  g_object_unref (gui);

  action_area = gtk_dialog_get_action_area (GTK_DIALOG (dialog));

  /* Display loading page */
  priv->loading = TRUE;

  priv->spinner = gtk_spinner_new ();

  gtk_spinner_start (GTK_SPINNER (priv->spinner));
  gtk_widget_show (priv->spinner);

  gtk_container_add (GTK_CONTAINER (alig), priv->spinner);

  gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook_account),
      NOTEBOOK_PAGE_LOADING);

  /* Remove button is insensitive until we have a selected account */
  gtk_widget_set_sensitive (priv->button_remove, FALSE);

  /* Add and Import buttons and treeview are insensitive while the dialog
   * is loading */
  gtk_widget_set_sensitive (priv->button_add, FALSE);
  gtk_widget_set_sensitive (priv->button_import, FALSE);
  gtk_widget_set_sensitive (priv->treeview, FALSE);

  if (priv->parent_window)
    gtk_window_set_transient_for (GTK_WINDOW (dialog),
        priv->parent_window);

  priv->infobar = gtk_info_bar_new ();
  gtk_container_add (GTK_CONTAINER (priv->alignment_infobar),
      priv->infobar);
  gtk_widget_show (priv->infobar);

  grid = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (grid), 6);
  gtk_container_add (
      GTK_CONTAINER (gtk_info_bar_get_content_area (
          GTK_INFO_BAR (priv->infobar))),
      grid);

  priv->image_type = gtk_image_new_from_stock (GTK_STOCK_CUT,
      GTK_ICON_SIZE_DIALOG);
  gtk_misc_set_alignment (GTK_MISC (priv->image_type), 0.0, 0.5);
  gtk_grid_attach (GTK_GRID (grid), priv->image_type, 0, 0, 1, 2);

  /* first row */
  priv->label_name = gtk_label_new (NULL);
  gtk_label_set_ellipsize (GTK_LABEL (priv->label_name), PANGO_ELLIPSIZE_END);
  gtk_grid_attach (GTK_GRID (grid), priv->label_name, 1, 0, 1, 1);

  /* second row */
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_hexpand (hbox, TRUE);
  gtk_widget_set_halign (hbox, GTK_ALIGN_CENTER);
  gtk_grid_attach (GTK_GRID (grid), hbox, 1, 1, 1, 1);

  /* set up spinner */
  priv->throbber = gtk_spinner_new ();

  priv->image_status = gtk_image_new_from_icon_name (
            empathy_icon_name_for_presence (
            TP_CONNECTION_PRESENCE_TYPE_OFFLINE), GTK_ICON_SIZE_SMALL_TOOLBAR);

  priv->label_status = gtk_label_new (NULL);
  gtk_label_set_ellipsize (GTK_LABEL (priv->label_status), PANGO_ELLIPSIZE_END);

  gtk_box_pack_start (GTK_BOX (hbox), priv->throbber, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), priv->image_status, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), priv->label_status, FALSE, FALSE, 0);

  /* enabled switch */
  priv->enabled_switch = gtk_switch_new ();
  gtk_widget_set_valign (priv->enabled_switch, GTK_ALIGN_CENTER);
  g_signal_connect (priv->enabled_switch, "notify::active",
      G_CALLBACK (accounts_dialog_enable_switch_active_cb), dialog);
  gtk_grid_attach (GTK_GRID (grid), priv->enabled_switch, 2, 0, 1, 2);

  gtk_widget_show_all (grid);

  /* Tweak the dialog */
  gtk_window_set_title (GTK_WINDOW (dialog), _("Messaging and VoIP Accounts"));
  gtk_window_set_role (GTK_WINDOW (dialog), "accounts");
  gtk_window_set_default_size (GTK_WINDOW (dialog), 640, 450);
  gtk_window_set_type_hint (GTK_WINDOW (dialog), GDK_WINDOW_TYPE_HINT_DIALOG);

  /* join the add/remove toolbar to the treeview */
  context = gtk_widget_get_style_context (sw);
  gtk_style_context_set_junction_sides (context, GTK_JUNCTION_BOTTOM);

  context = gtk_widget_get_style_context (toolbar);
  gtk_style_context_set_junction_sides (context, GTK_JUNCTION_TOP);

  /* add dialog buttons */
  gtk_widget_set_margin_top (action_area, 18);
  gtk_button_box_set_layout (GTK_BUTTON_BOX (action_area), GTK_BUTTONBOX_END);

  gtk_dialog_add_buttons (GTK_DIALOG (dialog),
      GTK_STOCK_HELP, GTK_RESPONSE_HELP,
      GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE,
      NULL);

  g_signal_connect (dialog, "response",
      G_CALLBACK (dialog_response_cb), dialog);

  g_signal_connect (dialog, "delete-event",
      G_CALLBACK (accounts_dialog_delete_event_cb), dialog);
}

static void
do_dispose (GObject *obj)
{
  EmpathyAccountsDialog *dialog = EMPATHY_ACCOUNTS_DIALOG (obj);
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);

  if (priv->user_info != NULL)
    {
      tpaw_user_info_apply_async ((TpawUserInfo *) priv->user_info,
          NULL, NULL);
      priv->user_info = NULL;
    }

  if (priv->connecting_id != 0)
    {
      g_source_remove (priv->connecting_id);
      priv->connecting_id = 0;
    }

  if (priv->connectivity)
    {
      g_object_unref (priv->connectivity);
      priv->connectivity = NULL;
    }

  if (priv->account_manager != NULL)
    {
      g_object_unref (priv->account_manager);
      priv->account_manager = NULL;
    }

  if (priv->cms != NULL)
    {
      g_object_unref (priv->cms);
      priv->cms = NULL;
    }

  if (priv->initial_selection != NULL)
    {
      g_object_unref (priv->initial_selection);
      priv->initial_selection = NULL;
    }

  tp_clear_pointer (&priv->icons_cache, g_hash_table_unref);

  G_OBJECT_CLASS (empathy_accounts_dialog_parent_class)->dispose (obj);
}

static void
do_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (object);

  switch (property_id)
    {
    case PROP_PARENT:
      g_value_set_object (value, priv->parent_window);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
do_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (object);

  switch (property_id)
    {
    case PROP_PARENT:
      priv->parent_window = g_value_get_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
do_constructed (GObject *object)
{
  EmpathyAccountsDialog *dialog = EMPATHY_ACCOUNTS_DIALOG (object);
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  GtkTreeModel *model;

  accounts_dialog_build_ui (dialog);
  accounts_dialog_model_setup (dialog);

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (priv->treeview));
  g_signal_connect (model, "row-inserted",
      (GCallback) accounts_dialog_accounts_model_row_inserted_cb, dialog);
  g_signal_connect (model, "row-deleted",
      (GCallback) accounts_dialog_accounts_model_row_deleted_cb, dialog);

  /* Set up signalling */
  priv->account_manager = tp_account_manager_dup ();

  tp_proxy_prepare_async (priv->account_manager, NULL,
      accounts_dialog_manager_ready_cb, dialog);

  priv->connectivity = g_network_monitor_get_default ();
  g_object_ref (priv->connectivity);
}

static void
empathy_accounts_dialog_class_init (EmpathyAccountsDialogClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GParamSpec *param_spec;

  oclass->dispose = do_dispose;
  oclass->constructed = do_constructed;
  oclass->set_property = do_set_property;
  oclass->get_property = do_get_property;

  param_spec = g_param_spec_object ("parent",
      "parent", "The parent window",
      GTK_TYPE_WINDOW,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (oclass, PROP_PARENT, param_spec);

  g_type_class_add_private (klass, sizeof (EmpathyAccountsDialogPriv));
}

static void
empathy_accounts_dialog_init (EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv;

  priv = G_TYPE_INSTANCE_GET_PRIVATE ((dialog),
      EMPATHY_TYPE_ACCOUNTS_DIALOG,
      EmpathyAccountsDialogPriv);
  dialog->priv = priv;

  priv->icons_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, g_object_unref);
}

/* public methods */

GtkWidget *
empathy_accounts_dialog_show (GtkWindow *parent,
    TpAccount *selected_account)
{
  EmpathyAccountsDialog *dialog;
  EmpathyAccountsDialogPriv *priv;

  dialog = g_object_new (EMPATHY_TYPE_ACCOUNTS_DIALOG,
      "parent", parent, NULL);

  priv = GET_PRIV (dialog);

  if (selected_account)
    {
      if (priv->cms != NULL && tpaw_connection_managers_is_ready (priv->cms))
        accounts_dialog_set_selected_account (dialog, selected_account);
      else
        /* save the selection to set it later when the cms
         * becomes ready.
         */
        priv->initial_selection = g_object_ref (selected_account);
    }

  gtk_window_present (GTK_WINDOW (dialog));

  return GTK_WIDGET (dialog);
}

#ifdef HAVE_UOA
typedef struct
{
  TpAccount *account;
  gboolean if_needed;
} LaunchUOACtx;

static LaunchUOACtx *
launch_uoa_ctx_new (TpAccount *account,
    gboolean if_needed)
{
  LaunchUOACtx *ctx;

  ctx = g_slice_new0 (LaunchUOACtx);
  if (account != NULL)
    ctx->account = g_object_ref (account);
  ctx->if_needed = if_needed;

  return ctx;
}

static void
launch_uoa_ctx_free (LaunchUOACtx *ctx)
{
  g_clear_object (&ctx->account);
  g_slice_free (LaunchUOACtx, ctx);
}

static void
am_prepare_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *manager = TP_ACCOUNT_MANAGER (source);
  GError *error = NULL;
  LaunchUOACtx *ctx = user_data;
  gchar *args = NULL;

  if (!tp_proxy_prepare_finish (manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      goto out;
    }

  if (ctx->if_needed && empathy_accounts_has_non_salut_accounts (manager))
    goto out;

  if (ctx->account != NULL)
    {
      const GValue *value;

      value = tp_account_get_storage_identifier (ctx->account);

      if (G_VALUE_HOLDS_UINT (value))
        args = g_strdup_printf ("account-details=%u", g_value_get_uint (value));
    }

  empathy_launch_external_app ("unity-credentials-panel.desktop", args, NULL);

  g_free (args);
out:
  launch_uoa_ctx_free (ctx);
}

static void
launch_uoa_panel (TpAccount *selected_account,
    gboolean if_needed,
    gboolean hidden)
{
  TpAccountManager *manager;

  if (hidden)
    /* Nothing to do */
    return;

  manager = tp_account_manager_dup ();

  tp_proxy_prepare_async (manager, NULL, am_prepare_cb,
      launch_uoa_ctx_new (selected_account, if_needed));

  g_object_unref (manager);
}

#else /* HAVE_UOA */

static void
launch_empathy_accounts (TpAccount *selected_account,
    gboolean if_needed,
    gboolean hidden)
{
  GString *args;

  g_return_if_fail (!selected_account || TP_IS_ACCOUNT (selected_account));

  args = g_string_new (NULL);

  if (selected_account != NULL)
    g_string_append_printf (args, " --select-account=%s",
        tp_account_get_path_suffix (selected_account));

  if (if_needed)
    g_string_append_printf (args, " --if-needed");

  if (hidden)
    g_string_append_printf (args, " --hidden");

  DEBUG ("Launching empathy-accounts (if_needed: %d, hidden: %d, account: %s)",
    if_needed, hidden,
    selected_account == NULL ? "<none selected>" :
      tp_proxy_get_object_path (TP_PROXY (selected_account)));

  empathy_launch_program (BIN_DIR, "empathy-accounts", args->str);

  g_string_free (args, TRUE);
}
#endif /* HAVE_UOA */

void
empathy_accounts_dialog_show_application (GdkScreen *screen,
    TpAccount *selected_account,
    gboolean if_needed,
    gboolean hidden)
{
#ifdef HAVE_UOA
  launch_uoa_panel (selected_account, if_needed, hidden);
#else
  launch_empathy_accounts (selected_account, if_needed, hidden);
#endif
}

gboolean
empathy_accounts_dialog_is_creating (EmpathyAccountsDialog *dialog)
{
  EmpathyAccountsDialogPriv *priv = GET_PRIV (dialog);
  gboolean result = FALSE;

  if (priv->setting_widget == NULL)
    goto out;

  g_object_get (priv->setting_widget,
      "creating-account", &result, NULL);

out:
  return result;
}
