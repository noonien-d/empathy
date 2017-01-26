/*
 * Copyright (C) 2008-2009 Collabora Ltd.
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

#include "config.h"
#include "empathy-webkit-utils.h"

#include <glib/gi18n-lib.h>

#include "empathy-smiley-manager.h"
#include "empathy-string-parser.h"
#include "empathy-theme-adium.h"
#include "empathy-ui-utils.h"

#define BORING_DPI_DEFAULT 96

static void
empathy_webkit_match_newline (const gchar *text,
    gssize len,
    TpawStringReplace replace_func,
    TpawStringParser *sub_parsers,
    gpointer user_data)
{
  GString *string = user_data;
  gint i;
  gint prev = 0;

  if (len < 0)
    len = G_MAXSSIZE;

  /* Replace \n by <br/> */
  for (i = 0; i < len && text[i] != '\0'; i++)
    {
      if (text[i] == '\n')
        {
          tpaw_string_parser_substr (text + prev, i - prev,
              sub_parsers, user_data);
          g_string_append (string, "<br/>");
          prev = i + 1;
        }
    }

  tpaw_string_parser_substr (text + prev, i - prev,
              sub_parsers, user_data);
}

static void
empathy_webkit_replace_smiley (const gchar *text,
    gssize len,
    gpointer match_data,
    gpointer user_data)
{
  EmpathySmileyHit *hit = match_data;
  GString *string = user_data;

  /* Replace smiley by a <img/> tag */
  g_string_append_printf (string,
      "<img src=\"%s\" alt=\"%.*s\" title=\"%.*s\"/>",
      hit->path, (int)len, text, (int)len, text);
}

static TpawStringParser string_parsers[] = {
  { tpaw_string_match_link, tpaw_string_replace_link },
  { empathy_webkit_match_newline, NULL },
  { tpaw_string_match_all, tpaw_string_replace_escaped },
  { NULL, NULL}
};

static TpawStringParser string_parsers_with_smiley[] = {
  { tpaw_string_match_link, tpaw_string_replace_link },
  { empathy_string_match_smiley, empathy_webkit_replace_smiley },
  { empathy_webkit_match_newline, NULL },
  { tpaw_string_match_all, tpaw_string_replace_escaped },
  { NULL, NULL }
};

TpawStringParser *
empathy_webkit_get_string_parser (gboolean smileys)
{
  if (smileys)
    return string_parsers_with_smiley;
  else
    return string_parsers;
}

static gboolean
webkit_get_font_family (GValue *value,
    GVariant *variant,
    gpointer user_data)
{
  PangoFontDescription *font = pango_font_description_from_string (
      g_variant_get_string (variant, NULL));

  if (font == NULL)
    return FALSE;

  g_value_set_string (value, pango_font_description_get_family (font));
  pango_font_description_free (font);

  return TRUE;
}

static gboolean
webkit_get_font_size (GValue *value,
    GVariant *variant,
    gpointer user_data)
{
  PangoFontDescription *font = pango_font_description_from_string (
      g_variant_get_string (variant, NULL));
  GdkScreen *screen = gdk_screen_get_default ();
  double dpi;
  int size;

  if (font == NULL)
    return FALSE;

  size = pango_font_description_get_size (font);
  if (!pango_font_description_get_size_is_absolute (font))
    size /= PANGO_SCALE;

  if (screen != NULL)
    dpi = gdk_screen_get_resolution (screen);
  else
    dpi = BORING_DPI_DEFAULT;

  g_value_set_uint (value, size / 72. * dpi);
  pango_font_description_free (font);

  return TRUE;
}

void
empathy_webkit_bind_font_setting (WebKitWebView *webview,
    GSettings *gsettings,
    const char *key)
{
  WebKitSettings *settings = webkit_web_view_get_settings (webview);

  g_settings_bind_with_mapping (gsettings, key,
      settings, "default-font-family",
      G_SETTINGS_BIND_GET,
      webkit_get_font_family,
      NULL,
      NULL, NULL);

  g_settings_bind_with_mapping (gsettings, key,
      settings, "default-font-size",
      G_SETTINGS_BIND_GET,
      webkit_get_font_size,
      NULL,
      NULL, NULL);
}

