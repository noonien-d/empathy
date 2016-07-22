/*
 * Copyright (C) 2007-2011 Collabora Ltd.
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
#include <clutter-gtk/clutter-gtk.h>
#include <clutter-gst/clutter-gst.h>
#include <tp-account-widgets/tpaw-utils.h>

#ifdef CLUTTER_WINDOWING_X11
#include <X11/Xlib.h>
#endif

#include "empathy-bus-names.h"
#include "empathy-call-factory.h"
#include "empathy-call-window.h"
#include "empathy-ui-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_VOIP
#include "empathy-debug.h"

/* Exit after $TIMEOUT seconds if not displaying any call window */
#define TIMEOUT 60

static GtkApplication *app = NULL;
static gboolean activated = FALSE;
static gboolean use_timer = TRUE;
static guint inhibit_id = 0;

static EmpathyCallFactory *call_factory = NULL;

/* An EmpathyContact -> EmpathyCallWindow hash table for all existing
 * Call windows. We own a ref on the EmpathyContacts. */
static GHashTable *call_windows;

static void
call_window_destroyed_cb (GtkWidget *window,
    EmpathyContact *contact)
{
  g_hash_table_remove (call_windows, contact);

  g_application_release (G_APPLICATION (app));
}

static gboolean
find_window_for_handle (gpointer key,
    gpointer value,
    gpointer user_data)
{
  EmpathyContact *contact = key;
  guint handle = GPOINTER_TO_UINT (user_data);

  if (handle == empathy_contact_get_handle (contact))
    return TRUE;

  return FALSE;
}

static gboolean
incoming_call_cb (EmpathyCallFactory *factory,
    guint handle,
    TpCallChannel *channel,
    TpChannelDispatchOperation *dispatch_operation,
    TpAddDispatchOperationContext *context,
    gpointer user_data)
{
  EmpathyCallWindow *window = g_hash_table_find (call_windows,
      find_window_for_handle, GUINT_TO_POINTER (handle));

  if (window != NULL)
    {
      /* The window takes care of accepting or rejecting the context. */
      empathy_call_window_start_ringing (window,
          channel, dispatch_operation, context);
      return TRUE;
    }

  return FALSE;
}

static void
call_window_inhibit_cb (EmpathyCallWindow *window,
    gboolean inhibit,
    gpointer user_data)
{
  if (inhibit)
    {
      if (inhibit_id != 0)
        return;

      inhibit_id = gtk_application_inhibit (GTK_APPLICATION (app),
          GTK_WINDOW (window),
          GTK_APPLICATION_INHIBIT_LOGOUT | GTK_APPLICATION_INHIBIT_SWITCH |
          GTK_APPLICATION_INHIBIT_SUSPEND | GTK_APPLICATION_INHIBIT_IDLE,
          _("In a call"));
    }
  else
    {
      if (inhibit_id == 0)
        return;

      gtk_application_uninhibit (GTK_APPLICATION (app), inhibit_id);
      inhibit_id = 0;
    }
}

static void
new_call_handler_cb (EmpathyCallFactory *factory,
    EmpathyCallHandler *handler,
    gint64 user_action_time,
    gpointer user_data)
{
  EmpathyCallWindow *window;
  EmpathyContact *contact;
  guint32 x11_time;
  gboolean present;

  DEBUG ("Show the call window");

  contact = empathy_call_handler_get_contact (handler);

  window = g_hash_table_lookup (call_windows, contact);

  present = tp_user_action_time_should_present (user_action_time,
      &x11_time);

  if (window != NULL)
    {
      empathy_call_window_new_handler (window, handler, present, x11_time);
    }
  else
    {
      window = empathy_call_window_new (handler);

      g_hash_table_insert (call_windows, g_object_ref (contact), window);
      g_application_hold (G_APPLICATION (app));
      g_signal_connect (window, "destroy",
          G_CALLBACK (call_window_destroyed_cb), contact);
      g_signal_connect (window, "inhibit",
          G_CALLBACK (call_window_inhibit_cb), NULL);

      gtk_widget_show (GTK_WIDGET (window));

      if (present)
        tpaw_window_present_with_time (GTK_WINDOW (window), x11_time);
    }
}

