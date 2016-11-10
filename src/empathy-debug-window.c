/*
*  Copyright (C) 2009 Collabora Ltd.
*
*  This library is free software; you can redistribute it and/or
*  modify it under the terms of the GNU Lesser General Public
*  License as published by the Free Software Foundation; either
*  version 2.1 of the License, or (at your option) any later version.
*
*  This library is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*  Lesser General Public License for more details.
*
*  You should have received a copy of the GNU Lesser General Public
*  License along with this library; if not, write to the Free Software
*  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*
*  Authors: Jonny Lamb <jonny.lamb@collabora.co.uk>
*           Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
*/

#include "config.h"
#include "empathy-debug-window.h"

#include <glib/gi18n.h>
#include <libsoup/soup.h>
#include <tp-account-widgets/tpaw-utils.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-geometry.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

G_DEFINE_TYPE (EmpathyDebugWindow, empathy_debug_window,
    GTK_TYPE_WINDOW)

typedef enum
{
  SERVICE_TYPE_CM = 0,
  SERVICE_TYPE_CLIENT,
  SERVICE_TYPE_MC,
} ServiceType;

enum
{
  COL_DEBUG_MESSAGE = 0,
  NUM_DEBUG_COLS
};

enum
{
  COL_NAME = 0,
  COL_UNIQUE_NAME,
  COL_GONE,
  COL_ACTIVE_BUFFER,
  COL_PAUSE_BUFFER,
  COL_PROXY,
  NUM_COLS
};

enum
{
  COL_LEVEL_NAME,
  COL_LEVEL_VALUE,
  NUM_COLS_LEVEL
};

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyDebugWindow)
struct _EmpathyDebugWindowPriv
{
  /* Toolbar items */
  GtkWidget *chooser;
  GtkToolItem *save_button;
  GtkToolItem *send_to_pastebin;
  GtkToolItem *copy_button;
  GtkToolItem *clear_button;
  GtkToolItem *pause_button;
  GtkToolItem *level_label;
  GtkWidget *level_filter;

  /* TreeView */
  GtkTreeModel *store_filter;
  GtkWidget *view;
  GtkWidget *scrolled_win;
  GtkWidget *not_supported_label;
  gboolean view_visible;

  /* Connection */
  TpDBusDaemon *dbus;
  TpProxySignalConnection *name_owner_changed_signal;

  /* Whether NewDebugMessage will be fired */
  gboolean paused;

  /* Service (CM, Client) chooser store */
  GtkListStore *service_store;

  /* Counters on services detected and added */
  guint services_detected;
  guint name_owner_cb_count;

  /* Debug to show upon creation */
  gchar *select_name;

  /* Misc. */
  gboolean dispose_run;
  TpAccountManager *am;
  GtkListStore *all_active_buffer;
};

static const gchar *
log_level_to_string (GLogLevelFlags level)
{
  switch (level)
    {
    case G_LOG_LEVEL_ERROR:
      return "Error";
      break;
    case G_LOG_LEVEL_CRITICAL:
      return "Critical";
      break;
    case G_LOG_LEVEL_WARNING:
      return "Warning";
      break;
    case G_LOG_LEVEL_MESSAGE:
      return "Message";
      break;
    case G_LOG_LEVEL_INFO:
      return "Info";
      break;
    case G_LOG_LEVEL_DEBUG:
      return "Debug";
      break;
    default:
      g_assert_not_reached ();
      break;
    }
}

static gchar *
get_active_service_name (EmpathyDebugWindow *self)
{
  GtkTreeIter iter;
  gchar *name;

  if (!gtk_combo_box_get_active_iter (GTK_COMBO_BOX (self->priv->chooser),
        &iter))
    return NULL;

  gtk_tree_model_get (GTK_TREE_MODEL (self->priv->service_store), &iter,
      COL_NAME, &name, -1);

  return name;
}

static gboolean
copy_buffered_messages (GtkTreeModel *buffer,
    GtkTreePath *path,
    GtkTreeIter *iter,
    gpointer data)
{
  GtkListStore *active_buffer = data;
  GtkTreeIter active_buffer_iter;
  TpDebugMessage *msg;

  gtk_tree_model_get (buffer, iter,
      COL_DEBUG_MESSAGE, &msg,
      -1);
  gtk_list_store_insert_with_values (active_buffer, &active_buffer_iter, -1,
      COL_DEBUG_MESSAGE, msg,
      -1);

  g_object_unref (msg);

  return FALSE;
}

static void
insert_values_in_buffer (GtkListStore *store,
    TpDebugMessage *msg)
{
  GtkTreeIter iter;

  gtk_list_store_insert_with_values (store, &iter, -1,
      COL_DEBUG_MESSAGE, msg,
      -1);
}

static void
debug_window_add_message (EmpathyDebugWindow *self,
    TpDebugClient *debug,
    TpDebugMessage *msg)
{
  GtkListStore *active_buffer, *pause_buffer;

  pause_buffer = g_object_get_data (G_OBJECT (debug), "pause-buffer");
  active_buffer = g_object_get_data (G_OBJECT (debug), "active-buffer");

  if (self->priv->paused)
    {
      insert_values_in_buffer (pause_buffer, msg);
    }
  else
    {
      /* Append 'this' message to this service's and All's active-buffers */
      insert_values_in_buffer (active_buffer, msg);

      insert_values_in_buffer (self->priv->all_active_buffer, msg);
    }
}

static void
debug_window_new_debug_message_cb (TpDebugClient *debug,
    TpDebugMessage *msg,
    gpointer user_data)
{
  EmpathyDebugWindow *self = user_data;

  debug_window_add_message (self, debug, msg);
}

static void
set_enabled_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  TpDebugClient *debug = TP_DEBUG_CLIENT (source);
  gboolean enabled = GPOINTER_TO_UINT (user_data);
  GError *error = NULL;

  if (!tp_debug_client_set_enabled_finish (debug, result, &error))
    {
      DEBUG ("Failed to %s debugging on %s", enabled ? "enable" : "disable",
          tp_proxy_get_bus_name (debug));
      g_error_free (error);
    }
}

static void
debug_window_set_enabled (TpDebugClient *debug,
    gboolean enabled)
{
  g_return_if_fail (debug != NULL);

  tp_debug_client_set_enabled_async (debug, enabled,
      set_enabled_cb, GUINT_TO_POINTER (enabled));
}

static void
debug_window_set_toolbar_sensitivity (EmpathyDebugWindow *self,
    gboolean sensitive)
{
  GtkWidget *vbox = gtk_bin_get_child (GTK_BIN (self));

  gtk_widget_set_sensitive (GTK_WIDGET (self->priv->save_button), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (self->priv->send_to_pastebin),
      sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (self->priv->copy_button), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (self->priv->clear_button), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (self->priv->pause_button), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (self->priv->level_label), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (self->priv->level_filter), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (self->priv->view), sensitive);

  if (sensitive && !self->priv->view_visible)
    {
      /* Add view and remove label */
      gtk_container_remove (GTK_CONTAINER (vbox),
          self->priv->not_supported_label);
      gtk_box_pack_start (GTK_BOX (vbox),
          self->priv->scrolled_win, TRUE, TRUE, 0);
      self->priv->view_visible = TRUE;
    }
  else if (!sensitive && self->priv->view_visible)
    {
      /* Add label and remove view */
      gtk_container_remove (GTK_CONTAINER (vbox), self->priv->scrolled_win);
      gtk_box_pack_start (GTK_BOX (vbox), self->priv->not_supported_label,
          TRUE, TRUE, 0);
      self->priv->view_visible = FALSE;
    }
}

static gboolean
debug_window_get_iter_for_active_buffer (GtkListStore *active_buffer,
    GtkTreeIter *iter,
    EmpathyDebugWindow *self)
{
  gboolean valid_iter;
  GtkTreeModel *model = GTK_TREE_MODEL (self->priv->service_store);

  gtk_tree_model_get_iter_first (model, iter);
  for (valid_iter = gtk_tree_model_iter_next (model, iter);
       valid_iter;
       valid_iter = gtk_tree_model_iter_next (model, iter))
    {
      GtkListStore *stored_active_buffer;

      gtk_tree_model_get (model, iter,
          COL_ACTIVE_BUFFER, &stored_active_buffer,
          -1);
      if (active_buffer == stored_active_buffer)
        {
          g_object_unref (stored_active_buffer);
          return valid_iter;
        }
      g_object_unref (stored_active_buffer);
    }

  return valid_iter;
}

