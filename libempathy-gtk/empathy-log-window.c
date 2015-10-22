/*
 * Copyright (C) 2006-2007 Imendio AB
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
 * Authors: Martyn Russell <martyn@imendio.com>
 *          Xavier Claessens <xclaesse@gmail.com>
 *          Emilio Pozuelo Monfort <emilio.pozuelo@collabora.co.uk>
 */

#include "config.h"
#include "empathy-log-window.h"

#include <glib/gi18n-lib.h>
#include <telepathy-glib/proxy-subclass.h>
#include <tp-account-widgets/tpaw-builder.h>
#include <tp-account-widgets/tpaw-images.h>
#include <tp-account-widgets/tpaw-camera-monitor.h>
#include <tp-account-widgets/tpaw-utils.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "action-chain-internal.h"
#include "empathy-account-chooser.h"
#include "empathy-call-utils.h"
#include "empathy-geometry.h"
#include "empathy-gsettings.h"
#include "empathy-images.h"
#include "empathy-individual-information-dialog.h"
#include "empathy-request-util.h"
#include "empathy-theme-manager.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"
#include "empathy-webkit-utils.h"
#include "extensions.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

#define EMPATHY_NS "http://live.gnome.org/Empathy"

G_DEFINE_TYPE (EmpathyLogWindow, empathy_log_window, GTK_TYPE_WINDOW);

struct _EmpathyLogWindowPriv
{
  GtkWidget *vbox;

  GtkWidget *button_profile;
  GtkWidget *button_chat;
  GtkWidget *button_call;
  GtkWidget *button_video;

  GtkWidget *search_entry;

  GtkWidget *notebook;
  GtkWidget *spinner;

  GtkWidget *treeview_who;
  GtkWidget *treeview_what;
  GtkWidget *treeview_when;
  GtkWidget *webview;

  GtkTreeStore *store_events;

  GtkWidget *account_chooser;

  gchar *last_find;

  /* List of selected GDates, free with g_list_free_full (l, g_date_free) */
  GList *current_dates;

  TplActionChain *chain;
  TplLogManager *log_manager;

  /* Hash of TpChannel<->TpAccount for use by the observer until we can
   * get a TpAccount from a TpConnection or wherever */
  GHashTable *channels;
  TpBaseClient *observer;

  EmpathyContact *selected_contact;
  EmpathyContact *events_contact;

  TpawCameraMonitor *camera_monitor;
  GBinding *button_video_binding;

  /* Used to cancel logger calls when no longer needed */
  guint count;

  /* List of owned TplLogSearchHits, free with tpl_log_search_hit_free */
  GList *hits;
  guint source;

  /* Only used while waiting for the account chooser to be ready */
  TpAccount *selected_account;
  gchar *selected_chat_id;
  gboolean selected_is_chatroom;

  GSettings *gsettings_chat;
  GSettings *gsettings_desktop;
};

static void log_window_search_entry_changed_cb   (GtkWidget        *entry,
                                                  EmpathyLogWindow *self);
static void log_window_search_entry_activate_cb  (GtkWidget        *widget,
                                                  EmpathyLogWindow *self);
static void log_window_search_entry_icon_pressed_cb (GtkEntry      *entry,
                                                  GtkEntryIconPosition icon_pos,
                                                  GdkEvent *event,
                                                  gpointer user_data);
static void log_window_who_populate              (EmpathyLogWindow *self);
static void log_window_who_setup                 (EmpathyLogWindow *self);
static void log_window_when_setup                (EmpathyLogWindow *self);
static void log_window_what_setup                (EmpathyLogWindow *self);
static void log_window_events_setup              (EmpathyLogWindow *self);
static void log_window_chats_accounts_changed_cb (GtkWidget        *combobox,
                                                  EmpathyLogWindow *self);
static void log_window_chats_set_selected        (EmpathyLogWindow *self);
static void log_window_chats_get_messages        (EmpathyLogWindow *self,
                                                  gboolean force_get_dates);
static void log_window_when_changed_cb           (GtkTreeSelection *selection,
                                                  EmpathyLogWindow *self);
static void log_window_delete_menu_clicked_cb    (GtkMenuItem      *menuitem,
                                                  EmpathyLogWindow *self);
static void start_spinner                        (void);

static void log_window_create_observer           (EmpathyLogWindow *window);
static gboolean log_window_events_button_press_event (GtkWidget *webview,
    GdkEventButton *event, EmpathyLogWindow *self);
static void log_window_update_buttons_sensitivity (EmpathyLogWindow *self);

static void
empathy_account_chooser_filter_has_logs (TpAccount *account,
    EmpathyAccountChooserFilterResultCallback callback,
    gpointer callback_data,
    gpointer user_data);

enum
{
  PAGE_EVENTS,
  PAGE_SPINNER,
  PAGE_EMPTY
};

enum
{
  COL_TYPE_ANY,
  COL_TYPE_SEPARATOR,
  COL_TYPE_NORMAL
};

enum
{
  COL_WHO_TYPE,
  COL_WHO_ICON,
  COL_WHO_NAME,
  COL_WHO_NAME_SORT_KEY,
  COL_WHO_ID,
  COL_WHO_ACCOUNT,
  COL_WHO_TARGET,
  COL_WHO_COUNT
};

enum
{
  COL_WHAT_TYPE,
  COL_WHAT_SUBTYPE,
  COL_WHAT_SENSITIVE,
  COL_WHAT_TEXT,
  COL_WHAT_ICON,
  COL_WHAT_COUNT
};

enum
{
  COL_WHEN_DATE,
  COL_WHEN_TEXT,
  COL_WHEN_ICON,
  COL_WHEN_COUNT
};

enum
{
  COL_EVENTS_TYPE,
  COL_EVENTS_TS,
  COL_EVENTS_PRETTY_DATE,
  COL_EVENTS_ICON,
  COL_EVENTS_TEXT,
  COL_EVENTS_ACCOUNT,
  COL_EVENTS_TARGET,
  COL_EVENTS_EVENT,
  COL_EVENTS_COUNT
};

#define CALENDAR_ICON "x-office-calendar"

/* Seconds between two messages to be considered one conversation */
#define MAX_GAP 30*60

#define WHAT_TYPE_SEPARATOR -1

typedef enum
{
  EVENT_CALL_INCOMING = 1 << 0,
  EVENT_CALL_OUTGOING = 1 << 1,
  EVENT_CALL_MISSED   = 1 << 2,
  EVENT_CALL_ALL      = 1 << 3,
} EventSubtype;

static gboolean
log_window_get_selected (EmpathyLogWindow *window,
    GList **accounts,
    GList **entities,
    gboolean *anyone,
    GList **dates,
    TplEventTypeMask *event_mask,
    EventSubtype *subtype);

static EmpathyLogWindow *log_window = NULL;

static gboolean has_element;

#ifndef _date_copy
#define _date_copy(d) g_date_new_julian (g_date_get_julian (d))
#endif

typedef struct
{
  EmpathyLogWindow *self;
  TpAccount *account;
  TplEntity *entity;
  GDate *date;
  TplEventTypeMask event_mask;
  EventSubtype subtype;
  guint count;
} Ctx;

static Ctx *
ctx_new (EmpathyLogWindow *self,
    TpAccount *account,
    TplEntity *entity,
    GDate *date,
    TplEventTypeMask event_mask,
    EventSubtype subtype,
    guint count)
{
  Ctx *ctx = g_slice_new0 (Ctx);

  ctx->self = self;
  if (account != NULL)
    ctx->account = g_object_ref (account);
  if (entity != NULL)
    ctx->entity = g_object_ref (entity);
  if (date != NULL)
    ctx->date = _date_copy (date);
  ctx->event_mask = event_mask;
  ctx->subtype = subtype;
  ctx->count = count;

  return ctx;
}

static void
ctx_free (Ctx *ctx)
{
  tp_clear_object (&ctx->account);
  tp_clear_object (&ctx->entity);
  tp_clear_pointer (&ctx->date, g_date_free);

  g_slice_free (Ctx, ctx);
}

static void
select_account_once_ready (EmpathyLogWindow *self,
    TpAccount *account,
    const gchar *chat_id,
    gboolean is_chatroom)
{
  EmpathyAccountChooser *account_chooser;

  account_chooser = EMPATHY_ACCOUNT_CHOOSER (self->priv->account_chooser);

  tp_clear_object (&self->priv->selected_account);
  self->priv->selected_account = g_object_ref (account);

  g_free (self->priv->selected_chat_id);
  self->priv->selected_chat_id = g_strdup (chat_id);

  self->priv->selected_is_chatroom = is_chatroom;

  empathy_account_chooser_set_account (account_chooser,
      self->priv->selected_account);
}

static void
toolbutton_profile_clicked (GtkToolButton *toolbutton,
    EmpathyLogWindow *self)
{
  FolksIndividual *individual;

  g_return_if_fail (self != NULL);
  g_return_if_fail (EMPATHY_IS_CONTACT (self->priv->selected_contact));

  individual = empathy_ensure_individual_from_tp_contact (
      empathy_contact_get_tp_contact (self->priv->selected_contact));

  empathy_display_individual_info (individual);

  g_object_unref (individual);
}

static void
toolbutton_chat_clicked (GtkToolButton *toolbutton,
    EmpathyLogWindow *self)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (EMPATHY_IS_CONTACT (self->priv->selected_contact));

  empathy_chat_with_contact (self->priv->selected_contact,
      gtk_get_current_event_time ());
}

static void
toolbutton_av_clicked (GtkToolButton *toolbutton,
    EmpathyLogWindow *self)
{
  gboolean video;

  g_return_if_fail (self != NULL);
  g_return_if_fail (EMPATHY_IS_CONTACT (self->priv->selected_contact));

  video = (GTK_WIDGET (toolbutton) == self->priv->button_video);

  empathy_call_new_with_streams (
      empathy_contact_get_id (self->priv->selected_contact),
      empathy_contact_get_account (self->priv->selected_contact),
      video, gtk_get_current_event_time ());
}

static void
insert_or_change_row (EmpathyLogWindow *self,
    const char *method,
    GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter)
{
  char *str = gtk_tree_path_to_string (path);
  char *script, *text, *date, *stock_icon;
  GString *escaped_text;
  char *icon = NULL;
  gint i;

  gtk_tree_model_get (model, iter,
      COL_EVENTS_TEXT, &text,
      COL_EVENTS_PRETTY_DATE, &date,
      COL_EVENTS_ICON, &stock_icon,
      -1);

  if (!tp_str_empty (stock_icon))
    {
      GtkIconInfo *icon_info = gtk_icon_theme_lookup_icon (
          gtk_icon_theme_get_default (),
          stock_icon,
          GTK_ICON_SIZE_MENU, 0);

      if (icon_info != NULL)
        icon = g_strdup (gtk_icon_info_get_filename (icon_info));

      g_object_unref (icon_info);
    }

  escaped_text = g_string_new (NULL);

  /* Only need to deal with «'» and «\».
   *
   * Note that these never appear in non-ascii utf8 characters, so just
   * pretend like we have an ascii string...
   */
  for (i = 0; text && text[i]; i++)
    {
      gchar c = text[i];

      if (c == '\'' || c == '\\')
        g_string_append_c (escaped_text, '\\');

      g_string_append_c (escaped_text, c);
    }

  script = g_strdup_printf ("javascript:%s([%s], '%s', '%s', '%s');",
      method,
      g_strdelimit (str, ":", ','),
      escaped_text->str,
      icon != NULL ? icon : "",
      date);

  webkit_web_view_execute_script (WEBKIT_WEB_VIEW (self->priv->webview),
      script);

  g_string_free (escaped_text, TRUE);
  g_free (str);
  g_free (text);
  g_free (date);
  g_free (stock_icon);
  g_free (icon);
  g_free (script);
}

static void
store_events_row_inserted (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    EmpathyLogWindow *self)
{
  insert_or_change_row (self, "insertRow", model, path, iter);
}

static void
store_events_row_changed (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    EmpathyLogWindow *self)
{
  insert_or_change_row (self, "changeRow", model, path, iter);
}

static void
store_events_row_deleted (GtkTreeModel *model,
    GtkTreePath *path,
    EmpathyLogWindow *self)
{
  char *str = gtk_tree_path_to_string (path);
  char *script;

  script = g_strdup_printf ("javascript:deleteRow([%s]);",
      g_strdelimit (str, ":", ','));

  webkit_web_view_execute_script (WEBKIT_WEB_VIEW (self->priv->webview),
      script);

  g_free (str);
  g_free (script);
}

static void
store_events_has_child_rows (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    EmpathyLogWindow *self)
{
  char *str = gtk_tree_path_to_string (path);
  char *script;

  script = g_strdup_printf ("javascript:hasChildRows([%s], %u);",
      g_strdelimit (str, ":", ','),
      gtk_tree_model_iter_has_child (model, iter));

  webkit_web_view_execute_script (WEBKIT_WEB_VIEW (self->priv->webview),
      script);

  g_free (str);
  g_free (script);
}

static void
store_events_rows_reordered (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    int *new_order,
    EmpathyLogWindow *self)
{
  char *str = gtk_tree_path_to_string (path);
  int i, children = gtk_tree_model_iter_n_children (model, iter);
  char **new_order_strv, *new_order_s;
  char *script;

  new_order_strv = g_new0 (char *, children + 1);

  for (i = 0; i < children; i++)
    new_order_strv[i] = g_strdup_printf ("%i", new_order[i]);

  new_order_s = g_strjoinv (",", new_order_strv);

  script = g_strdup_printf ("javascript:reorderRows([%s], [%s]);",
      str == NULL ? "" : g_strdelimit (str, ":", ','),
      new_order_s);

  webkit_web_view_execute_script (WEBKIT_WEB_VIEW (self->priv->webview),
      script);

  g_free (str);
  g_free (script);
  g_free (new_order_s);
  g_strfreev (new_order_strv);
}

static gboolean
events_webview_handle_navigation (WebKitWebView *webview,
    WebKitWebFrame *frame,
    WebKitNetworkRequest *request,
    WebKitWebNavigationAction *navigation_action,
    WebKitWebPolicyDecision *policy_decision,
    EmpathyLogWindow *window)
{
  empathy_url_show (GTK_WIDGET (webview),
      webkit_network_request_get_uri (request));

  webkit_web_policy_decision_ignore (policy_decision);
  return TRUE;
}

