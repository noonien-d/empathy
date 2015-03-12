/*
 * mcp-account-manager-goa.c
 *
 * McpAccountManagerGoa - a Mission Control plugin to expose GNOME Online
 * Accounts with chat capabilities (e.g. Facebook) to Mission Control
 *
 * Copyright (C) 2010-2014 Collabora Ltd.
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
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *    Danielle Madeley <danielle.madeley@collabora.co.uk>
 */

#include "config.h"
#include "mcp-account-manager-goa.h"

#define GOA_API_IS_SUBJECT_TO_CHANGE /* awesome! */
#include <goa/goa.h>

#define DEBUG g_debug
#define GET_PRIVATE(self) (((McpAccountManagerGoa *) self)->priv)
#define DECLARE_GASYNC_CALLBACK(name) \
  static void name (GObject *, GAsyncResult *, gpointer);

#define PLUGIN_NAME "goa"
#define PLUGIN_DESCRIPTION "Provide Telepathy Accounts from GOA"
#define PLUGIN_PROVIDER EMPATHY_GOA_PROVIDER

#define INITIAL_COMMENT "Parameters of GOA Telepathy accounts"

#ifdef MCP_API_VERSION_5_18

# define RESTRICTIONS TpStorageRestrictionFlags
# define WAS_CONST /* nothing */
  /* Its historical value was based on the KEYRING priority which no longer
   * exists. Using a large number is OK, because it uses unusual account names
   * which are unlikely to collide. */
# define PLUGIN_PRIORITY (10010)
  /* McpAccountStorageSetResult enum is defined by MC */

#else /* MC 5.16 or older */

# define RESTRICTIONS guint
# define WAS_CONST const
# define PLUGIN_PRIORITY (MCP_ACCOUNT_STORAGE_PLUGIN_PRIO_KEYRING + 10)

  /* we use this in helper functions */
  typedef enum {
      MCP_ACCOUNT_STORAGE_SET_RESULT_FAILED,
      MCP_ACCOUNT_STORAGE_SET_RESULT_UNCHANGED,
      MCP_ACCOUNT_STORAGE_SET_RESULT_CHANGED
  } McpAccountStorageSetResult;

#endif /* MC 5.16 or older */

static void account_storage_iface_init (McpAccountStorageIface *iface);

G_DEFINE_TYPE_WITH_CODE (McpAccountManagerGoa,
    mcp_account_manager_goa,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (MCP_TYPE_ACCOUNT_STORAGE,
      account_storage_iface_init))

struct _McpAccountManagerGoaPrivate
{
  gboolean ready;

  GoaClient *client;
  GHashTable *accounts; /* alloc'ed string -> ref'ed GoaObject */

  GKeyFile *store;
  gchar *filename;
};


static void
mcp_account_manager_goa_dispose (GObject *self)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (self);

  tp_clear_object (&priv->client);

  G_OBJECT_CLASS (mcp_account_manager_goa_parent_class)->dispose (self);
}


static void
mcp_account_manager_goa_finalize (GObject *self)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (self);

  g_hash_table_unref (priv->accounts);
  g_key_file_free (priv->store);
  g_free (priv->filename);

  G_OBJECT_CLASS (mcp_account_manager_goa_parent_class)->finalize (self);
}


static void
mcp_account_manager_goa_class_init (McpAccountManagerGoaClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = mcp_account_manager_goa_dispose;
  gobject_class->finalize = mcp_account_manager_goa_finalize;

  g_type_class_add_private (gobject_class,
      sizeof (McpAccountManagerGoaPrivate));
}

static GHashTable *
get_tp_parameters (GoaAccount *account)
{
  GHashTable *params = g_hash_table_new_full (g_str_hash, g_str_equal,
      NULL, g_free);
  const char *type = goa_account_get_provider_type (account);

#define PARAM(key, value) g_hash_table_insert (params, key, g_strdup (value));

  if (!tp_strdiff (type, "google"))
    {
      PARAM ("manager", "gabble");
      PARAM ("protocol", "jabber");
      PARAM ("Icon", "im-google-talk");
      PARAM ("Service", "google-talk");

      PARAM ("param-account", goa_account_get_identity (account));
      PARAM ("param-server", "talk.google.com");
      PARAM ("param-fallback-servers",
          "talkx.l.google.com;"
          "talkx.l.google.com:443,oldssl;"
          "talkx.l.google.com:80");
      PARAM ("param-extra-certificate-identities", "talk.google.com");
      PARAM ("param-require-encryption", "true");
    }
  else
    {
      DEBUG ("Unknown account type %s", type);
      g_hash_table_unref (params);
      return NULL;
    }

  /* generic properties */
  PARAM ("DisplayName", goa_account_get_presentation_identity (account));

#undef PARAM

  return params;
}


