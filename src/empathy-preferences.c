/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2003-2007 Imendio AB
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
 * Authors: Mikael Hallendal <micke@imendio.com>
 *          Richard Hult <richard@imendio.com>
 *          Martyn Russell <martyn@imendio.com>
 *          Danielle Madeley <danielle.madeley@collabora.co.uk>
 */

#include "config.h"
#include "empathy-preferences.h"

#include <glib/gi18n.h>
#include <tp-account-widgets/tpaw-builder.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-client-factory.h"
#include "empathy-gsettings.h"
#include "empathy-spell.h"
#include "empathy-theme-manager.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

G_DEFINE_TYPE (EmpathyPreferences, empathy_preferences, GTK_TYPE_DIALOG);

#define GET_PRIV(self) ((EmpathyPreferencesPriv *)((EmpathyPreferences *) self)->priv)

static const gchar * empathy_preferences_tabs[] =
{
  "general",
  "notifications",
  "sounds",
  "calls",
  "location",
  "spell",
  "themes",
};

struct _EmpathyPreferencesPriv {
	GtkWidget *notebook;

	GtkWidget *label_general_behavior;
	GtkWidget *checkbutton_events_notif_area;
	GtkWidget *checkbutton_autoconnect;

	GtkWidget *treeview_sounds;
	GtkWidget *treeview_spell_checker;

	GtkWidget *vbox_chat_theme;
	GtkWidget *combobox_chat_theme;
	GtkWidget *combobox_chat_theme_variant;
	GtkWidget *hbox_chat_theme_variant;
	GtkWidget *sw_chat_theme_preview;
	EmpathyThemeAdium *chat_theme_preview;
	EmpathyThemeManager *theme_manager;

	GSettings *gsettings;
	GSettings *gsettings_chat;
	GSettings *gsettings_call;
	GSettings *gsettings_loc;
	GSettings *gsettings_notify;
	GSettings *gsettings_sound;
	GSettings *gsettings_ui;
	GSettings *gsettings_logger;
};

static void     preferences_setup_widgets                (EmpathyPreferences      *preferences,
							  GtkBuilder		  *gui);
static void     preferences_languages_setup              (EmpathyPreferences      *preferences);
static void     preferences_languages_add                (EmpathyPreferences      *preferences);
static void     preferences_languages_save               (EmpathyPreferences      *preferences);
static gboolean preferences_languages_save_foreach       (GtkTreeModel           *model,
							  GtkTreePath            *path,
							  GtkTreeIter            *iter,
							  gchar                 **languages);
static void     preferences_languages_load               (EmpathyPreferences      *preferences);
static gboolean preferences_languages_load_foreach       (GtkTreeModel           *model,
							  GtkTreePath            *path,
							  GtkTreeIter            *iter,
							  GList                  *languages);
static void     preferences_languages_cell_toggled_cb    (GtkCellRendererToggle  *cell,
							  gchar                  *path_string,
							  EmpathyPreferences      *preferences);

enum {
	COL_LANG_ENABLED,
	COL_LANG_CODE,
	COL_LANG_NAME,
	COL_LANG_COUNT
};

enum {
	COL_THEME_VISIBLE_NAME,
	COL_THEME_ADIUM_NAME,
	COL_THEME_ADIUM_INFO,
	COL_THEME_COUNT
};

enum {
	COL_VARIANT_NAME,
	COL_VARIANT_DEFAULT,
	COL_VARIANT_COUNT
};

enum {
	COL_SOUND_ENABLED,
	COL_SOUND_NAME,
	COL_SOUND_KEY,
	COL_SOUND_COUNT
};

typedef struct {
	const char *name;
	const char *key;
} SoundEventEntry;

/* TODO: add phone related sounds also? */
static SoundEventEntry sound_entries [] = {
	{ N_("Message received"), EMPATHY_PREFS_SOUNDS_INCOMING_MESSAGE },
	{ N_("Message sent"), EMPATHY_PREFS_SOUNDS_OUTGOING_MESSAGE },
	{ N_("New conversation"), EMPATHY_PREFS_SOUNDS_NEW_CONVERSATION },
	{ N_("Contact comes online"), EMPATHY_PREFS_SOUNDS_CONTACT_LOGIN },
	{ N_("Contact goes offline"), EMPATHY_PREFS_SOUNDS_CONTACT_LOGOUT },
	{ N_("Account connected"), EMPATHY_PREFS_SOUNDS_SERVICE_LOGIN },
	{ N_("Account disconnected"), EMPATHY_PREFS_SOUNDS_SERVICE_LOGOUT }
};

static void
preferences_setup_widgets (EmpathyPreferences *preferences,
			   GtkBuilder *gui)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