static GObject *
empathy_log_window_constructor (GType type,
    guint n_props,
    GObjectConstructParam *props)
{
  GObject *retval;

  if (log_window != NULL)
    {
      retval = (GObject *) log_window;
    }
  else
    {
      retval = G_OBJECT_CLASS (empathy_log_window_parent_class)
          ->constructor (type, n_props, props);

      log_window = EMPATHY_LOG_WINDOW (retval);
      g_object_add_weak_pointer (retval, (gpointer) &log_window);
    }

  return retval;
}

static void
empathy_log_window_dispose (GObject *object)
{
  EmpathyLogWindow *self = EMPATHY_LOG_WINDOW (object);

  if (self->priv->source != 0)
    {
      g_source_remove (self->priv->source);
      self->priv->source = 0;
    }

  if (self->priv->current_dates != NULL)
    {
      g_list_free_full (self->priv->current_dates,
          (GDestroyNotify) g_date_free);
      self->priv->current_dates = NULL;
    }

  tp_clear_pointer (&self->priv->chain, _tpl_action_chain_free);
  tp_clear_pointer (&self->priv->channels, g_hash_table_unref);

  tp_clear_object (&self->priv->observer);
  tp_clear_object (&self->priv->log_manager);
  tp_clear_object (&self->priv->selected_account);
  tp_clear_object (&self->priv->selected_contact);
  tp_clear_object (&self->priv->events_contact);
  tp_clear_object (&self->priv->camera_monitor);

  tp_clear_object (&self->priv->gsettings_chat);
  tp_clear_object (&self->priv->gsettings_desktop);

  tp_clear_object (&self->priv->store_events);

  G_OBJECT_CLASS (empathy_log_window_parent_class)->dispose (object);
}

static void
empathy_log_window_finalize (GObject *object)
{
  EmpathyLogWindow *self = EMPATHY_LOG_WINDOW (object);

  g_free (self->priv->last_find);
  g_free (self->priv->selected_chat_id);

  G_OBJECT_CLASS (empathy_log_window_parent_class)->finalize (object);
}

static void
empathy_log_window_class_init (
  EmpathyLogWindowClass *empathy_log_window_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (empathy_log_window_class);

  g_type_class_add_private (empathy_log_window_class,
      sizeof (EmpathyLogWindowPriv));

  object_class->constructor = empathy_log_window_constructor;
  object_class->dispose = empathy_log_window_dispose;
  object_class->finalize = empathy_log_window_finalize;
}

static void
empathy_log_window_init (EmpathyLogWindow *self)
{
  EmpathyAccountChooser *account_chooser;
  GtkBuilder *gui;
  gchar *filename;
  GFile *gfile;
  GtkWidget *vbox, *accounts, *search, *label, *closeitem;
  GtkWidget *scrolledwindow_events;
  gchar *uri;

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_LOG_WINDOW, EmpathyLogWindowPriv);

  self->priv->chain = _tpl_action_chain_new_async (NULL, NULL, NULL);

  self->priv->camera_monitor = tpaw_camera_monitor_dup_singleton ();

  self->priv->log_manager = tpl_log_manager_dup_singleton ();

  self->priv->gsettings_chat = g_settings_new (EMPATHY_PREFS_CHAT_SCHEMA);
  self->priv->gsettings_desktop = g_settings_new (
      EMPATHY_PREFS_DESKTOP_INTERFACE_SCHEMA);

  gtk_window_set_title (GTK_WINDOW (self), _("History"));
  gtk_widget_set_can_focus (GTK_WIDGET (self), FALSE);
  gtk_window_set_default_size (GTK_WINDOW (self), 800, 600);

  filename = empathy_file_lookup ("empathy-log-window.ui", "libempathy-gtk");
  gui = tpaw_builder_get_file (filename,
      "vbox1", &self->priv->vbox,
      "toolbutton_profile", &self->priv->button_profile,
      "toolbutton_chat", &self->priv->button_chat,
      "toolbutton_call", &self->priv->button_call,
      "toolbutton_video", &self->priv->button_video,
      "toolbutton_accounts", &accounts,
      "toolbutton_search", &search,
      "imagemenuitem_close", &closeitem,
      "treeview_who", &self->priv->treeview_who,
      "treeview_what", &self->priv->treeview_what,
      "treeview_when", &self->priv->treeview_when,
      "scrolledwindow_events", &scrolledwindow_events,
      "notebook", &self->priv->notebook,
      "spinner", &self->priv->spinner,
      NULL);
  g_free (filename);

  tpaw_builder_connect (gui, self,
      "toolbutton_profile", "clicked", toolbutton_profile_clicked,
      "toolbutton_chat", "clicked", toolbutton_chat_clicked,
      "toolbutton_call", "clicked", toolbutton_av_clicked,
      "toolbutton_video", "clicked", toolbutton_av_clicked,
      "imagemenuitem_delete", "activate", log_window_delete_menu_clicked_cb,
      NULL);

  gtk_container_add (GTK_CONTAINER (self), self->priv->vbox);

  g_object_unref (gui);

  g_signal_connect_swapped (closeitem, "activate",
      G_CALLBACK (gtk_widget_destroy), self);

  /* Account chooser for chats */
  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);

  self->priv->account_chooser = empathy_account_chooser_new ();
  account_chooser = EMPATHY_ACCOUNT_CHOOSER (self->priv->account_chooser);
  empathy_account_chooser_set_has_all_option (account_chooser, TRUE);
  empathy_account_chooser_set_filter (account_chooser,
      empathy_account_chooser_filter_has_logs, NULL);
  empathy_account_chooser_set_all (account_chooser);

  gtk_style_context_add_class (gtk_widget_get_style_context (self->priv->account_chooser),
                               GTK_STYLE_CLASS_RAISED);

  g_signal_connect (self->priv->account_chooser, "changed",
      G_CALLBACK (log_window_chats_accounts_changed_cb),
      self);

  label = gtk_label_new (_("Show"));

  gtk_box_pack_start (GTK_BOX (vbox),
      self->priv->account_chooser,
      FALSE, FALSE, 0);

  gtk_box_pack_start (GTK_BOX (vbox),
      label,
      FALSE, FALSE, 0);

  gtk_widget_show_all (vbox);
  gtk_container_add (GTK_CONTAINER (accounts), vbox);

  /* Search entry */
  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);

  self->priv->search_entry = gtk_entry_new ();
  gtk_entry_set_icon_from_icon_name (GTK_ENTRY (self->priv->search_entry),
      GTK_ENTRY_ICON_SECONDARY, "edit-find-symbolic");
  gtk_entry_set_icon_sensitive (GTK_ENTRY (self->priv->search_entry),
                                GTK_ENTRY_ICON_SECONDARY, FALSE);

  label = gtk_label_new (_("Search"));

  gtk_box_pack_start (GTK_BOX (vbox),
      self->priv->search_entry,
      FALSE, FALSE, 0);

  gtk_box_pack_start (GTK_BOX (vbox),
      label,
      FALSE, FALSE, 0);

  gtk_widget_show_all (vbox);
  gtk_container_add (GTK_CONTAINER (search), vbox);

  g_signal_connect (self->priv->search_entry, "changed",
      G_CALLBACK (log_window_search_entry_changed_cb),
      self);

  g_signal_connect (self->priv->search_entry, "activate",
      G_CALLBACK (log_window_search_entry_activate_cb),
      self);

  g_signal_connect (self->priv->search_entry, "icon-press",
      G_CALLBACK (log_window_search_entry_icon_pressed_cb),
      self);

  /* Contacts */
  log_window_events_setup (self);
  log_window_who_setup (self);
  log_window_what_setup (self);
  log_window_when_setup (self);

  log_window_create_observer (self);

  log_window_who_populate (self);

  /* events */
  self->priv->webview = webkit_web_view_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow_events),
      GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add (GTK_CONTAINER (scrolledwindow_events),
      self->priv->webview);
  gtk_widget_show (self->priv->webview);

  empathy_webkit_bind_font_setting (WEBKIT_WEB_VIEW (self->priv->webview),
      self->priv->gsettings_desktop,
      EMPATHY_PREFS_DESKTOP_INTERFACE_FONT_NAME);

  filename = empathy_file_lookup ("empathy-log-window.html", "data");
  gfile = g_file_new_for_path (filename);
  g_free (filename);

  uri = g_file_get_uri (gfile);
  webkit_web_view_load_uri (WEBKIT_WEB_VIEW (self->priv->webview), uri);
  g_object_unref (gfile);
  g_free (uri);

  /* handle all navigation externally */
  g_signal_connect (self->priv->webview, "navigation-policy-decision-requested",
      G_CALLBACK (events_webview_handle_navigation), self);

  /* listen to changes to the treemodel */
  g_signal_connect (self->priv->store_events, "row-inserted",
      G_CALLBACK (store_events_row_inserted), self);
  g_signal_connect (self->priv->store_events, "row-changed",
      G_CALLBACK (store_events_row_changed), self);
  g_signal_connect (self->priv->store_events, "row-deleted",
      G_CALLBACK (store_events_row_deleted), self);
  g_signal_connect (self->priv->store_events, "rows-reordered",
      G_CALLBACK (store_events_rows_reordered), self);
  g_signal_connect (self->priv->store_events, "row-has-child-toggled",
      G_CALLBACK (store_events_has_child_rows), self);

  /* track clicked row */
  g_signal_connect (self->priv->webview, "button-press-event",
      G_CALLBACK (log_window_events_button_press_event), self);

  log_window_update_buttons_sensitivity (self);
  gtk_widget_show (GTK_WIDGET (self));

  empathy_geometry_bind (GTK_WINDOW (self), "log-window");
}

GtkWidget *
empathy_log_window_show (TpAccount *account,
     const gchar *chat_id,
     gboolean is_chatroom,
     GtkWindow *parent)
{
  log_window = g_object_new (EMPATHY_TYPE_LOG_WINDOW, NULL);

  gtk_window_present (GTK_WINDOW (log_window));

  if (account != NULL && chat_id != NULL)
    select_account_once_ready (log_window, account, chat_id, is_chatroom);

  if (parent != NULL)
    gtk_window_set_transient_for (GTK_WINDOW (log_window),
        GTK_WINDOW (parent));

  return GTK_WIDGET (log_window);
}

static gboolean
account_equal (TpAccount *a,
    TpAccount *b)
{
  return g_str_equal (tp_proxy_get_object_path (a),
      tp_proxy_get_object_path (b));
}

static gboolean
entity_equal (TplEntity *a,
    TplEntity *b)
{
  return g_str_equal (tpl_entity_get_identifier (a),
      tpl_entity_get_identifier (b));
}

static gboolean
is_same_confroom (TplEvent *e1,
    TplEvent *e2)
{
  TplEntity *sender1 = tpl_event_get_sender (e1);
  TplEntity *receiver1 = tpl_event_get_receiver (e1);
  TplEntity *sender2 = tpl_event_get_sender (e2);
  TplEntity *receiver2 = tpl_event_get_receiver (e2);
  TplEntity *room1, *room2;

  if (receiver1 == NULL || receiver2 == NULL)
    return FALSE;

  if (tpl_entity_get_entity_type (sender1) == TPL_ENTITY_ROOM)
    room1 = sender1;
  else if (tpl_entity_get_entity_type (receiver1) == TPL_ENTITY_ROOM)
    room1 = receiver1;
  else
    return FALSE;

  if (tpl_entity_get_entity_type (sender2) == TPL_ENTITY_ROOM)
    room2 = sender2;
  else if (tpl_entity_get_entity_type (receiver2) == TPL_ENTITY_ROOM)
    room2 = receiver2;
  else
    return FALSE;

  return g_str_equal (tpl_entity_get_identifier (room1),
      tpl_entity_get_identifier (room2));
}

static void
maybe_refresh_logs (TpChannel *channel,
    TpAccount *account)
{
  GList *accounts = NULL, *entities = NULL, *dates = NULL;
  GList *acc, *ent;
  TplEventTypeMask event_mask;
  GDate *anytime = NULL, *today = NULL;
  GDateTime *now = NULL;
  gboolean refresh = FALSE;
  gboolean anyone;
  const gchar *type;

  if (!log_window_get_selected (log_window,
      &accounts, &entities, &anyone, &dates, &event_mask, NULL))
    {
      DEBUG ("Could not get selected rows");
      return;
    }

  type = tp_channel_get_channel_type (channel);

  /* If the channel type is not in the What pane, whatever has happened
   * won't be displayed in the events pane. */
  if (!tp_strdiff (type, TP_IFACE_CHANNEL_TYPE_TEXT) &&
      !(event_mask & TPL_EVENT_MASK_TEXT))
    goto out;
  if ((!tp_strdiff (type, TP_IFACE_CHANNEL_TYPE_CALL)) &&
      !(event_mask & TPL_EVENT_MASK_CALL))
    goto out;

  anytime = g_date_new_dmy (2, 1, -1);
  now = g_date_time_new_now_local ();
  today = g_date_new_dmy (g_date_time_get_day_of_month (now),
      g_date_time_get_month (now),
      g_date_time_get_year (now));

  /* If Today (or anytime) isn't selected, anything that has happened now
   * won't be displayed. */
  if (!g_list_find_custom (dates, anytime, (GCompareFunc) g_date_compare) &&
      !g_list_find_custom (dates, today, (GCompareFunc) g_date_compare))
    goto out;

  if (anyone)
    {
      refresh = TRUE;
      goto out;
    }

  for (acc = accounts, ent = entities;
       acc != NULL && ent != NULL;
       acc = g_list_next (acc), ent = g_list_next (ent))
    {
      if (!account_equal (account, acc->data))
        continue;

      if (!tp_strdiff (tp_channel_get_identifier (channel),
                       tpl_entity_get_identifier (ent->data)))
        {
          refresh = TRUE;
          break;
        }
    }

 out:
  tp_clear_pointer (&anytime, g_date_free);
  tp_clear_pointer (&today, g_date_free);
  tp_clear_pointer (&now, g_date_time_unref);
  g_list_free_full (accounts, g_object_unref);
  g_list_free_full (entities, g_object_unref);
  g_list_free_full (dates, (GFreeFunc) g_date_free);

  if (refresh)
    {
      DEBUG ("Refreshing logs after received event");

      /* FIXME:  We need to populate the entities in case we
       * didn't have any previous logs with this contact. */
      log_window_chats_get_messages (log_window, FALSE);
    }
}

