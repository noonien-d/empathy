/*
 * Copyright (C) 2002-2007 Imendio AB
 * Copyright (C) 2007-2010 Collabora Ltd.
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
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 *          Danielle Madeley <danielle.madeley@collabora.co.uk>
 */

#include "config.h"
#include "empathy-roster-window.h"

#include <sys/stat.h>
#include <glib/gi18n.h>
#include <tp-account-widgets/tpaw-builder.h>

#include "empathy-about-dialog.h"
#include "empathy-accounts-dialog.h"
#include "empathy-call-observer.h"
#include "empathy-chat-manager.h"
#include "empathy-chat-window.h"
#include "empathy-chatroom-manager.h"
#include "empathy-chatrooms-window.h"
#include "empathy-client-factory.h"
#include "empathy-contact-blocking-dialog.h"
#include "empathy-contact-search-dialog.h"
#include "empathy-event-manager.h"
#include "empathy-ft-manager.h"
#include "empathy-geometry.h"
#include "empathy-gsettings.h"
#include "empathy-gsettings.h"
#include "empathy-gtk-enum-types.h"
#include "empathy-individual-dialogs.h"
#include "empathy-log-window.h"
#include "empathy-new-call-dialog.h"
#include "empathy-new-chatroom-dialog.h"
#include "empathy-new-message-dialog.h"
#include "empathy-preferences.h"
#include "empathy-presence-chooser.h"
#include "empathy-presence-manager.h"
#include "empathy-request-util.h"
#include "empathy-roster-model-manager.h"
#include "empathy-roster-view.h"
#include "empathy-status-presets.h"
#include "empathy-theme-manager.h"
#include "empathy-theme-manager.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

/* Flashing delay for icons (milliseconds). */
#define FLASH_TIMEOUT 500

/* Minimum width of roster window if something goes wrong. */
#define MIN_WIDTH 50

/* Accels (menu shortcuts) can be configured and saved */
#define ACCELS_FILENAME "accels.txt"

/* Name in the geometry file */
#define GEOMETRY_NAME "roster-window"

enum
{
  PAGE_CONTACT_LIST = 0,
  PAGE_MESSAGE
};

enum
{
  PROP_0,
  PROP_SHELL_RUNNING
};

G_DEFINE_TYPE (EmpathyRosterWindow, empathy_roster_window, GTK_TYPE_APPLICATION_WINDOW)

struct _EmpathyRosterWindowPriv {
  EmpathyRosterView *view;
  TpAccountManager *account_manager;
  EmpathyChatManager *chat_manager;
  EmpathyThemeManager *theme_manager;
  EmpathyChatroomManager *chatroom_manager;
  EmpathyEventManager *event_manager;
  EmpathySoundManager *sound_mgr;
  EmpathyCallObserver *call_observer;
  EmpathyIndividualManager *individual_manager;
  guint flash_timeout_id;
  gboolean flash_on;

  GSettings *gsettings_ui;

  GtkWidget *preferences;
  GtkWidget *main_vbox;
  GtkWidget *throbber;
  GtkWidget *presence_toolbar;
  GtkWidget *presence_chooser;
  GtkWidget *errors_vbox;
  GtkWidget *auth_vbox;
  GtkWidget *search_bar;
  GtkWidget *notebook;
  GtkWidget *no_entry_label;
  GtkWidget *button_account_settings;
  //GtkWidget *button_online;
  GtkWidget *button_show_offline;
  GtkWidget *button_add_contact;
  GtkWidget *spinner_loading;
  GtkWidget *tooltip_widget;
  GtkWidget *chat_window;

  GMenu *menumodel;
  GMenu *rooms_section;

  GtkWidget *balance_vbox;

  guint size_timeout_id;

  /* reffed TpAccount* => visible GtkInfoBar* */
  GHashTable *errors;

  /* EmpathyEvent* => visible GtkInfoBar* */
  GHashTable *auths;

  /* stores a mapping from TpAccount to Handler ID to prevent
   * to listen more than once to the status-changed signal */
  GHashTable *status_changed_handlers;

  /* Actions that are enabled when there are connected accounts */
  GList *actions_connected;

  gboolean shell_running;
};

static void
roster_window_remove_auth (EmpathyRosterWindow *self,
    EmpathyEvent *event)
{
  GtkWidget *error_widget;

  error_widget = g_hash_table_lookup (self->priv->auths, event);
  if (error_widget != NULL)
    {
      gtk_widget_destroy (error_widget);
      g_hash_table_remove (self->priv->auths, event);
    }
}

static void
roster_window_auth_add_clicked_cb (GtkButton *button,
    EmpathyRosterWindow *self)
{
  EmpathyEvent *event;

  event = g_object_get_data (G_OBJECT (button), "event");

  empathy_event_approve (event);

  roster_window_remove_auth (self, event);
}

static void
roster_window_auth_close_clicked_cb (GtkButton *button,
    EmpathyRosterWindow *self)
{
  EmpathyEvent *event;

  event = g_object_get_data (G_OBJECT (button), "event");

  empathy_event_decline (event);
  roster_window_remove_auth (self, event);
}

static void
roster_window_auth_display (EmpathyRosterWindow *self,
    EmpathyEvent *event)
{
  TpAccount *account = event->account;
  GtkWidget *info_bar;
  GtkWidget *content_area;
  GtkWidget *image;
  GtkWidget *label;
  GtkWidget *add_button;
  GtkWidget *close_button;
  GtkWidget *action_area;
  GtkWidget *action_grid;
  const gchar *icon_name;
  gchar *str;

  if (g_hash_table_lookup (self->priv->auths, event) != NULL)
    return;

  info_bar = gtk_info_bar_new ();
  gtk_info_bar_set_message_type (GTK_INFO_BAR (info_bar), GTK_MESSAGE_QUESTION);

  gtk_widget_set_no_show_all (info_bar, TRUE);
  gtk_container_add (GTK_CONTAINER (self->priv->auth_vbox), info_bar);
  gtk_widget_show (info_bar);

  icon_name = tp_account_get_icon_name (account);
  image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_widget_show (image);

  str = g_markup_printf_escaped ("<b>%s</b>\n%s",
      tp_account_get_display_name (account),
      _("Password required"));

  label = gtk_label_new (str);
  gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_misc_set_alignment (GTK_MISC (label), 0, 0.5);
  gtk_widget_show (label);

  g_free (str);

  content_area = gtk_info_bar_get_content_area (GTK_INFO_BAR (info_bar));
  gtk_container_add (GTK_CONTAINER (content_area), image);
  gtk_container_add (GTK_CONTAINER (content_area), label);

  image = gtk_image_new_from_stock (GTK_STOCK_ADD, GTK_ICON_SIZE_BUTTON);
  add_button = gtk_button_new ();
  gtk_button_set_image (GTK_BUTTON (add_button), image);
  gtk_widget_set_tooltip_text (add_button, _("Provide Password"));
  gtk_widget_show (add_button);

  image = gtk_image_new_from_stock (GTK_STOCK_CLOSE, GTK_ICON_SIZE_BUTTON);
  close_button = gtk_button_new ();
  gtk_button_set_image (GTK_BUTTON (close_button), image);
  gtk_widget_set_tooltip_text (close_button, _("Disconnect"));
  gtk_widget_show (close_button);

  action_grid = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (action_grid), 6);
  gtk_widget_show (action_grid);

  action_area = gtk_info_bar_get_action_area (GTK_INFO_BAR (info_bar));
  gtk_container_add (GTK_CONTAINER (action_area), action_grid);

  gtk_grid_attach (GTK_GRID (action_grid), add_button, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (action_grid), close_button, 1, 0, 1, 1);

  g_object_set_data_full (G_OBJECT (info_bar),
      "event", event, NULL);
  g_object_set_data_full (G_OBJECT (add_button),
      "event", event, NULL);
  g_object_set_data_full (G_OBJECT (close_button),
      "event", event, NULL);

  g_signal_connect (add_button, "clicked",
      G_CALLBACK (roster_window_auth_add_clicked_cb), self);
  g_signal_connect (close_button, "clicked",
      G_CALLBACK (roster_window_auth_close_clicked_cb), self);

  gtk_widget_show (self->priv->auth_vbox);

  g_hash_table_insert (self->priv->auths, event, info_bar);
}

static FolksIndividual *
ensure_individual_for_event (EmpathyEvent *event)
{
  TpContact *contact;

  contact = empathy_contact_get_tp_contact (event->contact);
  if (contact == NULL)
    return NULL;

  return empathy_ensure_individual_from_tp_contact (contact);
}

static void
roster_window_event_added_cb (EmpathyEventManager *manager,
    EmpathyEvent *event,
    EmpathyRosterWindow *self)
{
  if (event->contact)
    {
      FolksIndividual *individual;

      individual = ensure_individual_for_event (event);
      if (individual == NULL)
        return;

      event->roster_view_id = empathy_roster_view_add_event (self->priv->view,
          individual, event->icon_name, event);

      g_object_unref (individual);
    }
  else if (event->type == EMPATHY_EVENT_TYPE_AUTH)
    {
      roster_window_auth_display (self, event);
    }
}

static void
roster_window_event_removed_cb (EmpathyEventManager *manager,
    EmpathyEvent *event,
    EmpathyRosterWindow *self)
{
  if (event->type == EMPATHY_EVENT_TYPE_AUTH)
    {
      roster_window_remove_auth (self, event);
      return;
    }

  empathy_roster_view_remove_event (self->priv->view, event->roster_view_id);
}

