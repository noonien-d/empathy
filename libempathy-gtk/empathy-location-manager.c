/*
 * Copyright (C) 2009 Collabora Ltd.
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
 * Authors: Pierre-Luc Beaudoin <pierre-luc.beaudoin@collabora.co.uk>
 */

#include "config.h"
#include "empathy-location-manager.h"

#include <telepathy-glib/telepathy-glib-dbus.h>

#include <tp-account-widgets/tpaw-time.h>

#include "empathy-gsettings.h"
#include "empathy-location.h"
#include "empathy-geoclue-helper.h"

#define DEBUG_FLAG EMPATHY_DEBUG_LOCATION
#include "empathy-debug.h"

/* Seconds before updating the location */
#define TIMEOUT 10
static EmpathyLocationManager *location_manager = NULL;

typedef enum
{
  GEOCLUE_NONE = 0,
  GEOCLUE_STARTING,
  GEOCLUE_STARTED,
  GEOCLUE_FAILED,
} GeoclueStatus;

struct _EmpathyLocationManagerPrivate {
    GeoclueStatus geoclue_status;
    /* Contains the location to be sent to accounts.  Geoclue is used
     * to populate it.  This HashTable uses Telepathy's style (string,
     * GValue). Keys are defined in empathy-location.h
     */
    GHashTable *location;

    GSettings *gsettings_loc;

    gboolean reduce_accuracy;
    TpAccountManager *account_manager;
    EmpathyGeoclueHelper *geoclue;

    /* The idle id for publish_on_idle func */
    guint timeout_id;
};

G_DEFINE_TYPE (EmpathyLocationManager, empathy_location_manager, G_TYPE_OBJECT);

static GObject *
location_manager_constructor (GType type,
    guint n_construct_params,
    GObjectConstructParam *construct_params)
{
  GObject *retval;

  if (location_manager == NULL)
    {
      retval = G_OBJECT_CLASS (empathy_location_manager_parent_class)->constructor
          (type, n_construct_params, construct_params);

      location_manager = EMPATHY_LOCATION_MANAGER (retval);
      g_object_add_weak_pointer (retval, (gpointer) &location_manager);
    }
  else
    {
      retval = g_object_ref (location_manager);
    }

  return retval;
}

static void
location_manager_dispose (GObject *object)
{
  EmpathyLocationManager *self = (EmpathyLocationManager *) object;
  void (*dispose) (GObject *) =
    G_OBJECT_CLASS (empathy_location_manager_parent_class)->dispose;

  tp_clear_object (&self->priv->account_manager);
  tp_clear_object (&self->priv->gsettings_loc);
  tp_clear_pointer (&self->priv->location, g_hash_table_unref);

  if (dispose != NULL)
    dispose (object);
}

static void
empathy_location_manager_class_init (EmpathyLocationManagerClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);

  object_class->constructor = location_manager_constructor;
  object_class->dispose = location_manager_dispose;

  g_type_class_add_private (object_class, sizeof (EmpathyLocationManagerPrivate));
}

static void
publish_location_cb (TpConnection *connection,
                     const GError *error,
                     gpointer user_data,
                     GObject *weak_object)
{
  if (error != NULL)
      DEBUG ("Error setting location: %s", error->message);
}

static void
publish_location (EmpathyLocationManager *self,
    TpConnection *conn,
    gboolean force_publication)
{
  guint connection_status = -1;

  if (!conn)
    return;

  if (!force_publication)
    {
      if (!g_settings_get_boolean (self->priv->gsettings_loc,
            EMPATHY_PREFS_LOCATION_PUBLISH))
        return;
    }

  connection_status = tp_connection_get_status (conn, NULL);

  if (connection_status != TP_CONNECTION_STATUS_CONNECTED)
    return;

  DEBUG ("Publishing %s location to connection %p",
      (g_hash_table_size (self->priv->location) == 0 ? "empty" : ""),
      conn);

  tp_cli_connection_interface_location_call_set_location (conn, -1,
      self->priv->location, publish_location_cb, NULL, NULL, G_OBJECT (self));
}

typedef struct
{
  EmpathyLocationManager *self;
  gboolean force_publication;
} PublishToAllData;