static void refresh_all_buffer (EmpathyDebugWindow *self);

static void
proxy_invalidated_cb (TpProxy *proxy,
    guint domain,
    gint code,
    gchar *msg,
    gpointer user_data)
{
  EmpathyDebugWindow *self = (EmpathyDebugWindow *) user_data;
  GtkTreeModel *service_store;
  TpProxy *stored_proxy;
  GtkTreeIter iter;
  gboolean valid_iter;

  if (self->priv->service_store == NULL)
    return;

  service_store = GTK_TREE_MODEL (self->priv->service_store);

  /* Proxy has been invalidated so we find and set it to NULL
   * in service store */
  gtk_tree_model_get_iter_first (service_store, &iter);
  for (valid_iter = gtk_tree_model_iter_next (service_store, &iter);
       valid_iter;
       valid_iter = gtk_tree_model_iter_next (service_store, &iter))
    {
      gtk_tree_model_get (service_store, &iter,
          COL_PROXY, &stored_proxy,
          -1);

      if (proxy == stored_proxy)
        gtk_list_store_set (self->priv->service_store, &iter,
            COL_PROXY, NULL,
            -1);

      g_object_unref (stored_proxy);
    }

  /* Also, we refresh "All" selection's active buffer since it should not
   * show messages obtained from the proxy getting destroyed above */
  refresh_all_buffer (self);
}

static void
debug_window_get_messages_cb (GObject *object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpDebugClient *debug = TP_DEBUG_CLIENT (object);
  EmpathyDebugWindow *self = user_data;
  gchar *active_service_name;
  guint i;
  GtkListStore *active_buffer;
  gboolean valid_iter;
  GtkTreeIter iter;
  gchar *proxy_service_name;
  GPtrArray *messages;
  GError *error = NULL;

  active_buffer = g_object_get_data (object, "active-buffer");
  valid_iter = debug_window_get_iter_for_active_buffer (active_buffer, &iter,
      self);
  gtk_tree_model_get (GTK_TREE_MODEL (self->priv->service_store), &iter,
      COL_NAME, &proxy_service_name,
      -1);

  active_service_name = get_active_service_name (self);

  messages = tp_debug_client_get_messages_finish (debug, result, &error);
  if (messages == NULL)
    {
      DEBUG ("Failed to get debug messages: %s", error->message);
      g_error_free (error);

      /* We want to set the window sensitivity to false only when proxy for the
       * selected service is unable to fetch debug messages */
      if (!tp_strdiff (active_service_name, proxy_service_name))
        debug_window_set_toolbar_sensitivity (self, FALSE);
      return;
    }

  DEBUG ("Retrieved debug messages for %s", active_service_name);
  g_free (active_service_name);
  debug_window_set_toolbar_sensitivity (self, TRUE);

  for (i = 0; i < messages->len; i++)
    {
      TpDebugMessage *msg = g_ptr_array_index (messages, i);

      debug_window_add_message (self, debug, msg);
    }

  /* Now we save this precious proxy in the service_store along its service */
  if (valid_iter)
    {
      DEBUG ("Proxy for service: %s was successful in fetching debug"
          " messages. Saving it.", proxy_service_name);

      /* The store will take its own ref on the proxy preventing it to be
       * destroyed when leaving this callback. */
      gtk_list_store_set (self->priv->service_store, &iter,
          COL_PROXY, debug,
          -1);
    }
  g_ptr_array_unref (messages);

  g_free (proxy_service_name);

  /* Connect to "invalidated" signal */
  g_signal_connect_object (debug, "invalidated",
      G_CALLBACK (proxy_invalidated_cb), self, 0);

 /* Connect to NewDebugMessage */
  tp_g_signal_connect_object (debug, "new-debug-message",
      G_CALLBACK (debug_window_new_debug_message_cb), self, 0);

  /* Now that active-buffer is up to date, we can see which messages are
   * to be visible */
  gtk_tree_model_filter_refilter (GTK_TREE_MODEL_FILTER (
        self->priv->store_filter));

  /* Set the proxy to signal for new debug messages */
  debug_window_set_enabled (debug, TRUE);
}

static void
create_proxy_to_get_messages (EmpathyDebugWindow *self,
    GtkTreeIter *iter,
    TpDBusDaemon *dbus)
{
  gchar *bus_name, *name = NULL;
  TpDebugClient *new_proxy, *stored_proxy = NULL;
  GtkTreeModel *pause_buffer, *active_buffer;
  gboolean gone;
  GError *error = NULL;

  gtk_tree_model_get (GTK_TREE_MODEL (self->priv->service_store), iter,
      COL_NAME, &name,
      COL_GONE, &gone,
      COL_ACTIVE_BUFFER, &active_buffer,
      COL_PAUSE_BUFFER, &pause_buffer,
      COL_PROXY, &stored_proxy,
      -1);

  /* If the stored_proxy is not NULL then messages have been obtained and
   * new-debug-message-signal has been set on it. Also, the proxy is valid.
   * If the service is gone, we still display the messages-cached till now. */
  if (gone ||
      (!gone && stored_proxy != NULL))
    {
      /* Nothing needs to be done. The associated active-buffer has already
       * been set as view's model */
      goto finally;
    }

  DEBUG ("Preparing proxy to obtain messages for service %s", name);

  gtk_tree_model_get (GTK_TREE_MODEL (self->priv->service_store), iter,
      COL_UNIQUE_NAME, &bus_name, -1);

  new_proxy = tp_debug_client_new (dbus, bus_name, &error);

  if (new_proxy == NULL)
    {
      DEBUG ("Failed to create TpDebugClient on bus %s: %s", bus_name,
          error->message);
      g_free (bus_name);
      goto finally;
    }

  g_free (bus_name);

  g_object_set_data (G_OBJECT (new_proxy), "active-buffer", active_buffer);
  g_object_set_data (G_OBJECT (new_proxy), "pause-buffer", pause_buffer);

  /* Now we call GetMessages with fresh proxy.
   * The old proxy is NULL due to one of the following -
   * * Wasn't saved as last GetMessages call failed
   * * The service has newly arrived and no proxy has been prepared yet for it
   * * A service with the same name has reappeared but the owner maybe new */

  tp_debug_client_get_messages_async (TP_DEBUG_CLIENT (new_proxy),
      debug_window_get_messages_cb, self);

  g_object_unref (new_proxy);

finally:
  g_free (name);
  tp_clear_object (&stored_proxy);
  g_object_unref (active_buffer);
  g_object_unref (pause_buffer);
}

static GtkListStore *
new_list_store_for_service (void)
{
  return gtk_list_store_new (NUM_DEBUG_COLS,
             TP_TYPE_DEBUG_MESSAGE); /* COL_DEBUG_MESSAGE */
}

static gboolean
debug_window_visible_func (GtkTreeModel *model,
    GtkTreeIter *iter,
    gpointer user_data)
{
  EmpathyDebugWindow *self = user_data;
  GLogLevelFlags filter_value;
  GtkTreeModel *filter_model;
  GtkTreeIter filter_iter;
  TpDebugMessage *msg;
  gboolean result;

  filter_model = gtk_combo_box_get_model (
      GTK_COMBO_BOX (self->priv->level_filter));
  gtk_combo_box_get_active_iter (GTK_COMBO_BOX (self->priv->level_filter),
      &filter_iter);

  gtk_tree_model_get (model, iter, COL_DEBUG_MESSAGE, &msg, -1);
  gtk_tree_model_get (filter_model, &filter_iter,
      COL_LEVEL_VALUE, &filter_value, -1);

  result = (tp_debug_message_get_level (msg) <= filter_value);
  g_object_unref (msg);

  return result;
}

static gboolean
tree_view_search_equal_func_cb (GtkTreeModel *model,
    gint column,
    const gchar *key,
    GtkTreeIter *iter,
    gpointer search_data)
{
  gchar *str;
  gint key_len;
  gint len;
  gint i;
  gboolean ret = TRUE; /* The return value is counter-intuitive */

  gtk_tree_model_get (model, iter, column, &str, -1);

  key_len = strlen (key);
  len = strlen (str) - key_len;

  for (i = 0; i <= len; ++i)
    {
      if (!g_ascii_strncasecmp (key, str + i, key_len))
        {
          ret = FALSE;
          break;
        }
    }

  g_free (str);
  return ret;
}