static char *
get_tp_account_name (GoaAccount *account)
{
  GHashTable *params = get_tp_parameters (account);
  const char *type = goa_account_get_provider_type (account);
  const char *id = goa_account_get_id (account);
  char *name;

  if (params == NULL)
    return NULL;

  name = g_strdup_printf ("%s/%s/goa_%s_%s",
      (char *) g_hash_table_lookup (params, "manager"),
      (char *) g_hash_table_lookup (params, "protocol"),
      type, id);

  g_hash_table_unref (params);

  return name;
}

static void
object_chat_changed_cb (GoaObject *object,
    GParamSpec *spec,
    McpAccountManagerGoa *self)
{
  GoaAccount *account = goa_object_peek_account (object);
  char *name = get_tp_account_name (account);
  gboolean enabled;

  if (name == NULL)
    return;

  enabled = (goa_object_peek_chat (object) != NULL);

  DEBUG ("%s %s", name, enabled ? "enabled" : "disabled");

  if (self->priv->ready)
    mcp_account_storage_emit_toggled (MCP_ACCOUNT_STORAGE (self),
        name, enabled);
}

static void
_new_account (McpAccountManagerGoa *self,
    GoaObject *object)
{
  GoaAccount *account = goa_object_peek_account (object);
  char *account_name = get_tp_account_name (account);

  if (account_name == NULL)
    return;

  /* @account_name now is owned by the hash table */
  g_hash_table_insert (self->priv->accounts, account_name,
      g_object_ref (object));

  if (self->priv->ready)
    mcp_account_storage_emit_created (MCP_ACCOUNT_STORAGE (self),
        account_name);

  tp_g_signal_connect_object (object, "notify::chat",
      G_CALLBACK (object_chat_changed_cb), self, 0);
}


DECLARE_GASYNC_CALLBACK (_goa_client_new_cb);

static void
load_store (McpAccountManagerGoa *self)
{
  GError *error = NULL;

  if (!g_key_file_load_from_file (self->priv->store, self->priv->filename,
        G_KEY_FILE_KEEP_COMMENTS, &error))
    {
      gchar *dir;

      DEBUG ("Failed to load keyfile, creating a new one: %s", error->message);

      dir = g_path_get_dirname (self->priv->filename);

      g_mkdir_with_parents (dir, 0700);
      g_free (dir);

      g_key_file_set_comment (self->priv->store, NULL, NULL, INITIAL_COMMENT,
          NULL);

      g_error_free (error);
    }
}

static void
mcp_account_manager_goa_init (McpAccountManagerGoa *self)
{
  DEBUG ("GOA MC plugin initialised");

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      MCP_TYPE_ACCOUNT_MANAGER_GOA, McpAccountManagerGoaPrivate);

#ifdef MCP_API_VERSION_5_18
  /* the ready callback no longer exists, we may emit signals at any time */
  self->priv->ready = TRUE;
#endif

  self->priv->accounts = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, g_object_unref);

  goa_client_new (NULL, _goa_client_new_cb, self);

  /* key file store */
  self->priv->store = g_key_file_new ();
  self->priv->filename = g_build_filename (g_get_user_data_dir (), "telepathy",
      "mission-control", "accounts-goa.cfg", NULL);

  load_store (self);
}


static void
_account_added_cb (GoaClient *client,
    GoaObject *object,
    McpAccountManagerGoa *self)
{
  _new_account (self, object);
}


static void
_account_removed_cb (GoaClient *client,
    GoaObject *object,
    McpAccountManagerGoa *self)
{
  GoaAccount *account = goa_object_peek_account (object);
  char *name = get_tp_account_name (account);

  if (name == NULL)
    return;

  if (self->priv->ready)
    mcp_account_storage_emit_deleted (MCP_ACCOUNT_STORAGE (self), name);

  g_hash_table_remove (self->priv->accounts, name);

  g_free (name);
}