static gboolean
roster_window_load_events_idle_cb (gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;
  GSList *l;

  l = empathy_event_manager_get_events (self->priv->event_manager);
  while (l != NULL)
    {
      roster_window_event_added_cb (self->priv->event_manager, l->data,
          self);
      l = l->next;
    }

  return FALSE;
}

static void
hide_search_bar (EmpathyRosterWindow *roster_window)
{
  if (TPAW_IS_LIVE_SEARCH (roster_window->priv->search_bar) &&
      gtk_widget_is_visible (roster_window->priv->search_bar))
    gtk_widget_hide (roster_window->priv->search_bar);
}

static void
individual_activated_cb (EmpathyRosterView *self,
    FolksIndividual *individual,
    gpointer user_data)
{
  EmpathyContact *contact;

  contact = empathy_contact_dup_best_for_action (individual,
      EMPATHY_ACTION_CHAT);

  if (contact == NULL)
    return;

  DEBUG ("Starting a chat");

  empathy_chat_with_contact (contact, gtk_get_current_event_time ());

  g_object_unref (contact);

  /* Hide the search-bar upon hitting "Enter" on an individual */
  hide_search_bar (EMPATHY_ROSTER_WINDOW (user_data));
}

static void
event_activated_cb (EmpathyRosterView *self,
    FolksIndividual *individual,
    EmpathyEvent *event,
    gpointer user_data)
{
  empathy_event_activate (event);

  /* Hide the search-bar upon an event activation */
  hide_search_bar (EMPATHY_ROSTER_WINDOW (user_data));
}

static void
button_account_settings_clicked_cb (GtkButton *button,
    EmpathyRosterWindow *self)
{
  empathy_accounts_dialog_show_application (gdk_screen_get_default (),
      NULL, FALSE, FALSE);
}
/*
static void
button_online_clicked_cb (GtkButton *button,
    EmpathyRosterWindow *self)
{
  EmpathyPresenceManager *mgr;

  mgr = empathy_presence_manager_dup_singleton ();

  empathy_presence_manager_set_state (mgr,
      TP_CONNECTION_PRESENCE_TYPE_AVAILABLE);

  g_object_unref (mgr);
}*/

static void
button_show_offline_clicked_cb (GtkButton *button,
    EmpathyRosterWindow *self)
{
  g_settings_set_boolean (self->priv->gsettings_ui,
      EMPATHY_PREFS_UI_SHOW_OFFLINE, TRUE);
}

static void
button_add_contact_clicked_cb (GtkButton *button,
    EmpathyRosterWindow *self)
{
  empathy_new_individual_dialog_show (GTK_WINDOW (self));
}

typedef enum
{
  PAGE_MESSAGE_FLAG_NONE = 0,
  PAGE_MESSAGE_FLAG_ACCOUNTS = 1 << 0,
  PAGE_MESSAGE_FLAG_SPINNER = 1 << 2,
  PAGE_MESSAGE_FLAG_ONLINE = 1 << 3,
  PAGE_MESSAGE_FLAG_SHOW_OFFLINE = 1 << 4,
  PAGE_MESSAGE_FLAG_ADD_CONTACT = 1 << 5,
} PageMessageFlags;

static gboolean
can_add_contact (EmpathyRosterWindow *self)
{
  GList *accounts, *l;
  gboolean result = FALSE;

  accounts = tp_account_manager_dup_valid_accounts (
      self->priv->account_manager);
  for (l = accounts; l != NULL && !result; l = g_list_next (l))
    {
      TpAccount *account = TP_ACCOUNT (l->data);
      TpConnection *conn;

      conn = tp_account_get_connection (account);
      if (conn == NULL)
        continue;

      if (tp_connection_get_can_change_contact_list (conn))
        result = TRUE;
    }

  g_list_free_full (accounts, g_object_unref);
  return result;
}

static void
display_page_message (EmpathyRosterWindow *self,
    const gchar *msg,
    PageMessageFlags flags)
{
  if (msg != NULL)
    {
      gchar *tmp;

      tmp = g_strdup_printf ("<b><span size='xx-large'>%s</span></b>", msg);

      gtk_label_set_markup (GTK_LABEL (self->priv->no_entry_label), tmp);
      g_free (tmp);

      gtk_label_set_line_wrap (GTK_LABEL (self->priv->no_entry_label), TRUE);
      gtk_widget_show (self->priv->no_entry_label);
    }
  else
    {
      gtk_widget_hide (self->priv->no_entry_label);
    }

  gtk_widget_set_visible (self->priv->button_account_settings,
      (flags & PAGE_MESSAGE_FLAG_ACCOUNTS) != 0);
  gtk_widget_set_visible (self->priv->spinner_loading,
      (flags & PAGE_MESSAGE_FLAG_SPINNER) != 0);
//  gtk_widget_set_visible (self->priv->button_online,
//      (flags & PAGE_MESSAGE_FLAG_ONLINE) != 0);
  gtk_widget_set_visible (self->priv->button_show_offline,
      (flags & PAGE_MESSAGE_FLAG_SHOW_OFFLINE) != 0);
  gtk_widget_set_visible (self->priv->button_add_contact,
      (flags & PAGE_MESSAGE_FLAG_ADD_CONTACT) != 0);

  if ((flags & PAGE_MESSAGE_FLAG_ADD_CONTACT) != 0)
    gtk_widget_set_sensitive (self->priv->button_add_contact,
        can_add_contact (self));

  gtk_notebook_set_current_page (GTK_NOTEBOOK (self->priv->notebook),
      PAGE_MESSAGE);
}

static void
display_page_no_account (EmpathyRosterWindow *self)
{
  display_page_message (self,
      _("You need to set up an account to see contacts here."),
      PAGE_MESSAGE_FLAG_ACCOUNTS);
}

static void
display_page_contact_list (EmpathyRosterWindow *self)
{
  if (!empathy_individual_manager_get_contacts_loaded (
        self->priv->individual_manager))
    /* We'll display the contact list once we're done loading */
    return;

  gtk_notebook_set_current_page (GTK_NOTEBOOK (self->priv->notebook),
      PAGE_CONTACT_LIST);
}

static void
roster_window_remove_error (EmpathyRosterWindow *self,
    TpAccount *account)
{
  GtkWidget *error_widget;

  error_widget = g_hash_table_lookup (self->priv->errors, account);
  if (error_widget != NULL)
    {
      gtk_widget_destroy (error_widget);
      g_hash_table_remove (self->priv->errors, account);
    }
}

static void
roster_window_account_disabled_cb (TpAccountManager  *manager,
    TpAccount *account,
    EmpathyRosterWindow *self)
{
  roster_window_remove_error (self, account);
}

typedef enum
{
  ERROR_RESPONSE_RETRY,
  ERROR_RESPONSE_EDIT,
  ERROR_RESPONSE_CLOSE,
  ERROR_RESPONSE_UPGRADE,
} ErrorResponse;

static void
roster_window_error_response_cb (GtkInfoBar *infobar,
    gint response_id,
    EmpathyRosterWindow *self)
{
  TpAccount *account;

  account = g_object_get_data (G_OBJECT (infobar), "account");

  switch (response_id)
    {
      case ERROR_RESPONSE_RETRY:
        tp_account_reconnect_async (account, NULL, NULL);
        break;

      case ERROR_RESPONSE_EDIT:
        empathy_accounts_dialog_show_application (
            gtk_widget_get_screen (GTK_WIDGET (infobar)),
            account, FALSE, FALSE);
        break;

      case ERROR_RESPONSE_CLOSE:
        break;

      case ERROR_RESPONSE_UPGRADE:
        {
          GtkWidget *dialog;

          dialog = gtk_message_dialog_new (GTK_WINDOW (self),
              GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
              _("Sorry, %s accounts can’t be used until your %s software is updated."),
              tp_account_get_protocol_name (account),
              tp_account_get_protocol_name (account));

          g_signal_connect_swapped (dialog, "response",
              G_CALLBACK (gtk_widget_destroy),
              dialog);

          gtk_widget_show (dialog);
        }
    }

  roster_window_remove_error (self, account);
}

static GtkWidget *
roster_window_error_create_info_bar (EmpathyRosterWindow *self,
    TpAccount *account,
    GtkMessageType message_type,
    const gchar *message_markup)
{
  GtkWidget *info_bar;
  GtkWidget *content_area;
  GtkWidget *action_area;
  GtkWidget *label;
  GtkWidget *image;
  const gchar *icon_name;

  /* If there are other errors, remove them */
  roster_window_remove_error (self, account);

  info_bar = gtk_info_bar_new ();
  gtk_info_bar_set_message_type (GTK_INFO_BAR (info_bar), message_type);

  gtk_widget_set_no_show_all (info_bar, TRUE);
  gtk_container_add (GTK_CONTAINER (self->priv->errors_vbox), info_bar);
  gtk_widget_show (info_bar);

  icon_name = tp_account_get_icon_name (account);
  image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_widget_show (image);

  label = gtk_label_new (message_markup);
  gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_misc_set_alignment (GTK_MISC (label), 0, 0.5);
  gtk_widget_show (label);

  content_area = gtk_info_bar_get_content_area (GTK_INFO_BAR (info_bar));
  gtk_container_add (GTK_CONTAINER (content_area), image);
  gtk_container_add (GTK_CONTAINER (content_area), label);

  action_area = gtk_info_bar_get_action_area (GTK_INFO_BAR (info_bar));
  gtk_orientable_set_orientation (GTK_ORIENTABLE (action_area),
      GTK_ORIENTATION_HORIZONTAL);
  gtk_style_context_add_class (gtk_widget_get_style_context (action_area),
      "empathy-roster-window-error-button");

  g_object_set_data_full (G_OBJECT (info_bar),
      "account", g_object_ref (account),
      g_object_unref);

  g_signal_connect (info_bar, "response",
      G_CALLBACK (roster_window_error_response_cb), self);

  gtk_widget_show (self->priv->errors_vbox);

  g_hash_table_insert (self->priv->errors, g_object_ref (account), info_bar);

  return info_bar;
}