static void
update_store_filter (EmpathyDebugWindow *self,
    GtkListStore *active_buffer)
{
  debug_window_set_toolbar_sensitivity (self, FALSE);

  tp_clear_object (&self->priv->store_filter);
  self->priv->store_filter = gtk_tree_model_filter_new (
      GTK_TREE_MODEL (active_buffer), NULL);

  gtk_tree_model_filter_set_visible_func (
      GTK_TREE_MODEL_FILTER (self->priv->store_filter),
      debug_window_visible_func, self, NULL);
  gtk_tree_view_set_model (GTK_TREE_VIEW (self->priv->view),
      self->priv->store_filter);

  /* Since view's model has changed, reset the search column and
   * search_equal_func */
  gtk_tree_view_set_search_column (GTK_TREE_VIEW (self->priv->view),
      COL_DEBUG_MESSAGE);
  gtk_tree_view_set_search_equal_func (GTK_TREE_VIEW (self->priv->view),
      tree_view_search_equal_func_cb, NULL, NULL);

  debug_window_set_toolbar_sensitivity (self, TRUE);
}

static void
refresh_all_buffer (EmpathyDebugWindow *self)
{
  gboolean valid_iter;
  GtkTreeIter iter;
  GtkTreeModel *service_store = GTK_TREE_MODEL (self->priv->service_store);

  /* Clear All's active-buffer */
  gtk_list_store_clear (self->priv->all_active_buffer);

  /* Skipping the first service store iter which is reserved for "All" */
  gtk_tree_model_get_iter_first (service_store, &iter);
  for (valid_iter = gtk_tree_model_iter_next (service_store, &iter);
       valid_iter;
       valid_iter = gtk_tree_model_iter_next (service_store, &iter))
    {
      TpProxy *proxy = NULL;
      GtkListStore *service_active_buffer;
      gboolean gone;

      gtk_tree_model_get (service_store, &iter,
          COL_GONE, &gone,
          COL_PROXY, &proxy,
          COL_ACTIVE_BUFFER, &service_active_buffer,
          -1);

      if (gone)
        {
          gtk_tree_model_foreach (GTK_TREE_MODEL (service_active_buffer),
              copy_buffered_messages, self->priv->all_active_buffer);
        }
      else
        {
          if (proxy != NULL)
            {
              if (service_active_buffer == NULL)
                break;

              /* Copy the debug messages to all_active_buffer */
              gtk_tree_model_foreach (GTK_TREE_MODEL (service_active_buffer),
                  copy_buffered_messages, self->priv->all_active_buffer);
            }
          else
            {
              GError *error = NULL;
              TpDBusDaemon *dbus = tp_dbus_daemon_dup (&error);

              if (error != NULL)
                {
                  DEBUG ("Failed at duping the dbus daemon: %s", error->message);
                  g_error_free (error);
                }

              create_proxy_to_get_messages (self, &iter, dbus);

              g_object_unref (dbus);
            }
        }

      g_object_unref (service_active_buffer);
      tp_clear_object (&proxy);
    }
}

static void
debug_window_service_chooser_changed_cb (GtkComboBox *chooser,
    EmpathyDebugWindow *self)
{
  TpDBusDaemon *dbus;
  GError *error = NULL;
  GtkListStore *stored_active_buffer = NULL;
  gchar *name = NULL;
  GtkTreeIter iter;
  gboolean gone;

  if (!gtk_combo_box_get_active_iter (chooser, &iter))
    {
      DEBUG ("No CM is selected");
      if (gtk_tree_model_iter_n_children (
          GTK_TREE_MODEL (self->priv->service_store), NULL) > 0)
        {
          gtk_combo_box_set_active (chooser, 0);
        }
      return;
    }

  debug_window_set_toolbar_sensitivity (self, TRUE);

  gtk_tree_model_get (GTK_TREE_MODEL (self->priv->service_store), &iter,
      COL_NAME, &name,
      COL_GONE, &gone,
      COL_ACTIVE_BUFFER, &stored_active_buffer,
      -1);

  DEBUG ("Service chosen: %s", name);

  if (tp_strdiff (name, "All") && stored_active_buffer == NULL)
    {
      DEBUG ("No list store assigned to service %s", name);
      goto finally;
    }

  if (!tp_strdiff (name, "All"))
    {
      update_store_filter (self, self->priv->all_active_buffer);
      goto finally;
    }

  update_store_filter (self, stored_active_buffer);

  dbus = tp_dbus_daemon_dup (&error);

  if (error != NULL)
    {
      DEBUG ("Failed at duping the dbus daemon: %s", error->message);
    }

  create_proxy_to_get_messages (self, &iter, dbus);

  g_object_unref (dbus);

finally:
  g_free (name);
  tp_clear_object (&stored_active_buffer);
}

typedef struct
{
  const gchar *name;
  gboolean found;
  gboolean use_name;
  GtkTreeIter **found_iter;
} CmInModelForeachData;

static gboolean
debug_window_service_foreach (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    gpointer user_data)
{
  CmInModelForeachData *data = (CmInModelForeachData *) user_data;
  gchar *store_name;

  gtk_tree_model_get (model, iter,
      (data->use_name ? COL_NAME : COL_UNIQUE_NAME),
      &store_name,
      -1);

  if (!tp_strdiff (store_name, data->name))
    {
      data->found = TRUE;

      if (data->found_iter != NULL)
        *(data->found_iter) = gtk_tree_iter_copy (iter);
    }

  g_free (store_name);

  return data->found;
}

static gboolean
debug_window_service_is_in_model (EmpathyDebugWindow *self,
    const gchar *name,
    GtkTreeIter **iter,
    gboolean use_name)
{
  CmInModelForeachData *data;
  gboolean found;

  data = g_slice_new0 (CmInModelForeachData);
  data->name = name;
  data->found = FALSE;
  data->found_iter = iter;
  data->use_name = use_name;

  gtk_tree_model_foreach (GTK_TREE_MODEL (self->priv->service_store),
      debug_window_service_foreach, data);

  found = data->found;

  g_slice_free (CmInModelForeachData, data);

  return found;
}

static gchar *
get_cm_display_name (EmpathyDebugWindow *self,
    const char *cm_name)
{
  GHashTable *protocols = g_hash_table_new (g_str_hash, g_str_equal);
  GList *accounts, *ptr;
  char *retval;

  accounts = tp_account_manager_dup_valid_accounts (self->priv->am);

  for (ptr = accounts; ptr != NULL; ptr = ptr->next)
    {
      TpAccount *account = TP_ACCOUNT (ptr->data);

      if (!tp_strdiff (tp_account_get_cm_name (account), cm_name))
        {
          g_hash_table_insert (protocols,
              (char *) tp_account_get_protocol_name (account),
              GUINT_TO_POINTER (TRUE));
        }
    }

  g_list_free_full (accounts, g_object_unref);

  if (g_hash_table_size (protocols) > 0)
    {
      GHashTableIter iter;
      char **protocolsv;
      char *key, *str;
      guint i;

      protocolsv = g_new0 (char *, g_hash_table_size (protocols) + 1);

      g_hash_table_iter_init (&iter, protocols);
      for (i = 0; g_hash_table_iter_next (&iter, (gpointer) &key, NULL); i++)
        {
          protocolsv[i] = key;
        }

      str = g_strjoinv (", ", protocolsv);
      retval = g_strdup_printf ("%s (%s)", cm_name, str);

      g_free (protocolsv);
      g_free (str);
    }
  else
    {
      retval = g_strdup (cm_name);
    }

  g_hash_table_unref (protocols);

  return retval;
}

typedef struct
{
  EmpathyDebugWindow *self;
  gchar *name;
  ServiceType type;
} FillServiceChooserData;

static FillServiceChooserData *
fill_service_chooser_data_new (EmpathyDebugWindow *window,
    const gchar *name,
    ServiceType type)
{
  FillServiceChooserData * data = g_slice_new (FillServiceChooserData);

  data->self = window;
  data->name = g_strdup (name);
  data->type = SERVICE_TYPE_CM;
  return data;
}

static void
fill_service_chooser_data_free (FillServiceChooserData *data)
{
  g_free (data->name);
  g_slice_free (FillServiceChooserData, data);
}

static const gchar *
service_type_to_string (ServiceType type)
{
  switch (type)
    {
      case SERVICE_TYPE_CM:
        return "CM";
      case SERVICE_TYPE_CLIENT:
        return "Client";
      case SERVICE_TYPE_MC:
        return "MC";
    }

  return "other";
}

