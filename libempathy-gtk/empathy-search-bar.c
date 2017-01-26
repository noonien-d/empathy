/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * Copyright (C) 2010 Thomas Meire <blackskad@gmail.com>
 *
 * The code contained in this file is free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either version
 * 2.1 of the License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this code; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "empathy-search-bar.h"

#include <glib/gi18n-lib.h>
#include <tp-account-widgets/tpaw-builder.h>
#include <tp-account-widgets/tpaw-utils.h>

#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathySearchBar)

G_DEFINE_TYPE (EmpathySearchBar, empathy_search_bar, GTK_TYPE_BOX);

typedef struct _EmpathySearchBarPriv EmpathySearchBarPriv;
struct _EmpathySearchBarPriv
{
  EmpathyThemeAdium *chat_view;

  GtkWidget *search_entry;

  GtkWidget *search_match_case;

  GtkWidget *search_match_case_toolitem;

  GtkWidget *search_close;
  GtkWidget *search_previous;
  GtkWidget *search_next;
  GtkWidget *search_not_found;
};

GtkWidget *
empathy_search_bar_new (EmpathyThemeAdium *view)
{
  EmpathySearchBar *self = g_object_new (EMPATHY_TYPE_SEARCH_BAR, NULL);

  GET_PRIV (self)->chat_view = view;

  return GTK_WIDGET (self);
}

static void
empathy_search_bar_update_buttons (EmpathySearchBar *self,
    gchar *search,
    gboolean match_case)
{
  gboolean can_go_forward = FALSE;
  gboolean can_go_backward = FALSE;

  EmpathySearchBarPriv* priv = GET_PRIV (self);

  /* update previous / next buttons */
  empathy_theme_adium_find_abilities (priv->chat_view, search, match_case,
      &can_go_backward, &can_go_forward);

  gtk_widget_set_sensitive (priv->search_previous,
      can_go_backward && !TPAW_STR_EMPTY (search));
  gtk_widget_set_sensitive (priv->search_next,
      can_go_forward && !TPAW_STR_EMPTY (search));
}

static void
empathy_search_bar_search (EmpathySearchBar *self,
    gboolean next,
    gboolean new_search)
{
  gchar *search;
  gboolean found;
  gboolean match_case;
  EmpathySearchBarPriv *priv;

  priv = GET_PRIV (self);

  search = gtk_editable_get_chars (GTK_EDITABLE(priv->search_entry), 0, -1);
  match_case = gtk_toggle_button_get_active (
      GTK_TOGGLE_BUTTON (priv->search_match_case));

  /* highlight & search */
  if (new_search)
    empathy_theme_adium_search (priv->chat_view, search, match_case);
  else if (next)
    empathy_theme_adium_find_next (priv->chat_view);
  else
    empathy_theme_adium_find_previous (priv->chat_view);

  /* FIXME: We should connect to failed-to-find-text of WebKitFindController, but this
   * doesn't seem to work in any case.
   */
  found = TRUE;
  /* (don't) display the not found label */
  gtk_widget_set_visible (priv->search_not_found,
      !(found || TPAW_STR_EMPTY (search)));

  /* update the buttons */
  empathy_search_bar_update_buttons (self, search, match_case);

  g_free (search);
}

void
empathy_search_bar_show (EmpathySearchBar *self)
{
  EmpathySearchBarPriv *priv = GET_PRIV (self);

  /* update the highlighting and buttons */
  empathy_search_bar_search (self, FALSE, TRUE);

  /* grab the focus to the search entry */
  gtk_widget_grab_focus (priv->search_entry);

  gtk_widget_show (GTK_WIDGET (self));
}

void
empathy_search_bar_hide (EmpathySearchBar *self)
{
  EmpathySearchBarPriv *priv = GET_PRIV (self);

  empathy_theme_adium_search (priv->chat_view, "", FALSE);
  gtk_widget_hide (GTK_WIDGET (self));

  /* give the focus back to the focus-chain with the chat view */
  gtk_widget_grab_focus (GTK_WIDGET (priv->chat_view));
}

static void
empathy_search_bar_close_cb (GtkButton *button,
    gpointer user_data)
{
  empathy_search_bar_hide (EMPATHY_SEARCH_BAR (user_data));
}

static void
empathy_search_bar_entry_changed (GtkEditable *entry,
    gpointer user_data)
{
  empathy_search_bar_search (EMPATHY_SEARCH_BAR (user_data), FALSE, TRUE);
}