static void
roster_window_error_add_stock_button (GtkInfoBar *info_bar,
    const gchar *stock_id,
    const gchar *tooltip,
    ErrorResponse response_id)
{
  GtkWidget *image;
  GtkWidget *button;

  image = gtk_image_new_from_stock (stock_id, GTK_ICON_SIZE_BUTTON);
  button = gtk_button_new ();
  gtk_button_set_image (GTK_BUTTON (button), image);
  gtk_widget_set_tooltip_text (button, tooltip);
  gtk_widget_show (button);

  gtk_info_bar_add_action_widget (info_bar, button, response_id);
}

#ifdef HAVE_UOA
static const gchar *
uoa_account_display_string (TpAccount *account)
{
  const gchar *service;

  service = tp_account_get_service (account);

  /* Use well known service name, if available */
  if (!tp_strdiff (service, "windows-live"))
    return _("Windows Live");
  else if (!tp_strdiff (service, "google-talk"))
    return _("Google Talk");
  else if (!tp_strdiff (service, "facebook"))
    return _("Facebook");

  return tp_account_get_display_name (account);
}

static void
roster_window_uoa_auth_error (EmpathyRosterWindow *self,
    TpAccount *account)
{
  GtkWidget *info_bar;
  GtkWidget *image;
  GtkWidget *button;
  gchar *str;

  /* translators: %s is an account name like 'Facebook' or 'Google Talk' */
  str = g_strdup_printf (_("%s account requires authorisation"),
      uoa_account_display_string (account));

  info_bar = roster_window_error_create_info_bar (self, account,
      GTK_MESSAGE_OTHER, str);
  g_free (str);

  image = gtk_image_new_from_icon_name ("credentials-preferences",
      GTK_ICON_SIZE_BUTTON);
  button = gtk_button_new ();
  gtk_button_set_image (GTK_BUTTON (button), image);
  gtk_widget_set_tooltip_text (button, _("Online Accounts"));
  gtk_widget_show (button);

  gtk_info_bar_add_action_widget (GTK_INFO_BAR (info_bar), button,
      ERROR_RESPONSE_EDIT);
}
#endif

static void
roster_window_error_display (EmpathyRosterWindow *self,
    TpAccount *account)
{
  const gchar *error_message;
  gboolean user_requested;
  GtkWidget *info_bar;
  gchar *str;

  error_message =
    empathy_account_get_error_message (account, &user_requested);

  if (user_requested)
    return;

#ifdef HAVE_UOA
  if (!tp_strdiff (TP_ERROR_STR_AUTHENTICATION_FAILED,
       tp_account_get_detailed_error (account, NULL)) &&
      !tp_strdiff (tp_account_get_storage_provider (account),
       EMPATHY_UOA_PROVIDER))
    {
      roster_window_uoa_auth_error (self, account);
      return;
    }
#endif

  str = g_markup_printf_escaped ("<b>%s</b>\n%s",
      tp_account_get_display_name (account), error_message);

  info_bar = roster_window_error_create_info_bar (self, account,
      GTK_MESSAGE_ERROR, str);
  g_free (str);

  gtk_widget_set_tooltip_text (self->priv->errors_vbox, error_message);

  if (!tp_strdiff (TP_ERROR_STR_SOFTWARE_UPGRADE_REQUIRED,
       tp_account_get_detailed_error (account, NULL)))
    {
      roster_window_error_add_stock_button (GTK_INFO_BAR (info_bar),
          GTK_STOCK_REFRESH, _("Update software…"),
          ERROR_RESPONSE_RETRY);
    }
  else
    {
      roster_window_error_add_stock_button (GTK_INFO_BAR (info_bar),
          GTK_STOCK_REFRESH, _("Reconnect"),
          ERROR_RESPONSE_RETRY);

      roster_window_error_add_stock_button (GTK_INFO_BAR (info_bar),
          GTK_STOCK_EDIT, _("Edit Account"),
          ERROR_RESPONSE_EDIT);
    }

  roster_window_error_add_stock_button (GTK_INFO_BAR (info_bar),
      GTK_STOCK_CLOSE, _("Close"),
      ERROR_RESPONSE_CLOSE);
}

static void
roster_window_update_status (EmpathyRosterWindow *self)
{
  gboolean connected, connecting;
  GList *l;
  GAction *action;

  connected = empathy_account_manager_get_accounts_connected (&connecting);

  /* Update the spinner state */
  if (connecting)
    {
      gtk_spinner_start (GTK_SPINNER (self->priv->throbber));
      gtk_widget_show (self->priv->throbber);
    }
  else
    {
      gtk_spinner_stop (GTK_SPINNER (self->priv->throbber));
      gtk_widget_hide (self->priv->throbber);
    }

  /* Update widgets sensibility */
  for (l = self->priv->actions_connected; l; l = l->next)
    g_simple_action_set_enabled (l->data, connected);

  action = g_action_map_lookup_action (G_ACTION_MAP (self), "chat-add-contact");
  if (!can_add_contact (self))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), FALSE);
}

static void
roster_window_balance_update_balance (EmpathyRosterWindow *self,
    TpAccount *account)
{
  TpConnection *conn;
  GtkWidget *label;
  int amount = 0;
  guint scale = G_MAXINT32;
  const gchar *currency = "";
  char *money;

  conn = tp_account_get_connection (account);
  if (conn == NULL)
    return;

  if (!tp_connection_get_balance (conn, &amount, &scale, &currency))
    return;

  if (amount == 0 &&
      scale == G_MAXINT32 &&
      tp_str_empty (currency))
    {
      /* unknown balance */
      money = g_strdup ("--");
    }
  else
    {
      char *tmp = empathy_format_currency (amount, scale, currency);

      money = g_strdup_printf ("%s %s", currency, tmp);
      g_free (tmp);
    }

  /* update the money label in the roster */
  label = g_object_get_data (G_OBJECT (account), "balance-money-label");

  gtk_label_set_text (GTK_LABEL (label), money);
  g_free (money);
}

static void
roster_window_balance_changed_cb (TpConnection *conn,
    guint balance,
    guint scale,
    const gchar *currency,
    EmpathyRosterWindow *self)
{
  TpAccount *account;

  account = tp_connection_get_account (conn);
  if (account == NULL)
    return;

  roster_window_balance_update_balance (self, account);
}

static void
roster_window_setup_balance (EmpathyRosterWindow *self,
    TpAccount *account)
{
  TpConnection *conn = tp_account_get_connection (account);
  GtkWidget *hbox, *image, *label;
  const gchar *uri;

  if (conn == NULL)
    return;

  if (!tp_proxy_is_prepared (conn, TP_CONNECTION_FEATURE_BALANCE))
    return;

  DEBUG ("Setting up balance for acct: %s",
      tp_account_get_display_name (account));

  /* create the display widget */
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_container_set_border_width (GTK_CONTAINER (hbox), 3);

  /* protocol icon */
  image = gtk_image_new ();
  gtk_container_add (GTK_CONTAINER (hbox), image);
  g_object_bind_property (account, "icon-name", image, "icon-name",
      G_BINDING_SYNC_CREATE);

  /* account name label */
  label = gtk_label_new ("");
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_container_add (GTK_CONTAINER (hbox), label);
  g_object_bind_property (account, "display-name", label, "label",
      G_BINDING_SYNC_CREATE);

  /* balance label */
  label = gtk_label_new ("");
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
  gtk_container_add (GTK_CONTAINER (hbox), label);

  /* top up button */
  uri = tp_connection_get_balance_uri (conn);

  if (!tp_str_empty (uri))
    {
      GtkWidget *button;

      button = gtk_button_new ();
      gtk_container_add (GTK_CONTAINER (button),
          gtk_image_new_from_icon_name ("emblem-symbolic-link",
            GTK_ICON_SIZE_SMALL_TOOLBAR));
      gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
      gtk_widget_set_tooltip_text (button, _("Top up account"));
      gtk_container_add (GTK_CONTAINER (hbox), button);

      g_signal_connect_data (button, "clicked",
          G_CALLBACK (empathy_url_show),
          g_strdup (uri), (GClosureNotify) g_free,
          0);
    }

  gtk_container_add (GTK_CONTAINER (self->priv->balance_vbox), hbox);
  gtk_widget_show_all (hbox);

  g_object_set_data (G_OBJECT (account), "balance-money-label", label);
  g_object_set_data (G_OBJECT (account), "balance-money-hbox", hbox);

  roster_window_balance_update_balance (self, account);

  g_signal_connect (conn, "balance-changed",
      G_CALLBACK (roster_window_balance_changed_cb), self);
}