static gchar *
service_dup_display_name (EmpathyDebugWindow *self,
    ServiceType type,
    const gchar *name)
{
  if (type == SERVICE_TYPE_CM)
    return get_cm_display_name (self, name);
  else
    return g_strdup (name);
}

static void
debug_window_get_name_owner_cb (TpDBusDaemon *proxy,
    const gchar *out,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  FillServiceChooserData *data = (FillServiceChooserData *) user_data;
  EmpathyDebugWindow *self = EMPATHY_DEBUG_WINDOW (data->self);
  GtkTreeIter iter;

  self->priv->name_owner_cb_count++;

  if (error != NULL)
    {
      DEBUG ("GetNameOwner failed: %s", error->message);
      goto OUT;
    }

  if (!debug_window_service_is_in_model (data->self, out, NULL, FALSE))
    {
      char *name;
      GtkListStore *active_buffer, *pause_buffer;

      DEBUG ("Adding %s to list: %s at unique name: %s",
          service_type_to_string (data->type),
          data->name, out);

      name = service_dup_display_name (self, data->type, data->name);

      active_buffer = new_list_store_for_service ();
      pause_buffer = new_list_store_for_service ();

      gtk_list_store_insert_with_values (self->priv->service_store, &iter, -1,
          COL_NAME, name,
          COL_UNIQUE_NAME, out,
          COL_GONE, FALSE,
          COL_ACTIVE_BUFFER, active_buffer,
          COL_PAUSE_BUFFER, pause_buffer,
          COL_PROXY, NULL,
          -1);

      g_object_unref (active_buffer);
      g_object_unref (pause_buffer);

      if (self->priv->select_name != NULL &&
          !tp_strdiff (name, self->priv->select_name))
        {
          gtk_combo_box_set_active_iter (GTK_COMBO_BOX (self->priv->chooser),
              &iter);
          tp_clear_pointer (&self->priv->select_name, g_free);
        }

      g_free (name);
    }

    if (self->priv->services_detected == self->priv->name_owner_cb_count)
      {
        /* Time to add "All" selection to service_store */
        gtk_list_store_insert_with_values (self->priv->service_store, &iter, 0,
            COL_NAME, "All",
            COL_ACTIVE_BUFFER, NULL,
            -1);

        self->priv->all_active_buffer = new_list_store_for_service ();

        /* Populate active buffers for all services */
        refresh_all_buffer (self);

        gtk_combo_box_set_active (GTK_COMBO_BOX (self->priv->chooser), 0);
      }

OUT:
  fill_service_chooser_data_free (data);
}

static void
debug_window_name_owner_changed_cb (TpDBusDaemon *proxy,
    const gchar *arg0,
    const gchar *arg1,
    const gchar *arg2,
    gpointer user_data,
    GObject *weak_object)
{
  EmpathyDebugWindow *self = EMPATHY_DEBUG_WINDOW (user_data);
  ServiceType type;
  const gchar *name;

  if (g_str_has_prefix (arg0, TP_CM_BUS_NAME_BASE))
    {
      type = SERVICE_TYPE_CM;
      name = arg0 + strlen (TP_CM_BUS_NAME_BASE);
    }
  else if (g_str_has_prefix (arg0, TP_CLIENT_BUS_NAME_BASE))
    {
      type = SERVICE_TYPE_CLIENT;
      name = arg0 + strlen (TP_CLIENT_BUS_NAME_BASE);
    }
  else
    {
      return;
    }

  if (TPAW_STR_EMPTY (arg1) && !TPAW_STR_EMPTY (arg2))
    {
      GtkTreeIter *found_at_iter = NULL;
      gchar *display_name;

      display_name = service_dup_display_name (self, type, name);

      /* A service joined */
      if (!debug_window_service_is_in_model (user_data, display_name,
           &found_at_iter, TRUE))
        {
          GtkTreeIter iter;
          GtkListStore *active_buffer, *pause_buffer;

          DEBUG ("Adding new service '%s' at %s.", name, arg2);

          active_buffer = new_list_store_for_service ();
          pause_buffer = new_list_store_for_service ();

          gtk_list_store_insert_with_values (self->priv->service_store,
              &iter, -1,
              COL_NAME, display_name,
              COL_UNIQUE_NAME, arg2,
              COL_GONE, FALSE,
              COL_ACTIVE_BUFFER, active_buffer,
              COL_PAUSE_BUFFER, pause_buffer,
              COL_PROXY, NULL,
              -1);

          g_object_unref (active_buffer);
          g_object_unref (pause_buffer);
        }
      else
        {
          /* a service with the same name is already in the service_store,
           * update it and set it as re-enabled.
           */
          GtkListStore *active_buffer, *pause_buffer;
          TpProxy *stored_proxy;

          DEBUG ("Refreshing CM '%s' at '%s'.", name, arg2);

          active_buffer= new_list_store_for_service ();
          pause_buffer = new_list_store_for_service ();

          gtk_tree_model_get (GTK_TREE_MODEL (self->priv->service_store),
              found_at_iter, COL_PROXY, &stored_proxy, -1);

          tp_clear_object (&stored_proxy);

          gtk_list_store_set (self->priv->service_store, found_at_iter,
              COL_NAME, display_name,
              COL_UNIQUE_NAME, arg2,
              COL_GONE, FALSE,
              COL_ACTIVE_BUFFER, active_buffer,
              COL_PAUSE_BUFFER, pause_buffer,
              COL_PROXY, NULL,
              -1);

          g_object_unref (active_buffer);
          g_object_unref (pause_buffer);

          gtk_tree_iter_free (found_at_iter);

          debug_window_service_chooser_changed_cb
            (GTK_COMBO_BOX (self->priv->chooser), user_data);
        }

      /* If a new service arrives when "All" is selected, the view will
       * not show its messages which we do not want. So we refresh All's
       * active buffer.
       * Similarly for when a service with an already seen service name
       * appears. */
      refresh_all_buffer (self);

      g_free (display_name);
    }
  else if (!TPAW_STR_EMPTY (arg1) && TPAW_STR_EMPTY (arg2))
    {
      /* A service died */
      GtkTreeIter *iter = NULL;

      DEBUG ("Setting service disabled from %s.", arg1);

      /* set the service as disabled in the model */
      if (debug_window_service_is_in_model (user_data, arg1, &iter, FALSE))
        {
          gtk_list_store_set (self->priv->service_store,
              iter, COL_GONE, TRUE, -1);
          gtk_tree_iter_free (iter);
        }

      /* Refresh all's active buffer */
      refresh_all_buffer (self);
    }
}

static void
add_service (EmpathyDebugWindow *self,
    const gchar *bus_name,
    const gchar *display_name,
    ServiceType type)
{
  FillServiceChooserData *data;

  data = fill_service_chooser_data_new (self, display_name, type);

  tp_cli_dbus_daemon_call_get_name_owner (self->priv->dbus, -1,
      bus_name, debug_window_get_name_owner_cb, data, NULL, NULL);

  self->priv->services_detected ++;
}

static void
list_names_cb (TpDBusDaemon *bus_daemon,
    const gchar * const *names,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  EmpathyDebugWindow *self = EMPATHY_DEBUG_WINDOW (weak_object);
  guint i;

  if (error != NULL)
    {
      DEBUG ("Failed to list names: %s", error->message);
      return;
    }

  for (i = 0; names[i] != NULL; i++)
    {
      if (g_str_has_prefix (names[i], TP_CLIENT_BUS_NAME_BASE))
        {
          add_service (self, names[i],
              names[i] + strlen (TP_CLIENT_BUS_NAME_BASE), SERVICE_TYPE_CLIENT);
        }
      else if (g_str_has_prefix (names[i], TP_CM_BUS_NAME_BASE))
        {
          add_service (self, names[i],
              names[i] + strlen (TP_CM_BUS_NAME_BASE), SERVICE_TYPE_CM);
        }
      else if (!tp_strdiff (names[i], TP_ACCOUNT_MANAGER_BUS_NAME))
        {
          add_service (self, names[i], "Mission-Control", SERVICE_TYPE_MC);
        }
    }
}