static void
on_msg_sent (TpTextChannel *channel,
    TpSignalledMessage *message,
    guint flags,
    gchar *token,
    EmpathyLogWindow *self)
{
  TpAccount *account = g_hash_table_lookup (self->priv->channels, channel);

  maybe_refresh_logs (TP_CHANNEL (channel), account);
}

static void
on_msg_received (TpTextChannel *channel,
    TpSignalledMessage *message,
    EmpathyLogWindow *self)
{
  TpMessage *msg = TP_MESSAGE (message);
  TpChannelTextMessageType type = tp_message_get_message_type (msg);
  TpAccount *account = g_hash_table_lookup (self->priv->channels, channel);

  if (type != TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL &&
      type != TP_CHANNEL_TEXT_MESSAGE_TYPE_ACTION)
    return;

  maybe_refresh_logs (TP_CHANNEL (channel), account);
}

static void
on_channel_ended (TpChannel *channel,
    guint domain,
    gint code,
    gchar *message,
    EmpathyLogWindow *self)
{
  if (self->priv->channels != NULL)
    g_hash_table_remove (self->priv->channels, channel);
}

static void
on_call_ended (TpChannel *channel,
    guint domain,
    gint code,
    gchar *message,
    EmpathyLogWindow *self)
{
  TpAccount *account = g_hash_table_lookup (self->priv->channels, channel);

  maybe_refresh_logs (channel, account);

  if (self->priv->channels != NULL)
    g_hash_table_remove (self->priv->channels, channel);
}

static void
observe_channels (TpSimpleObserver *observer,
    TpAccount *account,
    TpConnection *connection,
    GList *channels,
    TpChannelDispatchOperation *dispatch_operation,
    GList *requests,
    TpObserveChannelsContext *context,
    gpointer user_data)
{
  EmpathyLogWindow *self = user_data;

  GList *l;

  for (l = channels; l != NULL; l = g_list_next (l))
    {
      TpChannel *channel = l->data;
      const gchar *type = tp_channel_get_channel_type (channel);

      if (!tp_strdiff (type, TP_IFACE_CHANNEL_TYPE_TEXT))
        {
          TpTextChannel *text_channel = TP_TEXT_CHANNEL (channel);

          g_hash_table_insert (self->priv->channels,
              g_object_ref (channel), g_object_ref (account));

          tp_g_signal_connect_object (text_channel, "message-sent",
              G_CALLBACK (on_msg_sent), self, 0);
          tp_g_signal_connect_object (text_channel, "message-received",
              G_CALLBACK (on_msg_received), self, 0);
          tp_g_signal_connect_object (channel, "invalidated",
              G_CALLBACK (on_channel_ended), self, 0);
        }
      else if (!tp_strdiff (type, TP_IFACE_CHANNEL_TYPE_CALL))
        {
          g_hash_table_insert (self->priv->channels,
              g_object_ref (channel), g_object_ref (account));

          tp_g_signal_connect_object (channel, "invalidated",
              G_CALLBACK (on_call_ended), self, 0);
        }
      else
        {
          g_warning ("Unknown channel type: %s", type);
        }
    }

  tp_observe_channels_context_accept (context);
}

static void
log_window_create_observer (EmpathyLogWindow *self)
{
  TpAccountManager *am;

  am = tp_account_manager_dup ();

  self->priv->observer = tp_simple_observer_new_with_am (am, TRUE, "LogWindow",
      TRUE, observe_channels,
      g_object_ref (self), g_object_unref);

  self->priv->channels = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      g_object_unref, g_object_unref);

  tp_base_client_take_observer_filter (self->priv->observer,
      tp_asv_new (
          TP_PROP_CHANNEL_CHANNEL_TYPE, G_TYPE_STRING,
            TP_IFACE_CHANNEL_TYPE_TEXT,
          NULL));
  tp_base_client_take_observer_filter (self->priv->observer,
      tp_asv_new (
          TP_PROP_CHANNEL_CHANNEL_TYPE, G_TYPE_STRING,
            TP_IFACE_CHANNEL_TYPE_CALL,
          NULL));

  tp_base_client_register (self->priv->observer, NULL);

  g_object_unref (am);
}

static TplEntity *
event_get_target (TplEvent *event)
{
  TplEntity *sender = tpl_event_get_sender (event);
  TplEntity *receiver = tpl_event_get_receiver (event);

  if (tpl_entity_get_entity_type (sender) == TPL_ENTITY_SELF)
    return receiver;

  return sender;
}

static gboolean
model_is_parent (GtkTreeModel *model,
    GtkTreeIter *iter,
    TplEvent *event)
{
  TplEvent *stored_event;
  TplEntity *target;
  TpAccount *account;
  gboolean found = FALSE;
  GtkTreeIter parent;

  if (gtk_tree_model_iter_parent (model, &parent, iter))
    return FALSE;

  gtk_tree_model_get (model, iter,
      COL_EVENTS_ACCOUNT, &account,
      COL_EVENTS_TARGET, &target,
      COL_EVENTS_EVENT, &stored_event,
      -1);

  if (G_OBJECT_TYPE (event) == G_OBJECT_TYPE (stored_event) &&
      account_equal (account, tpl_event_get_account (event)) &&
      (entity_equal (target, event_get_target (event)) ||
      is_same_confroom (event, stored_event)))
    {
      GtkTreeIter child;
      gint64 timestamp;

      gtk_tree_model_iter_nth_child (model, &child, iter,
          gtk_tree_model_iter_n_children (model, iter) - 1);

      gtk_tree_model_get (model, &child,
          COL_EVENTS_TS, &timestamp,
          -1);

      if (ABS (tpl_event_get_timestamp (event) - timestamp) < MAX_GAP)
        {
          /* The gap is smaller than 30 min */
          found = TRUE;
        }
    }

  g_object_unref (stored_event);
  g_object_unref (account);
  g_object_unref (target);

  return found;
}

static gchar *
get_display_string_for_chat_message (EmpathyMessage *message,
    TplEvent *event)
{
  EmpathyContact *sender, *receiver, *target;
  TplEntity *ent_sender, *ent_receiver;
  const gchar *format;

  sender = empathy_message_get_sender (message);
  receiver = empathy_message_get_receiver (message);

  ent_sender = tpl_event_get_sender (event);
  ent_receiver = tpl_event_get_receiver (event);

  /* If this is a MUC, we want to show "Chat in <room>". */
  if (tpl_entity_get_entity_type (ent_sender) == TPL_ENTITY_ROOM ||
      (ent_receiver != NULL &&
      tpl_entity_get_entity_type (ent_receiver) == TPL_ENTITY_ROOM))
    format = _("Chat in %s");
  else
    format = _("Chat with %s");

  if (tpl_entity_get_entity_type (ent_sender) == TPL_ENTITY_ROOM)
    target = sender;
  else if (ent_receiver != NULL &&
      tpl_entity_get_entity_type (ent_receiver) == TPL_ENTITY_ROOM)
    target = receiver;
  else if (empathy_contact_is_user (sender))
    target = receiver;
  else
    target = sender;

  return g_markup_printf_escaped (format, empathy_contact_get_alias (target));
}

static void
get_parent_iter_for_message (TplEvent *event,
    EmpathyMessage *message,
    GtkTreeIter *parent)
{
  GtkTreeStore *store;
  GtkTreeModel *model;
  GtkTreeIter iter;
  gboolean parent_found = FALSE;
  gboolean next;

  store = log_window->priv->store_events;
  model = GTK_TREE_MODEL (store);

  for (next = gtk_tree_model_get_iter_first (model, &iter);
       next;
       next = gtk_tree_model_iter_next (model, &iter))
    {
      if ((parent_found = model_is_parent (model, &iter, event)))
        break;
    }

  if (parent_found)
    {
      *parent = iter;
    }
  else
    {
      GDateTime *date;
      gchar *body, *pretty_date;

      date = g_date_time_new_from_unix_local (
          tpl_event_get_timestamp (event));

      pretty_date = g_date_time_format (date,
          C_("A date with the time", "%A, %e %B %Y %X"));

      body = get_display_string_for_chat_message (message, event);

      gtk_tree_store_append (store, &iter, NULL);
      gtk_tree_store_set (store, &iter,
          COL_EVENTS_TS, tpl_event_get_timestamp (event),
          COL_EVENTS_PRETTY_DATE, pretty_date,
          COL_EVENTS_TEXT, body,
          COL_EVENTS_ICON, "format-justify-fill",
          COL_EVENTS_ACCOUNT, tpl_event_get_account (event),
          COL_EVENTS_TARGET, event_get_target (event),
          COL_EVENTS_EVENT, event,
          -1);

      *parent = iter;

      g_free (body);
      g_free (pretty_date);
      g_date_time_unref (date);
    }
}

static const gchar *
get_icon_for_event (TplEvent *event)
{
  const gchar *icon = NULL;

  if (TPL_IS_TEXT_EVENT (event))
    {
      TplTextEvent *text = TPL_TEXT_EVENT (event);

      if (!tp_str_empty (tpl_text_event_get_supersedes_token (text)))
        icon = EMPATHY_IMAGE_EDIT_MESSAGE;
    }
  else if (TPL_IS_CALL_EVENT (event))
    {
      TplCallEvent *call = TPL_CALL_EVENT (event);
      TpCallStateChangeReason reason = tpl_call_event_get_end_reason (call);
      TplEntity *sender = tpl_event_get_sender (event);
      TplEntity *receiver = tpl_event_get_receiver (event);

      if (reason == TP_CALL_STATE_CHANGE_REASON_NO_ANSWER)
        icon = EMPATHY_IMAGE_CALL_MISSED;
      else if (tpl_entity_get_entity_type (sender) == TPL_ENTITY_SELF)
        icon = EMPATHY_IMAGE_CALL_OUTGOING;
      else if (tpl_entity_get_entity_type (receiver) == TPL_ENTITY_SELF)
        icon = EMPATHY_IMAGE_CALL_INCOMING;
    }

  return icon;
}

static void
log_window_append_chat_message (TplEvent *event,
    EmpathyMessage *message)
{
  GtkTreeStore *store = log_window->priv->store_events;
  GtkTreeIter iter, parent;
  gchar *pretty_date, *alias, *body;
  GDateTime *date;
  TpawStringParser *parsers;
  GString *msg;

  date = g_date_time_new_from_unix_local (
      tpl_event_get_timestamp (event));

  pretty_date = g_date_time_format (date, "%X");

  get_parent_iter_for_message (event, message, &parent);

  alias = g_markup_escape_text (
      tpl_entity_get_alias (tpl_event_get_sender (event)), -1);

  /* escape the text */
  parsers = empathy_webkit_get_string_parser (
      g_settings_get_boolean (log_window->priv->gsettings_chat,
        EMPATHY_PREFS_CHAT_SHOW_SMILEYS));
  msg = g_string_new ("");

  tpaw_string_parser_substr (empathy_message_get_body (message), -1,
      parsers, msg);

  if (tpl_text_event_get_message_type (TPL_TEXT_EVENT (event))
      == TP_CHANNEL_TEXT_MESSAGE_TYPE_ACTION)
    {
      /* Translators: this is an emote: '* Danielle waves' */
      body = g_strdup_printf (_("<i>* %s %s</i>"), alias, msg->str);
    }
  else
    {
      /* Translators: this is a message: 'Danielle: hello'
       * The string in bold is the sender's name */
      body = g_strdup_printf (_("<b>%s:</b> %s"), alias, msg->str);
    }

  gtk_tree_store_append (store, &iter, &parent);
  gtk_tree_store_set (store, &iter,
      COL_EVENTS_TS, tpl_event_get_timestamp (event),
      COL_EVENTS_PRETTY_DATE, pretty_date,
      COL_EVENTS_TEXT, body,
      COL_EVENTS_ICON, get_icon_for_event (event),
      COL_EVENTS_ACCOUNT, tpl_event_get_account (event),
      COL_EVENTS_TARGET, event_get_target (event),
      COL_EVENTS_EVENT, event,
      -1);

  g_string_free (msg, TRUE);
  g_free (body);
  g_free (alias);
  g_free (pretty_date);
  g_date_time_unref (date);
}

static void
log_window_append_call (TplEvent *event,
    EmpathyMessage *message)
{
  TplCallEvent *call = TPL_CALL_EVENT (event);
  GtkTreeStore *store = log_window->priv->store_events;
  GtkTreeIter iter, child;
  gchar *pretty_date, *duration, *finished;
  GDateTime *started_date, *finished_date;
  GTimeSpan span;

  /* If searching, only add the call if the search string appears anywhere */
  if (!TPAW_STR_EMPTY (log_window->priv->last_find))
    {
      if (strstr (tpl_entity_get_identifier (tpl_event_get_sender (event)),
              log_window->priv->last_find) == NULL &&
          strstr (tpl_entity_get_identifier (tpl_event_get_receiver (event)),
              log_window->priv->last_find) == NULL &&
          strstr (tpl_call_event_get_detailed_end_reason (call),
              log_window->priv->last_find) == NULL)
        {
          DEBUG ("TplCallEvent doesn't match search string, ignoring");
          return;
        }
    }

  started_date = g_date_time_new_from_unix_local (
      tpl_event_get_timestamp (event));

  pretty_date = g_date_time_format (started_date,
      C_("A date with the time", "%A, %e %B %Y %X"));

  gtk_tree_store_append (store, &iter, NULL);
  gtk_tree_store_set (store, &iter,
      COL_EVENTS_TS, tpl_event_get_timestamp (event),
      COL_EVENTS_PRETTY_DATE, pretty_date,
      COL_EVENTS_TEXT, empathy_message_get_body (message),
      COL_EVENTS_ICON, get_icon_for_event (event),
      COL_EVENTS_ACCOUNT, tpl_event_get_account (event),
      COL_EVENTS_TARGET, event_get_target (event),
      COL_EVENTS_EVENT, event,
      -1);

  if (tpl_call_event_get_end_reason (call) != TP_CALL_STATE_CHANGE_REASON_NO_ANSWER)
    {
      gchar *body;
      gchar *tmp;

      span = tpl_call_event_get_duration (TPL_CALL_EVENT (event));

      if (span < 60)
        {
          tmp = g_strdup_printf ("%" G_GINT64_FORMAT, span);
          duration = g_strdup_printf (
              ngettext ("%s second", "%s seconds", span), tmp);
          g_free (tmp);
        }
      else
        {
          tmp = g_strdup_printf ("%" G_GINT64_FORMAT, span / 60);
          duration = g_strdup_printf (
              ngettext ("%s minute", "%s minutes", span / 60), tmp);
          g_free (tmp);
        }

      finished_date = g_date_time_add (started_date, -span);
      finished = g_date_time_format (finished_date, "%X");
      g_date_time_unref (finished_date);

      body = g_strdup_printf (_("Call took %s, ended at %s"),
          duration, finished);

      g_free (duration);
      g_free (finished);

      gtk_tree_store_append (store, &child, &iter);
      gtk_tree_store_set (store, &child,
          COL_EVENTS_TS, tpl_event_get_timestamp (event),
          COL_EVENTS_TEXT, body,
          COL_EVENTS_ACCOUNT, tpl_event_get_account (event),
          COL_EVENTS_TARGET, event_get_target (event),
          COL_EVENTS_EVENT, event,
          -1);

      g_free (body);
    }

  g_free (pretty_date);
  g_date_time_unref (started_date);
}