static gboolean
empathy_search_bar_key_pressed (GtkWidget   *widget,
    GdkEventKey *event,
    gpointer user_data)
{
  if (event->keyval == GDK_KEY_Escape)
    {
      empathy_search_bar_hide (EMPATHY_SEARCH_BAR (widget));
      return TRUE;
    }
  return FALSE;
}

static void
empathy_search_bar_next_cb (GtkButton *button,
    gpointer user_data)
{
  empathy_search_bar_search (EMPATHY_SEARCH_BAR (user_data), TRUE, FALSE);
}

static void
empathy_search_bar_previous_cb (GtkButton *button,
    gpointer user_data)
{
  empathy_search_bar_search (EMPATHY_SEARCH_BAR (user_data), FALSE, FALSE);
}

static void
empathy_search_bar_match_case_toggled (GtkButton *button,
    gpointer user_data)
{
  empathy_search_bar_search (EMPATHY_SEARCH_BAR (user_data), FALSE, TRUE);
}

static void
empathy_search_bar_match_case_menu_toggled (GtkWidget *check,
    gpointer user_data)
{
  EmpathySearchBarPriv* priv = GET_PRIV ( EMPATHY_SEARCH_BAR (user_data));
  gboolean match_case;

  match_case = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (check));

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->search_match_case),
      match_case);
}

static gboolean
empathy_searchbar_create_menu_proxy_cb (GtkToolItem *toolitem,
    gpointer user_data)
{
  EmpathySearchBarPriv* priv = GET_PRIV ( EMPATHY_SEARCH_BAR (user_data));
  GtkWidget *checkbox_menu;
  gboolean match_case;

  checkbox_menu = gtk_check_menu_item_new_with_mnemonic (_("_Match case"));
  match_case = gtk_toggle_button_get_active (
      GTK_TOGGLE_BUTTON (priv->search_match_case));
  gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (checkbox_menu),
      match_case);

  g_signal_connect (checkbox_menu, "toggled",
      G_CALLBACK (empathy_search_bar_match_case_menu_toggled), user_data);

  gtk_tool_item_set_proxy_menu_item (toolitem, "menu-proxy",
      checkbox_menu);

  return TRUE;
}

static void
empathy_search_bar_init (EmpathySearchBar * self)
{
  gchar *filename;
  GtkBuilder *gui;
  GtkWidget *internal;
  EmpathySearchBarPriv *priv;

  priv = G_TYPE_INSTANCE_GET_PRIVATE (self, EMPATHY_TYPE_SEARCH_BAR,
      EmpathySearchBarPriv);

  self->priv = priv;

  filename = empathy_file_lookup ("empathy-search-bar.ui", "libempathy-gtk");
  gui = tpaw_builder_get_file (filename,
      "search_widget", &internal,
      "search_close", &priv->search_close,
      "search_entry", &priv->search_entry,
      "search_previous", &priv->search_previous,
      "search_next", &priv->search_next,
      "search_not_found", &priv->search_not_found,
      "search_match_case", &priv->search_match_case,
      NULL);
  g_free (filename);

  /* Add the signals */
  tpaw_builder_connect (gui, self,
      "search_close", "clicked", empathy_search_bar_close_cb,
      "search_entry", "changed", empathy_search_bar_entry_changed,
      "search_previous", "clicked", empathy_search_bar_previous_cb,
      "search_next", "clicked", empathy_search_bar_next_cb,
      "search_match_case", "toggled", empathy_search_bar_match_case_toggled,
      "search_match_case_toolitem", "create-menu-proxy", empathy_searchbar_create_menu_proxy_cb,
      NULL);

  g_signal_connect (G_OBJECT (self), "key-press-event",
      G_CALLBACK (empathy_search_bar_key_pressed), NULL);

  gtk_box_pack_start (GTK_BOX (self), internal, TRUE, TRUE, 0);
  gtk_widget_show_all (internal);
  gtk_widget_hide (priv->search_not_found);
  g_object_unref (gui);
}

static void
empathy_search_bar_class_init (EmpathySearchBarClass *class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (class);

  g_type_class_add_private (gobject_class, sizeof (EmpathySearchBarPriv));
}

void
empathy_search_bar_paste_clipboard (EmpathySearchBar *self)
{
  EmpathySearchBarPriv *priv = GET_PRIV (self);

  gtk_editable_paste_clipboard (GTK_EDITABLE (priv->search_entry));
}