static void
can_copy_callback (WebKitWebView *web_view,
    GAsyncResult *result,
    WebKitContextMenuItem *item)
{
  gboolean can_copy;

  can_copy = webkit_web_view_can_execute_editing_command_finish (web_view, result, NULL);
  gtk_action_set_visible (webkit_context_menu_item_get_action (item), can_copy);
  g_object_unref (item);
}

void
empathy_webkit_populate_context_menu (WebKitWebView *web_view,
    WebKitContextMenu *context_menu,
    WebKitHitTestResult *hit_test_result,
    EmpathyWebKitMenuFlags flags)
{
  WebKitContextMenuItem *item;

  webkit_context_menu_remove_all (context_menu);

  /* Select all item */
  webkit_context_menu_append (context_menu,
      webkit_context_menu_item_new_from_stock_action (WEBKIT_CONTEXT_MENU_ACTION_SELECT_ALL));

  /* Copy menu item */
  item = webkit_context_menu_item_new_from_stock_action (WEBKIT_CONTEXT_MENU_ACTION_COPY);
  webkit_context_menu_append (context_menu, item);
  webkit_web_view_can_execute_editing_command (web_view,
      WEBKIT_EDITING_COMMAND_COPY, NULL,
      (GAsyncReadyCallback)can_copy_callback,
      g_object_ref (item));

  /* Clear menu item */
  if (flags & EMPATHY_WEBKIT_MENU_CLEAR)
    {
      GtkAction *action;

      webkit_context_menu_append (context_menu, webkit_context_menu_item_new_separator ());
      action = gtk_action_new ("clear", NULL, NULL, GTK_STOCK_CLEAR);
      g_signal_connect_swapped (action, "activate",
          G_CALLBACK (empathy_theme_adium_clear),
          web_view);
      webkit_context_menu_append (context_menu, webkit_context_menu_item_new (action));
      g_object_unref (action);
    }

  /* We will only add the following menu items if we are
   * right-clicking a link */
  if (webkit_hit_test_result_context_is_link (hit_test_result))
    {
      /* Separator */
      webkit_context_menu_append (context_menu, webkit_context_menu_item_new_separator ());

      /* Copy Link Address menu item */
      webkit_context_menu_append (context_menu,
          webkit_context_menu_item_new_from_stock_action (WEBKIT_CONTEXT_MENU_ACTION_COPY_LINK_TO_CLIPBOARD));

      /* Open Link menu item */
      webkit_context_menu_append (context_menu,
          webkit_context_menu_item_new_from_stock_action (WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK));
     }

  if ((flags & EMPATHY_WEBKIT_MENU_INSPECT) != 0)
    {
      /* Separator */
      webkit_context_menu_append (context_menu, webkit_context_menu_item_new_separator ());

      /* Inspector */
      webkit_context_menu_append (context_menu,
          webkit_context_menu_item_new_from_stock_action (WEBKIT_CONTEXT_MENU_ACTION_INSPECT_ELEMENT));
    }
}

gboolean
empathy_webkit_handle_navigation (WebKitWebView *web_view,
    WebKitNavigationPolicyDecision *decision)
{
  WebKitNavigationAction *action;
  const char *requested_uri;

  action = webkit_navigation_policy_decision_get_navigation_action (decision);
  requested_uri = webkit_uri_request_get_uri (webkit_navigation_action_get_request (action));
  if (g_strcmp0 (webkit_web_view_get_uri (web_view), requested_uri) == 0)
    return FALSE;

  empathy_url_show (GTK_WIDGET (web_view), requested_uri);
  webkit_policy_decision_ignore (WEBKIT_POLICY_DECISION (decision));

  return TRUE;
}

WebKitWebContext *
empathy_webkit_get_web_context (void)
{
  static WebKitWebContext *web_context = NULL;

  if (!web_context)
    {
      web_context = webkit_web_context_get_default ();
      webkit_web_context_set_cache_model (web_context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
      webkit_web_context_set_process_model (web_context, WEBKIT_PROCESS_MODEL_SHARED_SECONDARY_PROCESS);
    }

  return web_context;
}

WebKitSettings *
empathy_webkit_get_web_settings (void)
{
  static WebKitSettings *settings = NULL;

  if (!settings)
    {
      settings = webkit_settings_new_with_settings (
          "enable-page-cache", FALSE,
          "enable-plugins", FALSE,
          "enable-developer-extras", TRUE,
          NULL);
    }

  return settings;
}