static void
log_window_append_message (TplEvent *event,
    EmpathyMessage *message)
{
  if (TPL_IS_TEXT_EVENT (event))
    log_window_append_chat_message (event, message);
  else if (TPL_IS_CALL_EVENT (event))
    log_window_append_call (event, message);
  else
    DEBUG ("Message type not handled");
}

static void
add_all_accounts_and_entities (GList **accounts,
    GList **entities)
{
  GtkTreeView      *view;
  GtkTreeModel     *model;
  GtkTreeIter       iter;

  view = GTK_TREE_VIEW (log_window->priv->treeview_who);
  model = gtk_tree_view_get_model (view);

  if (!gtk_tree_model_get_iter_first (model, &iter))
    return;

  do
    {
      TpAccount *account;
      TplEntity *entity;
      gint type;

      gtk_tree_model_get (model, &iter,
          COL_WHO_ACCOUNT, &account,
          COL_WHO_TARGET, &entity,
          COL_WHO_TYPE, &type,
          -1);

      if (type != COL_TYPE_NORMAL)
        continue;

      if (accounts != NULL)
        *accounts = g_list_append (*accounts, account);

      if (entities != NULL)
        *entities = g_list_append (*entities, entity);
    }
  while (gtk_tree_model_iter_next (model, &iter));
}

static gboolean
log_window_get_selected (EmpathyLogWindow *self,
    GList **accounts,
    GList **entities,
    gboolean *anyone,
    GList **dates,
    TplEventTypeMask *event_mask,
    EventSubtype *subtype)
{
  GtkTreeView      *view;
  GtkTreeModel     *model;
  GtkTreeSelection *selection;
  GtkTreeIter       iter;
  TplEventTypeMask  ev = 0;
  EventSubtype      st = 0;
  GList            *paths, *l;
  gint              type;

  view = GTK_TREE_VIEW (self->priv->treeview_who);
  model = gtk_tree_view_get_model (view);
  selection = gtk_tree_view_get_selection (view);

  paths = gtk_tree_selection_get_selected_rows (selection, NULL);
  if (paths == NULL)
    return FALSE;

  if (accounts != NULL)
    *accounts = NULL;
  if (entities != NULL)
    *entities = NULL;
  if (anyone != NULL)
    *anyone = FALSE;

  for (l = paths; l != NULL; l = l->next)
    {
      GtkTreePath *path = l->data;
      TpAccount *account;
      TplEntity *entity;

      gtk_tree_model_get_iter (model, &iter, path);
      gtk_tree_model_get (model, &iter,
          COL_WHO_ACCOUNT, &account,
          COL_WHO_TARGET, &entity,
          COL_WHO_TYPE, &type,
          -1);

      if (type == COL_TYPE_ANY)
        {
          if (accounts != NULL || entities != NULL)
            add_all_accounts_and_entities (accounts, entities);
          if (anyone != NULL)
            *anyone = TRUE;
          break;
        }

      if (accounts != NULL)
        *accounts = g_list_append (*accounts, g_object_ref (account));

      if (entities != NULL)
        *entities = g_list_append (*entities, g_object_ref (entity));

      g_object_unref (account);
      g_object_unref (entity);
    }
  g_list_free_full (paths, (GDestroyNotify) gtk_tree_path_free);

  view = GTK_TREE_VIEW (self->priv->treeview_what);
  model = gtk_tree_view_get_model (view);
  selection = gtk_tree_view_get_selection (view);

  paths = gtk_tree_selection_get_selected_rows (selection, NULL);
  for (l = paths; l != NULL; l = l->next)
    {
      GtkTreePath *path = l->data;
      TplEventTypeMask mask;
      EventSubtype submask;

      gtk_tree_model_get_iter (model, &iter, path);
      gtk_tree_model_get (model, &iter,
          COL_WHAT_TYPE, &mask,
          COL_WHAT_SUBTYPE, &submask,
          -1);

      ev |= mask;
      st |= submask;
    }
  g_list_free_full (paths, (GDestroyNotify) gtk_tree_path_free);

  view = GTK_TREE_VIEW (self->priv->treeview_when);
  model = gtk_tree_view_get_model (view);
  selection = gtk_tree_view_get_selection (view);

  if (dates != NULL)
    {
      *dates = NULL;

      paths = gtk_tree_selection_get_selected_rows (selection, NULL);
      for (l = paths; l != NULL; l = l->next)
        {
          GtkTreePath *path = l->data;
          GDate *date;

          gtk_tree_model_get_iter (model, &iter, path);
          gtk_tree_model_get (model, &iter,
              COL_WHEN_DATE, &date,
              -1);

          *dates = g_list_append (*dates, date);
        }
      g_list_free_full (paths, (GDestroyNotify) gtk_tree_path_free);
    }

  if (event_mask != NULL)
    *event_mask = ev;

  if (subtype != NULL)
    *subtype = st;

  return TRUE;
}

static gboolean
model_has_entity (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    gpointer data)
{
  TplLogSearchHit *hit = data;
  TplEntity *e;
  TpAccount *a;
  gboolean ret = FALSE;

  gtk_tree_model_get (model, iter,
      COL_WHO_TARGET, &e,
      COL_WHO_ACCOUNT, &a,
      -1);

  if (e != NULL && entity_equal (hit->target, e) &&
      a != NULL && account_equal (hit->account, a))
    {
      ret = has_element = TRUE;
    }

  tp_clear_object (&e);
  tp_clear_object (&a);

  return ret;
}

static gboolean
model_has_date (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    gpointer data)
{
  GDate *date = data;
  GDate *d;

  gtk_tree_model_get (model, iter,
      COL_WHEN_DATE, &d,
      -1);

  if (!g_date_compare (date, d))
    {
      has_element = TRUE;
      g_date_free (d);
      return TRUE;
    }

  g_date_free (d);
  return FALSE;
}

static void
get_events_for_date (TplActionChain *chain, gpointer user_data);

static void
populate_events_from_search_hits (GList *accounts,
    GList *targets,
    GList *dates)
{
  TplEventTypeMask event_mask;
  EventSubtype subtype;
  GDate *anytime;
  GList *l;
  gboolean is_anytime = FALSE;

  if (!log_window_get_selected (log_window,
      NULL, NULL, NULL, NULL, &event_mask, &subtype))
    return;

  anytime = g_date_new_dmy (2, 1, -1);
  if (g_list_find_custom (dates, anytime, (GCompareFunc) g_date_compare))
    is_anytime = TRUE;

  for (l = log_window->priv->hits; l != NULL; l = l->next)
    {
      TplLogSearchHit *hit = l->data;
      GList *acc, *targ;
      gboolean found = FALSE;

      /* Protect against invalid data (corrupt or old log files). */
      if (hit->account == NULL || hit->target == NULL)
        continue;

      for (acc = accounts, targ = targets;
           acc != NULL && targ != NULL && !found;
           acc = acc->next, targ = targ->next)
        {
          TpAccount *account = acc->data;
          TplEntity *target = targ->data;

          if (account_equal (hit->account, account) &&
              entity_equal (hit->target, target))
            found = TRUE;
        }

        if (!found)
          continue;

      if (is_anytime ||
          g_list_find_custom (dates, hit->date, (GCompareFunc) g_date_compare)
              != NULL)
        {
          Ctx *ctx;

          ctx = ctx_new (log_window, hit->account, hit->target, hit->date,
              event_mask, subtype, log_window->priv->count);
          _tpl_action_chain_append (log_window->priv->chain,
              get_events_for_date, ctx);
        }
    }

  start_spinner ();
  _tpl_action_chain_start (log_window->priv->chain);

  g_date_free (anytime);
}

static gchar *
format_date_for_display (GDate *date)
{
  gchar *text;
  GDate *now = NULL;
  gint days_elapsed;

  /* g_date_strftime sucks */

  now = g_date_new ();
  g_date_set_time_t (now, time (NULL));

  days_elapsed = g_date_days_between (date, now);

  if (days_elapsed < 0)
    {
      text = NULL;
    }
  else if (days_elapsed == 0)
    {
      text = g_strdup (_("Today"));
    }
  else if (days_elapsed == 1)
    {
      text = g_strdup (_("Yesterday"));
    }
  else
    {
      GDateTime *dt;

      dt = g_date_time_new_utc (g_date_get_year (date),
          g_date_get_month (date), g_date_get_day (date),
          0, 0, 0);

      if (days_elapsed <= 7)
        text = g_date_time_format (dt, "%A");
      else
        text = g_date_time_format (dt,
            /* Translators: A date such as '23 May 2010' (strftime format) */
            _("%e %B %Y"));

      g_date_time_unref (dt);
    }

  g_date_free (now);

  return text;
}

static void
add_date_if_needed (EmpathyLogWindow *self,
    GDate *date)
{
  GtkTreeModel *model;
  GtkListStore *store;
  gchar *text;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (
        log_window->priv->treeview_when));
  store = GTK_LIST_STORE (model);

  /* Add the date if it's not already there */
  has_element = FALSE;
  gtk_tree_model_foreach (model, model_has_date, date);
  if (has_element)
    return;

  text = format_date_for_display (date);

  gtk_list_store_insert_with_values (store, NULL, -1,
      COL_WHEN_DATE, date,
      COL_WHEN_TEXT, text,
      COL_WHEN_ICON, CALENDAR_ICON,
      -1);

  g_free (text);
}

static void
populate_dates_from_search_hits (GList *accounts,
    GList *targets)
{
  GList *l;
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkListStore *store;
  GtkTreeSelection *selection;
  GtkTreeIter iter;

  if (log_window == NULL)
    return;

  view = GTK_TREE_VIEW (log_window->priv->treeview_when);
  model = gtk_tree_view_get_model (view);
  store = GTK_LIST_STORE (model);
  selection = gtk_tree_view_get_selection (view);

  for (l = log_window->priv->hits; l != NULL; l = l->next)
    {
      TplLogSearchHit *hit = l->data;
      GList *acc, *targ;
      gboolean found = FALSE;

      /* Protect against invalid data (corrupt or old log files). */
      if (hit->account == NULL || hit->target == NULL)
        continue;

      for (acc = accounts, targ = targets;
           acc != NULL && targ != NULL && !found;
           acc = acc->next, targ = targ->next)
        {
          TpAccount *account = acc->data;
          TplEntity *target = targ->data;

          if (account_equal (hit->account, account) &&
              entity_equal (hit->target, target))
            found = TRUE;
        }

        if (!found)
          continue;

      add_date_if_needed (log_window, hit->date);
    }

  if (gtk_tree_model_get_iter_first (model, &iter))
    {
      GDate *date;

      date = g_date_new_dmy (1, 1, -1),

      gtk_list_store_prepend (store, &iter);
      gtk_list_store_set (store, &iter,
          COL_WHEN_DATE, date,
          COL_WHEN_TEXT, "separator",
          -1);

      g_date_free (date);

      date = g_date_new_dmy (2, 1, -1),
      gtk_list_store_prepend (store, &iter);
      gtk_list_store_set (store, &iter,
          COL_WHEN_DATE, date,
          COL_WHEN_TEXT, _("Anytime"),
          -1);

      g_date_free (date);

      if (gtk_tree_model_iter_nth_child (model, &iter, NULL, 2))
        gtk_tree_selection_select_iter (selection, &iter);
    }
}

static void
add_event_to_store (EmpathyLogWindow *self,
    TpAccount *account,
    TplEntity *entity)
{
  GtkListStore *store;
  TplEntityType type = tpl_entity_get_entity_type (entity);
  EmpathyContact *contact;
  const gchar *name;
  gchar *sort_key;
  gboolean room = type == TPL_ENTITY_ROOM;

  store = GTK_LIST_STORE (gtk_tree_view_get_model (
        GTK_TREE_VIEW (log_window->priv->treeview_who)));

  contact = empathy_contact_from_tpl_contact (account, entity);

  name = empathy_contact_get_alias (contact);
  sort_key = g_utf8_collate_key (name, -1);

  gtk_list_store_insert_with_values (store, NULL, -1,
      COL_WHO_TYPE, COL_TYPE_NORMAL,
      COL_WHO_ICON, room ? EMPATHY_IMAGE_GROUP_MESSAGE
                         : TPAW_IMAGE_AVATAR_DEFAULT,
      COL_WHO_NAME, name,
      COL_WHO_NAME_SORT_KEY, sort_key,
      COL_WHO_ID, tpl_entity_get_identifier (entity),
      COL_WHO_ACCOUNT, account,
      COL_WHO_TARGET, entity,
      -1);

  g_free (sort_key);
  g_object_unref (contact);
}

