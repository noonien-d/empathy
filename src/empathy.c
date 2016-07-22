/*
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
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 */

#include "config.h"

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <libnotify/notify.h>
#include <tp-account-widgets/tpaw-utils.h>

#ifdef HAVE_LIBCHAMPLAIN
#include <clutter-gtk/clutter-gtk.h>
#endif

#include "empathy-accounts-common.h"
#include "empathy-accounts-dialog.h"
#include "empathy-bus-names.h"
#include "empathy-chatroom-manager.h"
#include "empathy-client-factory.h"
#include "empathy-connection-aggregator.h"
#include "empathy-ft-factory.h"
#include "empathy-ft-manager.h"
#include "empathy-gsettings.h"
#include "empathy-location-manager.h"
#include "empathy-notifications-approver.h"
#include "empathy-presence-manager.h"
#include "empathy-request-util.h"
#include "empathy-roster-window.h"
#include "empathy-status-icon.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

#define EMPATHY_TYPE_APP (empathy_app_get_type ())
#define EMPATHY_APP(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), EMPATHY_TYPE_APP, EmpathyApp))
#define EMPATHY_APP_CLASS(obj) (G_TYPE_CHECK_CLASS_CAST ((obj), EMPATHY_TYPE_APP, EmpathyAppClass))
#define EMPATHY_IS_EMPATHY_APP(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), EMPATHY_TYPE_APP))
#define EMPATHY_IS_EMPATHY_APP_CLASS(obj) (G_TYPE_CHECK_CLASS_TYPE ((obj), EMPATHY_TYPE_APP))
#define EMPATHY_APP_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), EMPATHY_TYPE_APP, EmpathyAppClass))

typedef struct _EmpathyApp EmpathyApp;
typedef struct _EmpathyAppClass EmpathyAppClass;

enum
{
  PROP_NO_CONNECT = 1,
  PROP_START_HIDDEN
};

GType empathy_app_get_type (void);

struct _EmpathyAppClass
{
  GtkApplicationClass parent_class;
};

struct _EmpathyApp
{
  GtkApplication parent;

  /* Properties */
  gboolean no_connect;
  gboolean start_hidden;
  gboolean show_preferences;
  gchar *preferences_tab;

  gboolean activated;

  GtkWidget *window;
  EmpathyStatusIcon *icon;
  TpAccountManager *account_manager;
  TplLogManager *log_manager;
  EmpathyChatroomManager *chatroom_manager;
  EmpathyFTFactory  *ft_factory;
  EmpathyPresenceManager *presence_mgr;
  GSettings *gsettings;
  EmpathyNotificationsApprover *notifications_approver;
  EmpathyConnectionAggregator *conn_aggregator;
#ifdef HAVE_GEOCLUE
  EmpathyLocationManager *location_manager;
#endif
#ifdef ENABLE_DEBUG
  TpDebugSender *debug_sender;
#endif

  gboolean shell_running;
};


G_DEFINE_TYPE(EmpathyApp, empathy_app, GTK_TYPE_APPLICATION)

static void
empathy_app_dispose (GObject *object)
{
  EmpathyApp *self = EMPATHY_APP (object);
  void (*dispose) (GObject *) =
    G_OBJECT_CLASS (empathy_app_parent_class)->dispose;

  empathy_presence_manager_set_state (self->presence_mgr,
      TP_CONNECTION_PRESENCE_TYPE_OFFLINE);

#ifdef ENABLE_DEBUG
  tp_clear_object (&self->debug_sender);
#endif

  tp_clear_object (&self->presence_mgr);
  tp_clear_object (&self->icon);
  tp_clear_object (&self->account_manager);
  tp_clear_object (&self->log_manager);
  tp_clear_object (&self->chatroom_manager);
#ifdef HAVE_GEOCLUE
  tp_clear_object (&self->location_manager);
#endif
  tp_clear_object (&self->ft_factory);
  tp_clear_object (&self->gsettings);
  tp_clear_object (&self->notifications_approver);
  tp_clear_object (&self->conn_aggregator);

  if (dispose != NULL)
    dispose (object);
}

