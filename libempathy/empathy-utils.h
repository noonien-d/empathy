/*
 * Copyright (C) 2003-2007 Imendio AB
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
 * Authors: Richard Hult <richard@imendio.com>
 *          Martyn Russell <martyn@imendio.com>
 *          Xavier Claessens <xclaesse@gmail.com>
 */

#ifndef __EMPATHY_UTILS_H__
#define __EMPATHY_UTILS_H__

#include <glib.h>
#include <glib-object.h>
#include <libxml/tree.h>
#include <folks/folks.h>
#include <folks/folks-telepathy.h>
#include <telepathy-glib/telepathy-glib.h>

#include "empathy-contact.h"

#define EMPATHY_GET_PRIV(obj,type) ((type##Priv *) ((type *) obj)->priv)

G_BEGIN_DECLS

void empathy_init (void);

/* XML */
xmlNodePtr empathy_xml_node_get_child (xmlNodePtr node,
    const gchar *child_name);
xmlChar * empathy_xml_node_get_child_content (xmlNodePtr node,
    const gchar *child_name);
xmlNodePtr empathy_xml_node_find_child_prop_value (xmlNodePtr node,
    const gchar *prop_name,
    const gchar *prop_value);

/* Others */
const gchar * empathy_presence_get_default_message (
    TpConnectionPresenceType presence);
const gchar * empathy_presence_to_str (TpConnectionPresenceType presence);
TpConnectionPresenceType empathy_presence_from_str (const gchar *str);
gchar * empathy_file_lookup (const gchar *filename,
    const gchar *subdir);
gint empathy_uint_compare (gconstpointer a,
    gconstpointer b);

const gchar * empathy_account_get_error_message (TpAccount *account,
    gboolean *user_requested);

#define EMPATHY_ARRAY_TYPE_OBJECT (empathy_type_dbus_ao ())
GType empathy_type_dbus_ao (void);

gboolean empathy_account_manager_get_accounts_connected (gboolean *connecting);

TpConnectionPresenceType empathy_folks_presence_type_to_tp (
    FolksPresenceType type);
gboolean empathy_folks_individual_contains_contact (
    FolksIndividual *individual);
EmpathyContact * empathy_contact_dup_from_folks_individual (
    FolksIndividual *individual);
TpChannelGroupChangeReason tp_channel_group_change_reason_from_folks_groups_change_reason (
    FolksGroupDetailsChangeReason reason);
TpfPersonaStore * empathy_dup_persona_store_for_connection (
    TpConnection *connection);
gboolean empathy_connection_can_add_personas (TpConnection *connection);
gboolean empathy_connection_can_alias_personas (TpConnection *connection,
						FolksIndividual *individual);
gboolean empathy_connection_can_group_personas (TpConnection *connection,
						FolksIndividual *individual);
gboolean empathy_folks_persona_is_interesting (FolksPersona *persona);

gchar *empathy_format_currency (gint amount,
    guint scale,
    const gchar *currency);

TpContact * empathy_get_tp_contact_for_individual (FolksIndividual *individual,
    TpConnection *conn);

void empathy_individual_can_audio_video_call (FolksIndividual *individual,
    gboolean *can_audio_call,
    gboolean *can_video_call,
    EmpathyContact **out_contact);

gboolean empathy_client_types_contains_mobile_device (
    const GStrv types);

FolksIndividual * empathy_ensure_individual_from_tp_contact (
    TpContact *contact);

const gchar * const * empathy_individual_get_client_types (
    FolksIndividual *individual);

GVariant * empathy_asv_to_vardict (const GHashTable *asv);

GVariant * empathy_boxed_to_variant (GType gtype,
    const gchar *variant_type,
    gpointer boxed);

G_END_DECLS

#endif /*  __EMPATHY_UTILS_H__ */