static void
populate_entities_from_search_hits (void)
{
  EmpathyAccountChooser *account_chooser;
  TpAccount *account;
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeSelection *selection;
  GtkTreeIter iter;
  GtkListStore *store;
  GList *l;

  view = GTK_TREE_VIEW (log_window->priv->treeview_who);
  model = gtk_tree_view_get_model (view);
  store = GTK_LIST_STORE (model);
  selection = gtk_tree_view_get_selection (view);

  gtk_list_store_clear (store);

  account_chooser = EMPATHY_ACCOUNT_CHOOSER (log_window->priv->account_chooser);
  account = empathy_account_chooser_get_account (account_chooser);

  for (l = log_window->priv->hits; l; l = l->next)
    {
      TplLogSearchHit *hit = l->data;

      /* Protect against invalid data (corrupt or old log files). */
      if (hit->account == NULL || hit->target == NULL)
        continue;

      /* Filter based on the selected account */
      if (account != NULL && !account_equal (account, hit->account))
        continue;

      /* Add the entity if it's not already there */
      has_element = FALSE;
      gtk_tree_model_foreach (model, model_has_entity, hit);
      if (!has_element)
        {
          add_event_to_store (log_window, hit->account, hit->target);
        }
    }

  if (gtk_tree_model_get_iter_first (model, &iter))
    {
      gtk_list_store_prepend (store, &iter);
      gtk_list_store_set (store, &iter,
          COL_WHO_TYPE, COL_TYPE_SEPARATOR,
          COL_WHO_NAME, "separator",
          -1);

      gtk_list_store_prepend (store, &iter);
      gtk_list_store_set (store, &iter,
          COL_WHO_TYPE, COL_TYPE_ANY,
          COL_WHO_NAME, _("Anyone"),
          -1);
    }

  /* Select 'Anyone' */
  if (gtk_tree_model_get_iter_first (model, &iter))
    gtk_tree_selection_select_iter (selection, &iter);
}

static void
log_manager_searched_new_cb (GObject *manager,
    GAsyncResult *result,
    gpointer user_data)
{
  GList *hits;
  GtkTreeView *view;
  GtkTreeSelection *selection;
  GError *error = NULL;

  if (log_window == NULL)
    return;

  if (!tpl_log_manager_search_finish (TPL_LOG_MANAGER (manager),
      result, &hits, &error))
    {
      DEBUG ("%s. Aborting", error->message);
      g_error_free (error);
      return;
    }

  tp_clear_pointer (&log_window->priv->hits, tpl_log_manager_search_free);
  log_window->priv->hits = hits;

  view = GTK_TREE_VIEW (log_window->priv->treeview_when);
  selection = gtk_tree_view_get_selection (view);

  g_signal_handlers_unblock_by_func (selection,
      log_window_when_changed_cb,
      log_window);

  populate_entities_from_search_hits ();
}

static void
log_window_find_populate (EmpathyLogWindow *self,
    const gchar *search_criteria)
{
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeSelection *selection;
  GtkListStore *store;

  gtk_tree_store_clear (self->priv->store_events);

  view = GTK_TREE_VIEW (self->priv->treeview_who);
  model = gtk_tree_view_get_model (view);
  store = GTK_LIST_STORE (model);

  gtk_list_store_clear (store);

  view = GTK_TREE_VIEW (self->priv->treeview_when);
  model = gtk_tree_view_get_model (view);
  store = GTK_LIST_STORE (model);
  selection = gtk_tree_view_get_selection (view);

  gtk_list_store_clear (store);

  if (TPAW_STR_EMPTY (search_criteria))
    {
      tp_clear_pointer (&self->priv->hits, tpl_log_manager_search_free);
      webkit_web_view_set_highlight_text_matches (
          WEBKIT_WEB_VIEW (self->priv->webview), FALSE);
      log_window_who_populate (self);
      return;
    }

  g_signal_handlers_block_by_func (selection,
      log_window_when_changed_cb,
      self);

  /* highlight the search text */
  webkit_web_view_mark_text_matches (WEBKIT_WEB_VIEW (self->priv->webview),
      search_criteria, FALSE, 0);

  tpl_log_manager_search_async (self->priv->log_manager,
      search_criteria, TPL_EVENT_MASK_ANY,
      log_manager_searched_new_cb, NULL);
}

static gboolean
start_find_search (EmpathyLogWindow *self)
{
  const gchar *str;

  str = gtk_entry_get_text (GTK_ENTRY (self->priv->search_entry));

  /* Don't find the same crap again */
  if (self->priv->last_find && !tp_strdiff (self->priv->last_find, str))
    return FALSE;

  g_free (self->priv->last_find);
  self->priv->last_find = g_strdup (str);

  log_window_find_populate (self, str);

  return FALSE;
}

static void
log_window_search_entry_changed_cb (GtkWidget *entry,
    EmpathyLogWindow *self)
{
  const gchar *str;

  str = gtk_entry_get_text (GTK_ENTRY (self->priv->search_entry));

  if (!tp_str_empty (str))
    {
      gtk_entry_set_icon_from_icon_name (GTK_ENTRY (self->priv->search_entry),
          GTK_ENTRY_ICON_SECONDARY, "edit-clear-symbolic");
      gtk_entry_set_icon_sensitive (GTK_ENTRY (self->priv->search_entry),
          GTK_ENTRY_ICON_SECONDARY, TRUE);
    }
  else
    {
      gtk_entry_set_icon_from_icon_name (GTK_ENTRY (self->priv->search_entry),
          GTK_ENTRY_ICON_SECONDARY, "edit-find-symbolic");
      gtk_entry_set_icon_sensitive (GTK_ENTRY (self->priv->search_entry),
          GTK_ENTRY_ICON_SECONDARY, FALSE);
    }

  if (self->priv->source != 0)
    g_source_remove (self->priv->source);
  self->priv->source = g_timeout_add (500, (GSourceFunc) start_find_search,
      self);
}

static void
log_window_search_entry_activate_cb (GtkWidget *entry,
    EmpathyLogWindow *self)
{
  start_find_search (self);
}

static void
log_window_search_entry_icon_pressed_cb (GtkEntry *entry,
    GtkEntryIconPosition icon_pos,
    GdkEvent *event,
    gpointer user_data)
{
  if (icon_pos != GTK_ENTRY_ICON_SECONDARY)
    return;

  gtk_entry_buffer_set_text (gtk_entry_get_buffer (entry),
    "", -1);
}

static void
do_update_buttons_sensitivity (EmpathyLogWindow *self)
{
  EmpathyCapabilities capabilities;
  gboolean profile, chat, call, video;

  tp_clear_object (&self->priv->button_video_binding);

  if (self->priv->selected_contact != NULL)
    {
      capabilities = empathy_contact_get_capabilities (
          self->priv->selected_contact);

      profile = chat = TRUE;
      call = capabilities & EMPATHY_CAPABILITIES_AUDIO;
      video = capabilities & EMPATHY_CAPABILITIES_VIDEO;
    }
  else
    {
      profile = chat = call = video = FALSE;
    }

  gtk_widget_set_sensitive (self->priv->button_profile, profile);
  gtk_widget_set_sensitive (self->priv->button_chat, chat);
  gtk_widget_set_sensitive (self->priv->button_call, call);

  if (video)
    {
      self->priv->button_video_binding = g_object_bind_property (
          self->priv->camera_monitor, "available",
          self->priv->button_video, "sensitive",
          G_BINDING_SYNC_CREATE);
    }
  else
    {
      /* Don't override the binding */
      gtk_widget_set_sensitive (self->priv->button_video, video);
    }
}

static void
contact_capabilities_changed_cb (EmpathyContact *contact,
    GParamSpec *spec,
    EmpathyLogWindow *self)
{
  do_update_buttons_sensitivity (self);
}

static void
log_window_update_buttons_sensitivity (EmpathyLogWindow *self)
{
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeSelection *selection;
  TpAccount *account;
  TplEntity *target;
  GtkTreeIter iter;
  GList *paths;
  GtkTreePath *path;

  if (self->priv->selected_contact != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->priv->selected_contact,
          contact_capabilities_changed_cb, self);

      tp_clear_object (&self->priv->selected_contact);
    }

  view = GTK_TREE_VIEW (self->priv->treeview_who);
  model = gtk_tree_view_get_model (view);
  selection = gtk_tree_view_get_selection (view);

  if (!gtk_tree_model_get_iter_first (model, &iter))
    goto events;

  if (gtk_tree_selection_count_selected_rows (selection) != 1)
    goto events;

  if (gtk_tree_selection_iter_is_selected (selection, &iter))
    goto events;

  paths = gtk_tree_selection_get_selected_rows (selection, &model);
  g_return_if_fail (paths != NULL);

  path = paths->data;
  gtk_tree_model_get_iter (model, &iter, path);
  gtk_tree_model_get (model, &iter,
      COL_WHO_ACCOUNT, &account,
      COL_WHO_TARGET, &target,
      -1);

  g_list_free_full (paths, (GDestroyNotify) gtk_tree_path_free);

  self->priv->selected_contact = empathy_contact_from_tpl_contact (account,
      target);

  g_object_unref (account);
  g_object_unref (target);

  goto out;

 events:
  /* If the Who pane doesn't contain a contact (e.g. it has many
   * selected, or has 'Anyone', let's try to get the contact from
   * the selected event. */

  if (self->priv->events_contact != NULL)
    self->priv->selected_contact = g_object_ref (self->priv->events_contact);

 out:
  if (self->priv->selected_contact != NULL)
    {
      tp_g_signal_connect_object (self->priv->selected_contact,
          "notify::capabilities", G_CALLBACK (contact_capabilities_changed_cb),
          self, 0);
    }

  do_update_buttons_sensitivity (self);
}

static void
log_window_update_what_iter_sensitivity (GtkTreeModel *model,
    GtkTreeIter *iter,
    gboolean sensitive)
{
  GtkTreeStore *store = GTK_TREE_STORE (model);
  GtkTreeIter child;
  gboolean next;

  gtk_tree_store_set (store, iter,
      COL_WHAT_SENSITIVE, sensitive,
      -1);

  for (next = gtk_tree_model_iter_children (model, &child, iter);
       next;
       next = gtk_tree_model_iter_next (model, &child))
    {
      gtk_tree_store_set (store, &child,
          COL_WHAT_SENSITIVE, sensitive,
          -1);
    }
}

static void
log_window_update_what_sensitivity (EmpathyLogWindow *self)
{
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeIter iter;
  GList *accounts, *targets, *acc, *targ;
  gboolean next;

  if (!log_window_get_selected (self, &accounts, &targets, NULL, NULL,
      NULL, NULL))
    return;

  view = GTK_TREE_VIEW (self->priv->treeview_what);
  model = gtk_tree_view_get_model (view);

  /* For each event type... */
  for (next = gtk_tree_model_get_iter_first (model, &iter);
       next;
       next = gtk_tree_model_iter_next (model, &iter))
    {
      TplEventTypeMask type;

      gtk_tree_model_get (model, &iter,
          COL_WHAT_TYPE, &type,
          -1);

      /* ...we set the type and its subtypes (if any) unsensitive... */
      log_window_update_what_iter_sensitivity (model, &iter, FALSE);

      for (acc = accounts, targ = targets;
           acc != NULL && targ != NULL;
           acc = acc->next, targ = targ->next)
        {
          TpAccount *account = acc->data;
          TplEntity *target = targ->data;

          if (tpl_log_manager_exists (self->priv->log_manager,
                  account, target, type))
            {
              /* And then we set it (and its subtypes, again, if any)
               * as sensitive if there are logs of that type. */
              log_window_update_what_iter_sensitivity (model, &iter, TRUE);
              break;
            }
        }
    }

  g_list_free_full (accounts, g_object_unref);
  g_list_free_full (targets, g_object_unref);
}

static void
log_window_who_changed_cb (GtkTreeSelection *selection,
    EmpathyLogWindow *self)
{
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeIter iter;

  DEBUG ("log_window_who_changed_cb");

  view = gtk_tree_selection_get_tree_view (selection);
  model = gtk_tree_view_get_model (view);

  if (gtk_tree_model_get_iter_first (model, &iter))
    {
      /* If 'Anyone' is selected, everything else should be deselected */
      if (gtk_tree_selection_iter_is_selected (selection, &iter))
        {
          g_signal_handlers_block_by_func (selection,
              log_window_who_changed_cb,
              self);

          gtk_tree_selection_unselect_all (selection);
          gtk_tree_selection_select_iter (selection, &iter);

          g_signal_handlers_unblock_by_func (selection,
              log_window_who_changed_cb,
              self);
        }
    }

  log_window_update_what_sensitivity (self);
  log_window_update_buttons_sensitivity (self);

  /* The contact changed, so the dates need to be updated */
  log_window_chats_get_messages (self, TRUE);
}

static void
log_manager_got_entities_cb (GObject *manager,
    GAsyncResult *result,
    gpointer user_data)
{
  Ctx                   *ctx = user_data;
  GList                 *entities;
  GList                 *l;
  GtkTreeView           *view;
  GtkTreeModel          *model;
  GtkTreeSelection      *selection;
  GtkListStore          *store;
  GtkTreeIter            iter;
  GError                *error = NULL;
  gboolean               select_account = FALSE;

  if (log_window == NULL)
    goto out;

  if (log_window->priv->count != ctx->count)
    goto out;

  if (!tpl_log_manager_get_entities_finish (TPL_LOG_MANAGER (manager),
      result, &entities, &error))
    {
      DEBUG ("%s. Aborting", error->message);
      g_error_free (error);
      goto out;
    }

  view = GTK_TREE_VIEW (ctx->self->priv->treeview_who);
  model = gtk_tree_view_get_model (view);
  selection = gtk_tree_view_get_selection (view);
  store = GTK_LIST_STORE (model);

  /* Block signals to stop the logs being retrieved prematurely  */
  g_signal_handlers_block_by_func (selection,
      log_window_who_changed_cb, ctx->self);

  for (l = entities; l; l = l->next)
    {
      add_event_to_store (ctx->self, ctx->account, l->data);

      if (ctx->self->priv->selected_account != NULL &&
          !tp_strdiff (tp_proxy_get_object_path (ctx->account),
          tp_proxy_get_object_path (ctx->self->priv->selected_account)))
        select_account = TRUE;
    }
  g_list_free_full (entities, g_object_unref);

  if (gtk_tree_model_get_iter_first (model, &iter))
    {
      gint type;

      gtk_tree_model_get (model, &iter,
          COL_WHO_TYPE, &type,
          -1);

      if (type != COL_TYPE_ANY)
        {
          gtk_list_store_prepend (store, &iter);
          gtk_list_store_set (store, &iter,
              COL_WHO_TYPE, COL_TYPE_SEPARATOR,
              COL_WHO_NAME, "separator",
              -1);

          gtk_list_store_prepend (store, &iter);
          gtk_list_store_set (store, &iter,
              COL_WHO_TYPE, COL_TYPE_ANY,
              COL_WHO_NAME, _("Anyone"),
              -1);
        }
    }

  /* Unblock signals */
  g_signal_handlers_unblock_by_func (selection,
      log_window_who_changed_cb,
      ctx->self);

  /* We display the selected account if we populate the model with chats from
   * this account. */
  if (select_account)
    log_window_chats_set_selected (ctx->self);

out:
  _tpl_action_chain_continue (log_window->priv->chain);
  ctx_free (ctx);
}