static void
activate_cb (GApplication *application)
{
  GError *error = NULL;

  if (activated)
    return;

  activated = TRUE;

  if (!use_timer)
    {
      /* keep a 'ref' to the application */
      g_application_hold (G_APPLICATION (app));
    }

  g_assert (call_factory == NULL);
  call_factory = empathy_call_factory_initialise ();

  g_signal_connect (G_OBJECT (call_factory), "new-call-handler",
      G_CALLBACK (new_call_handler_cb), NULL);
  g_signal_connect (G_OBJECT (call_factory), "incoming-call",
      G_CALLBACK (incoming_call_cb), NULL);

  if (!empathy_call_factory_register (call_factory, &error))
    {
      g_critical ("Failed to register Handler: %s", error->message);
      g_error_free (error);
    }
}

int
main (int argc,
    char *argv[])
{
  GOptionContext *optcontext;
  GOptionEntry options[] = {
      { NULL }
  };
#ifdef ENABLE_DEBUG
  TpDebugSender *debug_sender;
#endif
  GError *error = NULL;
  gint retval;
  GtkSettings *gtk_settings;

  g_setenv ("GST_DEBUG_DUMP_DOT_DIR", g_get_tmp_dir (), FALSE);

#ifdef GDK_WINDOWING_X11
  /* We can't call clutter_gst_init() before gtk_clutter_init(), so no choice
   * but to intiialise X11 threading ourself */
  XInitThreads ();
#endif

  optcontext = g_option_context_new (N_("- Empathy Audio/Video Client"));
  g_option_context_add_group (optcontext, gst_init_get_option_group ());
  g_option_context_add_group (optcontext, gtk_get_option_group (TRUE));
  g_option_context_add_group (optcontext, cogl_get_option_group ());
  g_option_context_add_group (optcontext,
      clutter_get_option_group_without_init ());
  g_option_context_add_group (optcontext, gtk_clutter_get_option_group ());
  g_option_context_add_main_entries (optcontext, options, GETTEXT_PACKAGE);
  g_option_context_set_translation_domain (optcontext, GETTEXT_PACKAGE);

  if (!g_option_context_parse (optcontext, &argc, &argv, &error)) {
    g_print ("%s\nRun '%s --help' to see a full list of available command "
        "line options.\n",
        error->message, argv[0]);
    g_warning ("Error in empathy-call init: %s", error->message);
    return EXIT_FAILURE;
  }

  g_option_context_free (optcontext);

  clutter_gst_init (&argc, &argv);

  empathy_gtk_init ();
  textdomain (GETTEXT_PACKAGE);
  g_set_application_name (_("Empathy Audio/Video Client"));

  /* Make empathy and empathy-call appear as the same app in gnome-shell */
  g_set_prgname ("empathy");
  gtk_window_set_default_icon_name ("empathy");

  gtk_settings = gtk_settings_get_default ();
  g_object_set (G_OBJECT (gtk_settings), "gtk-application-prefer-dark-theme",
      TRUE, NULL);

  app = gtk_application_new (EMPATHY_CALL_BUS_NAME, G_APPLICATION_FLAGS_NONE);
  g_signal_connect (app, "activate", G_CALLBACK (activate_cb), NULL);

#ifdef ENABLE_DEBUG
  /* Set up debug sender */
  debug_sender = tp_debug_sender_dup ();
  g_log_set_default_handler (tp_debug_sender_log_handler, G_LOG_DOMAIN);
#endif

  if (g_getenv ("EMPATHY_PERSIST") != NULL)
    {
      DEBUG ("Disable timer");

      use_timer = FALSE;
    }

  call_windows = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      g_object_unref, NULL);

  /* the inactivity timeout can only be set while the application is held */
  g_application_hold (G_APPLICATION (app));
  g_application_set_inactivity_timeout (G_APPLICATION (app), TIMEOUT * 1000);
  g_application_release (G_APPLICATION (app));

  retval = g_application_run (G_APPLICATION (app), argc, argv);

  g_hash_table_unref (call_windows);
  g_object_unref (app);
  tp_clear_object (&call_factory);

#ifdef ENABLE_DEBUG
  g_object_unref (debug_sender);
#endif

  return retval;
}
