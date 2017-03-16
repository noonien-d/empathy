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

#include "config.h"
#include "empathy-utils.h"

#include <glib/gi18n-lib.h>
#include <dbus/dbus-protocol.h>
#include <math.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-client-factory.h"
#include "extensions.h"

#include <math.h>

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

/* Translation between presence types and string */
static struct {
  const gchar *name;
  TpConnectionPresenceType type;
} presence_types[] = {
  { "available", TP_CONNECTION_PRESENCE_TYPE_AVAILABLE },
  { "busy",      TP_CONNECTION_PRESENCE_TYPE_BUSY },
  { "away",      TP_CONNECTION_PRESENCE_TYPE_AWAY },
  { "ext_away",  TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY },
  { "hidden",    TP_CONNECTION_PRESENCE_TYPE_HIDDEN },
  { "offline",   TP_CONNECTION_PRESENCE_TYPE_OFFLINE },
  { "unset",     TP_CONNECTION_PRESENCE_TYPE_UNSET },
  { "unknown",   TP_CONNECTION_PRESENCE_TYPE_UNKNOWN },
  { "error",     TP_CONNECTION_PRESENCE_TYPE_ERROR },
  /* alternative names */
  { "dnd",      TP_CONNECTION_PRESENCE_TYPE_BUSY },
  { "brb",      TP_CONNECTION_PRESENCE_TYPE_AWAY },
  { "xa",       TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY },
  { NULL, },
};

static gboolean
properties_contains (gchar **list,
		     gint length,
		     const gchar *property);

static gboolean
check_writeable_property (TpConnection *connection,
			  FolksIndividual *individual,
			  gchar *property);

void
empathy_init (void)
{
  static gboolean initialized = FALSE;
  TpAccountManager *am;
  EmpathyClientFactory *factory;

  if (initialized)
    return;

  g_type_init ();

  /* Setup gettext */
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  /* Setup debug output for empathy and telepathy-glib */
  if (g_getenv ("EMPATHY_TIMING") != NULL)
    g_log_set_default_handler (tp_debug_timestamped_log_handler, NULL);

  empathy_debug_set_flags (g_getenv ("EMPATHY_DEBUG"));
  tp_debug_divert_messages (g_getenv ("EMPATHY_LOGFILE"));

  emp_cli_init ();

  initialized = TRUE;

  factory = empathy_client_factory_dup ();
  am = tp_account_manager_new_with_factory (TP_SIMPLE_CLIENT_FACTORY (factory));
  tp_account_manager_set_default (am);

  g_object_unref (factory);
  g_object_unref (am);
}

xmlNodePtr
empathy_xml_node_get_child (xmlNodePtr   node,
    const gchar *child_name)
{
  xmlNodePtr l;

  g_return_val_if_fail (node != NULL, NULL);
  g_return_val_if_fail (child_name != NULL, NULL);

  for (l = node->children; l; l = l->next)
    {
      if (l->name && strcmp ((const gchar *) l->name, child_name) == 0)
        return l;
    }

  return NULL;
}

xmlChar *
empathy_xml_node_get_child_content (xmlNodePtr   node,
    const gchar *child_name)
{
  xmlNodePtr l;

  g_return_val_if_fail (node != NULL, NULL);
  g_return_val_if_fail (child_name != NULL, NULL);

  l = empathy_xml_node_get_child (node, child_name);
  if (l != NULL)
    return xmlNodeGetContent (l);

  return NULL;
}

xmlNodePtr
empathy_xml_node_find_child_prop_value (xmlNodePtr   node,
    const gchar *prop_name,
    const gchar *prop_value)
{
  xmlNodePtr l;
  xmlNodePtr found = NULL;

  g_return_val_if_fail (node != NULL, NULL);
  g_return_val_if_fail (prop_name != NULL, NULL);
  g_return_val_if_fail (prop_value != NULL, NULL);

  for (l = node->children; l && !found; l = l->next)
    {
      xmlChar *prop;

      if (!xmlHasProp (l, (const xmlChar *) prop_name))
        continue;

      prop = xmlGetProp (l, (const xmlChar *) prop_name);
      if (prop && strcmp ((const gchar *) prop, prop_value) == 0)
        found = l;

      xmlFree (prop);
    }

  return found;
}

