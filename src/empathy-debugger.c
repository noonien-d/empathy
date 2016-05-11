/*
 * Copyright (C) 2005-2007 Imendio AB
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
 */

#include "config.h"

#include <glib/gi18n.h>

#include "empathy-bus-names.h"
#include "empathy-debug-window.h"
#include "empathy-ui-utils.h"

static GtkWidget *window = NULL;
static gchar *service = NULL;

static void
app_activate (GApplication *app)
{
  if (window == NULL)
    {
      window = empathy_debug_window_new (NULL);

      gtk_application_add_window (GTK_APPLICATION (app),
          GTK_WINDOW (window));
    }
  else
    {
      gtk_window_present (GTK_WINDOW (window));
    }

  if (!tp_str_empty (service))
    empathy_debug_window_show (EMPATHY_DEBUG_WINDOW (window),
        service);
}

static void
on_show_service_cb (GAction  *action,
    GVariant *parameter,
    gpointer  data)
{
  g_free (service);
  service = g_variant_dup_string (parameter, NULL);
}

static gboolean
local_cmdline (GApplication *app,
    gchar ***arguments,
    gint *exit_status)
{
  GError *error = NULL;
  GSimpleAction *action;
  gchar **argv;
  gint argc = 0;
  gint i;
  gboolean retval = TRUE;

  GOptionContext *optcontext;
  GOptionEntry options[] = {
      { "show-service", 's',
        0, G_OPTION_ARG_STRING, &service,
        N_("Show a particular service"),
        NULL },
      { NULL }
  };

  optcontext = g_option_context_new (N_("- Empathy Debugger"));
  g_option_context_add_group (optcontext, gtk_get_option_group (FALSE));
  g_option_context_add_main_entries (optcontext, options, GETTEXT_PACKAGE);
  g_option_context_set_translation_domain (optcontext, GETTEXT_PACKAGE);

  action = g_simple_action_new ("show-service", G_VARIANT_TYPE_STRING);
  g_signal_connect (action, "activate", G_CALLBACK (on_show_service_cb), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (action));
  g_object_unref (action);

  argv = *arguments;
  for (i = 0; argv[i] != NULL; i++)
    argc++;

  if (!g_option_context_parse (optcontext, &argc, &argv, &error))
    {
      g_print ("%s\nRun '%s --help' to see a full list of available command "
          "line options.\n",
          error->message, argv[0]);

      g_clear_error (&error);
      *exit_status = EXIT_FAILURE;
    }
  else
    {
      if (g_application_register (app, NULL, &error))
        {
          GVariant *parameter = g_variant_new_string (service ? service : "");
          g_action_group_activate_action (G_ACTION_GROUP (app), "show-service", parameter);

          g_application_activate (app);
        }
      else
        {
          g_warning ("Impossible to register empathy-debugger: %s", error->message);
          g_clear_error (&error);
          *exit_status = EXIT_FAILURE;
        }
    }

  g_option_context_free (optcontext);

  return retval;
}

int
main (int argc,
    char **argv)
{
  GtkApplication *app;
  GApplicationClass *app_class;
  gint retval;

  textdomain (GETTEXT_PACKAGE);

  app = gtk_application_new (EMPATHY_DEBUGGER_BUS_NAME, G_APPLICATION_FLAGS_NONE);
  app_class = G_APPLICATION_CLASS (G_OBJECT_GET_CLASS (app));
  app_class->local_command_line = local_cmdline;
  app_class->activate = app_activate;

  g_set_application_name (_("Empathy Debugger"));

  /* Make empathy and empathy-debugger appear as the same app in gnome-shell */
  g_set_prgname ("empathy");
  gtk_window_set_default_icon_name ("empathy");

  retval = g_application_run (G_APPLICATION (app), argc, argv);

  g_object_unref (app);

  return retval;
}