#define BIND_ACTIVE(schema, key, widget) \
	g_settings_bind (priv->gsettings_##schema, EMPATHY_PREFS_##key, \
			 gtk_builder_get_object (gui, widget), "active", \
			 G_SETTINGS_BIND_DEFAULT);

	BIND_ACTIVE (notify, NOTIFICATIONS_ENABLED,
		     "checkbutton_notifications_enabled");
	BIND_ACTIVE (notify, NOTIFICATIONS_DISABLED_AWAY,
		     "checkbutton_notifications_disabled_away");
	BIND_ACTIVE (notify, NOTIFICATIONS_FOCUS,
		     "checkbutton_notifications_focus");
	BIND_ACTIVE (notify, NOTIFICATIONS_CONTACT_SIGNIN,
		     "checkbutton_notifications_contact_signin");
	BIND_ACTIVE (notify, NOTIFICATIONS_CONTACT_SIGNOUT,
		     "checkbutton_notifications_contact_signout");

	BIND_ACTIVE (sound, SOUNDS_ENABLED,
		     "checkbutton_sounds_enabled");
	BIND_ACTIVE (sound, SOUNDS_DISABLED_AWAY,
		     "checkbutton_sounds_disabled_away");

	BIND_ACTIVE (ui, UI_SHOW_OFFLINE,
		     "checkbutton_show_offline");
	BIND_ACTIVE (ui, UI_SHOW_GROUPS,
		     "checkbutton_show_groups");
	BIND_ACTIVE (ui, UI_SEPARATE_CHAT_WINDOWS,
		     "radiobutton_chats_new_windows");
	BIND_ACTIVE (ui, UI_EVENTS_NOTIFY_AREA,
		     "checkbutton_events_notif_area");
	BIND_ACTIVE (ui, UI_SHOW_BALANCES,
		     "checkbutton_show_balances");

	BIND_ACTIVE (chat, CHAT_SHOW_SMILEYS,
		     "checkbutton_show_smileys");
	BIND_ACTIVE (chat, CHAT_SHOW_CONTACTS_IN_ROOMS,
		     "checkbutton_show_contacts_in_rooms");

	BIND_ACTIVE (call, CALL_ECHO_CANCELLATION,
		     "call_echo_cancellation");

	BIND_ACTIVE (loc, LOCATION_PUBLISH,
		     "checkbutton_location_publish");
	BIND_ACTIVE (loc, LOCATION_REDUCE_ACCURACY,
		     "checkbutton_location_reduce_accuracy");

	BIND_ACTIVE (logger, LOGGER_ENABLED,
		     "checkbutton_logging");

#undef BIND_ACTIVE

#define BIND_SENSITIVE(schema, key, widget) \
	g_settings_bind (priv->gsettings_##schema, EMPATHY_PREFS_##key, \
			 gtk_builder_get_object (gui, widget), "sensitive", \
			G_SETTINGS_BIND_GET);
	/* N.B. BIND_SENSITIVE() is in addition to the sensitivity setting of
	 *      BIND_ACTIVE() */
	BIND_SENSITIVE (notify, NOTIFICATIONS_ENABLED,
			"checkbutton_notifications_disabled_away");
	BIND_SENSITIVE (notify, NOTIFICATIONS_ENABLED,
			"checkbutton_notifications_focus");
	BIND_SENSITIVE (notify, NOTIFICATIONS_ENABLED,
			"checkbutton_notifications_contact_signin");
	BIND_SENSITIVE (notify, NOTIFICATIONS_ENABLED,
			"checkbutton_notifications_contact_signout");

	BIND_SENSITIVE (sound, SOUNDS_ENABLED,
			"checkbutton_sounds_disabled_away");
	BIND_SENSITIVE (sound, SOUNDS_ENABLED,
			"treeview_sounds");

	BIND_SENSITIVE (loc, LOCATION_PUBLISH,
			"checkbutton_location_reduce_accuracy");

#undef BIND_SENSITIVE

	g_settings_bind (priv->gsettings,
			 EMPATHY_PREFS_AUTOCONNECT,
			 gtk_builder_get_object (gui,
				 		 "checkbutton_autoconnect"),
			 "active",
			 G_SETTINGS_BIND_DEFAULT);
}

static void
preferences_sound_cell_toggled_cb (GtkCellRendererToggle *toggle,
				   char *path_string,
				   EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkTreePath *path;
	gboolean instore;
	GtkTreeIter iter;
	GtkTreeView *view;
	GtkTreeModel *model;
	char *key;

	view = GTK_TREE_VIEW (priv->treeview_sounds);
	model = gtk_tree_view_get_model (view);

	path = gtk_tree_path_new_from_string (path_string);

	gtk_tree_model_get_iter (model, &iter, path);
	gtk_tree_model_get (model, &iter, COL_SOUND_KEY, &key,
			    COL_SOUND_ENABLED, &instore, -1);

	instore ^= 1;

	gtk_list_store_set (GTK_LIST_STORE (model), &iter,
			    COL_SOUND_ENABLED, instore, -1);

	g_settings_set_boolean (priv->gsettings_sound, key, instore);

	g_free (key);
	gtk_tree_path_free (path);
}