static void
get_entities_for_account (TplActionChain *chain, gpointer user_data)
{
  Ctx *ctx = user_data;

  tpl_log_manager_get_entities_async (ctx->self->priv->log_manager, ctx->account,
      log_manager_got_entities_cb, ctx);
}

static void
select_first_entity (TplActionChain *chain, gpointer user_data)
{
  EmpathyLogWindow *self = user_data;
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeSelection *selection;
  GtkTreeIter iter;

  view = GTK_TREE_VIEW (self->priv->treeview_who);
  model = gtk_tree_view_get_model (view);
  selection = gtk_tree_view_get_selection (view);

  if (gtk_tree_model_get_iter_first (model, &iter))
    gtk_tree_selection_select_iter (selection, &iter);

  _tpl_action_chain_continue (self->priv->chain);
}

static void
log_window_who_populate (EmpathyLogWindow *self)
{
  EmpathyAccountChooser *account_chooser;
  TpAccount *account;
  gboolean all_accounts;
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeSelection *selection;
  GtkListStore *store;
  Ctx *ctx;

  if (self->priv->hits != NULL)
    {
      populate_entities_from_search_hits ();
      return;
    }

  account_chooser = EMPATHY_ACCOUNT_CHOOSER (self->priv->account_chooser);
  account = empathy_account_chooser_dup_account (account_chooser);
  all_accounts = empathy_account_chooser_has_all_selected (account_chooser);

  view = GTK_TREE_VIEW (self->priv->treeview_who);
  model = gtk_tree_view_get_model (view);
  selection = gtk_tree_view_get_selection (view);
  store = GTK_LIST_STORE (model);

  /* Block signals to stop the logs being retrieved prematurely  */
  g_signal_handlers_block_by_func (selection,
      log_window_who_changed_cb,
      self);

  gtk_list_store_clear (store);

  /* Unblock signals */
  g_signal_handlers_unblock_by_func (selection,
      log_window_who_changed_cb,
      self);

  _tpl_action_chain_clear (self->priv->chain);
  self->priv->count++;

  if (!all_accounts && account == NULL)
    {
      return;
    }
  else if (!all_accounts)
    {
      ctx = ctx_new (self, account, NULL, NULL, 0, 0, self->priv->count);
      _tpl_action_chain_append (self->priv->chain, get_entities_for_account, ctx);
    }
  else
    {
      TpAccountManager *manager;
      GList *accounts, *l;

      manager = empathy_account_chooser_get_account_manager (account_chooser);
      accounts = tp_account_manager_dup_valid_accounts (manager);

      for (l = accounts; l != NULL; l = l->next)
        {
          account = l->data;

          ctx = ctx_new (self, account, NULL, NULL, 0, 0, self->priv->count);
          _tpl_action_chain_append (self->priv->chain,
              get_entities_for_account, ctx);
        }

      g_list_free_full (accounts, g_object_unref);
    }
  _tpl_action_chain_append (self->priv->chain, select_first_entity, self);
  _tpl_action_chain_start (self->priv->chain);
}

static gint
sort_by_name_key (GtkTreeModel *model,
    GtkTreeIter *a,
    GtkTreeIter *b,
    gpointer user_data)
{
  gchar *key1, *key2;
  gint type1, type2;
  gint ret;

  gtk_tree_model_get (model, a,
      COL_WHO_TYPE, &type1,
      COL_WHO_NAME_SORT_KEY, &key1,
      -1);

  gtk_tree_model_get (model, b,
      COL_WHO_TYPE, &type2,
      COL_WHO_NAME_SORT_KEY, &key2,
      -1);

  if (type1 == COL_TYPE_ANY)
    ret = -1;
  else if (type2 == COL_TYPE_ANY)
    ret = 1;
  else if (type1 == COL_TYPE_SEPARATOR)
    ret = -1;
  else if (type2 == COL_TYPE_SEPARATOR)
    ret = 1;
  else
    ret = g_strcmp0 (key1, key2);

  g_free (key1);
  g_free (key2);

  return ret;
}

static gboolean
who_row_is_separator (GtkTreeModel *model,
    GtkTreeIter *iter,
    gpointer data)
{
  gint type;

  gtk_tree_model_get (model, iter,
      COL_WHO_TYPE, &type,
      -1);

  return (type == COL_TYPE_SEPARATOR);
}

static void
log_window_find_row (EmpathyLogWindow *self,
    GdkEventButton *event)
{
  WebKitHitTestResult *hit = webkit_web_view_get_hit_test_result (
      WEBKIT_WEB_VIEW (self->priv->webview), event);
  WebKitDOMNode *inner_node;

  tp_clear_object (&self->priv->events_contact);

  g_object_get (hit,
      "inner-node", &inner_node,
      NULL);

  if (inner_node != NULL)
    {
      GtkTreeModel *model = GTK_TREE_MODEL (self->priv->store_events);
      WebKitDOMNode *node;
      const char *path = NULL;
      GtkTreeIter iter;

      /* walk back up the DOM tree looking for a node with empathy:path set */
      for (node = inner_node; node != NULL;
           node = webkit_dom_node_get_parent_node (node))
        {
          if (!WEBKIT_DOM_IS_ELEMENT (node))
            continue;

          path = webkit_dom_element_get_attribute_ns (
              WEBKIT_DOM_ELEMENT (node), EMPATHY_NS, "path");

          if (!tp_str_empty (path))
            break;
        }

      /* look up the contact for this path */
      if (!tp_str_empty (path) &&
          gtk_tree_model_get_iter_from_string (model, &iter, path))
        {
          TpAccount *account;
          TplEntity *target;

          gtk_tree_model_get (model, &iter,
              COL_EVENTS_ACCOUNT, &account,
              COL_EVENTS_TARGET, &target,
              -1);

          self->priv->events_contact = empathy_contact_from_tpl_contact (
              account, target);

          g_object_unref (account);
          g_object_unref (target);
        }

      g_object_unref (inner_node);
    }

  g_object_unref (hit);

  log_window_update_buttons_sensitivity (self);
}

static gboolean
log_window_events_button_press_event (GtkWidget *webview,
    GdkEventButton *event,
    EmpathyLogWindow *self)
{
  switch (event->button)
    {
      case 1:
        log_window_find_row (self, event);
        break;

      case 3:
        empathy_webkit_context_menu_for_event (
            WEBKIT_WEB_VIEW (webview), event, 0);
        return TRUE;

      default:
        break;
    }

  return FALSE;
}

static void
log_window_events_setup (EmpathyLogWindow *self)
{
  GtkTreeSortable   *sortable;
  GtkTreeStore      *store;

  /* new store */
  self->priv->store_events = store = gtk_tree_store_new (COL_EVENTS_COUNT,
      G_TYPE_INT,           /* type */
      G_TYPE_INT64,         /* timestamp */
      G_TYPE_STRING,        /* stringified date */
      G_TYPE_STRING,        /* icon */
      G_TYPE_STRING,        /* name */
      TP_TYPE_ACCOUNT,      /* account */
      TPL_TYPE_ENTITY,      /* target */
      TPL_TYPE_EVENT);      /* event */

  sortable = GTK_TREE_SORTABLE (store);

  gtk_tree_sortable_set_sort_column_id (sortable,
      COL_EVENTS_TS,
      GTK_SORT_ASCENDING);
}

static void
log_window_who_setup (EmpathyLogWindow *self)
{
  GtkTreeView       *view;
  GtkTreeModel      *model;
  GtkTreeSelection  *selection;
  GtkTreeSortable   *sortable;
  GtkTreeViewColumn *column;
  GtkListStore      *store;
  GtkCellRenderer   *cell;

  view = GTK_TREE_VIEW (self->priv->treeview_who);
  selection = gtk_tree_view_get_selection (view);

  /* new store */
  store = gtk_list_store_new (COL_WHO_COUNT,
      G_TYPE_INT,           /* type */
      G_TYPE_STRING,        /* icon */
      G_TYPE_STRING,        /* name */
      G_TYPE_STRING,        /* name sort key */
      G_TYPE_STRING,        /* id */
      TP_TYPE_ACCOUNT,      /* account */
      TPL_TYPE_ENTITY);     /* target */

  model = GTK_TREE_MODEL (store);
  sortable = GTK_TREE_SORTABLE (store);

  gtk_tree_view_set_model (view, model);

  /* new column */
  column = gtk_tree_view_column_new ();
  gtk_tree_view_column_set_title (column, _("Who"));

  cell = gtk_cell_renderer_pixbuf_new ();
  gtk_tree_view_column_pack_start (column, cell, FALSE);
  gtk_tree_view_column_add_attribute (column, cell,
      "icon-name",
      COL_WHO_ICON);

  cell = gtk_cell_renderer_text_new ();
  g_object_set (cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  gtk_tree_view_column_pack_start (column, cell, TRUE);
  gtk_tree_view_column_add_attribute (column, cell,
      "text",
      COL_WHO_NAME);

  gtk_tree_view_append_column (view, column);

  /* set up treeview properties */
  gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);
  gtk_tree_view_set_row_separator_func (view, who_row_is_separator,
      NULL, NULL);

  gtk_tree_sortable_set_sort_column_id (sortable,
      COL_WHO_NAME_SORT_KEY,
      GTK_SORT_ASCENDING);
  gtk_tree_sortable_set_sort_func (sortable,
      COL_WHO_NAME_SORT_KEY, sort_by_name_key,
      NULL, NULL);

  gtk_tree_view_set_search_column (view, COL_WHO_NAME);
  gtk_tree_view_set_tooltip_column (view, COL_WHO_ID);

  /* set up signals */
  g_signal_connect (selection, "changed",
      G_CALLBACK (log_window_who_changed_cb), self);

  g_object_unref (store);
}

static void
log_window_chats_accounts_changed_cb (GtkWidget *combobox,
    EmpathyLogWindow *self)
{
  /* Clear all current messages shown in the textview */
  gtk_tree_store_clear (self->priv->store_events);

  log_window_who_populate (self);
}

static void
log_window_chats_set_selected (EmpathyLogWindow *self)
{
  GtkTreeView          *view;
  GtkTreeModel         *model;
  GtkTreeSelection     *selection;
  GtkTreeIter           iter;
  GtkTreePath          *path;
  gboolean              next;

  view = GTK_TREE_VIEW (self->priv->treeview_who);
  model = gtk_tree_view_get_model (view);
  selection = gtk_tree_view_get_selection (view);

  for (next = gtk_tree_model_get_iter_first (model, &iter);
       next;
       next = gtk_tree_model_iter_next (model, &iter))
    {
      TpAccount   *this_account;
      TplEntity   *this_target;
      const gchar *this_chat_id;
      gboolean     this_is_chatroom;
      gint         this_type;

      gtk_tree_model_get (model, &iter,
          COL_WHO_TYPE, &this_type,
          COL_WHO_ACCOUNT, &this_account,
          COL_WHO_TARGET, &this_target,
          -1);

      if (this_type != COL_TYPE_NORMAL)
        continue;

      this_chat_id = tpl_entity_get_identifier (this_target);
      this_is_chatroom = tpl_entity_get_entity_type (this_target)
          == TPL_ENTITY_ROOM;

      if (this_account == self->priv->selected_account &&
          !tp_strdiff (this_chat_id, self->priv->selected_chat_id) &&
          this_is_chatroom == self->priv->selected_is_chatroom)
        {
          gtk_tree_selection_select_iter (selection, &iter);
          path = gtk_tree_model_get_path (model, &iter);
          gtk_tree_view_scroll_to_cell (view, path, NULL, TRUE, 0.5, 0.0);
          gtk_tree_path_free (path);
          g_object_unref (this_account);
          g_object_unref (this_target);
          break;
        }

      g_object_unref (this_account);
      g_object_unref (this_target);
    }

  tp_clear_object (&self->priv->selected_account);
  tp_clear_pointer (&self->priv->selected_chat_id, g_free);
}

static gint
sort_by_date (GtkTreeModel *model,
    GtkTreeIter *a,
    GtkTreeIter *b,
    gpointer user_data)
{
  GDate *date1, *date2;
  gint result;

  gtk_tree_model_get (model, a,
      COL_WHEN_DATE, &date1,
      -1);

  gtk_tree_model_get (model, b,
      COL_WHEN_DATE, &date2,
      -1);

  result =  g_date_compare (date1, date2);

  g_date_free (date1);
  g_date_free (date2);
  return result;
}

static gboolean
when_row_is_separator (GtkTreeModel *model,
    GtkTreeIter *iter,
    gpointer data)
{
  gchar *when;
  gboolean ret;

  gtk_tree_model_get (model, iter,
      COL_WHEN_TEXT, &when,
      -1);

  ret = !tp_strdiff (when, "separator");
  g_free (when);
  return ret;
}