static void
publish_to_all_am_prepared_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *manager = TP_ACCOUNT_MANAGER (source_object);
  PublishToAllData *data = user_data;
  GList *accounts, *l;
  GError *error = NULL;

  if (!tp_proxy_prepare_finish (manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      goto out;
    }

  accounts = tp_account_manager_dup_valid_accounts (manager);
  for (l = accounts; l; l = l->next)
    {
      TpConnection *conn = tp_account_get_connection (TP_ACCOUNT (l->data));

      if (conn != NULL)
        publish_location (data->self, conn, data->force_publication);
    }
  g_list_free_full (accounts, g_object_unref);

out:
  g_object_unref (data->self);
  g_slice_free (PublishToAllData, data);
}

static void
publish_to_all_connections (EmpathyLocationManager *self,
    gboolean force_publication)
{
  PublishToAllData *data;

  data = g_slice_new0 (PublishToAllData);
  data->self = g_object_ref (self);
  data->force_publication = force_publication;

  tp_proxy_prepare_async (self->priv->account_manager, NULL,
      publish_to_all_am_prepared_cb, data);
}

static gboolean
publish_on_idle (gpointer user_data)
{
  EmpathyLocationManager *manager = EMPATHY_LOCATION_MANAGER (user_data);

  manager->priv->timeout_id = 0;
  publish_to_all_connections (manager, TRUE);
  return FALSE;
}

static void
new_connection_cb (TpAccount *account,
    guint old_status,
    guint new_status,
    guint reason,
    gchar *dbus_error_name,
    GHashTable *details,
    gpointer user_data)
{
  EmpathyLocationManager *self = user_data;
  TpConnection *conn;

  conn = tp_account_get_connection (account);

  DEBUG ("New connection %p", conn);

  /* Don't publish if it is already planned (ie startup) */
  if (self->priv->timeout_id == 0)
    {
      publish_location (EMPATHY_LOCATION_MANAGER (self), conn,
          FALSE);
    }
}

static void
update_location (EmpathyLocationManager *self,
    GClueLocation *proxy)
{
  gdouble latitude, longitude, accuracy;
  const gchar *desc;
  gint64 timestamp;

  latitude = gclue_location_get_latitude (proxy);
  longitude = gclue_location_get_longitude (proxy);
  accuracy = gclue_location_get_accuracy (proxy);
  desc = gclue_location_get_description (proxy);

  DEBUG ("Location updated: (%f %f) accuracy: %f (%s)",
      latitude, longitude, accuracy, desc);

  if (self->priv->reduce_accuracy)
    {
      /* Truncate at 1 decimal place */
      latitude = ((int) (latitude * 10)) / 10.0;
      longitude = ((int) (longitude * 10)) / 10.0;
    }
  else
    {
      /* Include the description only if we are not asked to reduce the
       * accuracy as it can contains a pretty specific description of the
       * location. */
      tp_asv_set_string (self->priv->location, EMPATHY_LOCATION_DESCRIPTION,
          desc);
    }

  tp_asv_set_double (self->priv->location, EMPATHY_LOCATION_LAT, latitude);
  tp_asv_set_double (self->priv->location, EMPATHY_LOCATION_LON, longitude);
  tp_asv_set_double (self->priv->location, EMPATHY_LOCATION_ACCURACY, accuracy);

  timestamp = tpaw_time_get_current ();
  tp_asv_set_int64 (self->priv->location, EMPATHY_LOCATION_TIMESTAMP,
      timestamp);

  if (self->priv->timeout_id == 0)
    self->priv->timeout_id = g_timeout_add_seconds (TIMEOUT, publish_on_idle,
        self);
}

static void
location_changed_cb (EmpathyGeoclueHelper *geoclue,
    GClueLocation *location,
    EmpathyLocationManager *self)
{
  update_location (self, location);
}