static void
preferences_sound_load (EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	guint i;
	GtkTreeView *view;
	GtkListStore *store;
	GtkTreeIter iter;
	gboolean set;

	view = GTK_TREE_VIEW (priv->treeview_sounds);
	store = GTK_LIST_STORE (gtk_tree_view_get_model (view));

	for (i = 0; i < G_N_ELEMENTS (sound_entries); i++) {
		set = g_settings_get_boolean (priv->gsettings_sound,
					      sound_entries[i].key);

		gtk_list_store_insert_with_values (store, &iter, i,
						   COL_SOUND_NAME, gettext (sound_entries[i].name),
						   COL_SOUND_KEY, sound_entries[i].key,
						   COL_SOUND_ENABLED, set, -1);
	}
}

static void
preferences_sound_setup (EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkTreeView *view;
	GtkListStore *store;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;

	view = GTK_TREE_VIEW (priv->treeview_sounds);

	store = gtk_list_store_new (COL_SOUND_COUNT,
				    G_TYPE_BOOLEAN, /* enabled */
				    G_TYPE_STRING,  /* name */
				    G_TYPE_STRING); /* key */

	gtk_tree_view_set_model (view, GTK_TREE_MODEL (store));

	renderer = gtk_cell_renderer_toggle_new ();
	g_signal_connect (renderer, "toggled",
			  G_CALLBACK (preferences_sound_cell_toggled_cb),
			  preferences);

	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (column, renderer,
					    "active", COL_SOUND_ENABLED);

	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (column, renderer,
					    "text", COL_SOUND_NAME);

	gtk_tree_view_append_column (view, column);

	gtk_tree_view_column_set_resizable (column, FALSE);
	gtk_tree_view_column_set_clickable (GTK_TREE_VIEW_COLUMN (column), TRUE);

	g_object_unref (store);
}

static void
preferences_languages_setup (EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkTreeView       *view;
	GtkListStore      *store;
	GtkTreeSelection  *selection;
	GtkTreeViewColumn *column;
	GtkCellRenderer   *renderer;
	guint              col_offset;

	view = GTK_TREE_VIEW (priv->treeview_spell_checker);

	store = gtk_list_store_new (COL_LANG_COUNT,
				    G_TYPE_BOOLEAN,  /* enabled */
				    G_TYPE_STRING,   /* code */
				    G_TYPE_STRING);  /* name */

	gtk_tree_view_set_model (view, GTK_TREE_MODEL (store));

	selection = gtk_tree_view_get_selection (view);
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);

	renderer = gtk_cell_renderer_toggle_new ();
	g_signal_connect (renderer, "toggled",
			  G_CALLBACK (preferences_languages_cell_toggled_cb),
			  preferences);

	column = gtk_tree_view_column_new_with_attributes (NULL, renderer,
							   "active", COL_LANG_ENABLED,
							   NULL);

	gtk_tree_view_append_column (view, column);

	renderer = gtk_cell_renderer_text_new ();
	col_offset = gtk_tree_view_insert_column_with_attributes (view,
								  -1, _("Language"),
								  renderer,
								  "text", COL_LANG_NAME,
								  NULL);

	g_object_set_data (G_OBJECT (renderer),
			   "column", GINT_TO_POINTER (COL_LANG_NAME));

	column = gtk_tree_view_get_column (view, col_offset - 1);
	gtk_tree_view_column_set_sort_column_id (column, COL_LANG_NAME);
	gtk_tree_view_column_set_resizable (column, FALSE);
	gtk_tree_view_column_set_clickable (GTK_TREE_VIEW_COLUMN (column), TRUE);

	g_object_unref (store);
}

static void
preferences_languages_add (EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkTreeView  *view;
	GtkListStore *store;
	GList        *codes, *l;

	view = GTK_TREE_VIEW (priv->treeview_spell_checker);
	store = GTK_LIST_STORE (gtk_tree_view_get_model (view));

	codes = empathy_spell_get_language_codes ();

	if (!codes) {
		gtk_widget_set_sensitive (priv->treeview_spell_checker, FALSE);
	}

	for (l = codes; l; l = l->next) {
		const gchar *code;
		const gchar *name;

		code = l->data;
		name = empathy_spell_get_language_name (code);
		if (!name) {
			continue;
		}

		gtk_list_store_insert_with_values (store, NULL, -1,
				    COL_LANG_CODE, code,
				    COL_LANG_NAME, name,
				    -1);
	}

	empathy_spell_free_language_codes (codes);
}

static void
preferences_languages_save (EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkTreeView       *view;
	GtkTreeModel      *model;

	gchar             *languages = NULL;

	view = GTK_TREE_VIEW (priv->treeview_spell_checker);
	model = gtk_tree_view_get_model (view);

	gtk_tree_model_foreach (model,
				(GtkTreeModelForeachFunc) preferences_languages_save_foreach,
				&languages);

	/* if user selects no languages, we don't want spell check */
	g_settings_set_boolean (priv->gsettings_chat,
				EMPATHY_PREFS_CHAT_SPELL_CHECKER_ENABLED,
				languages != NULL);

	g_settings_set_string (priv->gsettings_chat,
			       EMPATHY_PREFS_CHAT_SPELL_CHECKER_LANGUAGES,
			       languages != NULL ? languages : "");

	g_free (languages);
}