static void
log_window_when_changed_cb (GtkTreeSelection *selection,
    EmpathyLogWindow *self)
{
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeIter iter;

  DEBUG ("log_window_when_changed_cb");

  view = gtk_tree_selection_get_tree_view (selection);
  model = gtk_tree_view_get_model (view);

  /* If 'Anytime' is selected, everything else should be deselected */
  if (gtk_tree_model_get_iter_first (model, &iter))
    {
      if (gtk_tree_selection_iter_is_selected (selection, &iter))
        {
          g_signal_handlers_block_by_func (selection,
              log_window_when_changed_cb,
              self);

          gtk_tree_selection_unselect_all (selection);
          gtk_tree_selection_select_iter (selection, &iter);

          g_signal_handlers_unblock_by_func (selection,
              log_window_when_changed_cb,
              self);
        }
    }

  log_window_chats_get_messages (self, FALSE);
}

static void
log_window_when_setup (EmpathyLogWindow *self)
{
  GtkTreeView       *view;
  GtkTreeModel      *model;
  GtkTreeSelection  *selection;
  GtkTreeSortable   *sortable;
  GtkTreeViewColumn *column;
  GtkListStore      *store;
  GtkCellRenderer   *cell;

  view = GTK_TREE_VIEW (self->priv->treeview_when);
  selection = gtk_tree_view_get_selection (view);

  /* new store */
  store = gtk_list_store_new (COL_WHEN_COUNT,
      G_TYPE_DATE,        /* date */
      G_TYPE_STRING,      /* stringified date */
      G_TYPE_STRING);     /* icon */

  model = GTK_TREE_MODEL (store);
  sortable = GTK_TREE_SORTABLE (store);

  gtk_tree_view_set_model (view, model);

  /* new column */
  column = gtk_tree_view_column_new ();
  gtk_tree_view_column_set_title (column, _("When"));

  cell = gtk_cell_renderer_pixbuf_new ();
  gtk_tree_view_column_pack_start (column, cell, FALSE);
  gtk_tree_view_column_add_attribute (column, cell,
      "icon-name", COL_WHEN_ICON);

  cell = gtk_cell_renderer_text_new ();
  g_object_set (cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  gtk_tree_view_column_pack_start (column, cell, TRUE);
  gtk_tree_view_column_add_attribute (column, cell,
      "text",
      COL_WHEN_TEXT);

  gtk_tree_view_append_column (view, column);

  /* set up treeview properties */
  gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);
  gtk_tree_view_set_row_separator_func (view, when_row_is_separator,
      NULL, NULL);
  gtk_tree_sortable_set_sort_column_id (sortable,
      COL_WHEN_DATE,
      GTK_SORT_DESCENDING);
  gtk_tree_sortable_set_sort_func (sortable,
      COL_WHEN_DATE, sort_by_date,
      NULL, NULL);

  gtk_tree_view_set_search_column (view, COL_WHEN_TEXT);

  /* set up signals */
  g_signal_connect (selection, "changed",
      G_CALLBACK (log_window_when_changed_cb),
      self);

  g_object_unref (store);
}

static gboolean
what_row_is_separator (GtkTreeModel *model,
    GtkTreeIter *iter,
    gpointer data)
{
  gint type;

  gtk_tree_model_get (model, iter,
      COL_WHAT_TYPE, &type,
      -1);

  return (type == WHAT_TYPE_SEPARATOR);
}

static void
log_window_what_changed_cb (GtkTreeSelection *selection,
    EmpathyLogWindow *self)
{
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeIter iter;

  DEBUG ("log_window_what_changed_cb");

  view = gtk_tree_selection_get_tree_view (selection);
  model = gtk_tree_view_get_model (view);

  /* If 'Anything' is selected, everything else should be deselected */
  if (gtk_tree_model_get_iter_first (model, &iter))
    {
      if (gtk_tree_selection_iter_is_selected (selection, &iter))
        {
          g_signal_handlers_block_by_func (selection,
              log_window_what_changed_cb,
              self);

          gtk_tree_selection_unselect_all (selection);
          gtk_tree_selection_select_iter (selection, &iter);

          g_signal_handlers_unblock_by_func (selection,
              log_window_what_changed_cb,
              self);
        }
    }

  /* The dates need to be updated if we're not searching */
  log_window_chats_get_messages (self, self->priv->hits == NULL);
}

static gboolean
log_window_what_collapse_row_cb (GtkTreeView *tree_view,
    GtkTreeIter *iter,
    GtkTreePath *path,
    gpointer user_data)
{
  /* Reject collapsing */
  return TRUE;
}

struct event
{
  gint type;
  EventSubtype subtype;
  const gchar *icon;
  const gchar *text;
};

static void
log_window_what_setup (EmpathyLogWindow *self)
{
  GtkTreeView       *view;
  GtkTreeModel      *model;
  GtkTreeSelection  *selection;
  GtkTreeViewColumn *column;
  GtkTreeIter        iter;
  GtkTreeStore      *store;
  GtkCellRenderer   *cell;
  GtkTreeIter parent;
  guint i;

  struct event events [] = {
    { TPL_EVENT_MASK_ANY, 0, NULL, _("Anything") },
    { WHAT_TYPE_SEPARATOR, 0, NULL, "separator" },
    { TPL_EVENT_MASK_TEXT, 0, "format-justify-fill", _("Text chats") },
    { TPL_EVENT_MASK_CALL, EVENT_CALL_ALL, EMPATHY_IMAGE_CALL, _("Calls") },
  };

  struct event call_events [] = {
    { TPL_EVENT_MASK_CALL, EVENT_CALL_INCOMING, EMPATHY_IMAGE_CALL_INCOMING, _("Incoming calls") },
    { TPL_EVENT_MASK_CALL, EVENT_CALL_OUTGOING, EMPATHY_IMAGE_CALL_OUTGOING, _("Outgoing calls") },
    { TPL_EVENT_MASK_CALL, EVENT_CALL_MISSED, EMPATHY_IMAGE_CALL_MISSED, _("Missed calls") }
  };

  view = GTK_TREE_VIEW (self->priv->treeview_what);
  selection = gtk_tree_view_get_selection (view);

  /* new store */
  store = gtk_tree_store_new (COL_WHAT_COUNT,
      G_TYPE_INT,         /* history type */
      G_TYPE_INT,         /* history subtype */
      G_TYPE_BOOLEAN,     /* sensitive */
      G_TYPE_STRING,      /* stringified history type */
      G_TYPE_STRING);     /* icon */

  model = GTK_TREE_MODEL (store);

  gtk_tree_view_set_model (view, model);

  /* new column */
  column = gtk_tree_view_column_new ();
  gtk_tree_view_column_set_title (column, _("What"));

  cell = gtk_cell_renderer_pixbuf_new ();
  gtk_tree_view_column_pack_start (column, cell, FALSE);
  gtk_tree_view_column_add_attribute (column, cell,
      "icon-name", COL_WHAT_ICON);

  cell = gtk_cell_renderer_text_new ();
  g_object_set (cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  gtk_tree_view_column_pack_start (column, cell, TRUE);
  gtk_tree_view_column_add_attribute (column, cell,
      "text", COL_WHAT_TEXT);
  gtk_tree_view_column_add_attribute (column, cell,
      "sensitive", COL_WHAT_SENSITIVE);

  gtk_tree_view_append_column (view, column);
  gtk_tree_view_set_search_column (view, COL_WHAT_TEXT);

  /* set up treeview properties */
  gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);
  gtk_tree_view_set_show_expanders (view, FALSE);
  gtk_tree_view_set_level_indentation (view, 12);
  gtk_tree_view_expand_all (view);
  gtk_tree_view_set_row_separator_func (view, what_row_is_separator,
      NULL, NULL);

  /* populate */
  for (i = 0; i < G_N_ELEMENTS (events); i++)
    {
      gtk_tree_store_append (store, &iter, NULL);
      gtk_tree_store_set (store, &iter,
          COL_WHAT_TYPE, events[i].type,
          COL_WHAT_SUBTYPE, events[i].subtype,
          COL_WHAT_SENSITIVE, TRUE,
          COL_WHAT_TEXT, events[i].text,
          COL_WHAT_ICON, events[i].icon,
          -1);
    }

  gtk_tree_model_iter_nth_child (model, &parent, NULL, 3);
  for (i = 0; i < G_N_ELEMENTS (call_events); i++)
    {
      gtk_tree_store_append (store, &iter, &parent);
      gtk_tree_store_set (store, &iter,
          COL_WHAT_TYPE, call_events[i].type,
          COL_WHAT_SUBTYPE, call_events[i].subtype,
          COL_WHAT_SENSITIVE, TRUE,
          COL_WHAT_TEXT, call_events[i].text,
          COL_WHAT_ICON, call_events[i].icon,
          -1);
    }

  gtk_tree_view_expand_all (view);

  /* select 'Anything' */
  if (gtk_tree_model_get_iter_first (model, &iter))
    gtk_tree_selection_select_iter (selection, &iter);

  /* set up signals */
  g_signal_connect (view, "test-collapse-row",
      G_CALLBACK (log_window_what_collapse_row_cb),
      NULL);
  g_signal_connect (selection, "changed",
      G_CALLBACK (log_window_what_changed_cb),
      self);

  g_object_unref (store);
}

static void
log_window_maybe_expand_events (void)
{
  GtkTreeModel      *model = GTK_TREE_MODEL (log_window->priv->store_events);

  /* If there's only one result, expand it */
  if (gtk_tree_model_iter_n_children (model, NULL) == 1)
    webkit_web_view_execute_script (
        WEBKIT_WEB_VIEW (log_window->priv->webview),
        "javascript:expandAll()");
}

static gboolean
show_spinner (gpointer data)
{
  gboolean active;

  if (log_window == NULL)
    return FALSE;

  g_object_get (log_window->priv->spinner, "active", &active, NULL);

  if (active)
    gtk_notebook_set_current_page (GTK_NOTEBOOK (log_window->priv->notebook),
        PAGE_SPINNER);

  return FALSE;
}

static void
show_events (TplActionChain *chain,
    gpointer user_data)
{
  log_window_maybe_expand_events ();
  gtk_spinner_stop (GTK_SPINNER (log_window->priv->spinner));
  gtk_notebook_set_current_page (GTK_NOTEBOOK (log_window->priv->notebook),
      PAGE_EVENTS);

  _tpl_action_chain_continue (chain);
}

static void
start_spinner (void)
{
  gtk_spinner_start (GTK_SPINNER (log_window->priv->spinner));
  gtk_notebook_set_current_page (GTK_NOTEBOOK (log_window->priv->notebook),
      PAGE_EMPTY);

  g_timeout_add (1000, show_spinner, NULL);
  _tpl_action_chain_append (log_window->priv->chain, show_events, NULL);
}

static void
log_window_got_messages_for_date_cb (GObject *manager,
    GAsyncResult *result,
    gpointer user_data)
{
  Ctx *ctx = user_data;
  GtkTreeModel *model;
  GtkTreeIter iter;
  GList *events;
  GList *l;
  GError *error = NULL;
  gint n;

  if (log_window == NULL)
    {
      ctx_free (ctx);
      return;
    }

  if (log_window->priv->count != ctx->count)
    goto out;

  if (!tpl_log_manager_get_events_for_date_finish (TPL_LOG_MANAGER (manager),
      result, &events, &error))
    {
      DEBUG ("Unable to retrieve messages for the selected date: %s. Aborting",
          error->message);
      g_error_free (error);
      goto out;
    }

  for (l = events; l; l = l->next)
    {
      TplEvent *event = l->data;
      gboolean append = TRUE;

      if (TPL_IS_CALL_EVENT (l->data)
          && ctx->event_mask & TPL_EVENT_MASK_CALL
          && ctx->event_mask != TPL_EVENT_MASK_ANY)
        {
          TplCallEvent *call = l->data;

          append = FALSE;

          if (ctx->subtype & EVENT_CALL_ALL)
            {
              append = TRUE;
            }
          else
            {
              TpCallStateChangeReason reason =
                tpl_call_event_get_end_reason (call);
              TplEntity *sender = tpl_event_get_sender (event);
              TplEntity *receiver = tpl_event_get_receiver (event);

              if (reason == TP_CALL_STATE_CHANGE_REASON_NO_ANSWER)
                {
                  if (ctx->subtype & EVENT_CALL_MISSED)
                    append = TRUE;
                }
              else if (ctx->subtype & EVENT_CALL_OUTGOING
                  && tpl_entity_get_entity_type (sender) == TPL_ENTITY_SELF)
                {
                  append = TRUE;
                }
              else if (ctx->subtype & EVENT_CALL_INCOMING
                  && tpl_entity_get_entity_type (receiver) == TPL_ENTITY_SELF)
                {
                  append = TRUE;
                }
            }
        }

      if (append)
        {
          EmpathyMessage *msg = empathy_message_from_tpl_log_event (event);
          log_window_append_message (event, msg);
          tp_clear_object (&msg);
        }

      g_object_unref (event);
    }
  g_list_free (events);

  model = GTK_TREE_MODEL (log_window->priv->store_events);
  n = gtk_tree_model_iter_n_children (model, NULL) - 1;

  if (n >= 0 && gtk_tree_model_iter_nth_child (model, &iter, NULL, n))
    {
      GtkTreePath *path;
      char *str, *script;

      path = gtk_tree_model_get_path (model, &iter);
      str = gtk_tree_path_to_string (path);

      script = g_strdup_printf ("javascript:scrollToRow([%s]);",
          g_strdelimit (str, ":", ','));

      webkit_web_view_execute_script (
          WEBKIT_WEB_VIEW (log_window->priv->webview),
          script);

      gtk_tree_path_free (path);
      g_free (str);
      g_free (script);
    }

 out:
  ctx_free (ctx);

  _tpl_action_chain_continue (log_window->priv->chain);
}

static void
get_events_for_date (TplActionChain *chain, gpointer user_data)
{
  Ctx *ctx = user_data;

  tpl_log_manager_get_events_for_date_async (ctx->self->priv->log_manager,
      ctx->account, ctx->entity, ctx->event_mask,
      ctx->date,
      log_window_got_messages_for_date_cb,
      ctx);
}

