/*
 * Copyright (C) 2005-2007 Imendio AB
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
 * Authors: Martyn Russell <martyn@imendio.com>
 *          Xavier Claessens <xclaesse@gmail.com>
 *          Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
 *          Jonathan Tellier <jonathan.tellier@gmail.com>
 *          Travis Reitter <travis.reitter@collabora.co.uk>
 */

#include "config.h"
#include "empathy-accounts.h"

#include <glib/gi18n.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#ifdef HAVE_CHEESE
#include <cheese-gtk.h>
#endif

#include "empathy-accounts-common.h"
#include "empathy-bus-names.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_ACCOUNT
#include "empathy-debug.h"

static gboolean only_if_needed = FALSE;
static gboolean hidden = FALSE;
static gchar *selected_account_name = NULL;

static void
maybe_show_accounts_ui (TpAccountManager *manager,
    GApplication *app)
{
  if (hidden)
    return;

  if (only_if_needed && empathy_accounts_has_non_salut_accounts (manager))
    return;

  empathy_accounts_show_accounts_ui (manager, NULL, app);
}

static TpAccount *
find_account (TpAccountManager *mgr,
    const gchar *path)
{
  GList *accounts, *l;
  TpAccount *found = NULL;

  accounts = tp_account_manager_dup_valid_accounts (mgr);
  for (l = accounts; l != NULL; l = g_list_next (l))
    {
      if (!tp_strdiff (tp_proxy_get_object_path (l->data), path))
        {
          found = l->data;
          break;
        }
    }

  g_list_free_full (accounts, g_object_unref);
  return found;
}

static void
account_manager_ready_for_accounts_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *manager = TP_ACCOUNT_MANAGER (source_object);
  GError *error = NULL;
  GApplication *app = G_APPLICATION (user_data);

  if (!tp_proxy_prepare_finish (manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_clear_error (&error);
      goto out;
    }

  if (selected_account_name != NULL)
    {
      gchar *account_path;
      TpAccount *account;

      /* create and prep the corresponding TpAccount so it's fully ready by the
       * time we try to select it in the accounts dialog */
      if (g_str_has_prefix (selected_account_name, TP_ACCOUNT_OBJECT_PATH_BASE))
        account_path = g_strdup (selected_account_name);
      else
        account_path = g_strdup_printf ("%s%s", TP_ACCOUNT_OBJECT_PATH_BASE,
            selected_account_name);

      account = find_account (manager, account_path);

      if (account != NULL)
        {
          empathy_accounts_show_accounts_ui (manager, account, app);
          goto out;
        }
      else
        {
          DEBUG ("Failed to find account with path %s", account_path);

          g_clear_error (&error);

          maybe_show_accounts_ui (manager, app);
        }

      g_free (account_path);
    }
  else
    {
      maybe_show_accounts_ui (manager, app);
    }

out:
  g_application_release (app);
}

static void
app_activate (GApplication *app)
{
  TpAccountManager *account_manager;

  empathy_gtk_init ();
  account_manager = tp_account_manager_dup ();

  /* Hold the application while preparing the AM */
  g_application_hold (app);

  tp_proxy_prepare_async (account_manager, NULL,
    account_manager_ready_for_accounts_cb, app);

  g_object_unref (account_manager);
}

static gboolean
local_cmdline (GApplication *app,
    gchar ***arguments,
    gint *exit_status)
{
  gint i;
  gchar **argv;
  gint argc = 0;
  gboolean retval = TRUE;
  GError *error = NULL;

  GOptionContext *optcontext;
  GOptionEntry options[] = {
      { "hidden", 'h',
        0, G_OPTION_ARG_NONE, &hidden,
        N_("Don't display any dialogs; do any work (eg, importing) and exit"),
        NULL },
      { "if-needed", 'n',
        0, G_OPTION_ARG_NONE, &only_if_needed,
        N_("Don't display any dialogs unless there are only \"People Nearby\" accounts"),
        NULL },
      { "select-account", 's',
        G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_STRING, &selected_account_name,
        N_("Initially select given account (eg, "
            "gabble/jabber/foo_40example_2eorg0)"),
        N_("<account-id>") },

      { NULL }
  };

  optcontext = g_option_context_new (N_("- Empathy Accounts"));
  g_option_context_add_group (optcontext, gtk_get_option_group (FALSE));
  g_option_context_add_main_entries (optcontext, options, GETTEXT_PACKAGE);
  g_option_context_set_translation_domain (optcontext, GETTEXT_PACKAGE);

  argv = *arguments;
  for (i = 0; argv[i] != NULL; i++)
    argc++;

  if (!g_option_context_parse (optcontext, &argc, &argv, &error))
    {
      g_print ("%s\nRun '%s --help' to see a full list of available command line options.\n",
          error->message, argv[0]);
      g_warning ("Error in empathy init: %s", error->message);
      g_clear_error (&error);

      *exit_status = EXIT_FAILURE;
    }
  else
    {
      if (g_application_register (app, NULL, &error))
        {
          g_application_activate (app);
        }
      else
        {
          g_warning ("Impossible to register empathy-application: %s", error->message);
          g_clear_error (&error);
          *exit_status = EXIT_FAILURE;
        }
    }

  g_option_context_free (optcontext);

  return retval;
}

int
main (int argc, char *argv[])
{
  GtkApplication *app;
  GObjectClass *app_class;
  gint retval;

  g_type_init ();

#ifdef HAVE_CHEESE
  /* Used by the avatar chooser */
  g_return_val_if_fail (cheese_gtk_init (&argc, &argv), 1);
#endif

  empathy_init ();

  textdomain (GETTEXT_PACKAGE);
  g_set_application_name (_("Empathy Accounts"));

  /* Make empathy and empathy-accounts appear as the same app in gnome-shell */
  g_set_prgname ("empathy");
  gtk_window_set_default_icon_name ("empathy");

  app = gtk_application_new (EMPATHY_ACCOUNTS_BUS_NAME, G_APPLICATION_FLAGS_NONE);
  app_class = G_OBJECT_GET_CLASS (app);
  G_APPLICATION_CLASS (app_class)->local_command_line = local_cmdline;
  G_APPLICATION_CLASS (app_class)->activate = app_activate;

  retval = g_application_run (G_APPLICATION (app), argc, argv);

  g_object_unref (app);

  return retval;
}