static void
roster_window_remove_balance_action (EmpathyRosterWindow *self,
    TpAccount *account)
{
  GtkWidget *hbox =
    g_object_get_data (G_OBJECT (account), "balance-money-hbox");

  if (hbox == NULL)
    return;

  g_return_if_fail (GTK_IS_BOX (hbox));

  gtk_widget_destroy (hbox);
}

static void set_notebook_page (EmpathyRosterWindow *self);

static void
roster_window_connection_changed_cb (TpAccount  *account,
    guint old_status,
    guint current,
    guint reason,
    gchar *dbus_error_name,
    GHashTable *details,
    EmpathyRosterWindow *self)
{
  roster_window_update_status (self);
  set_notebook_page (self);

  if (current == TP_CONNECTION_STATUS_DISCONNECTED &&
      reason != TP_CONNECTION_STATUS_REASON_REQUESTED)
    {
      roster_window_error_display (self, account);
    }

  if (current == TP_CONNECTION_STATUS_DISCONNECTED)
    {
      empathy_sound_manager_play (self->priv->sound_mgr, GTK_WIDGET (self),
              EMPATHY_SOUND_ACCOUNT_DISCONNECTED);
    }

  if (current == TP_CONNECTION_STATUS_CONNECTED)
    {
      empathy_sound_manager_play (self->priv->sound_mgr, GTK_WIDGET (self),
              EMPATHY_SOUND_ACCOUNT_CONNECTED);

      /* Account connected without error, remove error message if any */
      roster_window_remove_error (self, account);
    }
}

static void
roster_window_accels_load (void)
{
  gchar *filename;

  filename = g_build_filename (g_get_user_config_dir (),
      PACKAGE_NAME, ACCELS_FILENAME, NULL);
  if (g_file_test (filename, G_FILE_TEST_EXISTS))
    {
      DEBUG ("Loading from:'%s'", filename);
      gtk_accel_map_load (filename);
    }

  g_free (filename);
}

static void
roster_window_accels_save (void)
{
  gchar *dir;
  gchar *file_with_path;

  dir = g_build_filename (g_get_user_config_dir (), PACKAGE_NAME, NULL);
  g_mkdir_with_parents (dir, S_IRUSR | S_IWUSR | S_IXUSR);
  file_with_path = g_build_filename (dir, ACCELS_FILENAME, NULL);
  g_free (dir);

  DEBUG ("Saving to:'%s'", file_with_path);
  gtk_accel_map_save (file_with_path);

  g_free (file_with_path);
}

static void
empathy_roster_window_finalize (GObject *window)
{
  EmpathyRosterWindow *self = EMPATHY_ROSTER_WINDOW (window);
  GHashTableIter iter;
  gpointer key, value;

  /* Save user-defined accelerators. */
  roster_window_accels_save ();

  g_list_free (self->priv->actions_connected);

  g_object_unref (self->priv->account_manager);
  g_object_unref (self->priv->sound_mgr);
  g_hash_table_unref (self->priv->errors);
  g_hash_table_unref (self->priv->auths);

  /* disconnect all handlers of status-changed signal */
  g_hash_table_iter_init (&iter, self->priv->status_changed_handlers);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_signal_handler_disconnect (TP_ACCOUNT (key), GPOINTER_TO_UINT (value));

  g_hash_table_unref (self->priv->status_changed_handlers);

  g_object_unref (self->priv->call_observer);
  g_object_unref (self->priv->event_manager);
  g_object_unref (self->priv->chat_manager);
  g_object_unref (self->priv->theme_manager);
  g_object_unref (self->priv->chatroom_manager);

  g_object_unref (self->priv->gsettings_ui);
  g_object_unref (self->priv->individual_manager);

  g_object_unref (self->priv->menumodel);
  g_object_unref (self->priv->rooms_section);

  g_clear_object (&self->priv->tooltip_widget);

  G_OBJECT_CLASS (empathy_roster_window_parent_class)->finalize (window);
}

static gboolean
roster_window_key_press_event_cb  (GtkWidget *window,
    GdkEventKey *event,
    EmpathyRosterWindow *self)
{
  if (event->keyval == GDK_KEY_T
      && event->state & GDK_SHIFT_MASK
      && event->state & GDK_CONTROL_MASK)
    empathy_chat_manager_call_undo_closed_chat ();

  if (event->keyval == GDK_KEY_f
      && event->state & GDK_CONTROL_MASK)
    gtk_widget_show (self->priv->search_bar);

  return FALSE;
}

static void
unprepare_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GtkWidget *self = user_data;

  gtk_widget_destroy (self);
}

static void
roster_window_chat_quit_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;

  /* Destroying the window will make us leave the main loop and so exit the
   * process. Before doing so we want to unprepare the individual manager.
   * Just hide the window now and actually destroy it once Folks is done.
   */
  gtk_widget_hide (GTK_WIDGET (self));

  empathy_individual_manager_unprepare_async (self->priv->individual_manager,
      unprepare_cb, self);
}

static void
roster_window_view_history_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;

  empathy_log_window_show (NULL, NULL, FALSE, GTK_WINDOW (self));
}

static void
roster_window_chat_new_message_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;

  empathy_new_message_dialog_show (GTK_WINDOW (self));
}

static void
roster_window_chat_new_call_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;

  empathy_new_call_dialog_show (GTK_WINDOW (self));
}

static void
roster_window_chat_add_contact_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;

  empathy_new_individual_dialog_show (GTK_WINDOW (self));
}

static void
roster_window_chat_search_contacts_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;
  GtkWidget *dialog;

  dialog = empathy_contact_search_dialog_new (
      GTK_WINDOW (self));

  gtk_widget_show (dialog);
}

static void
roster_window_view_show_ft_manager (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  empathy_ft_manager_show ();
}

static void
join_chatroom (EmpathyChatroom *chatroom,
    gint64 timestamp)
{
  TpAccount *account;
  const gchar *room;

  account = empathy_chatroom_get_account (chatroom);
  room = empathy_chatroom_get_room (chatroom);

  DEBUG ("Requesting channel for '%s'", room);
  empathy_join_muc (account, room, timestamp);
}

typedef struct
{
  TpAccount *account;
  EmpathyChatroom *chatroom;
  gint64 timestamp;
  glong sig_id;
  guint timeout;
} join_fav_account_sig_ctx;

static join_fav_account_sig_ctx *
join_fav_account_sig_ctx_new (TpAccount *account,
    EmpathyChatroom *chatroom,
    gint64 timestamp)
{
  join_fav_account_sig_ctx *ctx = g_slice_new0 (
      join_fav_account_sig_ctx);

  ctx->account = g_object_ref (account);
  ctx->chatroom = g_object_ref (chatroom);
  ctx->timestamp = timestamp;
  return ctx;
}

static void
join_fav_account_sig_ctx_free (join_fav_account_sig_ctx *ctx)
{
  g_object_unref (ctx->account);
  g_object_unref (ctx->chatroom);
  g_slice_free (join_fav_account_sig_ctx, ctx);
}

static void
account_status_changed_cb (TpAccount  *account,
    TpConnectionStatus old_status,
    TpConnectionStatus new_status,
    guint reason,
    gchar *dbus_error_name,
    GHashTable *details,
    gpointer user_data)
{
  join_fav_account_sig_ctx *ctx = user_data;

  switch (new_status)
    {
      case TP_CONNECTION_STATUS_DISCONNECTED:
        /* Don't wait any longer */
        goto finally;
        break;

      case TP_CONNECTION_STATUS_CONNECTING:
        /* Wait a bit */
        return;

      case TP_CONNECTION_STATUS_CONNECTED:
        /* We can join the room */
        break;

      default:
        g_assert_not_reached ();
    }

  join_chatroom (ctx->chatroom, ctx->timestamp);

finally:
  g_source_remove (ctx->timeout);
  g_signal_handler_disconnect (account, ctx->sig_id);
}

#define JOIN_FAVORITE_TIMEOUT 5

static gboolean
join_favorite_timeout_cb (gpointer data)
{
  join_fav_account_sig_ctx *ctx = data;

  /* stop waiting for joining the favorite room */
  g_signal_handler_disconnect (ctx->account, ctx->sig_id);
  return FALSE;
}

static void
roster_window_favorite_chatroom_join (EmpathyChatroom *chatroom)
{
  TpAccount *account;

  account = empathy_chatroom_get_account (chatroom);
  if (tp_account_get_connection_status (account, NULL) !=
               TP_CONNECTION_STATUS_CONNECTED)
    {
      join_fav_account_sig_ctx *ctx;

      ctx = join_fav_account_sig_ctx_new (account, chatroom,
        empathy_get_current_action_time ());

      ctx->sig_id = g_signal_connect_data (account, "status-changed",
        G_CALLBACK (account_status_changed_cb), ctx,
        (GClosureNotify) join_fav_account_sig_ctx_free, 0);

      ctx->timeout = g_timeout_add_seconds (JOIN_FAVORITE_TIMEOUT,
        join_favorite_timeout_cb, ctx);
      return;
    }

  join_chatroom (chatroom, empathy_get_current_action_time ());
}