static void
debug_window_fill_service_chooser (EmpathyDebugWindow *self)
{
  GError *error = NULL;

  self->priv->dbus = tp_dbus_daemon_dup (&error);

  if (error != NULL)
    {
      DEBUG ("Failed to dup dbus daemon: %s", error->message);
      g_error_free (error);
      return;
    }

  /* Keep a count of the services detected and added */
  self->priv->services_detected = 0;
  self->priv->name_owner_cb_count = 0;

  tp_dbus_daemon_list_names (self->priv->dbus, 2000,
      list_names_cb, NULL, NULL, G_OBJECT (self));

  self->priv->name_owner_changed_signal =
      tp_cli_dbus_daemon_connect_to_name_owner_changed (self->priv->dbus,
      debug_window_name_owner_changed_cb, self, NULL, NULL, NULL);
}

static void
debug_window_pause_toggled_cb (GtkToggleToolButton *pause_,
    EmpathyDebugWindow *self)
{
  GtkTreeIter iter;
  gboolean valid_iter;
  GtkTreeModel *model = GTK_TREE_MODEL (self->priv->service_store);

  self->priv->paused = gtk_toggle_tool_button_get_active (pause_);

  if (!self->priv->paused)
    {
      /* Pause has been released - flush all pause buffers */
      GtkTreeModel *service_store = GTK_TREE_MODEL (self->priv->service_store);

      /* Skipping the first iter which is reserved for "All" */
      gtk_tree_model_get_iter_first (model, &iter);
      for (valid_iter = gtk_tree_model_iter_next (model, &iter);
           valid_iter;
           valid_iter = gtk_tree_model_iter_next (model, &iter))
        {
          GtkListStore *pause_buffer, *active_buffer;

          gtk_tree_model_get (service_store, &iter,
              COL_PAUSE_BUFFER, &pause_buffer,
              COL_ACTIVE_BUFFER, &active_buffer,
              -1);

          gtk_tree_model_foreach (GTK_TREE_MODEL (pause_buffer),
              copy_buffered_messages, active_buffer);
          gtk_tree_model_foreach (GTK_TREE_MODEL (pause_buffer),
              copy_buffered_messages, self->priv->all_active_buffer);

          gtk_list_store_clear (pause_buffer);

          g_object_unref (active_buffer);
          g_object_unref (pause_buffer);
        }
    }
}

static void
debug_window_filter_changed_cb (GtkComboBox *filter,
    EmpathyDebugWindow *self)
{
  gtk_tree_model_filter_refilter (
      GTK_TREE_MODEL_FILTER (self->priv->store_filter));
}

static void
debug_window_clear_clicked_cb (GtkToolButton *clear_button,
    EmpathyDebugWindow *self)
{
  GtkTreeIter iter;
  GtkListStore *active_buffer;

  /* "All" is the first choice in the service chooser and it's buffer is
   * not saved in the service-store but is accessed using a self->private
   * reference */
  if (gtk_combo_box_get_active (GTK_COMBO_BOX (self->priv->chooser)) == 0)
    {
      gtk_list_store_clear (self->priv->all_active_buffer);
      return;
    }

  gtk_combo_box_get_active_iter (GTK_COMBO_BOX (self->priv->chooser), &iter);
  gtk_tree_model_get (GTK_TREE_MODEL (self->priv->service_store), &iter,
      COL_ACTIVE_BUFFER, &active_buffer, -1);

  gtk_list_store_clear (active_buffer);

  g_object_unref (active_buffer);
}

static void
debug_window_menu_copy_activate_cb (GtkMenuItem *menu_item,
    EmpathyDebugWindow *self)
{
  GtkTreePath *path;
  GtkTreeViewColumn *focus_column;
  GtkTreeIter iter;
  TpDebugMessage *msg;
  const gchar *message;
  GtkClipboard *clipboard;

  gtk_tree_view_get_cursor (GTK_TREE_VIEW (self->priv->view),
      &path, &focus_column);

  if (path == NULL)
    {
      DEBUG ("No row is in focus");
      return;
    }

  gtk_tree_model_get_iter (self->priv->store_filter, &iter, path);

  gtk_tree_model_get (self->priv->store_filter, &iter,
      COL_DEBUG_MESSAGE, &msg,
      -1);

  message = tp_debug_message_get_message (msg);

  if (TPAW_STR_EMPTY (message))
    {
      DEBUG ("Log message is empty");
      return;
    }

  clipboard = gtk_clipboard_get_for_display (
      gtk_widget_get_display (GTK_WIDGET (menu_item)),
      GDK_SELECTION_CLIPBOARD);

  gtk_clipboard_set_text (clipboard, message, -1);

  g_object_unref (msg);
}

typedef struct
{
  EmpathyDebugWindow *self;
  guint button;
  guint32 time;
} MenuPopupData;

static gboolean
debug_window_show_menu (gpointer user_data)
{
  MenuPopupData *data = (MenuPopupData *) user_data;
  GtkWidget *menu, *item;
  GtkMenuShell *shell;

  menu = empathy_context_menu_new (GTK_WIDGET (data->self));
  shell = GTK_MENU_SHELL (menu);

  item = gtk_image_menu_item_new_from_stock (GTK_STOCK_COPY, NULL);

  g_signal_connect (item, "activate",
      G_CALLBACK (debug_window_menu_copy_activate_cb), data->self);

  gtk_menu_shell_append (shell, item);
  gtk_widget_show (item);

  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL,
     data->button, data->time);

  g_slice_free (MenuPopupData, user_data);

  return FALSE;
}

static gboolean
debug_window_button_press_event_cb (GtkTreeView *view,
    GdkEventButton *event,
    gpointer user_data)
{
  /* A mouse button was pressed on the tree view. */

  if (event->button == 3)
    {
      /* The tree view was right-clicked. (3 == third mouse button) */
      MenuPopupData *data;
      data = g_slice_new0 (MenuPopupData);
      data->self = user_data;
      data->button = event->button;
      data->time = event->time;
      g_idle_add (debug_window_show_menu, data);
    }

  return FALSE;
}

static gchar *
debug_window_format_timestamp (TpDebugMessage *msg)
{
  GDateTime *t;
  gchar *time_str, *text;
  gint ms;

  t = tp_debug_message_get_time (msg);

  time_str = g_date_time_format (t, "%x %T");

  ms = g_date_time_get_microsecond (t);
  text = g_strdup_printf ("%s.%d", time_str, ms);

  g_free (time_str);
  return text;
}

static void
debug_window_time_formatter (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *tree_model,
    GtkTreeIter *iter,
    gpointer data)
{
  TpDebugMessage *msg;
  gchar *time_str;

  gtk_tree_model_get (tree_model, iter, COL_DEBUG_MESSAGE, &msg, -1);

  time_str = debug_window_format_timestamp (msg);

  g_object_set (G_OBJECT (cell), "text", time_str, NULL);

  g_object_unref (msg);
}

static void
debug_window_domain_formatter (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *tree_model,
    GtkTreeIter *iter,
    gpointer data)
{
  TpDebugMessage *msg;

  gtk_tree_model_get (tree_model, iter, COL_DEBUG_MESSAGE, &msg, -1);

  g_object_set (G_OBJECT (cell), "text", tp_debug_message_get_domain (msg),
      NULL);

  g_object_unref (msg);
}

static void
debug_window_category_formatter (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *tree_model,
    GtkTreeIter *iter,
    gpointer data)
{
  TpDebugMessage *msg;
  const gchar *category;

  gtk_tree_model_get (tree_model, iter, COL_DEBUG_MESSAGE, &msg, -1);

  category = tp_debug_message_get_category (msg);

  g_object_set (G_OBJECT (cell), "text", category ? category : "", NULL);

  g_object_unref (msg);
}

static void
debug_window_message_formatter (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *tree_model,
    GtkTreeIter *iter,
    gpointer data)
{
  TpDebugMessage *msg;

  gtk_tree_model_get (tree_model, iter, COL_DEBUG_MESSAGE, &msg, -1);

  g_object_set (G_OBJECT (cell), "text",
      tp_debug_message_get_message (msg), NULL);

  g_object_unref (msg);
}

static void
debug_window_level_formatter (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *tree_model,
    GtkTreeIter *iter,
    gpointer data)
{
  TpDebugMessage *msg;
  const gchar *level;

  gtk_tree_model_get (tree_model, iter, COL_DEBUG_MESSAGE, &msg, -1);

  level = log_level_to_string (tp_debug_message_get_level (msg));

  g_object_set (G_OBJECT (cell), "text", level, NULL);

  g_object_unref (msg);
}