static gboolean
preferences_languages_save_foreach (GtkTreeModel  *model,
				    GtkTreePath   *path,
				    GtkTreeIter   *iter,
				    gchar        **languages)
{
	gboolean  enabled;
	gchar    *code;

	if (!languages) {
		return TRUE;
	}

	gtk_tree_model_get (model, iter, COL_LANG_ENABLED, &enabled, -1);
	if (!enabled) {
		return FALSE;
	}

	gtk_tree_model_get (model, iter, COL_LANG_CODE, &code, -1);
	if (!code) {
		return FALSE;
	}

	if (!(*languages)) {
		*languages = g_strdup (code);
	} else {
		gchar *str = *languages;
		*languages = g_strdup_printf ("%s,%s", str, code);
		g_free (str);
	}

	g_free (code);

	return FALSE;
}

static void
preferences_languages_load (EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkTreeView   *view;
	GtkTreeModel  *model;
	GList         *enabled_codes;

	enabled_codes = empathy_spell_get_enabled_language_codes ();

	g_settings_set_boolean (priv->gsettings_chat,
				EMPATHY_PREFS_CHAT_SPELL_CHECKER_ENABLED,
				enabled_codes != NULL);

	if (enabled_codes == NULL)
		return;

	view = GTK_TREE_VIEW (priv->treeview_spell_checker);
	model = gtk_tree_view_get_model (view);

	gtk_tree_model_foreach (model,
				(GtkTreeModelForeachFunc) preferences_languages_load_foreach,
				enabled_codes);

	g_list_free (enabled_codes);
}

static gboolean
preferences_languages_load_foreach (GtkTreeModel  *model,
				    GtkTreePath   *path,
				    GtkTreeIter   *iter,
				    GList         *languages)
{
	gchar    *code;
	gboolean  found = FALSE;

	if (!languages) {
		return TRUE;
	}

	gtk_tree_model_get (model, iter, COL_LANG_CODE, &code, -1);
	if (!code) {
		return FALSE;
	}

	if (g_list_find_custom (languages, code, (GCompareFunc) strcmp)) {
		found = TRUE;
	}

	g_free (code);
	gtk_list_store_set (GTK_LIST_STORE (model), iter, COL_LANG_ENABLED, found, -1);
	return FALSE;
}

static void
preferences_languages_cell_toggled_cb (GtkCellRendererToggle *cell,
				       gchar                 *path_string,
				       EmpathyPreferences     *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkTreeView  *view;
	GtkTreeModel *model;
	GtkListStore *store;
	GtkTreePath  *path;
	GtkTreeIter   iter;
	gboolean      enabled;

	view = GTK_TREE_VIEW (priv->treeview_spell_checker);
	model = gtk_tree_view_get_model (view);
	store = GTK_LIST_STORE (model);

	path = gtk_tree_path_new_from_string (path_string);

	gtk_tree_model_get_iter (model, &iter, path);
	gtk_tree_model_get (model, &iter, COL_LANG_ENABLED, &enabled, -1);

	enabled ^= 1;

	gtk_list_store_set (store, &iter, COL_LANG_ENABLED, enabled, -1);
	gtk_tree_path_free (path);

	preferences_languages_save (preferences);
}

static void
preferences_preview_theme_append_message (EmpathyThemeAdium *view,
					  EmpathyContact *sender,
					  EmpathyContact *receiver,
					  const gchar *text,
					  gboolean should_highlight)
{
	EmpathyMessage *message;

	message = g_object_new (EMPATHY_TYPE_MESSAGE,
		"sender", sender,
		"receiver", receiver,
		"body", text,
		NULL);

	empathy_theme_adium_append_message (view, message, should_highlight);
	g_object_unref (message);
}