static void
log_window_get_messages_for_dates (EmpathyLogWindow *self,
    GList *dates)
{
  GList *accounts, *targets, *acc, *targ, *l;
  TplEventTypeMask event_mask;
  EventSubtype subtype;
  GDate *date, *anytime, *separator;

  if (!log_window_get_selected (self,
      &accounts, &targets, NULL, NULL, &event_mask, &subtype))
    return;

  anytime = g_date_new_dmy (2, 1, -1);
  separator = g_date_new_dmy (1, 1, -1);

  _tpl_action_chain_clear (self->priv->chain);
  self->priv->count++;

  for (acc = accounts, targ = targets;
       acc != NULL && targ != NULL;
       acc = acc->next, targ = targ->next)
    {
      TpAccount *account = acc->data;
      TplEntity *target = targ->data;

      for (l = dates; l != NULL; l = l->next)
        {
          date = l->data;

          /* Get events */
          if (g_date_compare (date, anytime) != 0)
            {
              Ctx *ctx;

              ctx = ctx_new (self, account, target, date, event_mask, subtype,
                  self->priv->count);
              _tpl_action_chain_append (self->priv->chain, get_events_for_date, ctx);
            }
          else
            {
              GtkTreeView *view = GTK_TREE_VIEW (self->priv->treeview_when);
              GtkTreeModel *model = gtk_tree_view_get_model (view);
              GtkTreeIter iter;
              gboolean next;
              GDate *d;

              for (next = gtk_tree_model_get_iter_first (model, &iter);
                   next;
                   next = gtk_tree_model_iter_next (model, &iter))
                {
                  Ctx *ctx;

                  gtk_tree_model_get (model, &iter,
                      COL_WHEN_DATE, &d,
                      -1);

                  if (g_date_compare (d, anytime) != 0 &&
                      g_date_compare (d, separator) != 0)
                    {
                      ctx = ctx_new (self, account, target, d,
                          event_mask, subtype, self->priv->count);
                      _tpl_action_chain_append (self->priv->chain, get_events_for_date, ctx);
                    }

                  g_date_free (d);
                }
            }
        }
    }

  start_spinner ();
  _tpl_action_chain_start (self->priv->chain);

  g_list_free_full (accounts, g_object_unref);
  g_list_free_full (targets, g_object_unref);
  g_date_free (separator);
  g_date_free (anytime);
}

static void
log_manager_got_dates_cb (GObject *manager,
    GAsyncResult *result,
    gpointer user_data)
{
  Ctx *ctx = user_data;
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkListStore *store;
  GtkTreeIter iter;
  GList *dates;
  GList *l;
  GError *error = NULL;

  if (log_window == NULL)
    {
      ctx_free (ctx);
      return;
    }

  if (log_window->priv->count != ctx->count)
    goto out;

  if (!tpl_log_manager_get_dates_finish (TPL_LOG_MANAGER (manager),
       result, &dates, &error))
    {
      DEBUG ("Unable to retrieve messages' dates: %s. Aborting",
          error->message);
      goto out;
    }

  view = GTK_TREE_VIEW (log_window->priv->treeview_when);
  model = gtk_tree_view_get_model (view);
  store = GTK_LIST_STORE (model);

  for (l = dates; l != NULL; l = l->next)
    {
      add_date_if_needed (log_window, l->data);
    }

  if (gtk_tree_model_get_iter_first (model, &iter))
    {
      gchar *separator = NULL;

      if (gtk_tree_model_iter_next (model, &iter))
        {
          gtk_tree_model_get (model, &iter,
              COL_WHEN_TEXT, &separator,
              -1);
        }

      if (g_strcmp0 (separator, "separator") != 0)
        {
          GDate *date;

          date = g_date_new_dmy (1, 1, -1);

          gtk_list_store_prepend (store, &iter);
          gtk_list_store_set (store, &iter,
              COL_WHEN_DATE, date,
              COL_WHEN_TEXT, "separator",
              -1);

          g_date_free (date);

          date = g_date_new_dmy (2, 1, -1);

          gtk_list_store_prepend (store, &iter);
          gtk_list_store_set (store, &iter,
              COL_WHEN_DATE, date,
              COL_WHEN_TEXT, _("Anytime"),
              -1);

          g_date_free (date);
        }

      g_free (separator);
    }

  g_list_free_full (dates, g_free);
 out:
  ctx_free (ctx);
  _tpl_action_chain_continue (log_window->priv->chain);
}

static void
select_date (TplActionChain *chain, gpointer user_data)
{
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkTreeSelection *selection;
  GtkTreeIter iter;
  gboolean next;
  gboolean selected = FALSE;

  view = GTK_TREE_VIEW (log_window->priv->treeview_when);
  model = gtk_tree_view_get_model (view);
  selection = gtk_tree_view_get_selection (view);

  if (log_window->priv->current_dates != NULL)
    {
      for (next = gtk_tree_model_get_iter_first (model, &iter);
           next;
           next = gtk_tree_model_iter_next (model, &iter))
        {
          GDate *date;

          gtk_tree_model_get (model, &iter,
              COL_WHEN_DATE, &date,
              -1);

          if (g_list_find_custom (log_window->priv->current_dates, date,
                  (GCompareFunc) g_date_compare) != NULL)
            {
              GtkTreePath *path;

              gtk_tree_selection_select_iter (selection, &iter);
              path = gtk_tree_model_get_path (model, &iter);
              gtk_tree_view_scroll_to_cell (view, path, NULL, FALSE, 0, 0);
              selected = TRUE;

              gtk_tree_path_free (path);
            }

          g_date_free (date);
        }
    }

  if (!selected)
    {
      /* Show messages of the most recent date */
      if (gtk_tree_model_iter_nth_child (model, &iter, NULL, 2))
        gtk_tree_selection_select_iter (selection, &iter);
    }

  _tpl_action_chain_continue (log_window->priv->chain);
}

static void
get_dates_for_entity (TplActionChain *chain, gpointer user_data)
{
  Ctx *ctx = user_data;

  tpl_log_manager_get_dates_async (ctx->self->priv->log_manager,
      ctx->account, ctx->entity, ctx->event_mask,
      log_manager_got_dates_cb, ctx);
}

static void
log_window_chats_get_messages (EmpathyLogWindow *self,
    gboolean force_get_dates)
{
  GList *accounts, *targets, *dates;
  TplEventTypeMask event_mask;
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkListStore *store;
  GtkTreeSelection *selection;

  if (!log_window_get_selected (self, &accounts, &targets, NULL,
      &dates, &event_mask, NULL))
    return;

  view = GTK_TREE_VIEW (self->priv->treeview_when);
  selection = gtk_tree_view_get_selection (view);
  model = gtk_tree_view_get_model (view);
  store = GTK_LIST_STORE (model);

  /* Clear all current messages shown in the textview */
  gtk_tree_store_clear (self->priv->store_events);

  _tpl_action_chain_clear (self->priv->chain);
  self->priv->count++;

  /* If there's a search use the returned hits */
  if (self->priv->hits != NULL)
    {
      if (force_get_dates)
        {
          g_signal_handlers_block_by_func (selection,
              log_window_when_changed_cb,
              self);

          gtk_list_store_clear (store);

          g_signal_handlers_unblock_by_func (selection,
              log_window_when_changed_cb,
              self);

          populate_dates_from_search_hits (accounts, targets);
        }
      else
        {
          populate_events_from_search_hits (accounts, targets, dates);
        }
    }
  /* Either use the supplied date or get the last */
  else if (force_get_dates || dates == NULL)
    {
      GList *acc, *targ;

      if (self->priv->current_dates != NULL)
        {
          g_list_free_full (self->priv->current_dates,
              (GDestroyNotify) g_date_free);
          self->priv->current_dates = NULL;
        }

      if (gtk_tree_selection_count_selected_rows (selection) > 0)
        {
          GList *paths, *l;
          GtkTreeIter iter;

          paths = gtk_tree_selection_get_selected_rows (selection, NULL);

          for (l = paths; l != NULL; l = l->next)
            {
              GtkTreePath *path = l->data;
              GDate *date;

              gtk_tree_model_get_iter (model, &iter, path);
              gtk_tree_model_get (model, &iter,
                  COL_WHEN_DATE, &date,
                  -1);

              /* The list takes ownership of the date. */
              self->priv->current_dates =
                  g_list_prepend (self->priv->current_dates, date);
            }

          g_list_free_full (paths, (GDestroyNotify) gtk_tree_path_free);
        }

      g_signal_handlers_block_by_func (selection,
          log_window_when_changed_cb,
          self);

      gtk_list_store_clear (store);

      g_signal_handlers_unblock_by_func (selection,
          log_window_when_changed_cb,
          self);

      /* Get a list of dates and show them on the treeview */
      for (targ = targets, acc = accounts;
           targ != NULL && acc != NULL;
           targ = targ->next, acc = acc->next)
        {
          TpAccount *account = acc->data;
          TplEntity *target = targ->data;
          Ctx *ctx = ctx_new (self, account, target, NULL, event_mask, 0,
              self->priv->count);

          _tpl_action_chain_append (self->priv->chain, get_dates_for_entity, ctx);
        }
      _tpl_action_chain_append (self->priv->chain, select_date, NULL);
      _tpl_action_chain_start (self->priv->chain);
    }
  else
    {
      /* Show messages of the selected date */
      log_window_get_messages_for_dates (self, dates);
    }

  g_list_free_full (accounts, g_object_unref);
  g_list_free_full (targets, g_object_unref);
  g_list_free_full (dates, (GFreeFunc) g_date_free);
}

typedef struct {
  EmpathyAccountChooserFilterResultCallback callback;
  gpointer user_data;
} FilterCallbackData;

static void
got_entities (GObject *manager,
    GAsyncResult *result,
    gpointer user_data)
{
  FilterCallbackData *data = user_data;
  GList *entities;
  GError *error = NULL;

  if (!tpl_log_manager_get_entities_finish (TPL_LOG_MANAGER (manager),
      result, &entities, &error))
    {
      DEBUG ("Could not get entities: %s", error->message);
      g_error_free (error);
      data->callback (FALSE, data->user_data);
    }
  else
    {
      data->callback (entities != NULL, data->user_data);

      g_list_free_full (entities, g_object_unref);
    }

  g_slice_free (FilterCallbackData, data);
}

static void
empathy_account_chooser_filter_has_logs (TpAccount *account,
    EmpathyAccountChooserFilterResultCallback callback,
    gpointer callback_data,
    gpointer user_data)
{
  TplLogManager *manager = tpl_log_manager_dup_singleton ();
  FilterCallbackData *cb_data = g_slice_new0 (FilterCallbackData);

  cb_data->callback = callback;
  cb_data->user_data = callback_data;

  tpl_log_manager_get_entities_async (manager, account, got_entities, cb_data);

  g_object_unref (manager);
}

static void
log_window_logger_clear_account_cb (TpProxy *proxy,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  EmpathyLogWindow *self = EMPATHY_LOG_WINDOW (user_data);

  if (error != NULL)
    g_warning ("Error when clearing logs: %s", error->message);

  /* Refresh the log viewer so the logs are cleared if the account
   * has been deleted */
  gtk_tree_store_clear (self->priv->store_events);
  log_window_who_populate (self);

  /* Re-filter the account chooser so the accounts without logs get
   * greyed out */
  empathy_account_chooser_refilter (
      EMPATHY_ACCOUNT_CHOOSER (self->priv->account_chooser));
}

static void
log_window_delete_menu_clicked_cb (GtkMenuItem *menuitem,
    EmpathyLogWindow *self)
{
  GtkWidget *dialog, *content_area, *hbox, *label;
  EmpathyAccountChooser *account_chooser;
  gint response_id;
  TpDBusDaemon *bus;
  TpProxy *logger;
  GError *error = NULL;

  account_chooser = (EmpathyAccountChooser *) empathy_account_chooser_new ();
  empathy_account_chooser_set_has_all_option (account_chooser, TRUE);

  empathy_account_chooser_refilter (account_chooser);

  /* Select the same account as in the history window */
  empathy_account_chooser_set_account (account_chooser,
      empathy_account_chooser_get_account (
        EMPATHY_ACCOUNT_CHOOSER (self->priv->account_chooser)));

  dialog = gtk_message_dialog_new_with_markup (GTK_WINDOW (self),
      GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
      GTK_BUTTONS_NONE,
      _("Are you sure you want to delete all logs of previous conversations?"));

  gtk_dialog_add_buttons (GTK_DIALOG (dialog),
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      _("Clear All"), GTK_RESPONSE_APPLY,
      NULL);

  content_area = gtk_message_dialog_get_message_area (
      GTK_MESSAGE_DIALOG (dialog));

  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  label = gtk_label_new (_("Delete from:"));
  gtk_box_pack_start (GTK_BOX (hbox), label,
      FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), GTK_WIDGET (account_chooser),
      FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content_area), hbox,
      FALSE, FALSE, 0);

  gtk_widget_show_all (hbox);

  response_id = gtk_dialog_run (GTK_DIALOG (dialog));

  if (response_id != GTK_RESPONSE_APPLY)
    goto out;

  bus = tp_dbus_daemon_dup (&error);
  if (error != NULL)
    {
      g_warning ("Could not delete logs: %s", error->message);
      g_error_free (error);
      goto out;
    }

  logger = g_object_new (TP_TYPE_PROXY,
      "bus-name", "org.freedesktop.Telepathy.Logger",
      "object-path", "/org/freedesktop/Telepathy/Logger",
      "dbus-daemon", bus,
      NULL);
  g_object_unref (bus);

  tp_proxy_add_interface_by_id (logger, EMP_IFACE_QUARK_LOGGER);

  if (empathy_account_chooser_has_all_selected (account_chooser))
    {
      DEBUG ("Deleting logs for all the accounts");

      emp_cli_logger_call_clear (logger, -1,
          log_window_logger_clear_account_cb,
          self, NULL, G_OBJECT (self));
    }
  else
    {
      TpAccount *account;

      account = empathy_account_chooser_get_account (account_chooser);

      DEBUG ("Deleting logs for %s", tp_proxy_get_object_path (account));

      emp_cli_logger_call_clear_account (logger, -1,
          tp_proxy_get_object_path (account),
          log_window_logger_clear_account_cb,
          self, NULL, G_OBJECT (self));
    }

  g_object_unref (logger);
 out:
  gtk_widget_destroy (dialog);
}