static void
_goa_client_new_cb (GObject *obj,
    GAsyncResult *result,
    gpointer user_data)
{
  McpAccountManagerGoa *self = user_data;
  GList *accounts, *ptr;
  GError *error = NULL;

  self->priv->client = goa_client_new_finish (result, &error);

  if (error != NULL)
    {
      DEBUG ("Failed to connect to GOA");
      return;
    }

  accounts = goa_client_get_accounts (self->priv->client);

  for (ptr = accounts; ptr != NULL; ptr = ptr->next)
    {
      _new_account (self, ptr->data);
    }

  g_list_free_full (accounts, g_object_unref);

  g_signal_connect (self->priv->client, "account-added",
      G_CALLBACK (_account_added_cb), self);
  g_signal_connect (self->priv->client, "account-removed",
      G_CALLBACK (_account_removed_cb), self);
}


static GList *
mcp_account_manager_goa_list (WAS_CONST McpAccountStorage *self,
    WAS_CONST McpAccountManager *am)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (self);
  GList *accounts = NULL;
  GHashTableIter iter;
  gpointer key;

  DEBUG (G_STRFUNC);

  g_hash_table_iter_init (&iter, priv->accounts);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    accounts = g_list_prepend (accounts, g_strdup (key));

  return accounts;
}


#ifdef MCP_API_VERSION_5_18

static GVariant *
get_item (McpAccountManagerGoa *self,
    McpAccountManager *am,
    const gchar *acc,
    const gchar *key,
    const GVariantType *type)
{
  GoaObject *object;
  GoaAccount *account;
  GHashTable *bits;
  gchar *esc;
  GVariant *ret = NULL;

  DEBUG ("%s: %s, %s", G_STRFUNC, acc, key);

  object = g_hash_table_lookup (self->priv->accounts, acc);

  g_return_val_if_fail (object != NULL, NULL);

  account = goa_object_peek_account (object);

  g_return_val_if_fail (account != NULL, NULL);

  if (!tp_strdiff (key, "Enabled"))
    {
      return g_variant_ref_sink (g_variant_new_boolean (
            !goa_account_get_chat_disabled (account)));
    }

  bits = get_tp_parameters (account);

  esc = g_hash_table_lookup (bits, key);

  if (esc == NULL)
    esc = g_key_file_get_value (self->priv->store, acc, key, NULL);
  else
    esc = g_strdup (esc);

  if (esc != NULL)
    {
      ret = mcp_account_manager_unescape_variant_from_keyfile (am,
          esc, type, NULL);

      g_free (esc);
    }

  g_hash_table_unref (bits);
  return ret;
}

static GVariant *
mcp_account_manager_goa_get_attribute (McpAccountStorage *self,
    McpAccountManager *am,
    const gchar *acc,
    const gchar *attribute,
    const GVariantType *type,
    McpAttributeFlags *flags)
{
  return get_item (MCP_ACCOUNT_MANAGER_GOA (self), am, acc, attribute, type);
}

static GVariant *
mcp_account_manager_goa_get_parameter (McpAccountStorage *storage,
    McpAccountManager *am,
    const gchar *acc,
    const gchar *parameter,
    const GVariantType *type,
    McpParameterFlags *flags)
{
  gchar *key;
  GVariant *ret;

  key = g_strdup_printf ("param-%s", parameter);

  ret = get_item (MCP_ACCOUNT_MANAGER_GOA (storage), am, acc, key, type);
  g_free (key);
  return ret;
}

#else /* MC 5.16 or older */

static void
get_enabled (WAS_CONST McpAccountStorage *self,
    WAS_CONST McpAccountManager *am,
    const gchar *acc,
    GoaAccount *account)
{
  mcp_account_manager_set_value (am, acc, "Enabled",
      goa_account_get_chat_disabled (account) == FALSE ? "true" : "false");
}

