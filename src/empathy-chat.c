/*
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
 *          Guillaume Desmottes <guillaume.desmottes@collabora.co.uk>
 */

#include "config.h"

#include <glib/gi18n.h>
#include <libnotify/notify.h>

#include "empathy-bus-names.h"
#include "empathy-chat-manager.h"
#include "empathy-chat-resources.h"
#include "empathy-presence-manager.h"
#include "empathy-theme-manager.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CHAT
#include "empathy-debug.h"

/* Exit after $TIMEOUT seconds if not displaying any call window */
#define TIMEOUT 60

static GtkApplication *app = NULL;
static gboolean activated = FALSE;
static gboolean use_timer = TRUE;

static EmpathyChatManager *chat_mgr = NULL;

static void
displayed_chats_changed_cb (EmpathyChatManager *mgr,
    guint nb_chats,
    gpointer user_data)
{
  DEBUG ("New chat count: %u", nb_chats);

  if (nb_chats == 0)
    g_application_release (G_APPLICATION (app));
  else
    g_application_hold (G_APPLICATION (app));
}

static void
activate_cb (GApplication *application)
{
  if (activated)
    return;

  activated = TRUE;
  empathy_gtk_init ();

  if (!use_timer)
    {
      /* keep a 'ref' to the application */
      g_application_hold (G_APPLICATION (application));
    }

  g_assert (chat_mgr == NULL);
  chat_mgr = empathy_chat_manager_dup_singleton ();

  g_signal_connect (chat_mgr, "displayed-chats-changed",
      G_CALLBACK (displayed_chats_changed_cb), GUINT_TO_POINTER (1));
}

int
main (int argc,
    char *argv[])
{
  GOptionContext *optcontext;
  GOptionEntry options[] = {
      { NULL }
  };
  GResource *resource;
#ifdef ENABLE_DEBUG
  TpDebugSender *debug_sender;
#endif
  GError *error = NULL;
  EmpathyPresenceManager *presence_mgr;
  EmpathyThemeManager *theme_mgr;
  gint retval;

  optcontext = g_option_context_new (N_("- Empathy Chat Client"));
  g_option_context_add_group (optcontext, gtk_get_option_group (FALSE));
  g_option_context_add_main_entries (optcontext, options, GETTEXT_PACKAGE);
  g_option_context_set_translation_domain (optcontext, GETTEXT_PACKAGE);

  if (!g_option_context_parse (optcontext, &argc, &argv, &error))
    {
      g_print ("%s\nRun '%s --help' to see a full list of available command "
          "line options.\n",
          error->message, argv[0]);
      g_warning ("Error in empathy-av init: %s", error->message);
      return EXIT_FAILURE;
    }

  g_option_context_free (optcontext);

  empathy_init ();

  /* Make empathy and empathy-chat appear as the same app in gnome-shell */
  g_set_prgname ("empathy");
  gtk_window_set_default_icon_name ("empathy");
  textdomain (GETTEXT_PACKAGE);

  notify_init (_(PACKAGE_NAME));

  resource = empathy_chat_get_resource ();
  g_resources_register (resource);

  app = gtk_application_new (EMPATHY_CHAT_BUS_NAME, G_APPLICATION_FLAGS_NONE);
  g_signal_connect (app, "activate", G_CALLBACK (activate_cb), NULL);

#ifdef ENABLE_DEBUG
  /* Set up debug sender */
  debug_sender = tp_debug_sender_dup ();
  g_log_set_default_handler (tp_debug_sender_log_handler, G_LOG_DOMAIN);
#endif

  /* Setting up Idle */
  presence_mgr = empathy_presence_manager_dup_singleton ();

  /* Keep the theme manager alive as it does some caching */
  theme_mgr = empathy_theme_manager_dup_singleton ();

  if (g_getenv ("EMPATHY_PERSIST") != NULL)
    {
      DEBUG ("Disable timer");

      use_timer = FALSE;
    }

  /* the inactivity timeout can only be set while the application is held */
  g_application_hold (G_APPLICATION (app));
  g_application_set_inactivity_timeout (G_APPLICATION (app), TIMEOUT * 1000);
  g_application_release (G_APPLICATION (app));

  DEBUG ("Waiting for text channels to handle");

  retval = g_application_run (G_APPLICATION (app), argc, argv);

  g_object_unref (app);
  g_object_unref (presence_mgr);
  g_object_unref (theme_mgr);
  tp_clear_object (&chat_mgr);

#ifdef ENABLE_DEBUG
  g_object_unref (debug_sender);
#endif

  g_resources_unregister (resource);
  g_resource_unref (resource);

  notify_uninit ();

  return retval;
}