static void
geoclue_new_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyLocationManager *self = EMPATHY_LOCATION_MANAGER (user_data);
  GError *error = NULL;
  GClueLocation *location;

  self->priv->geoclue = empathy_geoclue_helper_new_started_finish (result,
      &error);

  if (self->priv->geoclue == NULL)
    {
      DEBUG ("Failed to create Geoclue client: %s", error->message);
      g_error_free (error);
      self->priv->geoclue_status = GEOCLUE_FAILED;
      return;
    }

  self->priv->geoclue_status = GEOCLUE_STARTED;

  g_signal_connect_object (self->priv->geoclue, "location-changed",
      G_CALLBACK (location_changed_cb), self, 0);

  location = empathy_geoclue_helper_get_location (self->priv->geoclue);
  if (location != NULL)
    update_location (self, location);
}

static void
setup_geoclue (EmpathyLocationManager *self)
{
  switch (self->priv->geoclue_status)
    {
      case GEOCLUE_NONE:
        g_assert (self->priv->geoclue == NULL);
        self->priv->geoclue_status = GEOCLUE_STARTING;
        empathy_geoclue_helper_new_started_async (0, geoclue_new_cb, self);
        break;
      case GEOCLUE_STARTED:
      case GEOCLUE_STARTING:
      case GEOCLUE_FAILED:
      return;
    }
}

static void
publish_cb (GSettings *gsettings_loc,
            const gchar *key,
            gpointer user_data)
{
  EmpathyLocationManager *self = EMPATHY_LOCATION_MANAGER (user_data);

  DEBUG ("Publish Conf changed");

  if (g_settings_get_boolean (gsettings_loc, key))
    {
      setup_geoclue (self);
    }
  else
    {
      /* As per XEP-0080: send an empty location to have remove current
       * location from the servers
       */
      g_hash_table_remove_all (self->priv->location);
      publish_to_all_connections (self, TRUE);

      g_clear_object (&self->priv->geoclue);
      self->priv->geoclue_status = GEOCLUE_NONE;
    }
}

static void
reduce_accuracy_cb (GSettings *gsettings_loc,
                    const gchar *key,
                    gpointer user_data)
{
  EmpathyLocationManager *self = EMPATHY_LOCATION_MANAGER (user_data);

  DEBUG ("Reduce Accuracy changed");

  self->priv->reduce_accuracy = g_settings_get_boolean (gsettings_loc, key);
}

static void
account_manager_prepared_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  GList *accounts, *l;
  TpAccountManager *account_manager = TP_ACCOUNT_MANAGER (source_object);
  EmpathyLocationManager *self = user_data;
  GError *error = NULL;

  if (!tp_proxy_prepare_finish (account_manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  accounts = tp_account_manager_dup_valid_accounts (account_manager);
  for (l = accounts; l != NULL; l = l->next)
    {
      TpAccount *account = TP_ACCOUNT (l->data);

      tp_g_signal_connect_object (account, "status-changed",
          G_CALLBACK (new_connection_cb), self, 0);
    }
  g_list_free_full (accounts, g_object_unref);
}

static void
empathy_location_manager_init (EmpathyLocationManager *self)
{
  EmpathyLocationManagerPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_LOCATION_MANAGER, EmpathyLocationManagerPrivate);

  self->priv = priv;
  priv->location = tp_asv_new (NULL, NULL);
  priv->gsettings_loc = g_settings_new (EMPATHY_PREFS_LOCATION_SCHEMA);

  /* Setup account status callbacks */
  priv->account_manager = tp_account_manager_dup ();

  tp_proxy_prepare_async (priv->account_manager, NULL,
      account_manager_prepared_cb, self);

  /* Setup settings status callbacks */
  g_signal_connect (priv->gsettings_loc,
      "changed::" EMPATHY_PREFS_LOCATION_PUBLISH,
      G_CALLBACK (publish_cb), self);

  g_signal_connect (priv->gsettings_loc,
      "changed::" EMPATHY_PREFS_LOCATION_REDUCE_ACCURACY,
      G_CALLBACK (reduce_accuracy_cb), self);

  publish_cb (priv->gsettings_loc, EMPATHY_PREFS_LOCATION_PUBLISH, self);
  reduce_accuracy_cb (priv->gsettings_loc, EMPATHY_PREFS_LOCATION_REDUCE_ACCURACY, self);
}

EmpathyLocationManager *
empathy_location_manager_dup_singleton (void)
{
  return EMPATHY_LOCATION_MANAGER (g_object_new (EMPATHY_TYPE_LOCATION_MANAGER,
      NULL));
}
