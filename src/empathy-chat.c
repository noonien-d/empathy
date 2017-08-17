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

  empathy_chat_window_present_chat(NULL, 0);

  g_signal_connect (chat_mgr, "displayed-chats-changed",
      G_CALLBACK (displayed_chats_changed_cb), GUINT_TO_POINTER (1));
}