const gchar *
empathy_presence_get_default_message (TpConnectionPresenceType presence)
{
  switch (presence)
    {
      case TP_CONNECTION_PRESENCE_TYPE_AVAILABLE:
        return _("Available");
      case TP_CONNECTION_PRESENCE_TYPE_BUSY:
        return _("Busy");
      case TP_CONNECTION_PRESENCE_TYPE_AWAY:
      case TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY:
        return _("Away");
      case TP_CONNECTION_PRESENCE_TYPE_HIDDEN:
        return _("Invisible");
      case TP_CONNECTION_PRESENCE_TYPE_OFFLINE:
        return _("Offline");
      case TP_CONNECTION_PRESENCE_TYPE_UNKNOWN:
        /* translators: presence type is unknown */
        return C_("presence", "Unknown");
      case TP_CONNECTION_PRESENCE_TYPE_UNSET:
      case TP_CONNECTION_PRESENCE_TYPE_ERROR:
      default:
        return NULL;
    }

  return NULL;
}

const gchar *
empathy_presence_to_str (TpConnectionPresenceType presence)
{
  int i;

  for (i = 0 ; presence_types[i].name != NULL; i++)
    if (presence == presence_types[i].type)
      return presence_types[i].name;

  return NULL;
}

TpConnectionPresenceType
empathy_presence_from_str (const gchar *str)
{
  int i;

  for (i = 0 ; presence_types[i].name != NULL; i++)
    if (!tp_strdiff (str, presence_types[i].name))
      return presence_types[i].type;

  return TP_CONNECTION_PRESENCE_TYPE_UNSET;
}

static const gchar *
empathy_status_reason_get_default_message (TpConnectionStatusReason reason)
{
  switch (reason)
    {
      case TP_CONNECTION_STATUS_REASON_NONE_SPECIFIED:
        return _("No reason specified");
      case TP_CONNECTION_STATUS_REASON_REQUESTED:
        return _("Status is set to offline");
      case TP_CONNECTION_STATUS_REASON_NETWORK_ERROR:
        return _("Network error");
      case TP_CONNECTION_STATUS_REASON_AUTHENTICATION_FAILED:
        return _("Authentication failed");
      case TP_CONNECTION_STATUS_REASON_ENCRYPTION_ERROR:
        return _("Encryption error");
      case TP_CONNECTION_STATUS_REASON_NAME_IN_USE:
        return _("Name in use");
      case TP_CONNECTION_STATUS_REASON_CERT_NOT_PROVIDED:
        return _("Certificate not provided");
      case TP_CONNECTION_STATUS_REASON_CERT_UNTRUSTED:
        return _("Certificate untrusted");
      case TP_CONNECTION_STATUS_REASON_CERT_EXPIRED:
        return _("Certificate expired");
      case TP_CONNECTION_STATUS_REASON_CERT_NOT_ACTIVATED:
        return _("Certificate not activated");
      case TP_CONNECTION_STATUS_REASON_CERT_HOSTNAME_MISMATCH:
        return _("Certificate hostname mismatch");
      case TP_CONNECTION_STATUS_REASON_CERT_FINGERPRINT_MISMATCH:
        return _("Certificate fingerprint mismatch");
      case TP_CONNECTION_STATUS_REASON_CERT_SELF_SIGNED:
        return _("Certificate self-signed");
      case TP_CONNECTION_STATUS_REASON_CERT_OTHER_ERROR:
        return _("Certificate error");
      default:
        return _("Unknown reason");
    }
}