static void
roster_window_join_chatroom_menu_activate_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;
  const gchar *room, *path;
  EmpathyClientFactory *factory;
  TpAccount *account;
  GError *error = NULL;
  EmpathyChatroom *chatroom;

  g_variant_get (parameter, "(&s&s)", &room, &path);

  factory = empathy_client_factory_dup ();

  account = tp_simple_client_factory_ensure_account (
      TP_SIMPLE_CLIENT_FACTORY (factory), path, NULL, &error);
  if (account == NULL)
    {
      DEBUG ("Failed to get account '%s': %s", path, error->message);
      g_error_free (error);
      goto out;
    }

  chatroom = empathy_chatroom_manager_find (self->priv->chatroom_manager,
      account, room);
  if (chatroom == NULL)
    {
      DEBUG ("Failed to get chatroom '%s' on '%s'",
          room, path);
      goto out;
    }

  roster_window_favorite_chatroom_join (chatroom);

out:
  g_object_unref (factory);
}

static void
roster_window_favorite_chatroom_menu_add (EmpathyRosterWindow *self,
    EmpathyChatroom *chatroom)
{
  GMenuItem *item;
  const gchar *name, *account_name, *account_path;
  TpAccount *account;
  gchar *label;

  account = empathy_chatroom_get_account (chatroom);

  name = empathy_chatroom_get_name (chatroom);
  account_name = tp_account_get_display_name (account);
  account_path = tp_proxy_get_object_path (account);

  label = g_strdup_printf ("%s (%s)", name, account_name);

  item = g_menu_item_new (label, NULL);
  g_menu_item_set_action_and_target (item, "win.join", "(ss)",
      name, account_path);
  g_menu_item_set_attribute (item, "room-name", "s", name);
  g_menu_item_set_attribute (item, "account-path", "s", account_path);
  g_menu_append_item (self->priv->rooms_section, item);

  g_free (label);
}

static void
roster_window_favorite_chatroom_menu_added_cb (EmpathyChatroomManager *manager,
    EmpathyChatroom *chatroom,
    EmpathyRosterWindow *self)
{
  roster_window_favorite_chatroom_menu_add (self, chatroom);
}

static void
roster_window_favorite_chatroom_menu_removed_cb (
    EmpathyChatroomManager *manager,
    EmpathyChatroom *chatroom,
    EmpathyRosterWindow *self)
{
  GList *chatrooms;
  guint i, n;
  TpAccount *account;
  const gchar *account_path;

  account = empathy_chatroom_get_account (chatroom);
  account_path = tp_proxy_get_object_path (account);

  n = g_menu_model_get_n_items (G_MENU_MODEL (self->priv->rooms_section));

  for (i = 0; i < n; i++)
    {
      gchar *tmp;

      if (!g_menu_model_get_item_attribute (
            G_MENU_MODEL (self->priv->rooms_section), i,
            "room-name", "s", &tmp))
        continue;

      if (tp_strdiff (tmp, empathy_chatroom_get_name (chatroom)))
        {
          g_free (tmp);
          continue;
        }

      g_free (tmp);

      if (!g_menu_model_get_item_attribute (
            G_MENU_MODEL (self->priv->rooms_section), i,
            "account-path", "s", &tmp))
        continue;

      if (tp_strdiff (tmp, account_path))
        {
          g_free (tmp);
          continue;
        }

      g_menu_remove (self->priv->rooms_section, i);
      g_free (tmp);
      break;
    }

  chatrooms = empathy_chatroom_manager_get_chatrooms (
      self->priv->chatroom_manager, NULL);

  g_list_free (chatrooms);
}

static void
roster_window_favorite_chatroom_menu_setup (EmpathyRosterWindow *self)
{
  GList *chatrooms, *l;

  self->priv->chatroom_manager = empathy_chatroom_manager_dup_singleton (NULL);

  chatrooms = empathy_chatroom_manager_get_chatrooms (
    self->priv->chatroom_manager, NULL);

  for (l = chatrooms; l; l = l->next)
    roster_window_favorite_chatroom_menu_add (self, l->data);

  g_signal_connect (self->priv->chatroom_manager, "chatroom-added",
      G_CALLBACK (roster_window_favorite_chatroom_menu_added_cb),
      self);

  g_signal_connect (self->priv->chatroom_manager, "chatroom-removed",
      G_CALLBACK (roster_window_favorite_chatroom_menu_removed_cb),
      self);

  g_list_free (chatrooms);
}

static void
roster_window_room_join_new_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;

  empathy_new_chatroom_dialog_show (GTK_WINDOW (self));
}

static void
roster_window_room_join_favorites_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;
  GList *chatrooms, *l;

  chatrooms = empathy_chatroom_manager_get_chatrooms (self->priv->chatroom_manager,
      NULL);

  for (l = chatrooms; l; l = l->next)
    roster_window_favorite_chatroom_join (l->data);

  g_list_free (chatrooms);
}

static void
roster_window_room_manage_favorites_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;

  empathy_chatrooms_window_show (GTK_WINDOW (self));
}

static void
roster_window_edit_accounts_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  empathy_accounts_dialog_show_application (gdk_screen_get_default (),
      NULL, FALSE, FALSE);
}

static void
roster_window_edit_blocked_contacts_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;
  GtkWidget *dialog;

  dialog = empathy_contact_blocking_dialog_new (GTK_WINDOW (self));
  gtk_widget_show (dialog);

  g_signal_connect (dialog, "response",
      G_CALLBACK (gtk_widget_destroy), NULL);
}

void
empathy_roster_window_show_preferences (EmpathyRosterWindow *self,
    const gchar *tab)
{
  if (self->priv->preferences == NULL)
    {
      self->priv->preferences = empathy_preferences_new (GTK_WINDOW (self),
                                                   self->priv->shell_running);
      g_object_add_weak_pointer (G_OBJECT (self->priv->preferences),
               (gpointer) &self->priv->preferences);

      gtk_widget_show (self->priv->preferences);
    }
  else
    {
      gtk_window_present (GTK_WINDOW (self->priv->preferences));
    }

  if (tab != NULL)
    empathy_preferences_show_tab (
      EMPATHY_PREFERENCES (self->priv->preferences), tab);
}

static void
roster_window_edit_preferences_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;

  empathy_roster_window_show_preferences (self, NULL);
}

static void
roster_window_help_about_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;

  empathy_about_dialog_new (GTK_WINDOW (self));
}

static void
roster_window_help_contents_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;

  empathy_url_show (GTK_WIDGET (self), "help:empathy");
}

static void
next_tab_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;
  empathy_chat_window_next_tab (EMPATHY_CHAT_WINDOW (self->priv->chat_window));
}

static void
prev_tab_cb (GSimpleAction *action,
    GVariant *parameter,
    gpointer user_data)
{
  EmpathyRosterWindow *self = user_data;
  empathy_chat_window_prev_tab (EMPATHY_CHAT_WINDOW (self->priv->chat_window));
}


static gboolean
roster_window_throbber_button_press_event_cb (GtkWidget *throbber,
    GdkEventButton *event,
    EmpathyRosterWindow *self)
{
  if (event->type != GDK_BUTTON_PRESS ||
      event->button != 1)
    return FALSE;

  empathy_accounts_dialog_show_application (
      gtk_widget_get_screen (GTK_WIDGET (throbber)),
      NULL, FALSE, FALSE);

  return FALSE;
}

static void
roster_window_account_removed_cb (TpAccountManager  *manager,
    TpAccount *account,
    EmpathyRosterWindow *self)
{
  /* remove errors if any */
  roster_window_remove_error (self, account);

  /* remove the balance action if required */
  roster_window_remove_balance_action (self, account);
}

static void
account_connection_notify_cb (TpAccount *account,
    GParamSpec *spec,
    EmpathyRosterWindow *self)
{
  TpConnection *conn;

  conn = tp_account_get_connection (account);

  if (conn != NULL)
    {
      roster_window_setup_balance (self, account);
    }
  else
    {
      /* remove balance action if required */
      roster_window_remove_balance_action (self, account);
    }
}

static void
add_account (EmpathyRosterWindow *self,
    TpAccount *account)
{
  gulong handler_id;

  handler_id = GPOINTER_TO_UINT (g_hash_table_lookup (
    self->priv->status_changed_handlers, account));

  /* connect signal only if it was not connected yet */
  if (handler_id != 0)
    return;

  handler_id = g_signal_connect (account, "status-changed",
    G_CALLBACK (roster_window_connection_changed_cb), self);

  g_hash_table_insert (self->priv->status_changed_handlers,
    account, GUINT_TO_POINTER (handler_id));

  /* roster_window_setup_balance() relies on the TpConnection to be ready on
   * the TpAccount so we connect this signal as well. */
  tp_g_signal_connect_object (account, "notify::connection",
      G_CALLBACK (account_connection_notify_cb), self, 0);

  roster_window_setup_balance (self, account);
}

/* @account: if not %NULL, the only account which can be enabled */
static void
display_page_account_not_enabled (EmpathyRosterWindow *self,
    TpAccount *account)
{
  if (account == NULL)
    {
      display_page_message (self,
          _("You need to enable one of your accounts to see contacts here."),
          PAGE_MESSAGE_FLAG_ACCOUNTS);
    }
  else
    {
      gchar *tmp;

      /* translators: argument is an account name */
      tmp = g_strdup_printf (_("You need to enable %s to see contacts here."),
          tp_account_get_display_name (account));

      display_page_message (self, tmp, PAGE_MESSAGE_FLAG_ACCOUNTS);
      g_free (tmp);
    }
}
static gboolean
has_enabled_account (GList *accounts)
{
  GList *l;

  for (l = accounts; l != NULL; l = g_list_next (l))
    {
      TpAccount *account = l->data;

      if (tp_account_is_enabled (account))
        return TRUE;
    }

  return FALSE;
}