static gboolean
debug_window_copy_model_foreach (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    gpointer user_data)
{
  gchar **text = (gchar **) user_data;
  gchar *tmp;
  gchar *level_upper;
  const gchar *level_str, *category;
  gchar *line, *time_str;
  TpDebugMessage *msg;

  if (*text == NULL)
    *text = g_strdup ("");

  gtk_tree_model_get (model, iter,
      COL_DEBUG_MESSAGE, &msg,
      -1);

  level_str = log_level_to_string (tp_debug_message_get_level (msg));
  level_upper = g_ascii_strup (level_str, -1);

  time_str = debug_window_format_timestamp (msg);
  category = tp_debug_message_get_category (msg);

  line = g_strdup_printf ("%s%s%s-%s: %s: %s\n",
      tp_debug_message_get_domain (msg),
      category ? "" : "/", category ? category : "",
      level_upper, time_str, tp_debug_message_get_message (msg));

  g_free (time_str);

  tmp = g_strconcat (*text, line, NULL);

  g_free (*text);
  g_free (line);
  g_free (level_upper);
  g_object_unref (msg);

  *text = tmp;

  return FALSE;
}

static void
debug_window_save_file_chooser_response_cb (GtkDialog *dialog,
    gint response_id,
    EmpathyDebugWindow *self)
{
  gchar *filename = NULL;
  GFile *gfile = NULL;
  gchar *debug_data = NULL;
  GFileOutputStream *output_stream = NULL;
  GError *file_open_error = NULL;
  GError *file_write_error = NULL;

  if (response_id != GTK_RESPONSE_ACCEPT)
    goto OUT;

  filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));

  DEBUG ("Saving log as %s", filename);

  gfile = g_file_new_for_path (filename);
  output_stream = g_file_replace (gfile, NULL, FALSE,
      G_FILE_CREATE_NONE, NULL, &file_open_error);

  if (file_open_error != NULL)
    {
      DEBUG ("Failed to open file for writing: %s", file_open_error->message);
      g_error_free (file_open_error);
      goto OUT;
    }

  gtk_tree_model_foreach (self->priv->store_filter,
      debug_window_copy_model_foreach, &debug_data);

  g_output_stream_write (G_OUTPUT_STREAM (output_stream), debug_data,
      strlen (debug_data), NULL, &file_write_error);
  g_free (debug_data);

  if (file_write_error != NULL)
    {
      DEBUG ("Failed to write to file: %s", file_write_error->message);
      g_error_free (file_write_error);
    }

OUT:
  if (gfile != NULL)
    g_object_unref (gfile);

  if (output_stream != NULL)
    g_object_unref (output_stream);

  if (filename != NULL)
    g_free (filename);

  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
debug_window_save_clicked_cb (GtkToolButton *tool_button,
    EmpathyDebugWindow *self)
{
  GtkWidget *file_chooser;
  gchar *name, *tmp = NULL;
  char time_str[32];
  time_t t;
  struct tm *tm_s;

  file_chooser = gtk_file_chooser_dialog_new (_("Save"),
      GTK_WINDOW (self), GTK_FILE_CHOOSER_ACTION_SAVE,
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
      NULL);

  gtk_window_set_modal (GTK_WINDOW (file_chooser), TRUE);
  gtk_file_chooser_set_do_overwrite_confirmation (
      GTK_FILE_CHOOSER (file_chooser), TRUE);

  gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (file_chooser),
      g_get_home_dir ());

  name = get_active_service_name (self);

  t = time (NULL);
  tm_s = localtime (&t);
  if (tm_s != NULL)
    {
      if (strftime (time_str, sizeof (time_str), "%d-%m-%y_%H-%M-%S", tm_s))
        tmp = g_strdup_printf ("%s-%s.log", name, time_str);
    }

  if (tmp == NULL)
    tmp = g_strdup_printf ("%s.log", name);
  g_free (name);

  gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (file_chooser), tmp);
  g_free (tmp);

  g_signal_connect (file_chooser, "response",
      G_CALLBACK (debug_window_save_file_chooser_response_cb),
      self);

  gtk_widget_show (file_chooser);
}

static void
debug_window_pastebin_response_dialog_closed_cb (GtkDialog *dialog,
    gint response_id,
    SoupBuffer *buffer)
{
  soup_buffer_free (buffer);

  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
debug_window_pastebin_callback (SoupSession *session,
    SoupMessage *msg,
    gpointer self)
{
  GtkWidget *dialog;
  SoupBuffer *buffer;

  buffer = soup_message_body_flatten (msg->response_body);
  if (g_str_has_prefix (buffer->data, "http://pastebin.com/"))
    {
      dialog = gtk_message_dialog_new (GTK_WINDOW (self),
          GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
          _("Pastebin link"));

      gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog),
          "<a href=\"%s\">%s</a>", buffer->data, buffer->data);
    }
  else
    {
      dialog = gtk_message_dialog_new (GTK_WINDOW (self),
          GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
          _("Pastebin response"));

      if (!tp_str_empty (buffer->data))
        gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog),
            "%s", buffer->data);
      else
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
            _("Data too large for a single paste. Please save logs to file."));
    }

  g_object_unref (session);

  gtk_window_set_transient_for (GTK_WINDOW (dialog), self);

  gtk_widget_show_all (GTK_WIDGET (dialog));

  g_signal_connect_after (dialog, "response", G_CALLBACK (
      debug_window_pastebin_response_dialog_closed_cb), buffer);
}

static void
debug_window_message_dialog (EmpathyDebugWindow *self,
    const gchar *primary_text,
    const gchar *secondary_text)
{
  GtkWidget *dialog;

  dialog = gtk_message_dialog_new (GTK_WINDOW (self),
      GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
      "%s", _(primary_text));
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
      "%s", _(secondary_text));
  gtk_window_set_transient_for (GTK_WINDOW (dialog),
      GTK_WINDOW (self));

  gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);
}

static void
debug_window_send_to_pastebin (EmpathyDebugWindow *self,
    gchar *debug_data)
{
  SoupSession *session;
  SoupMessage *msg;
  gchar       *api_dev_key, *api_paste_code, *api_paste_name, *formdata;

  if (tp_str_empty (debug_data))
    {
      debug_window_message_dialog (self, "Error", "No data to send");
      return;
    }

  /* Constructing a valid URL for http post. See http://pastebin.com/api#2 */

  /* The api_dev_key is the author's developer key to access the Pastebin API
   * This developer key is published here with the autorization of pastebin;
   * see PASTEBIN-API-KEY.txt */
  api_dev_key = soup_uri_encode ("f6ccfabfdcd4b77b825ee38a30d11d52", NULL);
  api_paste_code = soup_uri_encode (debug_data, NULL);
  api_paste_name = soup_uri_encode ("Empathy debug data", NULL);
  formdata = g_strdup_printf ("api_dev_key=%s&api_paste_code=%s"
      "&api_paste_name=%s&api_paste_format=text&api_option=paste",
      api_dev_key, api_paste_code, api_paste_name);

  session = soup_session_new ();

  msg = soup_message_new ("POST", "http://pastebin.com/api/api_post.php");
  soup_message_set_request (msg,
      "application/x-www-form-urlencoded;charset=UTF-8", SOUP_MEMORY_COPY,
      formdata, strlen (formdata));

  g_free (api_dev_key);
  g_free (api_paste_code);
  g_free (api_paste_name);
  g_free (formdata);

  soup_session_queue_message (session, msg, debug_window_pastebin_callback,
      self);
}

static void
debug_window_send_to_pastebin_cb (GtkToolButton *tool_button,
    EmpathyDebugWindow *self)
{
  gchar *debug_data = NULL;

  DEBUG ("Preparing debug data for sending to pastebin.");

  gtk_tree_model_foreach (self->priv->store_filter,
      debug_window_copy_model_foreach, &debug_data);

  debug_window_send_to_pastebin (self, debug_data);
  g_free (debug_data);
}

static void
debug_window_copy_clicked_cb (GtkToolButton *tool_button,
    EmpathyDebugWindow *self)
{
  GtkClipboard *clipboard;
  gchar *text = NULL;

  gtk_tree_model_foreach (self->priv->store_filter,
      debug_window_copy_model_foreach, &text);

  clipboard = gtk_clipboard_get_for_display (
      gtk_widget_get_display (GTK_WIDGET (tool_button)),
      GDK_SELECTION_CLIPBOARD);

  DEBUG ("Copying text to clipboard (length: %" G_GSIZE_FORMAT ")",
      strlen (text));

  gtk_clipboard_set_text (clipboard, text, -1);

  g_free (text);
}