static gboolean
mcp_account_manager_goa_get (WAS_CONST McpAccountStorage *self,
    WAS_CONST McpAccountManager *am,
    const gchar *acc,
    const gchar *key)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (self);
  GoaObject *object;
  GoaAccount *account;

  DEBUG ("%s: %s, %s", G_STRFUNC, acc, key);

  object = g_hash_table_lookup (priv->accounts, acc);

  if (object == NULL)
    return FALSE;

  account = goa_object_peek_account (object);

  if (account == NULL)
    return FALSE;

  if (key == NULL)
    {
      /* load all keys */
      GHashTable *params = get_tp_parameters (account);
      GHashTableIter iter;
      gpointer k, value;
      GStrv keys;
      guint i;
      gsize nkeys = 0;

      /* Properties from GOA */
      g_hash_table_iter_init (&iter, params);
      while (g_hash_table_iter_next (&iter, &k, &value))
        mcp_account_manager_set_value (am, acc, k, value);

      g_hash_table_unref (params);

      /* Stored properties */
      keys = g_key_file_get_keys (priv->store, acc, &nkeys, NULL);

      for (i = 0; i < nkeys; i++)
        {
          gchar *v = g_key_file_get_value (priv->store, acc, keys[i], NULL);

          if (v != NULL)
            {
              mcp_account_manager_set_value (am, acc, keys[i], v);
              g_free (v);
            }
        }

      g_strfreev (keys);

      /* Enabled */
      get_enabled (self, am, acc, account);
    }
  else if (!tp_strdiff (key, "Enabled"))
    {
      get_enabled (self, am, acc, account);
    }
  else
    {
      /* get a specific key */
      GHashTable *params = get_tp_parameters (account);
      gchar *value;

      value = g_hash_table_lookup (params, key);

      if (value == NULL)
        value = g_key_file_get_value (priv->store, acc, key, NULL);
      else
        value = g_strdup (value);

      mcp_account_manager_set_value (am, acc, key, value);

      g_hash_table_unref (params);
      g_free (value);
    }

  return TRUE;
}

#endif /* MC 5.16 or older */


static gboolean
account_is_in_goa (WAS_CONST McpAccountStorage *self,
    const gchar *account)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (self);

  return (g_hash_table_lookup (priv->accounts, account) != NULL);
}

static McpAccountStorageSetResult
mcp_account_manager_goa_set_enabled (McpAccountStorage *self,
    const gchar *account,
    gboolean enabled)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (self);
  GoaObject *object;
  GoaAccount *acc;

  object = g_hash_table_lookup (priv->accounts, account);

  if (object == NULL)
    return MCP_ACCOUNT_STORAGE_SET_RESULT_FAILED;

  acc = goa_object_peek_account (object);

  if (acc == NULL)
    return MCP_ACCOUNT_STORAGE_SET_RESULT_FAILED;

  if (goa_account_get_chat_disabled (acc) == !enabled)
    return MCP_ACCOUNT_STORAGE_SET_RESULT_UNCHANGED;

  goa_account_set_chat_disabled (acc, !enabled);
  return MCP_ACCOUNT_STORAGE_SET_RESULT_CHANGED;
}

#ifdef MCP_API_VERSION_5_18

static McpAccountStorageSetResult
mcp_account_manager_goa_set_attribute (McpAccountStorage *storage,
    McpAccountManager *am,
    const gchar *account_name,
    const gchar *attribute,
    GVariant *value,
    McpAttributeFlags flags)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (storage);
  McpAccountStorageSetResult ret = MCP_ACCOUNT_STORAGE_SET_RESULT_FAILED;

  g_return_val_if_fail (account_is_in_goa (storage, account_name),
      MCP_ACCOUNT_STORAGE_SET_RESULT_FAILED);

  if (!tp_strdiff (attribute, "Enabled"))
    {
      g_return_val_if_fail (
          g_variant_classify (value) == G_VARIANT_CLASS_BOOLEAN,
          MCP_ACCOUNT_STORAGE_SET_RESULT_FAILED);

      return mcp_account_manager_goa_set_enabled (storage, account_name,
            g_variant_get_boolean (value));
    }
  /* FIXME: filter out manager, protocol, Icon, Service? */
  else if (value != NULL)
    {
      gchar *esc = mcp_account_manager_escape_variant_for_keyfile (am,
          value);
      gchar *old;

      if (esc == NULL)
        return MCP_ACCOUNT_STORAGE_SET_RESULT_FAILED;

      old = g_key_file_get_value (priv->store, account_name, attribute, NULL);

      if (tp_strdiff (old, esc))
        {
          g_key_file_set_value (priv->store, account_name, attribute, esc);
          ret = MCP_ACCOUNT_STORAGE_SET_RESULT_CHANGED;
        }
      else
        {
          ret = MCP_ACCOUNT_STORAGE_SET_RESULT_UNCHANGED;
        }

      g_free (esc);
      g_free (old);
    }
  else
    {
      if (g_key_file_has_key (priv->store, account_name, attribute, NULL))
        {
          g_key_file_remove_key (priv->store, account_name, attribute, NULL);
          ret = MCP_ACCOUNT_STORAGE_SET_RESULT_CHANGED;
        }
      else
        {
          ret = MCP_ACCOUNT_STORAGE_SET_RESULT_UNCHANGED;
        }
    }

  return ret;
}