static void
set_notebook_page (EmpathyRosterWindow *self)
{
  GList *accounts;
  guint len;
  TpConnectionPresenceType presence;
  gboolean connected, connecting;

  connected = empathy_account_manager_get_accounts_connected (&connecting);

  /* Display the loading page if either:
   * - We are fetching contacts from Folks (startup)
   * - There is no account connected but at least one is connecting
   */
  if (!empathy_individual_manager_get_contacts_loaded (
        self->priv->individual_manager) ||
      (!connected && connecting))
    {
      display_page_message (self, NULL, PAGE_MESSAGE_FLAG_SPINNER);
      gtk_spinner_start (GTK_SPINNER (self->priv->spinner_loading));
      return;
    }

  gtk_spinner_stop (GTK_SPINNER (self->priv->spinner_loading));

  accounts = tp_account_manager_dup_valid_accounts (
      self->priv->account_manager);

  len = g_list_length (accounts);

  if (len == 0)
    {
      /* No account */
      display_page_no_account (self);
      goto out;
    }

  if (!has_enabled_account (accounts))
    {
      TpAccount *account = NULL;

      /* Pass the account if there is only one which can be enabled */
      if (len == 1)
        account = accounts->data;

      display_page_account_not_enabled (self, account);
      goto out;
    }

  presence = tp_account_manager_get_most_available_presence (
      self->priv->account_manager, NULL, NULL);

  if (presence == TP_CONNECTION_PRESENCE_TYPE_OFFLINE)
    {
// This message shows up always. WTF telepathy?
//      display_page_message (self,
//          _("Change your presence to see contacts here"),
//          PAGE_MESSAGE_FLAG_ONLINE);
      goto out;
    }

  if (empathy_roster_view_is_empty (self->priv->view))
    {
      if (empathy_roster_view_is_searching (self->priv->view))
        {
          display_page_message (self, _("No match found"),
              PAGE_MESSAGE_FLAG_NONE);
        }
      else
        {
          if (g_settings_get_boolean (self->priv->gsettings_ui,
                EMPATHY_PREFS_UI_SHOW_OFFLINE))
            display_page_message (self, _("You haven’t added any contacts yet"),
                PAGE_MESSAGE_FLAG_ADD_CONTACT);
          else
            display_page_message (self, _("No online contacts"),
                PAGE_MESSAGE_FLAG_SHOW_OFFLINE);
        }
      goto out;
    }

  display_page_contact_list (self);

out:
  g_list_free_full (accounts, g_object_unref);
}

static void
roster_window_account_validity_changed_cb (TpAccountManager  *manager,
    TpAccount *account,
    gboolean valid,
    EmpathyRosterWindow *self)
{
  if (valid)
    add_account (self, account);
  else
    roster_window_account_removed_cb (manager, account, self);

  set_notebook_page (self);
}

static void
roster_window_connection_items_setup (EmpathyRosterWindow *self)
{
  guint i;
  const gchar *actions_connected[] = {
      "room-join-new",
      "room-join-favorites",
      "chat-new-message",
      "chat-new-call",
      "chat-search-contacts",
      "chat-add-contact",
      "edit-blocked-contacts",
  };

  for (i = 0; i < G_N_ELEMENTS (actions_connected); i++)
    {
      GAction *action;

      action = g_action_map_lookup_action (G_ACTION_MAP (self),
          actions_connected[i]);

      self->priv->actions_connected = g_list_prepend (
          self->priv->actions_connected, action);
    }
}

static void
account_enabled_cb (TpAccountManager *manager,
    TpAccount *account,
    EmpathyRosterWindow *self)
{
  set_notebook_page (self);
}

static void
account_disabled_cb (TpAccountManager *manager,
    TpAccount *account,
    EmpathyRosterWindow *self)
{
  set_notebook_page (self);
}

static void
account_removed_cb (TpAccountManager *manager,
    TpAccount *account,
    EmpathyRosterWindow *self)
{
  set_notebook_page (self);
}