static void
preferences_preview_theme_changed_cb (EmpathyThemeManager *manager,
				      EmpathyPreferences  *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	EmpathyClientFactory *factory;
	TpAccount *account;
	EmpathyContact *juliet;
	EmpathyContact *romeo;

	DEBUG ("Theme changed, update preview widget");

	if (priv->chat_theme_preview != NULL) {
		gtk_widget_destroy (GTK_WIDGET (priv->chat_theme_preview));
	}
	priv->chat_theme_preview = empathy_theme_manager_create_view (manager);
	if (GTK_IS_SCROLLABLE (priv->chat_theme_preview))
	  {
	    gtk_container_add (GTK_CONTAINER (priv->sw_chat_theme_preview),
			       GTK_WIDGET (priv->chat_theme_preview));
	  }
	else
	  {
	    gtk_widget_hide (priv->sw_chat_theme_preview);
	    gtk_box_pack_start (GTK_BOX (priv->vbox_chat_theme),
				GTK_WIDGET (priv->chat_theme_preview),
				TRUE, TRUE, 0);
	  }
	gtk_widget_show (GTK_WIDGET (priv->chat_theme_preview));

	/* FIXME: It is ugly to add a fake conversation like that.
	 * Would be cool if we could request a TplLogManager for a fake
	 * conversation */
	factory = empathy_client_factory_dup ();

	account = tp_simple_client_factory_ensure_account (
		TP_SIMPLE_CLIENT_FACTORY (factory),
		TP_ACCOUNT_OBJECT_PATH_BASE "cm/jabber/account", NULL, NULL);
	juliet = g_object_new (EMPATHY_TYPE_CONTACT,
		"account", account,
		"id", "juliet",
		/* translators: Contact name for the chat theme preview */
		"alias", _("Juliet"),
		"is-user", FALSE,
		NULL);
	romeo = g_object_new (EMPATHY_TYPE_CONTACT,
		"account", account,
		"id", "romeo",
		/* translators: Contact name for the chat theme preview */
		"alias", _("Romeo"),
		"is-user", TRUE,
		NULL);

	preferences_preview_theme_append_message (priv->chat_theme_preview,
		/* translators: Quote from Romeo & Julier, for chat theme preview */
		juliet, romeo, _("O Romeo, Romeo, wherefore art thou Romeo?"),
		TRUE /* this message mentions Romeo */);
	preferences_preview_theme_append_message (priv->chat_theme_preview,
		/* translators: Quote from Romeo & Julier, for chat theme preview */
		juliet, romeo, _("Deny thy father and refuse thy name;"), FALSE);
	preferences_preview_theme_append_message (priv->chat_theme_preview,
		/* translators: Quote from Romeo & Julier, for chat theme preview */
		juliet, romeo, _("Or if thou wilt not, be but sworn my love"), FALSE);
	preferences_preview_theme_append_message (priv->chat_theme_preview,
		/* translators: Quote from Romeo & Julier, for chat theme preview */
		juliet, romeo, _("And I’ll no longer be a Capulet."), FALSE);
	preferences_preview_theme_append_message (priv->chat_theme_preview,
		/* translators: Quote from Romeo & Julier, for chat theme preview */
		romeo, juliet, _("Shall I hear more, or shall I speak at this?"), FALSE);

	/* translators: Quote from Romeo & Julier, for chat theme preview */
	empathy_theme_adium_append_event (priv->chat_theme_preview, _("Juliet has disconnected"));

	g_object_unref (juliet);
	g_object_unref (romeo);
	g_object_unref (account);
	g_object_unref (factory);
}

static void
preferences_theme_variant_changed_cb (GtkComboBox        *combo,
				      EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkTreeIter   iter;

	if (gtk_combo_box_get_active_iter (combo, &iter)) {
		GtkTreeModel *model;
		gchar        *name;

		model = gtk_combo_box_get_model (combo);
		gtk_tree_model_get (model, &iter,
				    COL_VARIANT_NAME, &name,
				    -1);

		g_settings_set_string (priv->gsettings_chat,
				       EMPATHY_PREFS_CHAT_THEME_VARIANT,
				       name);

		g_free (name);
	}
}

static void
preferences_theme_variant_notify_cb (GSettings   *gsettings,
				     const gchar *key,
				     gpointer     user_data)
{
	EmpathyPreferences *preferences = user_data;
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkComboBox        *combo;
	gchar              *conf_name;
	GtkTreeModel       *model;
	GtkTreeIter         iter;
	GtkTreeIter         default_iter;
	gboolean            found_default = FALSE;
	gboolean            found = FALSE;
	gboolean            ok;

	conf_name = g_settings_get_string (gsettings, EMPATHY_PREFS_CHAT_THEME_VARIANT);
	combo = GTK_COMBO_BOX (priv->combobox_chat_theme_variant);
	model = gtk_combo_box_get_model (combo);

	for (ok = gtk_tree_model_get_iter_first (model, &iter);
	     ok && !found;
	     ok = gtk_tree_model_iter_next (model, &iter)) {
		gchar *name;
		gboolean is_default;

		gtk_tree_model_get (model, &iter,
				    COL_VARIANT_NAME, &name,
				    COL_VARIANT_DEFAULT, &is_default,
				    -1);

		if (!tp_strdiff (name, conf_name)) {
			found = TRUE;
			gtk_combo_box_set_active_iter (combo, &iter);
		}
		if (is_default) {
			found_default = TRUE;
			default_iter = iter;
		}

		g_free (name);
	}

	/* Fallback to the first one. */
	if (!found) {
		if (found_default) {
			gtk_combo_box_set_active_iter (combo, &default_iter);
		} else if (gtk_tree_model_get_iter_first (model, &iter)) {
			gtk_combo_box_set_active_iter (combo, &iter);
		}
	}

	g_free (conf_name);
}

