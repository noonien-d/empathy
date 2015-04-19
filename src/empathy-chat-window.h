/*
 * Copyright (C) 2003-2007 Imendio AB
 * Copyright (C) 2007-2012 Collabora Ltd.
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
 * Authors: Mikael Hallendal <micke@imendio.com>
 *          Richard Hult <richard@imendio.com>
 *          Martyn Russell <martyn@imendio.com>
 *          Geert-Jan Van den Bogaerde <geertjan@gnome.org>
 *          Xavier Claessens <xclaesse@gmail.com>
 */

#ifndef __EMPATHY_CHAT_WINDOW_H__
#define __EMPATHY_CHAT_WINDOW_H__

#include <gtk/gtk.h>
#include <telepathy-glib/telepathy-glib.h>

#include "empathy-chat.h"
#include "empathy-individual-manager.h"

G_BEGIN_DECLS

#define EMPATHY_TYPE_CHAT_WINDOW \
  (empathy_chat_window_get_type ())
#define EMPATHY_CHAT_WINDOW(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), \
    EMPATHY_TYPE_CHAT_WINDOW, \
    EmpathyChatWindow))
#define EMPATHY_CHAT_WINDOW_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), \
    EMPATHY_TYPE_CHAT_WINDOW, \
    EmpathyChatWindowClass))
#define EMPATHY_IS_CHAT_WINDOW(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), \
    EMPATHY_TYPE_CHAT_WINDOW))
#define EMPATHY_IS_CHAT_WINDOW_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), \
    EMPATHY_TYPE_CHAT_WINDOW))
#define EMPATHY_CHAT_WINDOW_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
    EMPATHY_TYPE_CHAT_WINDOW, \
    EmpathyChatWindowClass))


typedef struct _EmpathyChatWindow EmpathyChatWindow;
typedef struct _EmpathyChatWindowClass EmpathyChatWindowClass;
typedef struct _EmpathyChatWindowPriv EmpathyChatWindowPriv;

struct _EmpathyChatWindow
{
  GtkBin parent;
  EmpathyChatWindowPriv *priv;
};

struct _EmpathyChatWindowClass
{
  GtkBinClass parent_class;
};

GType empathy_chat_window_get_type (void);

EmpathyChat * empathy_chat_window_find_chat (TpAccount *account,
    const gchar *id,
    gboolean sms_channel);

EmpathyChatWindow * empathy_chat_window_present_chat (EmpathyChat *chat,
    gint64 timestamp);

EmpathyChatWindow * empathy_chat_window_new (void);

EmpathyIndividualManager * empathy_chat_window_get_individual_manager (
    EmpathyChatWindow *self);

void empathy_chat_window_next_tab (EmpathyChatWindow *self);
void empathy_chat_window_prev_tab (EmpathyChatWindow *self);

G_END_DECLS

#endif