static GHashTable *
create_errors_to_message_hash (void)
{
  GHashTable *errors;

  errors = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (errors, TP_ERROR_STR_NETWORK_ERROR, _("Network error"));
  g_hash_table_insert (errors, TP_ERROR_STR_AUTHENTICATION_FAILED,
    _("Authentication failed"));
  g_hash_table_insert (errors, TP_ERROR_STR_ENCRYPTION_ERROR,
    _("Encryption error"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_NOT_PROVIDED,
    _("Certificate not provided"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_UNTRUSTED,
    _("Certificate untrusted"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_EXPIRED,
    _("Certificate expired"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_NOT_ACTIVATED,
    _("Certificate not activated"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_HOSTNAME_MISMATCH,
    _("Certificate hostname mismatch"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_FINGERPRINT_MISMATCH,
    _("Certificate fingerprint mismatch"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_SELF_SIGNED,
    _("Certificate self-signed"));
  g_hash_table_insert (errors, TP_ERROR_STR_CANCELLED,
    _("Status is set to offline"));
  g_hash_table_insert (errors, TP_ERROR_STR_ENCRYPTION_NOT_AVAILABLE,
    _("Encryption is not available"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_INVALID,
    _("Certificate is invalid"));
  g_hash_table_insert (errors, TP_ERROR_STR_CONNECTION_REFUSED,
    _("Connection has been refused"));
  g_hash_table_insert (errors, TP_ERROR_STR_CONNECTION_FAILED,
    _("Connection can’t be established"));
  g_hash_table_insert (errors, TP_ERROR_STR_CONNECTION_LOST,
    _("Connection has been lost"));
  g_hash_table_insert (errors, TP_ERROR_STR_ALREADY_CONNECTED,
    _("This account is already connected to the server"));
  g_hash_table_insert (errors, TP_ERROR_STR_CONNECTION_REPLACED,
    _("Connection has been replaced by a new connection using the "
    "same resource"));
  g_hash_table_insert (errors, TP_ERROR_STR_REGISTRATION_EXISTS,
    _("The account already exists on the server"));
  g_hash_table_insert (errors, TP_ERROR_STR_SERVICE_BUSY,
    _("Server is currently too busy to handle the connection"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_REVOKED,
    _("Certificate has been revoked"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_INSECURE,
    _("Certificate uses an insecure cipher algorithm or is "
    "cryptographically weak"));
  g_hash_table_insert (errors, TP_ERROR_STR_CERT_LIMIT_EXCEEDED,
    _("The length of the server certificate, or the depth of the "
    "server certificate chain, exceed the limits imposed by the "
    "cryptography library"));
  g_hash_table_insert (errors, TP_ERROR_STR_SOFTWARE_UPGRADE_REQUIRED,
    _("Your software is too old"));
  g_hash_table_insert (errors, DBUS_ERROR_NO_REPLY,
    _("Internal error"));

  return errors;
}

static const gchar *
empathy_dbus_error_name_get_default_message  (const gchar *error)
{
  static GHashTable *errors_to_message = NULL;

  if (error == NULL)
    return NULL;

  if (G_UNLIKELY (errors_to_message == NULL))
    errors_to_message = create_errors_to_message_hash ();

  return g_hash_table_lookup (errors_to_message, error);
}

const gchar *
empathy_account_get_error_message (TpAccount *account,
    gboolean *user_requested)
{
  const gchar *dbus_error;
  const gchar *message;
  const GHashTable *details = NULL;
  TpConnectionStatusReason reason;

  dbus_error = tp_account_get_detailed_error (account, &details);

  if (user_requested != NULL)
    {
      if (tp_asv_get_boolean (details, "user-requested", NULL))
        *user_requested = TRUE;
      else
        *user_requested = FALSE;
    }

  message = empathy_dbus_error_name_get_default_message (dbus_error);
  if (message != NULL)
    return message;

  tp_account_get_connection_status (account, &reason);

  DEBUG ("Don't understand error '%s'; fallback to the status reason (%u)",
    dbus_error, reason);

  return empathy_status_reason_get_default_message (reason);
}

gchar *
empathy_file_lookup (const gchar *filename, const gchar *subdir)
{
  gchar *path;

  if (subdir == NULL)
    subdir = ".";

  path = g_build_filename (g_getenv ("EMPATHY_SRCDIR"), subdir, filename, NULL);
  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_free (path);
      path = g_build_filename (DATADIR, "empathy", filename, NULL);
    }

  return path;
}

gint
empathy_uint_compare (gconstpointer a,
    gconstpointer b)
{
  return *(guint *) a - *(guint *) b;
}

GType
empathy_type_dbus_ao (void)
{
  static GType t = 0;

  if (G_UNLIKELY (t == 0))
     t = dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH);

  return t;
}

gboolean
empathy_account_manager_get_accounts_connected (gboolean *connecting)
{
  TpAccountManager *manager;
  GList *accounts, *l;
  gboolean out_connecting = FALSE;
  gboolean out_connected = FALSE;

  manager = tp_account_manager_dup ();

  if (G_UNLIKELY (!tp_proxy_is_prepared (manager,
          TP_ACCOUNT_MANAGER_FEATURE_CORE)))
    g_critical (G_STRLOC ": %s called before AccountManager ready", G_STRFUNC);

  accounts = tp_account_manager_dup_valid_accounts (manager);

  for (l = accounts; l != NULL; l = l->next)
    {
      TpConnectionStatus s = tp_account_get_connection_status (
          TP_ACCOUNT (l->data), NULL);

      if (s == TP_CONNECTION_STATUS_CONNECTING)
        out_connecting = TRUE;
      else if (s == TP_CONNECTION_STATUS_CONNECTED)
        out_connected = TRUE;

      if (out_connecting && out_connected)
        break;
    }

  g_list_free_full (accounts, g_object_unref);
  g_object_unref (manager);

  if (connecting != NULL)
    *connecting = out_connecting;

  return out_connected;
}

/* Translate Folks' general presence type to the Tp presence type */
TpConnectionPresenceType
empathy_folks_presence_type_to_tp (FolksPresenceType type)
{
  return (TpConnectionPresenceType) type;
}

/* Returns TRUE if the given Individual contains a TpContact */
gboolean
empathy_folks_individual_contains_contact (FolksIndividual *individual)
{
  GeeSet *personas;
  GeeIterator *iter;
  gboolean retval = FALSE;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), FALSE);

  personas = folks_individual_get_personas (individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (!retval && gee_iterator_next (iter))
    {
      FolksPersona *persona = gee_iterator_get (iter);
      TpContact *contact = NULL;

      if (empathy_folks_persona_is_interesting (persona))
        contact = tpf_persona_get_contact (TPF_PERSONA (persona));

      g_clear_object (&persona);

      if (contact != NULL)
        retval = TRUE;
    }
  g_clear_object (&iter);

  return retval;
}

/* TODO: this needs to be eliminated (and replaced in some cases with user
 * prompts) when we break the assumption that FolksIndividuals are 1:1 with
 * TpContacts */

/* Retrieve the EmpathyContact corresponding to the first TpContact contained
 * within the given Individual. Note that this is a temporary convenience. See
 * the TODO above. */
EmpathyContact *
empathy_contact_dup_from_folks_individual (FolksIndividual *individual)
{
  GeeSet *personas;
  GeeIterator *iter;
  EmpathyContact *contact = NULL;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);

  personas = folks_individual_get_personas (individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (gee_iterator_next (iter) && (contact == NULL))
    {
      TpfPersona *persona = gee_iterator_get (iter);

      if (empathy_folks_persona_is_interesting (FOLKS_PERSONA (persona)))
        {
          TpContact *tp_contact;

          tp_contact = tpf_persona_get_contact (persona);
          if (tp_contact != NULL)
            {
              contact = empathy_contact_dup_from_tp_contact (tp_contact);
              empathy_contact_set_persona (contact, FOLKS_PERSONA (persona));
            }
        }
      g_clear_object (&persona);
    }
  g_clear_object (&iter);

  if (contact == NULL)
    {
      DEBUG ("Can't create an EmpathyContact for Individual %s",
          folks_individual_get_id (individual));
    }

  return contact;
}

TpChannelGroupChangeReason
tp_channel_group_change_reason_from_folks_groups_change_reason (
    FolksGroupDetailsChangeReason reason)
{
  return (TpChannelGroupChangeReason) reason;
}

TpfPersonaStore *
empathy_dup_persona_store_for_connection (TpConnection *connection)
{
  FolksBackendStore *backend_store;
  FolksBackend *backend;
  TpfPersonaStore *result = NULL;

  backend_store = folks_backend_store_dup ();
  backend = folks_backend_store_dup_backend_by_name (backend_store,
      "telepathy");
  if (backend != NULL)
    {
      GeeMap *stores_map;
      GeeMapIterator *iter;

      stores_map = folks_backend_get_persona_stores (backend);
      iter = gee_map_map_iterator (stores_map);
      while (gee_map_iterator_next (iter))
        {
          TpfPersonaStore *persona_store = gee_map_iterator_get_value (iter);
          TpAccount *account;
          TpConnection *conn_cur;

          account = tpf_persona_store_get_account (persona_store);
          conn_cur = tp_account_get_connection (account);
          if (conn_cur == connection)
            result = g_object_ref (persona_store);

          g_clear_object (&persona_store);
        }
      g_clear_object (&iter);
    }

  g_object_unref (backend);
  g_object_unref (backend_store);

  return result;
}

gboolean
empathy_connection_can_add_personas (TpConnection *connection)
{
  gboolean retval;
  FolksPersonaStore *persona_store;

  g_return_val_if_fail (TP_IS_CONNECTION (connection), FALSE);

  if (tp_connection_get_status (connection, NULL) !=
          TP_CONNECTION_STATUS_CONNECTED)
      return FALSE;

  persona_store = FOLKS_PERSONA_STORE (
      empathy_dup_persona_store_for_connection (connection));

  retval = (folks_persona_store_get_can_add_personas (persona_store) ==
      FOLKS_MAYBE_BOOL_TRUE);

  g_clear_object (&persona_store);

  return retval;
}

gboolean
empathy_connection_can_alias_personas (TpConnection *connection,
				       FolksIndividual *individual)
{
  gboolean retval;

  g_return_val_if_fail (TP_IS_CONNECTION (connection), FALSE);

  if (tp_connection_get_status (connection, NULL) !=
          TP_CONNECTION_STATUS_CONNECTED)
      return FALSE;

  retval = check_writeable_property (connection, individual, "alias");

  return retval;
}

gboolean
empathy_connection_can_group_personas (TpConnection *connection,
				       FolksIndividual *individual)
{
  gboolean retval;

  g_return_val_if_fail (TP_IS_CONNECTION (connection), FALSE);

  if (tp_connection_get_status (connection, NULL) !=
          TP_CONNECTION_STATUS_CONNECTED)
      return FALSE;

  retval = check_writeable_property (connection, individual, "groups");

  return retval;
}

gboolean
empathy_folks_persona_is_interesting (FolksPersona *persona)
{
  /* We're not interested in non-Telepathy personas */
  if (!TPF_IS_PERSONA (persona))
    return FALSE;

  /* We're not interested in user personas which haven't been added to the
   * contact list (see bgo#637151). */
  if (folks_persona_get_is_user (persona) &&
      !tpf_persona_get_is_in_contact_list (TPF_PERSONA (persona)))
    {
      return FALSE;
    }

  return TRUE;
}

gchar *
empathy_format_currency (gint amount,
    guint scale,
    const gchar *currency)
{
#define MINUS "\342\210\222"
#define EURO "\342\202\254"
#define YEN "\302\245"
#define POUND "\302\243"

  /* localised representations of currency */
  /* FIXME: check these, especially negatives and decimals */
  static const struct {
    const char *currency;
    const char *positive;
    const char *negative;
    const char *decimal;
  } currencies[] = {
    /* sym   positive    negative          decimal */
    { "EUR", EURO "%s",  MINUS EURO "%s",  "." },
    { "USD", "$%s",      MINUS "$%s",      "." },
    { "JPY", YEN "%s"    MINUS YEN "%s",   "." },
    { "GBP", POUND "%s", MINUS POUND "%s", "." },
    { "PLN", "%s zl",    MINUS "%s zl",    "." },
    { "BRL", "R$%s",     MINUS "R$%s",     "." },
    { "SEK", "%s kr",    MINUS "%s kr",    "." },
    { "DKK", "kr %s",    "kr " MINUS "%s", "." },
    { "HKD", "$%s",      MINUS "$%s",      "." },
    { "CHF", "%s Fr.",   MINUS "%s Fr.",   "." },
    { "NOK", "kr %s",    "kr" MINUS "%s",  "," },
    { "CAD", "$%s",      MINUS "$%s",      "." },
    { "TWD", "$%s",      MINUS "$%s",      "." },
    { "AUD", "$%s",      MINUS "$%s",      "." },
  };

  const char *positive = "%s";
  const char *negative = MINUS "%s";
  const char *decimal = ".";
  char *fmt_amount, *money;
  guint i;

  /* get the localised currency format */
  for (i = 0; i < G_N_ELEMENTS (currencies); i++)
    {
      if (!tp_strdiff (currency, currencies[i].currency))
        {
          positive = currencies[i].positive;
          negative = currencies[i].negative;
          decimal = currencies[i].decimal;
          break;
        }
    }

  /* format the amount using the scale */
  if (scale == 0)
    {
      /* no decimal point required */
      fmt_amount = g_strdup_printf ("%d", amount);
    }
  else
    {
      /* don't use floating point arithmatic, it's noisy;
       * we take the absolute values, because we want the minus
       * sign to appear before the $ */
      int divisor = pow (10, scale);
      int dollars = abs (amount / divisor);
      int cents = abs (amount % divisor);

      fmt_amount = g_strdup_printf ("%d%s%0*d",
        dollars, decimal, scale, cents);
    }

  money = g_strdup_printf (amount < 0 ? negative : positive, fmt_amount);
  g_free (fmt_amount);

  return money;
}

/* Return the TpContact on @conn associated with @individual, if any */
TpContact *
empathy_get_tp_contact_for_individual (FolksIndividual *individual,
    TpConnection *conn)
{
  TpContact *contact = NULL;
  GeeSet *personas;
  GeeIterator *iter;

  personas = folks_individual_get_personas (individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (contact == NULL && gee_iterator_next (iter))
    {
      TpfPersona *persona = gee_iterator_get (iter);
      TpConnection *contact_conn;
      TpContact *contact_cur = NULL;

      if (TPF_IS_PERSONA (persona))
        {
          contact_cur = tpf_persona_get_contact (persona);
          if (contact_cur != NULL)
            {
              contact_conn = tp_contact_get_connection (contact_cur);

              if (!tp_strdiff (tp_proxy_get_object_path (contact_conn),
                    tp_proxy_get_object_path (conn)))
                contact = contact_cur;
            }
        }

      g_clear_object (&persona);
    }
  g_clear_object (&iter);

  return contact;
}

static gboolean
properties_contains (gchar **list,
		     gint length,
		     const gchar *property)
{
  gint i;

  for (i = 0; i < length; i++)
    {
      if (!tp_strdiff (list[i], property))
	return TRUE;
    }

  return FALSE;
}

static gboolean
check_writeable_property (TpConnection *connection,
			  FolksIndividual *individual,
			  gchar *property)
{
  gchar **properties;
  gint prop_len;
  gboolean retval = FALSE;
  GeeSet *personas;
  GeeIterator *iter;
  FolksPersonaStore *persona_store;

  persona_store = FOLKS_PERSONA_STORE (
      empathy_dup_persona_store_for_connection (connection));

  properties =
    folks_persona_store_get_always_writeable_properties (persona_store,
							 &prop_len);
  retval = properties_contains (properties, prop_len, property);
  if (retval == TRUE)
    goto out;

  /* Lets see if the Individual contains a Persona with the given property */
  personas = folks_individual_get_personas (individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (!retval && gee_iterator_next (iter))
    {
      FolksPersona *persona = gee_iterator_get (iter);

      properties =
	folks_persona_get_writeable_properties (persona, &prop_len);
      retval = properties_contains (properties, prop_len, property);

      g_clear_object (&persona);

      if (retval == TRUE)
	break;
    }
  g_clear_object (&iter);

out:
  g_clear_object (&persona_store);
  return retval;
}

/* Calculate whether the Individual can do audio or video calls.
 * FIXME: We can remove this once libfolks has grown capabilities support
 * again: bgo#626179. */
void
empathy_individual_can_audio_video_call (FolksIndividual *individual,
    gboolean *can_audio_call,
    gboolean *can_video_call,
    EmpathyContact **out_contact)
{
  GeeSet *personas;
  GeeIterator *iter;
  gboolean can_audio = FALSE, can_video = FALSE;

  personas = folks_individual_get_personas (individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (gee_iterator_next (iter))
    {
      FolksPersona *persona = gee_iterator_get (iter);
      TpContact *tp_contact;

      if (!empathy_folks_persona_is_interesting (persona))
        goto while_finish;

      tp_contact = tpf_persona_get_contact (TPF_PERSONA (persona));
      if (tp_contact != NULL)
        {
          EmpathyContact *contact;

          contact = empathy_contact_dup_from_tp_contact (tp_contact);
          empathy_contact_set_persona (contact, persona);

          can_audio = can_audio || empathy_contact_get_capabilities (contact) &
              EMPATHY_CAPABILITIES_AUDIO;
          can_video = can_video || empathy_contact_get_capabilities (contact) &
              EMPATHY_CAPABILITIES_VIDEO;

          if (out_contact != NULL)
            *out_contact = g_object_ref (contact);

          g_object_unref (contact);
        }

while_finish:
      g_clear_object (&persona);

      if (can_audio && can_video)
        break;
    }

  g_clear_object (&iter);

  if (can_audio_call != NULL)
    *can_audio_call = can_audio;

  if (can_video_call != NULL)
    *can_video_call = can_video;
}

gboolean
empathy_client_types_contains_mobile_device (const GStrv types) {
  int i;

  if (types == NULL)
    return FALSE;

  for (i = 0; types[i] != NULL; i++)
    if (!tp_strdiff (types[i], "phone") || !tp_strdiff (types[i], "handheld"))
        return TRUE;

  return FALSE;
}

static FolksIndividual *
create_individual_from_persona (FolksPersona *persona)
{
  GeeSet *personas;
  FolksIndividual *individual;

  personas = GEE_SET (
      gee_hash_set_new (FOLKS_TYPE_PERSONA, g_object_ref, g_object_unref,
      NULL, NULL, NULL, NULL, NULL, NULL));

  gee_collection_add (GEE_COLLECTION (personas), persona);

  individual = folks_individual_new (personas);

  g_clear_object (&personas);

  return individual;
}

/* Look for a FolksIndividual containing @contact as one of his persona
 * and create one if needed */
FolksIndividual *
empathy_ensure_individual_from_tp_contact (TpContact *contact)
{
  TpfPersona *persona;
  FolksIndividual *individual;

  persona = tpf_persona_dup_for_contact (contact);
  if (persona == NULL)
    {
      DEBUG ("Failed to get a persona for %s",
          tp_contact_get_identifier (contact));
      return NULL;
    }

  individual = folks_persona_get_individual (FOLKS_PERSONA (persona));

  if (individual != NULL)
    g_object_ref (individual);
  else
    individual = create_individual_from_persona (FOLKS_PERSONA (persona));

  g_object_unref (persona);
  return individual;
}

const gchar * const *
empathy_individual_get_client_types (FolksIndividual *individual)
{
  GeeSet *personas;
  GeeIterator *iter;
  const gchar * const *types = NULL;
  FolksPresenceType presence_type = FOLKS_PRESENCE_TYPE_UNSET;

  personas = folks_individual_get_personas (individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (gee_iterator_next (iter))
    {
      FolksPresenceDetails *presence;
      FolksPersona *persona = gee_iterator_get (iter);

      /* We only want personas which have presence and a TpContact */
      if (!empathy_folks_persona_is_interesting (persona))
        goto while_finish;

      presence = FOLKS_PRESENCE_DETAILS (persona);

      if (folks_presence_details_typecmp (
              folks_presence_details_get_presence_type (presence),
              presence_type) > 0)
        {
          TpContact *tp_contact;

          presence_type = folks_presence_details_get_presence_type (presence);

          tp_contact = tpf_persona_get_contact (TPF_PERSONA (persona));
          if (tp_contact != NULL)
            types = tp_contact_get_client_types (tp_contact);
        }

while_finish:
      g_clear_object (&persona);
    }
  g_clear_object (&iter);

  return types;
}

GVariant *
empathy_asv_to_vardict (const GHashTable *asv)
{
  return empathy_boxed_to_variant (TP_HASH_TYPE_STRING_VARIANT_MAP, "a{sv}",
      (gpointer) asv);
}

GVariant *
empathy_boxed_to_variant (GType gtype,
    const gchar *variant_type,
    gpointer boxed)
{
  GValue v = G_VALUE_INIT;
  GVariant *ret;

  g_return_val_if_fail (boxed != NULL, NULL);

  g_value_init (&v, gtype);
  g_value_set_boxed (&v, boxed);

  ret = dbus_g_value_build_g_variant (&v);
  g_return_val_if_fail (!tp_strdiff (g_variant_get_type_string (ret),
        variant_type), NULL);

  g_value_unset (&v);

  return g_variant_ref_sink (ret);
}