static McpAccountStorageSetResult
mcp_account_manager_goa_set_parameter (McpAccountStorage *storage,
    McpAccountManager *am,
    const gchar *account_name,
    const gchar *parameter,
    GVariant *value,
    McpParameterFlags flags)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (storage);
  McpAccountStorageSetResult ret = MCP_ACCOUNT_STORAGE_SET_RESULT_FAILED;
  gchar *key = NULL;
  gchar *esc = NULL;
  gchar *old = NULL;

  g_return_val_if_fail (account_is_in_goa (storage, account_name),
      MCP_ACCOUNT_STORAGE_SET_RESULT_FAILED);

  key = g_strdup_printf ("param-%s", parameter);

  /* FIXME: filter out reserved keys for this account? */
  if (value != NULL)
    {
      esc = mcp_account_manager_escape_variant_for_keyfile (am,
          value);

      if (esc == NULL)
        goto out;

      old = g_key_file_get_value (priv->store, account_name, key, NULL);

      if (tp_strdiff (esc, old))
        {
          g_key_file_set_value (priv->store, account_name, key, esc);
          ret = MCP_ACCOUNT_STORAGE_SET_RESULT_CHANGED;
        }
      else
        {
          ret = MCP_ACCOUNT_STORAGE_SET_RESULT_UNCHANGED;
        }
    }
  else
    {
      if (g_key_file_has_key (priv->store, account_name, key, NULL))
        {
          g_key_file_remove_key (priv->store, account_name, key, NULL);
          ret = MCP_ACCOUNT_STORAGE_SET_RESULT_CHANGED;
        }
      else
        {
          ret = MCP_ACCOUNT_STORAGE_SET_RESULT_UNCHANGED;
        }

    }

out:
  g_free (key);
  g_free (esc);
  g_free (old);
  return ret;
}

#else /* MC 5.16 or older */

static gboolean
mcp_account_manager_goa_set (const McpAccountStorage *self,
    const McpAccountManager *am,
    const gchar *account,
    const gchar *key,
    const gchar *val)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (self);

  if (!account_is_in_goa (self, account))
    return FALSE;

  DEBUG ("%s: (%s, %s, %s)", G_STRFUNC, account, key, val);

  if (!tp_strdiff (key, "Enabled"))
    {
      if (mcp_account_manager_goa_set_enabled ((McpAccountStorage *) self,
          account,
          !tp_strdiff (val, "true")) != MCP_ACCOUNT_STORAGE_SET_RESULT_FAILED)
        return TRUE;
      else
        return FALSE;
    }

  if (val != NULL)
    g_key_file_set_value (priv->store, account, key, val);
  else
    g_key_file_remove_key (priv->store, account, key, NULL);

  /* Pretend we save everything so MC won't save this in accounts.cfg */
  return TRUE;
}

#endif /* MC 5.16 or older */


static gboolean
mcp_account_manager_goa_delete (WAS_CONST McpAccountStorage *self,
    WAS_CONST McpAccountManager *am,
    const gchar *account,
    const gchar *key)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (self);

  if (!account_is_in_goa (self, account))
    return FALSE;

  DEBUG ("%s: (%s, %s)", G_STRFUNC, account, key);

  if (key == NULL)
    {
      g_key_file_remove_group (priv->store, account, NULL);
    }
  else
    {
      g_key_file_remove_key (priv->store, account, key, NULL);
    }

  /* Pretend we deleted everything */
  return TRUE;
}


