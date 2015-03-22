/*
 * empathy-geoclue-helper.c
 *
 * Copyright (C) 2013 Collabora Ltd. <http://www.collabora.co.uk/>
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

#include <gio/gio.h>

#include "empathy-geoclue-helper.h"

#define DEBUG_FLAG EMPATHY_DEBUG_LOCATION
#include "empathy-debug.h"

#define GEOCLUE_BUS_NAME "org.freedesktop.GeoClue2"
#define EMPATHY_DESKTOP_ID "empathy"

/**
 * SECTION: empathy-geoclue-helper
 * @title: EmpathyGeoclueHelper
 * @short_description: TODO
 *
 * TODO
 */

/**
 * EmpathyGeoclueHelper:
 *
 * Data structure representing a #EmpathyGeoclueHelper.
 *
 * Since: UNRELEASED
 */

/**
 * EmpathyGeoclueHelperClass:
 *
 * The class of a #EmpathyGeoclueHelper.
 *
 * Since: UNRELEASED
 */

static void async_initable_iface_init (GAsyncInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (EmpathyGeoclueHelper, empathy_geoclue_helper,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE, async_initable_iface_init));

enum
{
  PROP_DISTANCE_THRESHOLD = 1,
  PROP_LOCATION,
  N_PROPS
};

enum
{
  SIG_LOCATION_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

struct _EmpathyGeoclueHelperPriv
{
  guint distance_threshold;
  GClueLocation *location;

  gboolean started;
  GClueClient *client;
};

static void
empathy_geoclue_helper_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyGeoclueHelper *self = EMPATHY_GEOCLUE_HELPER (object);

  switch (property_id)
    {
      case PROP_DISTANCE_THRESHOLD:
        g_value_set_uint (value, self->priv->distance_threshold);
        break;
      case PROP_LOCATION:
        g_value_set_object (value, self->priv->location);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
empathy_geoclue_helper_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyGeoclueHelper *self = EMPATHY_GEOCLUE_HELPER (object);

  switch (property_id)
    {
      case PROP_DISTANCE_THRESHOLD:
        self->priv->distance_threshold = g_value_get_uint (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
empathy_geoclue_helper_constructed (GObject *object)
{
  //EmpathyGeoclueHelper *self = EMPATHY_GEOCLUE_HELPER (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_geoclue_helper_parent_class)->constructed;

  chain_up (object);
}

static void
empathy_geoclue_helper_dispose (GObject *object)
{
  EmpathyGeoclueHelper *self = EMPATHY_GEOCLUE_HELPER (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_geoclue_helper_parent_class)->dispose;

  if (self->priv->started)
    {
      gclue_client_call_stop (self->priv->client, NULL, NULL, NULL);

      self->priv->started = FALSE;
    }

  g_clear_object (&self->priv->location);
  g_clear_object (&self->priv->client);

  chain_up (object);
}

static void
empathy_geoclue_helper_finalize (GObject *object)
{
  //EmpathyGeoclueHelper *self = EMPATHY_GEOCLUE_HELPER (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_geoclue_helper_parent_class)->finalize;

  chain_up (object);
}

static void
empathy_geoclue_helper_class_init (
    EmpathyGeoclueHelperClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GParamSpec *spec;

  oclass->get_property = empathy_geoclue_helper_get_property;
  oclass->set_property = empathy_geoclue_helper_set_property;
  oclass->constructed = empathy_geoclue_helper_constructed;
  oclass->dispose = empathy_geoclue_helper_dispose;
  oclass->finalize = empathy_geoclue_helper_finalize;

  /**
   * EmpathyGeoclueHelper:distance-threshold:
   *
   * TODO
   *
   * Since: UNRELEASED
   */
  spec = g_param_spec_uint ("distance-threshold", "distance-threshold",
      "DistanceThreshold",
      0, G_MAXUINT32, 0,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_DISTANCE_THRESHOLD, spec);

  /**
   * EmpathyGeoclueHelper:location:
   *
   * TODO
   *
   * Since: UNRELEASED
   */
  spec = g_param_spec_object ("location", "location", "GClueLocation",
      GCLUE_TYPE_LOCATION,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_LOCATION, spec);

  /**
   * EmpathyGeoclueHelper::location-changed:
   * @self: a #EmpathyGeoclueHelper
   *
   * TODO
   *
   * Since: UNRELEASED
   */
  signals[SIG_LOCATION_CHANGED] = g_signal_new ("location-changed",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      1, GCLUE_TYPE_LOCATION);

  g_type_class_add_private (klass, sizeof (EmpathyGeoclueHelperPriv));
}

static void
empathy_geoclue_helper_init (EmpathyGeoclueHelper *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_GEOCLUE_HELPER, EmpathyGeoclueHelperPriv);
}

static void
location_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyGeoclueHelper *self = user_data;
  GError *error = NULL;

  g_clear_object (&self->priv->location);

  self->priv->location = gclue_location_proxy_new_finish (result, &error);
  if (self->priv->location == NULL)
    {
      DEBUG ("Failed to create Location proxy: %s", error->message);
      g_error_free (error);
    }

  g_signal_emit (self, signals[SIG_LOCATION_CHANGED], 0, self->priv->location);

  g_object_notify (G_OBJECT (self), "location");
}

static void
location_updated_cb (GClueClient *client,
    const gchar *old,
    const gchar *new,
    EmpathyGeoclueHelper *self)
{
  gclue_location_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
      GEOCLUE_BUS_NAME, new,
      NULL, location_cb, self);
}

static void
client_start_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GTask *task = user_data;
  EmpathyGeoclueHelper *self = g_task_get_source_object (task);
  GClueClient *client = GCLUE_CLIENT (source);
  GError *error = NULL;