static gboolean
debug_window_key_press_event_cb (GtkWidget *widget,
    GdkEventKey *event,
    gpointer user_data)
{
  if ((event->state & GDK_CONTROL_MASK && event->keyval == GDK_KEY_w)
      || event->keyval == GDK_KEY_Escape)
    {
      gtk_widget_destroy (widget);
      return TRUE;
    }

  return FALSE;
}

static void
empathy_debug_window_select_name (EmpathyDebugWindow *self,
    const gchar *name)
{
  GtkTreeModel *model = GTK_TREE_MODEL (self->priv->service_store);
  GtkTreeIter iter;
  gchar *iter_name;
  gboolean valid, found = FALSE;

  for (valid = gtk_tree_model_get_iter_first (model, &iter);
       valid;
       valid = gtk_tree_model_iter_next (model, &iter))
    {
      gtk_tree_model_get (model, &iter,
          COL_NAME, &iter_name,
          -1);

      if (!tp_strdiff (name, iter_name))
        found = TRUE;

      g_free (iter_name);

      if (found)
        break;
    }

  if (found)
    gtk_combo_box_set_active_iter (GTK_COMBO_BOX (self->priv->chooser), &iter);
}

static void
am_prepared_cb (GObject *am,
    GAsyncResult *res,
    gpointer user_data)
{
  EmpathyDebugWindow *self = user_data;
  GObject *object = user_data;
  GtkWidget *vbox;
  GtkWidget *toolbar;
  GtkWidget *image;
  GtkWidget *label;
  GtkToolItem *item;
  GtkCellRenderer *renderer;
  GtkListStore *level_store;
  GtkTreeIter iter;
  GError *error = NULL;
  GtkWidget *infobar, *content;

  if (!tp_proxy_prepare_finish (am, res, &error))
    {
      g_warning ("Failed to prepare AM: %s", error->message);
      g_clear_error (&error);
    }

  empathy_set_css_provider (GTK_WIDGET (object));

  gtk_window_set_title (GTK_WINDOW (object), _("Debug Window"));
  gtk_widget_set_size_request (GTK_WIDGET (object), 600, 300);
  empathy_geometry_bind (GTK_WINDOW (object), "debug-window");

  g_signal_connect (object, "key-press-event",
      G_CALLBACK (debug_window_key_press_event_cb), NULL);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add (GTK_CONTAINER (object), vbox);
  gtk_widget_show (vbox);

  toolbar = gtk_toolbar_new ();
  gtk_toolbar_set_style (GTK_TOOLBAR (toolbar), GTK_TOOLBAR_BOTH_HORIZ);
  gtk_toolbar_set_show_arrow (GTK_TOOLBAR (toolbar), TRUE);
  gtk_toolbar_set_icon_size (GTK_TOOLBAR (toolbar),
      GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_style_context_add_class (gtk_widget_get_style_context (toolbar),
			       GTK_STYLE_CLASS_PRIMARY_TOOLBAR);
  gtk_widget_show (toolbar);

  gtk_box_pack_start (GTK_BOX (vbox), toolbar, FALSE, FALSE, 0);

  /* CM */
  self->priv->chooser = gtk_combo_box_text_new ();
  self->priv->service_store = gtk_list_store_new (NUM_COLS,
      G_TYPE_STRING,  /* COL_NAME */
      G_TYPE_STRING,  /* COL_UNIQUE_NAME */
      G_TYPE_BOOLEAN, /* COL_GONE */
      G_TYPE_OBJECT,  /* COL_ACTIVE_BUFFER */
      G_TYPE_OBJECT,  /* COL_PAUSE_BUFFER */
      TP_TYPE_PROXY); /* COL_PROXY */
  gtk_combo_box_set_model (GTK_COMBO_BOX (self->priv->chooser),
      GTK_TREE_MODEL (self->priv->service_store));
  gtk_widget_show (self->priv->chooser);

  item = gtk_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (item));
  gtk_container_add (GTK_CONTAINER (item), self->priv->chooser);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);
  g_signal_connect (self->priv->chooser, "changed",
      G_CALLBACK (debug_window_service_chooser_changed_cb), object);
  gtk_widget_show (GTK_WIDGET (self->priv->chooser));

  item = gtk_separator_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (item));
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);

  /* Save */
  self->priv->save_button = gtk_tool_button_new_from_stock (GTK_STOCK_SAVE);
  g_signal_connect (self->priv->save_button, "clicked",
      G_CALLBACK (debug_window_save_clicked_cb), object);
  gtk_widget_show (GTK_WIDGET (self->priv->save_button));
  gtk_tool_item_set_is_important (GTK_TOOL_ITEM (self->priv->save_button),
      TRUE);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), self->priv->save_button, -1);

  /* Send to pastebin */
  self->priv->send_to_pastebin = gtk_tool_button_new_from_stock (
      GTK_STOCK_PASTE);
  gtk_tool_button_set_label (GTK_TOOL_BUTTON (self->priv->send_to_pastebin),
      _("Send to pastebin"));
  g_signal_connect (self->priv->send_to_pastebin, "clicked",
      G_CALLBACK (debug_window_send_to_pastebin_cb), object);
  gtk_widget_show (GTK_WIDGET (self->priv->send_to_pastebin));
  gtk_tool_item_set_is_important (GTK_TOOL_ITEM (self->priv->send_to_pastebin),
      TRUE);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), self->priv->send_to_pastebin, -1);

  /* Copy */
  self->priv->copy_button = gtk_tool_button_new_from_stock (GTK_STOCK_COPY);
  g_signal_connect (self->priv->copy_button, "clicked",
      G_CALLBACK (debug_window_copy_clicked_cb), object);
  gtk_widget_show (GTK_WIDGET (self->priv->copy_button));
  gtk_tool_item_set_is_important (GTK_TOOL_ITEM (self->priv->copy_button),
      TRUE);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), self->priv->copy_button, -1);

  /* Clear */
  self->priv->clear_button = gtk_tool_button_new_from_stock (GTK_STOCK_CLEAR);
  g_signal_connect (self->priv->clear_button, "clicked",
      G_CALLBACK (debug_window_clear_clicked_cb), object);
  gtk_widget_show (GTK_WIDGET (self->priv->clear_button));
  gtk_tool_item_set_is_important (GTK_TOOL_ITEM (self->priv->clear_button),
      TRUE);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), self->priv->clear_button, -1);

  item = gtk_separator_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (item));
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);

  /* Pause */
  self->priv->paused = FALSE;
  image = gtk_image_new_from_stock (GTK_STOCK_MEDIA_PAUSE,
      GTK_ICON_SIZE_MENU);
  gtk_widget_show (image);
  self->priv->pause_button = gtk_toggle_tool_button_new ();
  gtk_toggle_tool_button_set_active (
      GTK_TOGGLE_TOOL_BUTTON (self->priv->pause_button), self->priv->paused);
  g_signal_connect (self->priv->pause_button, "toggled",
      G_CALLBACK (debug_window_pause_toggled_cb), object);
  gtk_widget_show (GTK_WIDGET (self->priv->pause_button));
  gtk_tool_item_set_is_important (GTK_TOOL_ITEM (self->priv->pause_button),
      TRUE);
  gtk_tool_button_set_label (GTK_TOOL_BUTTON (self->priv->pause_button),
      _("Pause"));
  gtk_tool_button_set_icon_widget (
      GTK_TOOL_BUTTON (self->priv->pause_button), image);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), self->priv->pause_button, -1);

  item = gtk_separator_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (item));
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);

  /* Level */
  self->priv->level_label = gtk_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (self->priv->level_label));
  label = gtk_label_new (_("Level "));
  gtk_widget_show (label);
  gtk_container_add (GTK_CONTAINER (self->priv->level_label), label);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), self->priv->level_label, -1);

  self->priv->level_filter = gtk_combo_box_text_new ();
  gtk_widget_show (self->priv->level_filter);

  item = gtk_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (item));
  gtk_container_add (GTK_CONTAINER (item), self->priv->level_filter);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);

  level_store = gtk_list_store_new (NUM_COLS_LEVEL,
      G_TYPE_STRING, G_TYPE_UINT);
  gtk_combo_box_set_model (GTK_COMBO_BOX (self->priv->level_filter),
      GTK_TREE_MODEL (level_store));

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Debug"),
      COL_LEVEL_VALUE, G_LOG_LEVEL_DEBUG,
      -1);

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Info"),
      COL_LEVEL_VALUE, G_LOG_LEVEL_INFO,
      -1);

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Message"),
      COL_LEVEL_VALUE, G_LOG_LEVEL_MESSAGE,
      -1);

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Warning"),
      COL_LEVEL_VALUE, G_LOG_LEVEL_WARNING,
      -1);

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Critical"),
      COL_LEVEL_VALUE, G_LOG_LEVEL_CRITICAL,
      -1);

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Error"),
      COL_LEVEL_VALUE, G_LOG_LEVEL_ERROR,
      -1);

  gtk_combo_box_set_active (GTK_COMBO_BOX (self->priv->level_filter), 0);
  g_signal_connect (self->priv->level_filter, "changed",
      G_CALLBACK (debug_window_filter_changed_cb), object);

  /* Info bar */
  infobar = gtk_info_bar_new ();
  gtk_info_bar_set_message_type (GTK_INFO_BAR (infobar), GTK_MESSAGE_INFO);

  label = gtk_label_new (
        _("Even if they don’t display passwords, logs can contain sensitive "
          "information such as your list of contacts or the messages you "
          "recently sent or received.\nIf you don’t want to see such "
          "information available in a public bug report, you "
          "can choose to limit the visibility of your bug to "
          "Empathy developers when reporting it by displaying "
          "the advanced fields in the "
          "<a href=\"https://bugzilla.gnome.org/enter_bug.cgi?product=empathy\">"
          "bug report</a>."));
  gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_style_context_add_class (gtk_widget_get_style_context (label),
      GTK_STYLE_CLASS_DIM_LABEL);

  content = gtk_info_bar_get_content_area (GTK_INFO_BAR (infobar));
  gtk_box_pack_start (GTK_BOX (content), label, FALSE, FALSE, 0);

  gtk_widget_show (infobar);
  gtk_widget_show (label);
  gtk_box_pack_start (GTK_BOX (vbox), infobar, FALSE, FALSE, 0);

  /* Debug treeview */
  self->priv->view = gtk_tree_view_new ();
  gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (self->priv->view), TRUE);

  g_signal_connect (self->priv->view, "button-press-event",
      G_CALLBACK (debug_window_button_press_event_cb), object);

  renderer = gtk_cell_renderer_text_new ();
  g_object_set (renderer, "yalign", (gfloat) 0, NULL);

  gtk_tree_view_insert_column_with_data_func (GTK_TREE_VIEW (self->priv->view),
      -1, _("Time"), renderer,
      (GtkTreeCellDataFunc) debug_window_time_formatter, NULL, NULL);
  gtk_tree_view_insert_column_with_data_func (GTK_TREE_VIEW (self->priv->view),
      -1, _("Domain"), renderer,
      (GtkTreeCellDataFunc) debug_window_domain_formatter, NULL, NULL);
  gtk_tree_view_insert_column_with_data_func (GTK_TREE_VIEW (self->priv->view),
      -1, _("Category"), renderer,
      (GtkTreeCellDataFunc) debug_window_category_formatter, NULL, NULL);
  gtk_tree_view_insert_column_with_data_func (GTK_TREE_VIEW (self->priv->view),
      -1, _("Level"), renderer,
      (GtkTreeCellDataFunc) debug_window_level_formatter, NULL, NULL);

  renderer = gtk_cell_renderer_text_new ();

  g_object_set (renderer,
      "family", "Monospace",
      "ellipsize", PANGO_ELLIPSIZE_END,
      NULL);

  gtk_tree_view_insert_column_with_data_func (GTK_TREE_VIEW (self->priv->view),
      -1, _("Message"), renderer,
      (GtkTreeCellDataFunc) debug_window_message_formatter, NULL, NULL);

  self->priv->store_filter = NULL;

  gtk_tree_view_set_model (GTK_TREE_VIEW (self->priv->view),
      self->priv->store_filter);

  /* Scrolled window */
  self->priv->scrolled_win = g_object_ref (gtk_scrolled_window_new (
        NULL, NULL));
  gtk_scrolled_window_set_policy (
      GTK_SCROLLED_WINDOW (self->priv->scrolled_win),
      GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  gtk_widget_show (self->priv->view);
  gtk_container_add (GTK_CONTAINER (self->priv->scrolled_win),
      self->priv->view);

  gtk_widget_show (self->priv->scrolled_win);

  /* Not supported label */
  self->priv->not_supported_label = g_object_ref (gtk_label_new (
          _("The selected connection manager does not support the remote "
              "debugging extension.")));
  gtk_widget_show (self->priv->not_supported_label);
  gtk_box_pack_start (GTK_BOX (vbox), self->priv->not_supported_label,
      TRUE, TRUE, 0);

  self->priv->view_visible = FALSE;

  self->priv->all_active_buffer = NULL;

  debug_window_set_toolbar_sensitivity (EMPATHY_DEBUG_WINDOW (object), FALSE);
  debug_window_fill_service_chooser (EMPATHY_DEBUG_WINDOW (object));
  gtk_widget_show (GTK_WIDGET (object));
}