static void
account_manager_prepared_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  GList *accounts, *j;
  TpAccountManager *manager = TP_ACCOUNT_MANAGER (source_object);
  EmpathyRosterWindow *self = user_data;
  GError *error = NULL;

  if (!tp_proxy_prepare_finish (manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  accounts = tp_account_manager_dup_valid_accounts (
      self->priv->account_manager);
  for (j = accounts; j != NULL; j = j->next)
    {
      TpAccount *account = TP_ACCOUNT (j->data);

      add_account (self, account);
    }

  g_signal_connect (manager, "account-validity-changed",
      G_CALLBACK (roster_window_account_validity_changed_cb), self);
  tp_g_signal_connect_object (manager, "account-removed",
      G_CALLBACK (account_removed_cb), self, 0);
  tp_g_signal_connect_object (manager, "account-disabled",
      G_CALLBACK (account_disabled_cb), self, 0);
  tp_g_signal_connect_object (manager, "account-enabled",
      G_CALLBACK (account_enabled_cb), self, 0);

  roster_window_update_status (self);

  set_notebook_page (self);

  g_list_free_full (accounts, g_object_unref);
}

void
empathy_roster_window_set_shell_running (EmpathyRosterWindow *self,
    gboolean shell_running)
{
  if (self->priv->shell_running == shell_running)
    return;

  self->priv->shell_running = shell_running;
  g_object_notify (G_OBJECT (self), "shell-running");
}

static GObject *
empathy_roster_window_constructor (GType type,
    guint n_construct_params,
    GObjectConstructParam *construct_params)
{
  static GObject *window = NULL;

  if (window != NULL)
    return g_object_ref (window);

  window = G_OBJECT_CLASS (empathy_roster_window_parent_class)->constructor (
    type, n_construct_params, construct_params);

  g_object_add_weak_pointer (window, (gpointer) &window);

  return window;
}

static GActionEntry menubar_entries[] = {
  {"chat-new-message", roster_window_chat_new_message_cb},
  {"chat-new-call", roster_window_chat_new_call_cb},
  {"chat-add-contact", roster_window_chat_add_contact_cb},
  {"chat-search-contacts", roster_window_chat_search_contacts_cb},
  {"chat-quit", roster_window_chat_quit_cb},

  {"edit-accounts", roster_window_edit_accounts_cb},
  {"edit-blocked-contacts", roster_window_edit_blocked_contacts_cb},
  {"edit-preferences", roster_window_edit_preferences_cb},

  {"view-history", roster_window_view_history_cb},
  {"view-show-ft-manager", roster_window_view_show_ft_manager},

  {"room-join-new", roster_window_room_join_new_cb},
  {"room-join-favorites", roster_window_room_join_favorites_cb},
  {"join", roster_window_join_chatroom_menu_activate_cb, "(ss)"},
  {"room-manage-favorites", roster_window_room_manage_favorites_cb},

  {"help-contents", roster_window_help_contents_cb},
  {"help-about", roster_window_help_about_cb},
};

static GActionEntry app_entries[] =
{
  { "tab-next", next_tab_cb },
  { "tab-prev", prev_tab_cb }
};


static void
empathy_roster_window_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyRosterWindow *self = EMPATHY_ROSTER_WINDOW (object);

  switch (property_id)
    {
      case PROP_SHELL_RUNNING:
        self->priv->shell_running = g_value_get_boolean (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
empathy_roster_window_get_property (GObject    *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyRosterWindow *self = EMPATHY_ROSTER_WINDOW (object);

  switch (property_id)
    {
      case PROP_SHELL_RUNNING:
        g_value_set_boolean (value, self->priv->shell_running);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
empathy_roster_window_constructed (GObject *self)
{
  G_OBJECT_CLASS (empathy_roster_window_parent_class)->constructed (self);
}

static void
empathy_roster_window_class_init (EmpathyRosterWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *pspec;

  object_class->finalize = empathy_roster_window_finalize;
  object_class->constructor = empathy_roster_window_constructor;
  object_class->constructed = empathy_roster_window_constructed;

  object_class->set_property = empathy_roster_window_set_property;
  object_class->get_property = empathy_roster_window_get_property;

  pspec = g_param_spec_boolean ("shell-running",
      "Shell running",
      "Whether the Shell is running or not",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_SHELL_RUNNING, pspec);

  g_type_class_add_private (object_class, sizeof (EmpathyRosterWindowPriv));
}

static void
contacts_loaded_cb (EmpathyIndividualManager *manager,
    EmpathyRosterWindow *self)
{
  set_notebook_page (self);
}

static void
roster_window_setup_actions (EmpathyRosterWindow *self)
{
  GAction *action;

#define ADD_GSETTINGS_ACTION(schema, key) \
  action = g_settings_create_action (self->priv->gsettings_##schema, \
      EMPATHY_PREFS_##key); \
  g_action_map_add_action (G_ACTION_MAP (self), action); \
  g_object_unref (action);

  ADD_GSETTINGS_ACTION (ui, UI_SHOW_OFFLINE);

#undef ADD_GSETTINGS_ACTION
}

static void
menu_deactivate_cb (GtkMenuShell *menushell,
    gpointer user_data)
{
  /* FIXME: we shouldn't have to disconnect the signal (bgo #641327) */
  g_signal_handlers_disconnect_by_func (menushell,
      menu_deactivate_cb, user_data);

  gtk_menu_detach (GTK_MENU (menushell));
}

static void
menu_item_activated_cb (GtkMenuShell *menushell,
    gpointer user_data)
{
    EmpathyRosterWindow *roster_window = EMPATHY_ROSTER_WINDOW (user_data);

    hide_search_bar (roster_window);
}

static void
popup_individual_menu_cb (EmpathyRosterView *view,
    const gchar *active_group,
    FolksIndividual *individual,
    guint button,
    guint time,
    gpointer user_data)
{
  GtkWidget *menu;
  EmpathyIndividualFeatureFlags features = EMPATHY_INDIVIDUAL_FEATURE_CHAT |
    EMPATHY_INDIVIDUAL_FEATURE_CALL |
    EMPATHY_INDIVIDUAL_FEATURE_EDIT |
    EMPATHY_INDIVIDUAL_FEATURE_INFO |
    EMPATHY_INDIVIDUAL_FEATURE_LOG |
    EMPATHY_INDIVIDUAL_FEATURE_SMS |
    EMPATHY_INDIVIDUAL_FEATURE_CALL_PHONE |
    EMPATHY_INDIVIDUAL_FEATURE_REMOVE |
    EMPATHY_INDIVIDUAL_FEATURE_FILE_TRANSFER;

  menu = empathy_individual_menu_new (individual, active_group,
      features, NULL);

  /* menu is initially unowned but gtk_menu_attach_to_widget() takes its
   * floating ref. We can either wait for the view to release its ref
   * when it is destroyed (when leaving Empathy) or explicitly
   * detach the menu when it's not displayed any more.
   * We go for the latter as we don't want to keep useless menus in memory
   * during the whole lifetime of Empathy. */
  g_signal_connect (menu, "deactivate", G_CALLBACK (menu_deactivate_cb),
      NULL);
  g_signal_connect (menu, "menu-item-activated",
      G_CALLBACK (menu_item_activated_cb), user_data);

  gtk_menu_attach_to_widget (GTK_MENU (menu), GTK_WIDGET (view), NULL);
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL, button, time);
}

static void
view_empty_cb (EmpathyRosterView *view,
    GParamSpec *spec,
    EmpathyRosterWindow *self)
{
  set_notebook_page (self);

  if (!empathy_roster_view_is_empty (view))
    {
      gtk_widget_grab_focus (GTK_WIDGET (self->priv->view));

      /* The store is being filled, it will be done after an idle cb.
       * So we can then get events. If we do that too soon, event's
       * contact is not yet in the store and it won't get marked as
       * having events. */
      g_idle_add (roster_window_load_events_idle_cb, self);
    }
}

static void
tooltip_destroy_cb (GtkWidget *widget,
    EmpathyRosterWindow *self)
{
  g_clear_object (&self->priv->tooltip_widget);
}

static gboolean
individual_tooltip_cb (EmpathyRosterView *view,
    FolksIndividual *individual,
    gboolean keyboard_mode,
    GtkTooltip *tooltip,
    EmpathyRosterWindow *self)
{
  if (self->priv->tooltip_widget == NULL)
    {
      self->priv->tooltip_widget = empathy_individual_widget_new (individual,
          EMPATHY_INDIVIDUAL_WIDGET_FOR_TOOLTIP |
          EMPATHY_INDIVIDUAL_WIDGET_SHOW_LOCATION |
          EMPATHY_INDIVIDUAL_WIDGET_SHOW_CLIENT_TYPES);

      gtk_container_set_border_width (
          GTK_CONTAINER (self->priv->tooltip_widget), 8);

      g_object_ref (self->priv->tooltip_widget);

      tp_g_signal_connect_object (self->priv->tooltip_widget, "destroy",
          G_CALLBACK (tooltip_destroy_cb), self, 0);

      gtk_widget_show (self->priv->tooltip_widget);
    }
  else
    {
      empathy_individual_widget_set_individual (
        EMPATHY_INDIVIDUAL_WIDGET (self->priv->tooltip_widget), individual);
    }

  gtk_tooltip_set_custom (tooltip, self->priv->tooltip_widget);

  return TRUE;
}

typedef enum
{
  DND_DRAG_TYPE_INVALID = -1,
  DND_DRAG_TYPE_URI_LIST,
} DndDragType;

#define DRAG_TYPE(T,I) \
  { (gchar *) T, 0, I }

static const GtkTargetEntry drag_types_dest[] = {
  DRAG_TYPE ("text/path-list", DND_DRAG_TYPE_URI_LIST),
  DRAG_TYPE ("text/uri-list", DND_DRAG_TYPE_URI_LIST),
};

static GdkAtom drag_atoms_dest[G_N_ELEMENTS (drag_types_dest)];

static DndDragType
get_drag_type (GtkWidget *widget,
    GdkDragContext *context)
{
  GdkAtom target;
  guint i;

  target = gtk_drag_dest_find_target (widget, context, NULL);

  for (i = 0; i < G_N_ELEMENTS (drag_atoms_dest); i++)
    {
      if (target == drag_atoms_dest[i])
          return drag_types_dest[i].info;
    }

  return DND_DRAG_TYPE_INVALID;
}

static gboolean
individual_supports_ft (FolksIndividual *individual)
{
  EmpathyContact *contact;
  EmpathyCapabilities caps;
  gboolean result;

  contact = empathy_contact_dup_from_folks_individual (individual);
  if (contact == NULL)
    return FALSE;

  caps = empathy_contact_get_capabilities (contact);
  result = (caps & EMPATHY_CAPABILITIES_FT);

  g_object_unref (contact);
  return result;
}

static gboolean
view_drag_motion_cb (GtkWidget *widget,
    GdkDragContext *context,
    gint x,
    gint y,
    guint time_,
    EmpathyRosterWindow *self)
{
  DndDragType type;

  type = get_drag_type (widget, context);

  if (type == DND_DRAG_TYPE_URI_LIST)
    {
      /* Check if contact supports FT */
      FolksIndividual *individual;
      GtkListBoxRow *row;

      individual = empathy_roster_view_get_individual_at_y (self->priv->view,
          y, &row);
      if (individual == NULL)
        goto no_hl;

      if (!individual_supports_ft (individual))
        goto no_hl;

      gtk_list_box_drag_highlight_row (GTK_LIST_BOX (widget), row);
      return FALSE;
    }

no_hl:
  gtk_list_box_drag_unhighlight_row (GTK_LIST_BOX (widget));
  return FALSE;
}

static gboolean
view_drag_drop_cb (GtkWidget *widget,
    GdkDragContext *context,
    gint x,
    gint y,
    guint time_,
    EmpathyRosterWindow *self)
{
  DndDragType type;
  FolksIndividual *individual;

  type = get_drag_type (widget, context);
  if (type == DND_DRAG_TYPE_INVALID)
    return FALSE;

  individual = empathy_roster_view_get_individual_at_y (self->priv->view, y,
      NULL);
  if (individual == NULL)
    return FALSE;

  if (!individual_supports_ft (individual))
    return FALSE;

  gtk_drag_get_data (widget, context,
      gtk_drag_dest_find_target (widget, context, NULL), time_);

  return TRUE;
}

static void
view_drag_data_received_cb (GtkWidget *widget,
    GdkDragContext *context,
    gint x,
    gint y,
    GtkSelectionData *selection,
    guint info,
    guint time_,
    EmpathyRosterWindow *self)
{
  gboolean success = FALSE;

  if (selection == NULL)
    goto out;

  if (info == DND_DRAG_TYPE_URI_LIST)
    {
      const gchar *path;
      FolksIndividual *individual;
      EmpathyContact *contact;

      individual = empathy_roster_view_get_individual_at_y (self->priv->view,
          y, NULL);
      g_return_if_fail (individual != NULL);

      path = (const gchar *) gtk_selection_data_get_data (selection);

      contact = empathy_contact_dup_from_folks_individual (individual);
      empathy_send_file_from_uri_list (contact, path);

      g_object_unref (contact);

      success = TRUE;
    }

out:
  gtk_drag_finish (context, success, FALSE, time_);
}

static void
roster_window_most_available_presence_changed_cb (TpAccountManager *manager,
    TpConnectionPresenceType presence,
    const gchar *status,
    const gchar *message,
    EmpathyRosterWindow *self)
{
  set_notebook_page (self);
}

static void
empathy_roster_window_init (EmpathyRosterWindow *self)
{
  GtkBuilder *gui;
  GtkWidget *sw;
  gchar *filename;
  GtkWidget *header_bar;
  GtkWidget *new_conversation_button;
  GtkWidget *image;
  GtkWidget *chat_vbox;
  guint i;
  EmpathyRosterModel *model;

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_ROSTER_WINDOW, EmpathyRosterWindowPriv);

  empathy_set_css_provider (GTK_WIDGET (self));

  self->priv->gsettings_ui = g_settings_new (EMPATHY_PREFS_UI_SCHEMA);

  self->priv->sound_mgr = empathy_sound_manager_dup_singleton ();

  gtk_window_set_title (GTK_WINDOW (self), _("Contact List"));
  gtk_window_set_role (GTK_WINDOW (self), "contact_list");
  gtk_window_set_default_size (GTK_WINDOW (self), 225, 325);

  /* Set up interface */
  filename = empathy_file_lookup ("empathy-roster-window.ui", "src");
  gui = tpaw_builder_get_file (filename,
      "main_vbox", &self->priv->main_vbox,
      "chat_vbox", &chat_vbox,
      "balance_vbox", &self->priv->balance_vbox,
      "errors_vbox", &self->priv->errors_vbox,
      "auth_vbox", &self->priv->auth_vbox,
      "presence_toolbar", &self->priv->presence_toolbar,
      "notebook", &self->priv->notebook,
      "no_entry_label", &self->priv->no_entry_label,
      "roster_scrolledwindow", &sw,
      "button_account_settings", &self->priv->button_account_settings,
//      "button_online", &self->priv->button_online,
      "button_show_offline", &self->priv->button_show_offline,
      "button_add_contact", &self->priv->button_add_contact,
      "spinner_loading", &self->priv->spinner_loading,
      NULL);
  g_free (filename);

  header_bar = gtk_header_bar_new ();
  gtk_header_bar_set_title (GTK_HEADER_BAR(header_bar), _("Conversations"));
  gtk_header_bar_set_show_close_button (GTK_HEADER_BAR(header_bar), TRUE);

  image = gtk_image_new_from_icon_name ("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
  new_conversation_button = gtk_button_new ();
  g_signal_connect (new_conversation_button, "clicked",
      G_CALLBACK (roster_window_chat_new_message_cb), self);
  gtk_button_set_image (GTK_BUTTON (new_conversation_button), image);
  gtk_widget_set_tooltip_text (new_conversation_button, _("New Conversation"));
  gtk_window_set_titlebar (GTK_WINDOW (self), header_bar);
  gtk_container_add (GTK_CONTAINER (header_bar), new_conversation_button);
  gtk_widget_show_all (header_bar);

  gtk_container_add (GTK_CONTAINER (self), self->priv->main_vbox);
  gtk_widget_show (self->priv->main_vbox);

  g_signal_connect (self, "key-press-event",
      G_CALLBACK (roster_window_key_press_event_cb), self);

  g_object_unref (gui);

  self->priv->account_manager = tp_account_manager_dup ();

  tp_proxy_prepare_async (self->priv->account_manager, NULL,
      account_manager_prepared_cb, self);

  self->priv->errors = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      g_object_unref, NULL);

  self->priv->auths = g_hash_table_new (NULL, NULL);

  self->priv->status_changed_handlers = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, NULL);

  /* set up accelerators */
  g_action_map_add_action_entries (G_ACTION_MAP (self),
      app_entries, G_N_ELEMENTS (app_entries), self);

  /* set up menus */
  g_action_map_add_action_entries (G_ACTION_MAP (self),
      menubar_entries, G_N_ELEMENTS (menubar_entries), self);
  roster_window_setup_actions (self);

  filename = empathy_file_lookup ("empathy-roster-window-menubar.ui", "src");
  gui = tpaw_builder_get_file (filename,
      "appmenu", &self->priv->menumodel,
      "rooms", &self->priv->rooms_section,
      NULL);
  g_free (filename);

  g_object_ref (self->priv->menumodel);
  g_object_ref (self->priv->rooms_section);

  /* Set up connection related actions. */
  roster_window_connection_items_setup (self);
  roster_window_favorite_chatroom_menu_setup (self);

  g_object_unref (gui);

  /* Set up contact list. */
  empathy_status_presets_get_all ();

  /* Set up presence chooser */
  self->priv->presence_chooser = empathy_presence_chooser_new ();
  gtk_widget_show (self->priv->presence_chooser);
  gtk_container_add (GTK_CONTAINER (self->priv->presence_toolbar), self->priv->presence_chooser);

  /* Set up the throbber */
  self->priv->throbber = gtk_spinner_new ();
  gtk_widget_set_size_request (self->priv->throbber, 16, -1);
  gtk_widget_set_events (self->priv->throbber, GDK_BUTTON_PRESS_MASK);
  g_signal_connect (self->priv->throbber, "button-press-event",
    G_CALLBACK (roster_window_throbber_button_press_event_cb),
    self);
  gtk_container_add (GTK_CONTAINER (self->priv->presence_toolbar), self->priv->throbber);

  self->priv->individual_manager = empathy_individual_manager_dup_singleton ();

  model = EMPATHY_ROSTER_MODEL (empathy_roster_model_manager_new (self->priv->individual_manager));

  tp_g_signal_connect_object (self->priv->individual_manager,
      "contacts-loaded", G_CALLBACK (contacts_loaded_cb), self, 0);

  self->priv->view = EMPATHY_ROSTER_VIEW (
      empathy_roster_view_new (model));


  g_object_unref (model);

  gtk_widget_show (GTK_WIDGET (self->priv->view));

  gtk_container_add (GTK_CONTAINER (sw), GTK_WIDGET (self->priv->view));

  g_signal_connect (self->priv->view, "individual-activated",
      G_CALLBACK (individual_activated_cb), self);
  g_signal_connect (self->priv->view, "event-activated",
      G_CALLBACK (event_activated_cb), self);
  g_signal_connect (self->priv->view, "popup-individual-menu",
      G_CALLBACK (popup_individual_menu_cb), self);
  g_signal_connect (self->priv->view, "notify::empty",
      G_CALLBACK (view_empty_cb), self);
  g_signal_connect (self->priv->view, "individual-tooltip",
      G_CALLBACK (individual_tooltip_cb), self);

  /* DnD - destination */
  gtk_drag_dest_set (GTK_WIDGET (self->priv->view), GTK_DEST_DEFAULT_MOTION,
      drag_types_dest, G_N_ELEMENTS (drag_types_dest), GDK_ACTION_COPY);

  for (i = 0; i < G_N_ELEMENTS (drag_types_dest); ++i)
    drag_atoms_dest[i] = gdk_atom_intern (drag_types_dest[i].target, FALSE);

  g_signal_connect (self->priv->view, "drag-motion",
      G_CALLBACK (view_drag_motion_cb), self);
  g_signal_connect (self->priv->view, "drag-drop",
      G_CALLBACK (view_drag_drop_cb), self);
  g_signal_connect (self->priv->view, "drag-data-received",
      G_CALLBACK (view_drag_data_received_cb), self);

  gtk_widget_set_has_tooltip (GTK_WIDGET (self->priv->view), TRUE);

  /* Set up search bar */
  self->priv->search_bar = tpaw_live_search_new (
      GTK_WIDGET (self->priv->view));
  empathy_roster_view_set_live_search (self->priv->view,
      TPAW_LIVE_SEARCH (self->priv->search_bar));

  g_signal_connect_swapped (self, "map",
      G_CALLBACK (gtk_widget_grab_focus), self->priv->view);

  /* Load user-defined accelerators. */
  roster_window_accels_load ();

  gtk_window_set_default_size (GTK_WINDOW (self), 900, 600);
  /* Set window size. */
  empathy_geometry_bind (GTK_WINDOW (self), GEOMETRY_NAME);

  self->priv->chat_window = GTK_WIDGET (empathy_chat_window_new ());
  gtk_widget_show (GTK_WIDGET (self->priv->chat_window) );
  gtk_container_add (GTK_CONTAINER (chat_vbox), self->priv->chat_window);

  /* Enable event handling */
  self->priv->call_observer = empathy_call_observer_dup_singleton ();
  self->priv->event_manager = empathy_event_manager_dup_singleton ();
  self->priv->chat_manager = empathy_chat_manager_dup_singleton ();

  self->priv->theme_manager = empathy_theme_manager_dup_singleton ();

  tp_g_signal_connect_object (self->priv->event_manager, "event-added",
      G_CALLBACK (roster_window_event_added_cb), self, 0);
  tp_g_signal_connect_object (self->priv->event_manager, "event-removed",
      G_CALLBACK (roster_window_event_removed_cb), self, 0);

  g_signal_connect (self->priv->account_manager, "account-validity-changed",
      G_CALLBACK (roster_window_account_validity_changed_cb), self);
  g_signal_connect (self->priv->account_manager, "account-removed",
      G_CALLBACK (roster_window_account_removed_cb), self);
  g_signal_connect (self->priv->account_manager, "account-disabled",
      G_CALLBACK (roster_window_account_disabled_cb), self);
  g_signal_connect (self->priv->account_manager,
      "most-available-presence-changed",
      G_CALLBACK (roster_window_most_available_presence_changed_cb), self);

  g_object_set (G_OBJECT (self->priv->view),
                  "show-offline", TRUE,
                  "show-groups", FALSE,
                  NULL);

  g_settings_bind (self->priv->gsettings_ui, "show-balance-in-roster",
      self->priv->balance_vbox, "visible",
      G_SETTINGS_BIND_GET);

  g_signal_connect (self->priv->button_account_settings, "clicked",
      G_CALLBACK (button_account_settings_clicked_cb), self);
//  g_signal_connect (self->priv->button_online, "clicked",
//      G_CALLBACK (button_online_clicked_cb), self);
  g_signal_connect (self->priv->button_show_offline, "clicked",
      G_CALLBACK (button_show_offline_clicked_cb), self);
  g_signal_connect (self->priv->button_add_contact, "clicked",
      G_CALLBACK (button_add_contact_clicked_cb), self);
}

GtkWidget *
empathy_roster_window_new (GtkApplication *app)
{
  return g_object_new (EMPATHY_TYPE_ROSTER_WINDOW,
      "application", app,
      NULL);
}

GMenuModel *
empathy_roster_window_get_menu_model (EmpathyRosterWindow *self)
{
  g_return_val_if_fail (EMPATHY_IS_ROSTER_WINDOW (self), NULL);

  return G_MENU_MODEL (self->priv->menumodel);
}