#ifdef MCP_API_VERSION_5_18
static void
mcp_account_manager_goa_delete_async (McpAccountStorage *self,
    McpAccountManager *am,
    const gchar *account_name,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GTask *task = g_task_new (self, cancellable, callback, user_data);

  if (mcp_account_manager_goa_delete (self, am, account_name, NULL))
    {
      g_task_return_boolean (task, TRUE);
    }
  else
    {
      g_task_return_new_error (task, TP_ERROR, TP_ERROR_DOES_NOT_EXIST,
          "Account does not exist in GOA");
    }

  g_object_unref (task);
}

static gboolean
mcp_account_manager_goa_delete_finish (McpAccountStorage *self,
    GAsyncResult *res,
    GError **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}
#endif /* MC >= 5.18 API */

static gboolean
commit (McpAccountManagerGoa *self)
{
  McpAccountManagerGoaPrivate *priv = self->priv;
  gchar *data;
  gsize len;
  GError *error = NULL;

  DEBUG ("Save config to %s", priv->filename);

  data = g_key_file_to_data (priv->store, &len, &error);
  if (data == NULL)
    {
      DEBUG ("Failed to get data from store: %s", error->message);

      g_error_free (error);
      return FALSE;
    }

  if (!g_file_set_contents (priv->filename, data, len, &error))
    {
      DEBUG ("Failed to write file: %s", error->message);

      g_free (data);
      g_error_free (error);
      return FALSE;
    }

  g_free (data);

  return TRUE;
}

#ifdef MCP_API_VERSION_5_18
static gboolean
mcp_account_manager_goa_commit (McpAccountStorage *self,
  McpAccountManager *am,
  const gchar *account)
{
  return commit (MCP_ACCOUNT_MANAGER_GOA (self));
}
#else
static gboolean
mcp_account_manager_goa_commit (const McpAccountStorage *self,
  const McpAccountManager *am)
{
  return commit (MCP_ACCOUNT_MANAGER_GOA (self));
}
#endif


#ifndef MCP_API_VERSION_5_18
/* removed in 5.18, MC should now be ready to receive signals at any time */
static void
mcp_account_manager_goa_ready (WAS_CONST McpAccountStorage *self,
    WAS_CONST McpAccountManager *am)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (self);

  priv->ready = TRUE;
}
#endif


static RESTRICTIONS
mcp_account_manager_goa_get_restrictions (WAS_CONST McpAccountStorage *self,
    const gchar *account)
{
  return TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_PARAMETERS |
         TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_SERVICE;
}


static void
mcp_account_manager_goa_get_identifier (WAS_CONST McpAccountStorage *self,
    const gchar *acc,
    GValue *identifier)
{
  McpAccountManagerGoaPrivate *priv = GET_PRIVATE (self);
  GoaObject *object;
  GoaAccount *account;

  object = g_hash_table_lookup (priv->accounts, acc);
  g_return_if_fail (object != NULL);

  account = goa_object_peek_account (object);
  g_return_if_fail (account != NULL);

  g_value_init (identifier, G_TYPE_STRING);
  g_value_set_string (identifier, goa_account_get_id (account));
}


static void
account_storage_iface_init (McpAccountStorageIface *iface)
{
  iface->name = PLUGIN_NAME;
  iface->desc = PLUGIN_DESCRIPTION;
  iface->priority = PLUGIN_PRIORITY;
  iface->provider = PLUGIN_PROVIDER;

#define IMPLEMENT(x) iface->x = mcp_account_manager_goa_##x

  IMPLEMENT (list);
  IMPLEMENT (commit);
  IMPLEMENT (get_restrictions);
  IMPLEMENT (get_identifier);

#ifdef MCP_API_VERSION_5_18
  IMPLEMENT (delete_async);
  IMPLEMENT (delete_finish);
  IMPLEMENT (get_attribute);
  IMPLEMENT (get_parameter);
  IMPLEMENT (set_attribute);
  IMPLEMENT (set_parameter);
#else
  IMPLEMENT (get);
  IMPLEMENT (set);
  IMPLEMENT (delete);
  IMPLEMENT (ready);
#endif

#undef IMPLEMENT
}
