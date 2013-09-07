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
  int size;

  if (font == NULL)
    return FALSE;

  size = pango_font_description_get_size (font) / PANGO_SCALE;

  if (pango_font_description_get_size_is_absolute (font))
    {
      GdkScreen *screen = gdk_screen_get_default ();
      double dpi;

      if (screen != NULL)
        dpi = gdk_screen_get_resolution (screen);
      else
        dpi = BORING_DPI_DEFAULT;

      size = (gint) (size / (dpi / 72));
    }

  g_value_set_int (value, size);
  pango_font_description_free (font);

  return TRUE;
}

void
empathy_webkit_bind_font_setting (WebKitWebView *webview,
    GSettings *gsettings,
    const char *key)
{
  WebKitWebSettings *settings = webkit_web_view_get_settings (webview);

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
empathy_webkit_copy_address_cb (GtkMenuItem *menuitem,
    gpointer user_data)
{
  WebKitHitTestResult *hit_test_result = WEBKIT_HIT_TEST_RESULT (user_data);
  gchar *uri;
  GtkClipboard *clipboard;

  g_object_get (G_OBJECT (hit_test_result),
      "link-uri", &uri,
      NULL);

  clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text (clipboard, uri, -1);

  clipboard = gtk_clipboard_get (GDK_SELECTION_PRIMARY);
  gtk_clipboard_set_text (clipboard, uri, -1);

  g_free (uri);
}

static void
empathy_webkit_open_address_cb (GtkMenuItem *menuitem,
    gpointer     user_data)
{
  WebKitHitTestResult *hit_test_result = WEBKIT_HIT_TEST_RESULT (user_data);
  gchar *uri;

  g_object_get (G_OBJECT (hit_test_result),
      "link-uri", &uri,
      NULL);

  empathy_url_show (GTK_WIDGET (menuitem), uri);

  g_free (uri);
}

static void
empathy_webkit_inspect_cb (GtkMenuItem *menuitem,
    WebKitWebView *view)
{
  empathy_webkit_show_inspector (view);
}

static void
empathy_webkit_context_menu_selection_done_cb (GtkMenuShell *menu,
    gpointer user_data)
{
  WebKitHitTestResult *hit_test_result = WEBKIT_HIT_TEST_RESULT (user_data);

  g_object_unref (hit_test_result);
}

GtkWidget *
empathy_webkit_create_context_menu (WebKitWebView *view,
    WebKitHitTestResult *hit_test_result,
    EmpathyWebKitMenuFlags flags)
{
  WebKitHitTestResultContext context;
  GtkWidget *menu;
  GtkWidget *item;

  g_object_get (G_OBJECT (hit_test_result),
      "context", &context,
      NULL);

  /* The menu */
  menu = empathy_context_menu_new (GTK_WIDGET (view));

  /* Select all item */
  item = gtk_image_menu_item_new_from_stock (GTK_STOCK_SELECT_ALL, NULL);
  gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);

  g_signal_connect_swapped (item, "activate",
      G_CALLBACK (webkit_web_view_select_all),
      view);

  /* Copy menu item */
  if (webkit_web_view_can_copy_clipboard (view))
    {
      item = gtk_image_menu_item_new_from_stock (GTK_STOCK_COPY, NULL);
      gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);

      g_signal_connect_swapped (item, "activate",
          G_CALLBACK (webkit_web_view_copy_clipboard),
          view);
    }

  /* Clear menu item */
  if (flags & EMPATHY_WEBKIT_MENU_CLEAR)
    {
      item = gtk_separator_menu_item_new ();
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

      item = gtk_image_menu_item_new_from_stock (GTK_STOCK_CLEAR, NULL);
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

      g_signal_connect_swapped (item, "activate",
          G_CALLBACK (empathy_theme_adium_clear),
          view);
    }

  /* We will only add the following menu items if we are
   * right-clicking a link */
  if (context & WEBKIT_HIT_TEST_RESULT_CONTEXT_LINK)
    {
      /* Separator */
      item = gtk_separator_menu_item_new ();
      gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);

      /* Copy Link Address menu item */
      item = gtk_menu_item_new_with_mnemonic (_("_Copy Link Address"));
      g_signal_connect (item, "activate",
          G_CALLBACK (empathy_webkit_copy_address_cb),
          hit_test_result);
      gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);

      /* Open Link menu item */
      item = gtk_menu_item_new_with_mnemonic (_("_Open Link"));
      g_signal_connect (item, "activate",
          G_CALLBACK (empathy_webkit_open_address_cb),
          hit_test_result);
      gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);
    }

  if ((flags & EMPATHY_WEBKIT_MENU_INSPECT) != 0)
    {
      /* Separator */
      item = gtk_separator_menu_item_new ();
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

      /* Inspector */
      item = gtk_menu_item_new_with_mnemonic (_("Inspect HTML"));
      g_signal_connect (item, "activate",
          G_CALLBACK (empathy_webkit_inspect_cb), view);
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
    }

  g_signal_connect (GTK_MENU_SHELL (menu), "selection-done",
      G_CALLBACK (empathy_webkit_context_menu_selection_done_cb),
      g_object_ref (hit_test_result));

  return menu;
}

void
empathy_webkit_context_menu_for_event (WebKitWebView *view,
    GdkEventButton *event,
    EmpathyWebKitMenuFlags flags)
{
  GtkWidget *menu;
  WebKitHitTestResult *hit_test_result;

  hit_test_result = webkit_web_view_get_hit_test_result (view, event);

  menu = empathy_webkit_create_context_menu (view, hit_test_result, flags);

  gtk_widget_show_all (menu);
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL,
      event->button, event->time);

  g_object_unref (hit_test_result);
}

void
empathy_webkit_show_inspector (WebKitWebView *view)
{
  WebKitWebInspector *inspector;

  g_object_set (G_OBJECT (webkit_web_view_get_settings (view)),
      "enable-developer-extras", TRUE, NULL);

  inspector = webkit_web_view_get_inspector (view);
  webkit_web_inspector_show (inspector);
}