/* return TRUE if we added at least one variant */
static gboolean
preferences_theme_variants_fill (EmpathyPreferences *preferences,
				 GHashTable         *info)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkTreeModel *model;
	GtkListStore *store;
	GPtrArray    *variants;
	const gchar  *default_variant;
	guint         i;
	gboolean      result = FALSE;

	model = gtk_combo_box_get_model (GTK_COMBO_BOX (priv->combobox_chat_theme_variant));
	store = GTK_LIST_STORE (model);
	gtk_list_store_clear (store);

	variants = empathy_adium_info_get_available_variants (info);
	default_variant = empathy_adium_info_get_default_variant (info);
	for (i = 0; i < variants->len; i++) {
		gchar *name = g_ptr_array_index (variants, i);

		gtk_list_store_insert_with_values (store, NULL, -1,
			COL_VARIANT_NAME, name,
			COL_VARIANT_DEFAULT, !tp_strdiff (name, default_variant),
			-1);

		result = TRUE;
	}

	/* Select the variant from the GSetting key */
	preferences_theme_variant_notify_cb (priv->gsettings_chat,
					     EMPATHY_PREFS_CHAT_THEME_VARIANT,
					     preferences);

	return result;
}

static void
preferences_theme_variants_setup (EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkComboBox   *combo;
	GtkCellLayout *cell_layout;
	GtkCellRenderer *renderer;
	GtkListStore  *store;

	combo = GTK_COMBO_BOX (priv->combobox_chat_theme_variant);
	cell_layout = GTK_CELL_LAYOUT (combo);

	/* Create the model */
	store = gtk_list_store_new (COL_VARIANT_COUNT,
				    G_TYPE_STRING,      /* name */
				    G_TYPE_BOOLEAN);    /* is default */
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store),
		COL_VARIANT_NAME, GTK_SORT_ASCENDING);

	/* Add cell renderer */
	renderer = gtk_cell_renderer_text_new ();
	gtk_cell_layout_pack_start (cell_layout, renderer, TRUE);
	gtk_cell_layout_set_attributes (cell_layout, renderer,
		"text", COL_VARIANT_NAME, NULL);

	gtk_combo_box_set_model (combo, GTK_TREE_MODEL (store));
	g_object_unref (store);

	g_signal_connect (combo, "changed",
			  G_CALLBACK (preferences_theme_variant_changed_cb),
			  preferences);

	/* Track changes of the GSetting key */
	g_signal_connect (priv->gsettings_chat,
			  "changed::" EMPATHY_PREFS_CHAT_THEME_VARIANT,
			  G_CALLBACK (preferences_theme_variant_notify_cb),
			  preferences);
}

static void
preferences_theme_changed_cb (GtkComboBox        *combo,
			      EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkTreeIter   iter;

	if (gtk_combo_box_get_active_iter (combo, &iter)) {
		GtkTreeModel *model;
		gchar        *name;
		GHashTable   *info;
		gboolean variant;

		model = gtk_combo_box_get_model (combo);
		gtk_tree_model_get (model, &iter,
				    COL_THEME_ADIUM_NAME, &name,
				    COL_THEME_ADIUM_INFO, &info,
				    -1);

		g_settings_set_string (priv->gsettings_chat,
				       EMPATHY_PREFS_CHAT_THEME,
				       name);

		variant = preferences_theme_variants_fill (preferences, info);
		gtk_widget_set_visible (priv->hbox_chat_theme_variant, variant);

		g_free (name);
		tp_clear_pointer (&info, g_hash_table_unref);
	}
}

static void
preferences_theme_notify_cb (GSettings   *gsettings,
			     const gchar *key,
			     gpointer     user_data)
{
	EmpathyPreferences *preferences = user_data;
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkComboBox        *combo;
	gchar              *theme;
	GtkTreeModel       *model;
	GtkTreeIter         iter;
	gboolean            found = FALSE;
	gboolean            ok;

	theme = g_settings_get_string (gsettings, key);

	combo = GTK_COMBO_BOX (priv->combobox_chat_theme);
	model = gtk_combo_box_get_model (combo);
	for (ok = gtk_tree_model_get_iter_first (model, &iter);
	     ok && !found;
	     ok = gtk_tree_model_iter_next (model, &iter)) {
		gchar *name;

		gtk_tree_model_get (model, &iter,
				    COL_THEME_ADIUM_NAME, &name,
				    -1);

		if (!tp_strdiff (name, theme)) {
			found = TRUE;
			gtk_combo_box_set_active_iter (combo, &iter);
		}

		g_free (name);
	}

	/* Fallback to the first one. */
	if (!found) {
		if (gtk_tree_model_get_iter_first (model, &iter)) {
			gtk_combo_box_set_active_iter (combo, &iter);
		}
	}

	g_free (theme);
}