static void
empathy_app_finalize (GObject *object)
{
  EmpathyApp *self = EMPATHY_APP (object);
  void (*finalize) (GObject *) =
    G_OBJECT_CLASS (empathy_app_parent_class)->finalize;

  g_free (self->preferences_tab);

  if (finalize != NULL)
    finalize (object);
}

static void account_manager_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data);

static void
empathy_app_set_property (GObject *object,
    guint prop_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyApp *self = EMPATHY_APP (object);

  switch (prop_id)
    {
      case PROP_NO_CONNECT:
        self->no_connect = g_value_get_boolean (value);
        break;
      case PROP_START_HIDDEN:
        self->start_hidden = g_value_get_boolean (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
new_incoming_transfer_cb (EmpathyFTFactory *factory,
    EmpathyFTHandler *handler,
    GError *error,
    gpointer user_data)
{
  if (error)
    empathy_ft_manager_display_error (handler, error);
  else
    empathy_receive_file_with_file_chooser (handler);
}

static void
new_ft_handler_cb (EmpathyFTFactory *factory,
    EmpathyFTHandler *handler,
    GError *error,
    gpointer user_data)
{
  if (error)
    empathy_ft_manager_display_error (handler, error);
  else
    empathy_ft_manager_add_handler (handler);

  g_object_unref (handler);
}

static void
empathy_presence_manager_set_auto_away_cb (GSettings *gsettings,
    const gchar *key,
    gpointer user_data)
{
  EmpathyPresenceManager *presence_mgr = user_data;

  empathy_presence_manager_set_auto_away (presence_mgr,
      g_settings_get_boolean (gsettings, key));
}

#define GNOME_SHELL_BUS_NAME "org.gnome.Shell"

static void
list_names_cb (TpDBusDaemon *bus_daemon,
        const gchar * const *names,
        const GError *error,
        gpointer user_data,
        GObject *weak_object)
{
  EmpathyApp *self = (EmpathyApp *) weak_object;
  guint i;

  if (error != NULL)
      goto out;

  for (i = 0; names[i] != NULL; i++)
    {
      if (!tp_strdiff (names[i], GNOME_SHELL_BUS_NAME))
        {
          self->shell_running = TRUE;
          break;
        }
    }

out:
  if (self->shell_running)
    {
      DEBUG ("GNOME Shell is running, don't create status icon");

      /* Rely on GNOME Shell to watch session state */
      empathy_presence_manager_set_auto_away (self->presence_mgr, FALSE);

      empathy_roster_window_set_shell_running (
          EMPATHY_ROSTER_WINDOW (self->window), TRUE);
    }
  else
    {
      gboolean autoaway;

      self->icon = empathy_status_icon_new (GTK_WINDOW (self->window),
          self->start_hidden);

      /* Allow Empathy to watch session state */
      autoaway = g_settings_get_boolean (self->gsettings,
          EMPATHY_PREFS_AUTOAWAY);

      g_signal_connect (self->gsettings,
          "changed::" EMPATHY_PREFS_AUTOAWAY,
          G_CALLBACK (empathy_presence_manager_set_auto_away_cb),
          self->presence_mgr);

      empathy_presence_manager_set_auto_away (self->presence_mgr, autoaway);
    }

  /* Now that we have been started, reset this flag so future invocation will
   * be able to raise the window, see bgo#729282 */
  self->start_hidden = FALSE;
}

static void
empathy_app_activate (GApplication *app)
{
  EmpathyApp *self = (EmpathyApp *) app;

  if (!self->activated)
    {
      GError *error = NULL;
      TpDBusDaemon *dbus;

      empathy_gtk_init ();

      /* Create the FT factory */
      self->ft_factory = empathy_ft_factory_dup_singleton ();
      g_signal_connect (self->ft_factory, "new-ft-handler",
          G_CALLBACK (new_ft_handler_cb), NULL);
      g_signal_connect (self->ft_factory, "new-incoming-transfer",
          G_CALLBACK (new_incoming_transfer_cb), NULL);

      if (!empathy_ft_factory_register (self->ft_factory, &error))
        {
          g_warning ("Failed to register FileTransfer handler: %s",
              error->message);
          g_error_free (error);
        }

      self->activated = TRUE;

      /* Setting up UI */
      self->window = empathy_roster_window_new (GTK_APPLICATION (app));
      gtk_application_set_app_menu (GTK_APPLICATION (self),
          empathy_roster_window_get_menu_model (
            EMPATHY_ROSTER_WINDOW (self->window)));

      gtk_application_add_window (GTK_APPLICATION (app),
          GTK_WINDOW (self->window));
      gtk_application_add_accelerator (GTK_APPLICATION (app),
          "<Primary>h",
          "win." EMPATHY_PREFS_UI_SHOW_OFFLINE,
          NULL);

      /* check if Shell is running */
      dbus = tp_dbus_daemon_dup (&error);
      g_assert_no_error (error);

      tp_dbus_daemon_list_names (dbus, -1, list_names_cb,
              self, NULL, G_OBJECT (self));

      g_object_unref (dbus);

      self->notifications_approver =
        empathy_notifications_approver_dup_singleton ();
    }

  if (self->show_preferences)
    {
      empathy_roster_window_show_preferences (
          EMPATHY_ROSTER_WINDOW (self->window),
          tp_str_empty (self->preferences_tab) ? NULL : self->preferences_tab);

      self->show_preferences = FALSE;
    }
  else
    {
      if (!self->start_hidden)
        tpaw_window_present (GTK_WINDOW (self->window));
    }

  /* Display the accounts dialog if needed */
  tp_proxy_prepare_async (self->account_manager, NULL,
      account_manager_ready_cb, self);
}

static gboolean
preferences_cb (const char *option_name,
    const char *value,
    gpointer data,
    GError **error)
{
  EmpathyApp *self = data;

  self->show_preferences = TRUE;

  g_free (self->preferences_tab);
  self->preferences_tab = g_strdup (value);

  return TRUE;
}

static gboolean
show_version_cb (const char *option_name,
    const char *value,
    gpointer data,
    GError **error);

static void
open_preference_action_cb (GAction  *action,
    GVariant *parameter,
    gpointer  data)
{
  EmpathyApp *self = EMPATHY_APP (data);

  self->show_preferences = TRUE;

  g_free (self->preferences_tab);
  self->preferences_tab = g_variant_dup_string (parameter, NULL);
}

static gboolean
empathy_app_local_command_line (GApplication *app,
    gchar ***arguments,
    gint *exit_status)
{
  EmpathyApp *self = (EmpathyApp *) app;
  GSimpleAction *action;
  gint i;
  gchar **argv;
  gint argc = 0;
  gboolean retval = TRUE;
  GError *error = NULL;
  gboolean no_connect = FALSE, start_hidden = FALSE;

  GOptionContext *optcontext;
  GOptionGroup *group;
  GOptionEntry options[] = {
      { "no-connect", 'n',
        0, G_OPTION_ARG_NONE, &no_connect,
        N_("Don't connect on startup"),
        NULL },
      { "start-hidden", 'h',
        0, G_OPTION_ARG_NONE, &start_hidden,
        N_("Don't display the contact list or any other dialogs on startup"),
        NULL },
      { "show-preferences", 'p',
        G_OPTION_FLAG_OPTIONAL_ARG, G_OPTION_ARG_CALLBACK, &preferences_cb,
        NULL, NULL },
      { "version", 'v',
        G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, show_version_cb,
        NULL, NULL },
      { NULL }
  };

  if (!g_application_register (app, NULL, &error))
    {
      g_warning ("Impossible to register empathy: %s", error->message);
      g_clear_error (&error);
      *exit_status = EXIT_FAILURE;
      return retval;
    }

  action = g_simple_action_new ("open-preferences", G_VARIANT_TYPE_STRING);
  g_signal_connect (action, "activate", G_CALLBACK (open_preference_action_cb), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (action));
  g_object_unref (action);

  /* We create a group so that GOptionArgFuncs get the user data */
  group = g_option_group_new ("empathy", NULL, NULL, app, NULL);
  g_option_group_set_translation_domain (group, GETTEXT_PACKAGE);
  g_option_group_add_entries (group, options);

  optcontext = g_option_context_new (N_("- Empathy IM Client"));
  g_option_context_add_group (optcontext, gtk_get_option_group (FALSE));
  g_option_context_set_main_group (optcontext, group);
  g_option_context_set_translation_domain (optcontext, GETTEXT_PACKAGE);

  argc = g_strv_length (*arguments);

  /* We dup the args because g_option_context_parse() sets things to NULL,
   * but we want to parse all the command line to the primary instance
   * if necessary. */
  argv = g_new (gchar*, argc + 1);
  for (i = 0; i <= argc; i++)
    argv[i] = (*arguments)[i];

  if (!g_option_context_parse (optcontext, &argc, &argv, &error))
    {
      g_print ("%s\nRun '%s --help' to see a full list of available command "
          "line options.\n",
          error->message, argv[0]);
      g_warning ("Error in empathy init: %s", error->message);
      g_clear_error (&error);

      *exit_status = EXIT_FAILURE;
    }
  else
    {
      self->no_connect = no_connect;
      self->start_hidden = start_hidden;

      if (self->show_preferences)
        {
          GVariant *parameter;
          parameter = g_variant_new_string (self->preferences_tab ? self->preferences_tab : "");
          g_action_group_activate_action (G_ACTION_GROUP (app), "open-preferences", parameter);
        }

      g_application_activate (app);
    }

  g_free (argv);
  g_option_context_free (optcontext);

  return retval;
}

static void empathy_app_constructed (GObject *object);

static void
empathy_app_class_init (EmpathyAppClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GApplicationClass *g_app_class = G_APPLICATION_CLASS (klass);
  GParamSpec *spec;

  gobject_class->set_property = empathy_app_set_property;
  gobject_class->constructed = empathy_app_constructed;
  gobject_class->dispose = empathy_app_dispose;
  gobject_class->finalize = empathy_app_finalize;

  g_app_class->local_command_line = empathy_app_local_command_line;
  g_app_class->activate = empathy_app_activate;

  spec = g_param_spec_boolean ("no-connect", "no connect",
      "Don't connect on startup",
      FALSE,
      G_PARAM_STATIC_STRINGS | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (gobject_class, PROP_NO_CONNECT, spec);

  spec = g_param_spec_boolean ("start-hidden", "start hidden",
      "Don't display the contact list or any other dialogs on startup",
      FALSE,
      G_PARAM_STATIC_STRINGS | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (gobject_class, PROP_START_HIDDEN, spec);
}

static void
empathy_app_init (EmpathyApp *self)
{
}

static void
migrate_config_to_xdg_dir (void)
{
  gchar *xdg_dir, *old_dir, *xdg_filename, *old_filename;
  int i;
  GFile *xdg_file, *old_file;
  static const gchar* filenames[] = {
    "geometry.ini",
    "irc-networks.xml",
    "chatrooms.xml",
    "contact-groups.xml",
    "status-presets.xml",
    "accels.txt",
    NULL
  };

  xdg_dir = g_build_filename (g_get_user_config_dir (), PACKAGE_NAME, NULL);
  if (g_file_test (xdg_dir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))
    {
      /* xdg config dir already exists */
      g_free (xdg_dir);
      return;
    }

  old_dir = g_build_filename (g_get_home_dir (), ".gnome2",
      PACKAGE_NAME, NULL);
  if (!g_file_test (old_dir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))
    {
      /* old config dir didn't exist */
      g_free (xdg_dir);
      g_free (old_dir);
      return;
    }

  if (g_mkdir_with_parents (xdg_dir, (S_IRUSR | S_IWUSR | S_IXUSR)) == -1)
    {
      DEBUG ("Failed to create configuration directory; aborting migration");
      g_free (xdg_dir);
      g_free (old_dir);
      return;
    }

  for (i = 0; filenames[i]; i++)
    {
      old_filename = g_build_filename (old_dir, filenames[i], NULL);
      if (!g_file_test (old_filename, G_FILE_TEST_EXISTS))
        {
          g_free (old_filename);
          continue;
        }
      xdg_filename = g_build_filename (xdg_dir, filenames[i], NULL);
      old_file = g_file_new_for_path (old_filename);
      xdg_file = g_file_new_for_path (xdg_filename);

      if (!g_file_move (old_file, xdg_file, G_FILE_COPY_NONE,
          NULL, NULL, NULL, NULL))
        DEBUG ("Failed to migrate %s", filenames[i]);

      g_free (old_filename);
      g_free (xdg_filename);
      g_object_unref (old_file);
      g_object_unref (xdg_file);
    }

  g_free (xdg_dir);
  g_free (old_dir);
}

static void
show_accounts_ui (EmpathyApp *self,
    GdkScreen *screen,
    gboolean if_needed)
{
  empathy_accounts_dialog_show_application (screen,
      NULL, if_needed, self->start_hidden);
}

static gboolean
show_version_cb (const char *option_name,
    const char *value,
    gpointer data,
    GError **error)
{
  g_print ("%s\n", PACKAGE_STRING);

  exit (EXIT_SUCCESS);
}

static void
account_manager_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *manager = TP_ACCOUNT_MANAGER (source_object);
  EmpathyApp *self = user_data;
  GError *error = NULL;
  TpConnectionPresenceType presence;

  if (!tp_proxy_prepare_finish (manager, result, &error))
    {
      GtkWidget *dialog;

      DEBUG ("Failed to prepare account manager: %s", error->message);

      dialog = gtk_message_dialog_new (NULL, GTK_DIALOG_MODAL,
          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
          _("Error contacting the Account Manager"));
      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
          _("There was an error while trying to connect to the Telepathy "
            "Account Manager. The error was:\n\n%s"),
          error->message);

      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);

      g_error_free (error);
      return;
    }

  /* Autoconnect */
  presence = tp_account_manager_get_most_available_presence (manager, NULL,
      NULL);

  if (g_settings_get_boolean (self->gsettings, EMPATHY_PREFS_AUTOCONNECT) &&
      !self->no_connect &&
      tp_connection_presence_type_cmp_availability
          (presence, TP_CONNECTION_PRESENCE_TYPE_OFFLINE)
            <= 0)
      /* if current state is Offline, then put it online */
      empathy_presence_manager_set_state (self->presence_mgr,
          TP_CONNECTION_PRESENCE_TYPE_AVAILABLE);

  /* Pop up the accounts dialog if we don't have any account */
  if (!empathy_accounts_has_accounts (manager))
    show_accounts_ui (self, gdk_screen_get_default (), TRUE);
}

static void
account_join_chatrooms (TpAccount *account,
  EmpathyChatroomManager *chatroom_manager)
{
  TpConnection *conn;
  GList *chatrooms, *p;

  /* Wait if we are not connected or the TpConnection is not prepared yet */
  conn = tp_account_get_connection (account);
  if (conn == NULL)
    return;

  chatrooms = empathy_chatroom_manager_get_chatrooms (
          chatroom_manager, account);

  for (p = chatrooms; p != NULL; p = p->next)
    {
      EmpathyChatroom *room = EMPATHY_CHATROOM (p->data);

      if (!empathy_chatroom_get_auto_connect (room))
        continue;

      empathy_join_muc (account, empathy_chatroom_get_room (room),
        TP_USER_ACTION_TIME_NOT_USER_ACTION);
    }
  g_list_free (chatrooms);
}

static void
account_connection_changed_cb (TpAccount *account,
    GParamSpec *spec,
    EmpathyChatroomManager *manager)
{
  account_join_chatrooms (account, manager);
}

static void
account_manager_chatroom_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *account_manager = TP_ACCOUNT_MANAGER (source_object);
  EmpathyChatroomManager *chatroom_manager = user_data;
  GList *accounts, *l;
  GError *error = NULL;

  if (!tp_proxy_prepare_finish (account_manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  accounts = tp_account_manager_dup_valid_accounts (account_manager);

  for (l = accounts; l != NULL; l = g_list_next (l))
    {
      TpAccount *account = TP_ACCOUNT (l->data);

      /* Try to join all rooms if we're connected */
      account_join_chatrooms (account, chatroom_manager);

      /* And/or join them on (re)connection */
      tp_g_signal_connect_object (account, "notify::connection",
        G_CALLBACK (account_connection_changed_cb), chatroom_manager, 0);
    }
  g_list_free_full (accounts, g_object_unref);
}

static void
chatroom_manager_ready_cb (EmpathyChatroomManager *chatroom_manager,
    GParamSpec *pspec,
    gpointer user_data)
{
  TpAccountManager *account_manager = user_data;

  tp_proxy_prepare_async (account_manager, NULL,
      account_manager_chatroom_ready_cb, chatroom_manager);
}

static void
empathy_app_constructed (GObject *object)
{
  EmpathyApp *self = (EmpathyApp *) object;
  gboolean chatroom_manager_ready;

  textdomain (GETTEXT_PACKAGE);
  g_set_application_name (_(PACKAGE_NAME));

  gtk_window_set_default_icon_name ("empathy");

#ifdef ENABLE_DEBUG
  /* Set up debug sender */
  self->debug_sender = tp_debug_sender_dup ();
  g_log_set_default_handler (tp_debug_sender_log_handler, G_LOG_DOMAIN);
#endif

  notify_init (_(PACKAGE_NAME));

  /* Setting up Idle */
  self->presence_mgr = empathy_presence_manager_dup_singleton ();

  self->gsettings = g_settings_new (EMPATHY_PREFS_SCHEMA);

  /* account management */
  self->account_manager = tp_account_manager_dup ();
  tp_proxy_prepare_async (self->account_manager, NULL,
      account_manager_ready_cb, self);

  tp_account_manager_enable_restart (self->account_manager);

  migrate_config_to_xdg_dir ();

  /* Logging */
  self->log_manager = tpl_log_manager_dup_singleton ();

  self->chatroom_manager = empathy_chatroom_manager_dup_singleton (NULL);

  g_object_get (self->chatroom_manager, "ready", &chatroom_manager_ready, NULL);
  if (!chatroom_manager_ready)
    {
      g_signal_connect (G_OBJECT (self->chatroom_manager), "notify::ready",
          G_CALLBACK (chatroom_manager_ready_cb), self->account_manager);
    }
  else
    {
      chatroom_manager_ready_cb (self->chatroom_manager, NULL,
          self->account_manager);
    }

  /* Location mananger */
#ifdef HAVE_GEOCLUE
  self->location_manager = empathy_location_manager_dup_singleton ();
#endif

  self->conn_aggregator = empathy_connection_aggregator_dup_singleton ();

  self->activated = FALSE;
  self->ft_factory = NULL;
  self->window = NULL;
}

static void
add_empathy_features (void)
{
  /* Add 'empathy' specific feature before doing any preparation */
  EmpathyClientFactory *factory;

  factory = empathy_client_factory_dup ();

  tp_simple_client_factory_add_connection_features_varargs (
      TP_SIMPLE_CLIENT_FACTORY (factory),
      /* empathy_connection_aggregator_get_all_groups(), used by
       * EmpathyGroupsWidget relies on it */
      TP_CONNECTION_FEATURE_CONTACT_GROUPS,
      /* empathy_connection_aggregator_dup_all_contacts(), used by
       * EmpathyEventManager relies on it */
      TP_CONNECTION_FEATURE_CONTACT_LIST,
      NULL);

  g_object_unref (factory);
}

int
main (int argc, char *argv[])
{
  EmpathyApp *app;
  gint retval;

  g_type_init ();

#ifdef HAVE_LIBCHAMPLAIN
  g_return_val_if_fail (gtk_clutter_init (&argc, &argv) ==
      CLUTTER_INIT_SUCCESS, 1);
#endif

  g_type_init ();
  empathy_init ();

  add_empathy_features ();

  app = g_object_new (EMPATHY_TYPE_APP,
      "application-id", EMPATHY_BUS_NAME,
      NULL);

  retval = g_application_run (G_APPLICATION (app), argc, argv);

  notify_uninit ();
  xmlCleanupParser ();

  g_object_unref (app);

  return retval;
}
