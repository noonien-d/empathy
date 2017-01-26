/*
 * Copyright (C) 2008-2012 Collabora Ltd.
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
 *
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 */

#ifndef __EMPATHY_THEME_ADIUM_H__
#define __EMPATHY_THEME_ADIUM_H__

#include <webkit2/webkit2.h>

#include "empathy-message.h"

G_BEGIN_DECLS

/* TYPE MACROS */
#define EMPATHY_TYPE_THEME_ADIUM \
  (empathy_theme_adium_get_type ())
#define EMPATHY_THEME_ADIUM(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), \
    EMPATHY_TYPE_THEME_ADIUM, \
    EmpathyThemeAdium))
#define EMPATHY_THEME_ADIUM_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), \
    EMPATHY_TYPE_THEME_ADIUM, \
    EmpathyThemeAdiumClass))
#define EMPATHY_IS_THEME_ADIUM(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), \
    EMPATHY_TYPE_THEME_ADIUM))
#define EMPATHY_IS_THEME_ADIUM_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), \
    EMPATHY_TYPE_THEME_ADIUM))
#define EMPATHY_THEME_ADIUM_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
    EMPATHY_TYPE_THEME_ADIUM, \
    EmpathyThemeAdiumClass))



typedef struct _EmpathyThemeAdium EmpathyThemeAdium;
typedef struct _EmpathyThemeAdiumClass EmpathyThemeAdiumClass;
typedef struct _EmpathyThemeAdiumPriv EmpathyThemeAdiumPriv;

typedef struct _EmpathyAdiumData EmpathyAdiumData;

struct _EmpathyThemeAdium
{
  WebKitWebView parent;
  EmpathyThemeAdiumPriv *priv;
};

struct _EmpathyThemeAdiumClass
{
  WebKitWebViewClass parent_class;
};

GType empathy_theme_adium_get_type (void) G_GNUC_CONST;

EmpathyThemeAdium *empathy_theme_adium_new (EmpathyAdiumData *data,
    const gchar *variant);
void empathy_theme_adium_set_variant (EmpathyThemeAdium *theme,
                const gchar *variant);
void empathy_theme_adium_show_inspector (EmpathyThemeAdium *theme);

void empathy_theme_adium_append_message (EmpathyThemeAdium *self,
    EmpathyMessage *msg,
    gboolean should_highlight);

void empathy_theme_adium_append_event (EmpathyThemeAdium *self,
    const gchar *str);

void empathy_theme_adium_append_event_markup (EmpathyThemeAdium *self,
    const gchar *markup_text,
    const gchar *fallback_text);

void empathy_theme_adium_prepend_message (EmpathyThemeAdium *self,
    EmpathyMessage *msg,
    gboolean should_highlight);

void empathy_theme_adium_edit_message (EmpathyThemeAdium *self,
    EmpathyMessage *message);

void empathy_theme_adium_scroll (EmpathyThemeAdium *self,
    gboolean allow_scrolling);

void empathy_theme_adium_scroll_down (EmpathyThemeAdium *self);

void empathy_theme_adium_can_copy (EmpathyThemeAdium *self,
    GCancellable* cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean empathy_theme_adium_can_copy_finish (EmpathyThemeAdium *self,
    GAsyncResult* result,
    GError** error);

void empathy_theme_adium_clear (EmpathyThemeAdium *self);

void empathy_theme_adium_find_previous (EmpathyThemeAdium *self);

void empathy_theme_adium_find_next (EmpathyThemeAdium *self);

void empathy_theme_adium_find_abilities (EmpathyThemeAdium *self,
    const gchar *search_criteria,
    gboolean match_case,
    gboolean *can_do_previous,
    gboolean *can_do_next);

void empathy_theme_adium_search (EmpathyThemeAdium *self,
    const gchar *text,
    gboolean match_case);

void empathy_theme_adium_copy_clipboard (EmpathyThemeAdium *self);

void empathy_theme_adium_focus_toggled (EmpathyThemeAdium *self,
    gboolean has_focus);

void empathy_theme_adium_message_acknowledged (EmpathyThemeAdium *self,
    EmpathyMessage *message);

void empathy_theme_adium_set_show_avatars (EmpathyThemeAdium *self,
    gboolean show_avatars);

/* not methods functions */

gboolean empathy_adium_path_is_valid (const gchar *path);

GHashTable *empathy_adium_info_new (const gchar *path);
const gchar * empathy_adium_info_get_default_variant (GHashTable *info);
GPtrArray * empathy_adium_info_get_available_variants (GHashTable *info);

#define EMPATHY_TYPE_ADIUM_DATA (empathy_adium_data_get_type ())
GType empathy_adium_data_get_type (void) G_GNUC_CONST;
EmpathyAdiumData *empathy_adium_data_new (const gchar *path);
EmpathyAdiumData *empathy_adium_data_new_with_info (const gchar *path,
    GHashTable *info);
EmpathyAdiumData *empathy_adium_data_ref (EmpathyAdiumData *data);
void empathy_adium_data_unref (EmpathyAdiumData *data);
GHashTable *empathy_adium_data_get_info (EmpathyAdiumData *data);
const gchar *empathy_adium_data_get_path (EmpathyAdiumData *data);

G_END_DECLS

#endif /* __EMPATHY_THEME_ADIUM_H__ */