static void
preferences_themes_setup (EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (preferences);
	GtkComboBox   *combo;
	GtkCellLayout *cell_layout;
	GtkCellRenderer *renderer;
	GtkListStore  *store;
	GList         *adium_themes;

	preferences_theme_variants_setup (preferences);

	combo = GTK_COMBO_BOX (priv->combobox_chat_theme);
	cell_layout = GTK_CELL_LAYOUT (combo);

	/* Create the model */
	store = gtk_list_store_new (COL_THEME_COUNT,
				    G_TYPE_STRING,      /* Display name */
				    G_TYPE_STRING,      /* Adium theme path */
				    G_TYPE_HASH_TABLE); /* Adium theme info */
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store),
		COL_THEME_VISIBLE_NAME, GTK_SORT_ASCENDING);

	/* Fill the model */
	adium_themes = empathy_theme_manager_get_adium_themes ();
	while (adium_themes != NULL) {
		GHashTable *info;
		const gchar *visible_name;
		gchar *name;

		info = adium_themes->data;
		visible_name = tp_asv_get_string (info, "CFBundleName");
		name = empathy_theme_manager_dup_theme_name_from_path (
			tp_asv_get_string (info, "path"));

		if (visible_name != NULL && name != NULL) {
			gtk_list_store_insert_with_values (store, NULL, -1,
				COL_THEME_VISIBLE_NAME, visible_name,
				COL_THEME_ADIUM_NAME, name,
				COL_THEME_ADIUM_INFO, info,
				-1);
		}
		g_hash_table_unref (info);
		g_free (name);
		adium_themes = g_list_delete_link (adium_themes, adium_themes);
	}

	/* Add cell renderer */
	renderer = gtk_cell_renderer_text_new ();
	gtk_cell_layout_pack_start (cell_layout, renderer, TRUE);
	gtk_cell_layout_set_attributes (cell_layout, renderer,
		"text", COL_THEME_VISIBLE_NAME, NULL);

	gtk_combo_box_set_model (combo, GTK_TREE_MODEL (store));
	g_object_unref (store);

	g_signal_connect (combo, "changed",
			  G_CALLBACK (preferences_theme_changed_cb),
			  preferences);

	/* Select the theme from the GSetting key and track changes */
	preferences_theme_notify_cb (priv->gsettings_chat,
				     EMPATHY_PREFS_CHAT_THEME,
				     preferences);

	g_signal_connect (priv->gsettings_chat,
			  "changed::" EMPATHY_PREFS_CHAT_THEME,
			  G_CALLBACK (preferences_theme_notify_cb),
			  preferences);
}

static void
empathy_preferences_response (GtkDialog *widget,
			      gint response)
{
	gtk_widget_destroy (GTK_WIDGET (widget));
}

static void
empathy_preferences_finalize (GObject *self)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (self);

	g_object_unref (priv->theme_manager);

	g_object_unref (priv->gsettings);
	g_object_unref (priv->gsettings_chat);
	g_object_unref (priv->gsettings_call);
	g_object_unref (priv->gsettings_loc);
	g_object_unref (priv->gsettings_notify);
	g_object_unref (priv->gsettings_sound);
	g_object_unref (priv->gsettings_ui);
	g_object_unref (priv->gsettings_logger);

	G_OBJECT_CLASS (empathy_preferences_parent_class)->finalize (self);
}

static void
empathy_preferences_class_init (EmpathyPreferencesClass *klass)
{
	GtkDialogClass *dialog_class = GTK_DIALOG_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	dialog_class->response = empathy_preferences_response;

	object_class->finalize = empathy_preferences_finalize;

	g_type_class_add_private (object_class,
				  sizeof (EmpathyPreferencesPriv));
}

