/* # Generated using empathy/ubuntu-online-accounts/cc-plugins/generate-plugins.py
 * Do NOT edit manually */

/*
 * empathy-accounts-plugin-local-xmpp.c
 *
 * Copyright (C) 2012 Collabora Ltd. <http://www.collabora.co.uk/>
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

#include "empathy-accounts-plugin-local-xmpp.h"

G_DEFINE_TYPE (EmpathyAccountsPluginLocalXmpp, empathy_accounts_plugin_local_xmpp,\
        EMPATHY_TYPE_ACCOUNTS_PLUGIN)

static void
empathy_accounts_plugin_local_xmpp_class_init (
    EmpathyAccountsPluginLocalXmppClass *klass)
{
}

static void
empathy_accounts_plugin_local_xmpp_init (EmpathyAccountsPluginLocalXmpp *self)
{
}

GType
ap_module_get_object_type (void)
{
  return EMPATHY_TYPE_ACCOUNTS_PLUGIN_LOCAL_XMPP;
}