static void
debug_window_constructed (GObject *object)
{
  EmpathyDebugWindow *self = EMPATHY_DEBUG_WINDOW (object);

  self->priv->am = tp_account_manager_dup ();
  tp_proxy_prepare_async (self->priv->am, NULL, am_prepared_cb, object);
}

static void
empathy_debug_window_init (EmpathyDebugWindow *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_DEBUG_WINDOW, EmpathyDebugWindowPriv);
}

static void
debug_window_set_property (GObject *object,
    guint prop_id,
    const GValue *value,
    GParamSpec *pspec)
{
  switch (prop_id)
    {
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
debug_window_get_property (GObject *object,
    guint prop_id,
    GValue *value,
    GParamSpec *pspec)
{
  switch (prop_id)
    {
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
debug_window_finalize (GObject *object)
{
  EmpathyDebugWindow *self = EMPATHY_DEBUG_WINDOW (object);

  g_free (self->priv->select_name);

  (G_OBJECT_CLASS (empathy_debug_window_parent_class)->finalize) (object);
}

static void
disable_all_debug_clients (EmpathyDebugWindow *self)
{
  GtkTreeIter iter;
  gboolean valid_iter;
  GtkTreeModel *model;

  if (self->priv->service_store == NULL)
    return;
  model = GTK_TREE_MODEL (self->priv->service_store);

  /* Skipping the first service store iter which is reserved for "All" */
  gtk_tree_model_get_iter_first (model, &iter);
  for (valid_iter = gtk_tree_model_iter_next (model, &iter);
       valid_iter;
       valid_iter = gtk_tree_model_iter_next (model, &iter))
    {
      TpDebugClient *debug;

      gtk_tree_model_get (model, &iter,
          COL_PROXY, &debug,
          -1);

      debug_window_set_enabled (debug, FALSE);

      g_object_unref (debug);
    }
}

static void
debug_window_dispose (GObject *object)
{
  EmpathyDebugWindow *self = EMPATHY_DEBUG_WINDOW (object);

  if (self->priv->name_owner_changed_signal != NULL)
    tp_proxy_signal_connection_disconnect (
        self->priv->name_owner_changed_signal);

  /* Disable Debug on all proxies */
  disable_all_debug_clients (self);

  g_clear_object (&self->priv->service_store);
  g_clear_object (&self->priv->dbus);
  g_clear_object (&self->priv->am);
  g_clear_object (&self->priv->all_active_buffer);

  (G_OBJECT_CLASS (empathy_debug_window_parent_class)->dispose) (object);
}

static void
empathy_debug_window_class_init (EmpathyDebugWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->constructed = debug_window_constructed;
  object_class->dispose = debug_window_dispose;
  object_class->finalize = debug_window_finalize;
  object_class->set_property = debug_window_set_property;
  object_class->get_property = debug_window_get_property;

  g_type_class_add_private (klass, sizeof (EmpathyDebugWindowPriv));
}

/* public methods */

GtkWidget *
empathy_debug_window_new (GtkWindow *parent)
{
  g_return_val_if_fail (parent == NULL || GTK_IS_WINDOW (parent), NULL);

  return GTK_WIDGET (g_object_new (EMPATHY_TYPE_DEBUG_WINDOW,
      "transient-for", parent, NULL));
}

void
empathy_debug_window_show (EmpathyDebugWindow *self,
    const gchar *name)
{
  if (self->priv->service_store != NULL)
    {
      empathy_debug_window_select_name (self, name);
    }
  else
    {
      g_free (self->priv->select_name);
      self->priv->select_name = g_strdup (name);
    }
}