  if (!gclue_client_call_start_finish (client, result, &error))
    {
      DEBUG ("Failed to start Geoclue client: %s", error->message);
      g_error_free (error);
      return;
    }

  self->priv->started = TRUE;

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static void
client_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GTask *task = user_data;
  EmpathyGeoclueHelper *self = g_task_get_source_object (task);
  GError *error = NULL;

  self->priv->client = gclue_client_proxy_new_finish (result, &error);
  if (!gclue_client_proxy_new_for_bus_finish (result, &error))
    {
      DEBUG ("Failed to create Geoclue client: %s", error->message);
      g_task_return_error (task, error);
      goto out;
    }

  g_signal_connect_object (self->priv->client, "location-updated",
      G_CALLBACK (location_updated_cb), self, 0);

  g_object_set (self->priv->client,
      "distance-threshold", self->priv->distance_threshold,
      "desktop-id", EMPATHY_DESKTOP_ID,
      NULL);

  g_task_return_boolean (task, TRUE);

out:
  g_object_unref (task);
}

static void
get_client_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GTask *task = user_data;
  GError *error = NULL;
  gchar *path;

  if (!gclue_manager_call_get_client_finish (GCLUE_MANAGER (source), &path,
        result, &error))
    {
      DEBUG ("GetClient failed: %s", error->message);
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  gclue_client_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
      GEOCLUE_BUS_NAME, path, NULL, client_cb, task);

  g_free (path);
}

static void
manager_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GTask *task = user_data;
  GError *error = NULL;
  GClueManager *mgr;

  mgr = gclue_manager_proxy_new_for_bus_finish (result, &error);
  if (mgr == NULL)
    {
      DEBUG ("Failed to create Geoclue manager: %s", error->message);
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  gclue_manager_call_get_client (mgr, NULL, get_client_cb, task);
  g_object_unref (mgr);
}

void
empathy_geoclue_helper_start_async (EmpathyGeoclueHelper *self,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GTask *task;

  task = g_task_new (self, NULL, callback, user_data);

  if (self->priv->started)
    {
      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
      return;
    }

  gclue_client_call_start (self->priv->client, NULL, client_start_cb, task);
}

gboolean
empathy_geoclue_helper_start_finish (EmpathyGeoclueHelper *self,
    GAsyncResult *result,
    GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

GClueLocation *
empathy_geoclue_helper_get_location (EmpathyGeoclueHelper *self)
{
  return self->priv->location;
}

static void
empathy_geoclue_helper_init_async (GAsyncInitable *initable,
    gint io_priority,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GTask *task;

  task = g_task_new (initable, cancellable, callback, user_data);

  gclue_manager_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
      GEOCLUE_BUS_NAME, "/org/freedesktop/GeoClue2/Manager",
      NULL, manager_cb, task);
}

static gboolean
empathy_geoclue_helper_init_finish (GAsyncInitable *initable,
    GAsyncResult *result,
    GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, initable), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
async_initable_iface_init (GAsyncInitableIface *iface)
{
  iface->init_async = empathy_geoclue_helper_init_async;
  iface->init_finish = empathy_geoclue_helper_init_finish;
}

void
empathy_geoclue_helper_new_async (guint distance_threshold,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  g_async_initable_new_async (EMPATHY_TYPE_GEOCLUE_HELPER,
      G_PRIORITY_DEFAULT, NULL, callback, user_data,
      "distance-threshold", distance_threshold,
      NULL);
}

EmpathyGeoclueHelper *
empathy_geoclue_helper_new_finish (GAsyncResult *result,
    GError **error)
{
  GObject *object, *source_object;

  source_object = g_async_result_get_source_object (result);

  object = g_async_initable_new_finish (G_ASYNC_INITABLE (source_object),
      result, error);
  g_object_unref (source_object);

  if (object != NULL)
    return EMPATHY_GEOCLUE_HELPER (object);
  else
    return NULL;
}

static void
new_started_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyGeoclueHelper *self = EMPATHY_GEOCLUE_HELPER (source);
  GTask *new_started_task = user_data;
  GError *error = NULL;

  if (!empathy_geoclue_helper_start_finish (self, result, &error))
    {
      g_task_return_error (new_started_task, error);
      g_object_unref (self);
      goto out;
    }

  /* pass ownership of self to g_task_return_error */
  g_task_return_pointer (new_started_task, self, g_object_unref);

out:
  g_object_unref (new_started_task);
}

static void
new_started_init_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GTask *new_started_task = user_data;
  EmpathyGeoclueHelper *self;
  GError *error = NULL;

  self = empathy_geoclue_helper_new_finish (result, &error);
  if (self == NULL)
    {
      g_task_return_error (new_started_task, error);
      g_object_unref (new_started_task);
      return;
    }

  /* Pass owernship of 'self' to new_started_cb */
  empathy_geoclue_helper_start_async (self, new_started_cb, new_started_task);
}

void
empathy_geoclue_helper_new_started_async (guint distance_threshold,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GTask *new_started_task;

  new_started_task = g_task_new (NULL, NULL, callback, user_data);

  empathy_geoclue_helper_new_async (distance_threshold, new_started_init_cb,
      new_started_task);
}

EmpathyGeoclueHelper *
empathy_geoclue_helper_new_started_finish (GAsyncResult *result,
    GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