static void
empathy_preferences_init (EmpathyPreferences *preferences)
{
	EmpathyPreferencesPriv    *priv;
	GtkBuilder                *gui;
	gchar                     *filename;
	GtkWidget                 *page;

	priv = preferences->priv = G_TYPE_INSTANCE_GET_PRIVATE (preferences,
			EMPATHY_TYPE_PREFERENCES, EmpathyPreferencesPriv);

	gtk_dialog_add_button (GTK_DIALOG (preferences),
			       GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE);

	gtk_container_set_border_width (GTK_CONTAINER (preferences), 5);
	gtk_window_set_title (GTK_WINDOW (preferences), _("Preferences"));
	gtk_window_set_role (GTK_WINDOW (preferences), "preferences");
	gtk_window_set_position (GTK_WINDOW (preferences),
				 GTK_WIN_POS_CENTER_ON_PARENT);
	gtk_window_set_icon_name (GTK_WINDOW (preferences), "preferences-desktop");

	filename = empathy_file_lookup ("empathy-preferences.ui", "src");
	gui = tpaw_builder_get_file (filename,
		"notebook", &priv->notebook,
		"vbox_chat_theme", &priv->vbox_chat_theme,
		"combobox_chat_theme", &priv->combobox_chat_theme,
		"combobox_chat_theme_variant", &priv->combobox_chat_theme_variant,
		"hbox_chat_theme_variant", &priv->hbox_chat_theme_variant,
		"sw_chat_theme_preview", &priv->sw_chat_theme_preview,
		"treeview_sounds", &priv->treeview_sounds,
		"treeview_spell_checker", &priv->treeview_spell_checker,
		"label_general_behavior", &priv->label_general_behavior,
		"checkbutton_events_notif_area", &priv->checkbutton_events_notif_area,
		"checkbutton_autoconnect", &priv->checkbutton_autoconnect,
		NULL);
	g_free (filename);

	gtk_container_add (GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (preferences))), priv->notebook);
	gtk_widget_show (priv->notebook);

	priv->gsettings = g_settings_new (EMPATHY_PREFS_SCHEMA);
	priv->gsettings_chat = g_settings_new (EMPATHY_PREFS_CHAT_SCHEMA);
	priv->gsettings_call = g_settings_new (EMPATHY_PREFS_CALL_SCHEMA);
	priv->gsettings_loc = g_settings_new (EMPATHY_PREFS_LOCATION_SCHEMA);
	priv->gsettings_notify = g_settings_new (EMPATHY_PREFS_NOTIFICATIONS_SCHEMA);
	priv->gsettings_sound = g_settings_new (EMPATHY_PREFS_SOUNDS_SCHEMA);
	priv->gsettings_ui = g_settings_new (EMPATHY_PREFS_UI_SCHEMA);
	priv->gsettings_logger = g_settings_new (EMPATHY_PREFS_LOGGER_SCHEMA);

	/* Create chat theme preview, and track changes */
	priv->theme_manager = empathy_theme_manager_dup_singleton ();
	tp_g_signal_connect_object (priv->theme_manager, "theme-changed",
			  G_CALLBACK (preferences_preview_theme_changed_cb),
			  preferences, 0);
	preferences_preview_theme_changed_cb (priv->theme_manager, preferences);

	preferences_themes_setup (preferences);

	preferences_setup_widgets (preferences, gui);

	preferences_languages_setup (preferences);
	preferences_languages_add (preferences);
	preferences_languages_load (preferences);

	preferences_sound_setup (preferences);
	preferences_sound_load (preferences);

	g_object_unref (gui);

	if (empathy_spell_supported ()) {
		page = gtk_notebook_get_nth_page (GTK_NOTEBOOK (priv->notebook), EMPATHY_PREFERENCES_TAB_SPELL);
		gtk_widget_show (page);
	}

	page = gtk_notebook_get_nth_page (GTK_NOTEBOOK (priv->notebook), EMPATHY_PREFERENCES_TAB_LOCATION);
#ifdef HAVE_GEOCLUE
	gtk_widget_show (page);
#else
	gtk_widget_hide (page);
#endif
}

static EmpathyPreferencesTab
empathy_preferences_tab_from_string (const gchar *str)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (empathy_preferences_tabs); i++)
    {
      if (!tp_strdiff (str, empathy_preferences_tabs[i]))
        return i;
    }

  g_warn_if_reached ();
  return -1;
}

const gchar *
empathy_preferences_tab_to_string (EmpathyPreferencesTab tab)
{
  g_return_val_if_fail (tab < G_N_ELEMENTS (empathy_preferences_tabs), NULL);

  return empathy_preferences_tabs[tab];
}

GtkWidget *
empathy_preferences_new (GtkWindow *parent,
                         gboolean  shell_running)
{
	GtkWidget              *self;
	EmpathyPreferencesPriv *priv;
	GtkWidget              *notif_page;

	g_return_val_if_fail (parent == NULL || GTK_IS_WINDOW (parent), NULL);

	self = g_object_new (EMPATHY_TYPE_PREFERENCES, NULL);

	if (parent != NULL) {
		gtk_window_set_transient_for (GTK_WINDOW (self),
					      parent);
	}

	/* when running in Gnome Shell we must hide these options since they
	 * are meaningless in that context:
	 * - General->Behavior label
	 * - 'Display incoming events in the notification area' (General->Behavior)
	 * - 'Automatically connect at startup' (General->Behavior)
	 * - 'Notifications' tab
	 */
	priv = GET_PRIV (self);
	if (shell_running) {
		/* Behavior */
		gtk_widget_hide (priv->label_general_behavior);
		gtk_widget_hide (priv->checkbutton_events_notif_area);
		gtk_widget_hide (priv->checkbutton_autoconnect);

		/* Notifications tab */
		notif_page = gtk_notebook_get_nth_page (GTK_NOTEBOOK (priv->notebook),
		                                        EMPATHY_PREFERENCES_TAB_NOTIFICATIONS);
		gtk_widget_hide (notif_page);
	}

	return self;
}

void
empathy_preferences_show_tab (EmpathyPreferences *self,
			      const gchar *page)
{
	EmpathyPreferencesPriv *priv = GET_PRIV (self);

	gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook),
				       empathy_preferences_tab_from_string (page));
}
