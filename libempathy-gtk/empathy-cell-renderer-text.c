/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2004-2007 Imendio AB
 * Copyright (C) 2010 Collabora Ltd
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
 */

#include "config.h"
#include "empathy-cell-renderer-text.h"

#include <tp-account-widgets/tpaw-utils.h>

#include "empathy-utils.h"

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyCellRendererText)
typedef struct {
	gchar    *name;
	TpConnectionPresenceType presence_type;
	gchar    *status;
	gboolean  is_group;

	gboolean  is_valid;
	gboolean  is_selected;

	gchar   **types;

	gboolean  compact;
} EmpathyCellRendererTextPriv;

static void cell_renderer_text_finalize          (GObject                     *object);
static void cell_renderer_text_get_property      (GObject                     *object,
						  guint                        param_id,
						  GValue                      *value,
						  GParamSpec                  *pspec);
static void cell_renderer_text_set_property      (GObject                     *object,
						  guint                        param_id,
						  const GValue                *value,
						  GParamSpec                  *pspec);
static void cell_renderer_text_render            (GtkCellRenderer             *cell,
						  cairo_t *cr,
						  GtkWidget                   *widget,
						  const GdkRectangle          *background_area,
						  const GdkRectangle          *cell_area,
						  GtkCellRendererState         flags);
static void cell_renderer_text_update_text       (EmpathyCellRendererText      *cell,
						  GtkWidget                   *widget,
						  gboolean                     selected);

/* Properties */
enum {
	PROP_0,
	PROP_NAME,
	PROP_PRESENCE_TYPE,
	PROP_STATUS,
	PROP_IS_GROUP,
	PROP_COMPACT,
	PROP_CLIENT_TYPES
};

G_DEFINE_TYPE (EmpathyCellRendererText, empathy_cell_renderer_text, GTK_TYPE_CELL_RENDERER_TEXT);

static void
cell_renderer_text_get_preferred_height_for_width (GtkCellRenderer *renderer,
								GtkWidget *widget,
								gint width,
								gint *minimum_size,
								gint *natural_size)
{
	EmpathyCellRendererText *self = EMPATHY_CELL_RENDERER_TEXT (renderer);
	EmpathyCellRendererTextPriv *priv = GET_PRIV (self);

	/* Only update if not already valid so we get the right size. */
	cell_renderer_text_update_text (self, widget, priv->is_selected);

	GTK_CELL_RENDERER_CLASS (empathy_cell_renderer_text_parent_class)->
			get_preferred_height_for_width (renderer, widget, width,
					minimum_size, natural_size);
}


static void
empathy_cell_renderer_text_class_init (EmpathyCellRendererTextClass *klass)
{
	GObjectClass         *object_class;
	GtkCellRendererClass *cell_class;
	GParamSpec           *spec;

	object_class = G_OBJECT_CLASS (klass);
	cell_class = GTK_CELL_RENDERER_CLASS (klass);

	object_class->finalize = cell_renderer_text_finalize;

	object_class->get_property = cell_renderer_text_get_property;
	object_class->set_property = cell_renderer_text_set_property;

	cell_class->get_preferred_height_for_width = cell_renderer_text_get_preferred_height_for_width;
	cell_class->render = cell_renderer_text_render;

	spec = g_param_spec_string ("name", "Name", "Contact name", NULL,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_property (object_class, PROP_NAME, spec);

	spec = g_param_spec_uint ("presence-type", "TpConnectionPresenceType",
		"The contact's presence type",
		0, G_MAXUINT, /* Telepathy enum, can be extended */
		TP_CONNECTION_PRESENCE_TYPE_UNKNOWN,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_property (object_class, PROP_PRESENCE_TYPE,
		spec);

	spec = g_param_spec_string ("status", "Status message",
		"Contact's custom status message", NULL,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_property (object_class, PROP_STATUS, spec);

	spec = g_param_spec_boolean ("is-group", "Is group",
		"Whether this cell is a group", FALSE,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_property (object_class, PROP_IS_GROUP, spec);

	spec = g_param_spec_boolean ("compact", "Compact",
		"TRUE to show the status alongside the contact name;"
		"FALSE to show it on its own line",
		FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_property (object_class, PROP_COMPACT, spec);

	spec = g_param_spec_boxed ("client-types", "Contact client types",
		"Client types of the contact",
		G_TYPE_STRV, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_property (object_class, PROP_CLIENT_TYPES, spec);

	g_type_class_add_private (object_class, sizeof (EmpathyCellRendererTextPriv));
}

static void
empathy_cell_renderer_text_init (EmpathyCellRendererText *cell)
{
	EmpathyCellRendererTextPriv *priv = G_TYPE_INSTANCE_GET_PRIVATE (cell,
		EMPATHY_TYPE_CELL_RENDERER_TEXT, EmpathyCellRendererTextPriv);

	cell->priv = priv;
	g_object_set (cell,
		      "ellipsize", PANGO_ELLIPSIZE_END,
		      NULL);

	priv->name = g_strdup ("");
	priv->status = g_strdup ("");
	priv->compact = FALSE;
}

static void
cell_renderer_text_finalize (GObject *object)
{
	EmpathyCellRendererText     *cell;
	EmpathyCellRendererTextPriv *priv;

	cell = EMPATHY_CELL_RENDERER_TEXT (object);
	priv = GET_PRIV (cell);

	g_free (priv->name);
	g_free (priv->status);
	g_strfreev (priv->types);

	(G_OBJECT_CLASS (empathy_cell_renderer_text_parent_class)->finalize) (object);
}

static void
cell_renderer_text_get_property (GObject    *object,
				 guint       param_id,
				 GValue     *value,
				 GParamSpec *pspec)
{
	EmpathyCellRendererText     *cell;
	EmpathyCellRendererTextPriv *priv;

	cell = EMPATHY_CELL_RENDERER_TEXT (object);
	priv = GET_PRIV (cell);

	switch (param_id) {
	case PROP_NAME:
		g_value_set_string (value, priv->name);
		break;
	case PROP_PRESENCE_TYPE:
		g_value_set_uint (value, priv->presence_type);
		break;
	case PROP_STATUS:
		g_value_set_string (value, priv->status);
		break;
	case PROP_IS_GROUP:
		g_value_set_boolean (value, priv->is_group);
		break;
	case PROP_COMPACT:
		g_value_set_boolean (value, priv->compact);
		break;
	case PROP_CLIENT_TYPES:
		g_value_set_boxed (value, priv->types);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static void
cell_renderer_text_set_property (GObject      *object,
				 guint         param_id,
				 const GValue *value,
				 GParamSpec   *pspec)
{
	EmpathyCellRendererText     *cell;
	EmpathyCellRendererTextPriv *priv;
	const gchar                *str;

	cell = EMPATHY_CELL_RENDERER_TEXT (object);
	priv = GET_PRIV (cell);

	switch (param_id) {
	case PROP_NAME:
		g_free (priv->name);
		str = g_value_get_string (value);
		priv->name = g_strdup (str ? str : "");
		g_strdelimit (priv->name, "\n\r\t", ' ');
		priv->is_valid = FALSE;
		break;
	case PROP_PRESENCE_TYPE:
		priv->presence_type = g_value_get_uint (value);
		priv->is_valid = FALSE;
		break;
	case PROP_STATUS:
		g_free (priv->status);
		str = g_value_get_string (value);
		priv->status = g_strdup (str ? str : "");
		g_strdelimit (priv->status, "\n\r\t", ' ');
		priv->is_valid = FALSE;
		break;
	case PROP_IS_GROUP:
		priv->is_group = g_value_get_boolean (value);
		priv->is_valid = FALSE;
		break;
	case PROP_COMPACT:
		priv->compact = g_value_get_boolean (value);
		priv->is_valid = FALSE;
		break;
	case PROP_CLIENT_TYPES:
		g_strfreev (priv->types);
		priv->types = g_value_dup_boxed (value);
		priv->is_valid = FALSE;
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static void
cell_renderer_text_render (GtkCellRenderer      *cell,
			   cairo_t *cr,
			   GtkWidget            *widget,
			   const GdkRectangle   *background_area,
			   const GdkRectangle   *cell_area,
			   GtkCellRendererState  flags)
{
	EmpathyCellRendererText *celltext;

	celltext = EMPATHY_CELL_RENDERER_TEXT (cell);

	cell_renderer_text_update_text (celltext,
					widget,
					(flags & GTK_CELL_RENDERER_SELECTED));

	(GTK_CELL_RENDERER_CLASS (empathy_cell_renderer_text_parent_class)->render) (
		cell, cr,
		widget,
		background_area,
		cell_area,
		flags);
}

static void
cell_renderer_text_update_text (EmpathyCellRendererText *cell,
				GtkWidget              *widget,
				gboolean                selected)
{
	EmpathyCellRendererTextPriv *priv;
	PangoFontDescription *font_desc;
	PangoAttrList              *attr_list;
	PangoAttribute             *attr_color = NULL, *attr_size;
	GtkStyleContext            *style;
	gchar                      *str;
	gint                        font_size;

	priv = GET_PRIV (cell);

	if (priv->is_valid && priv->is_selected == selected) {
		return;
	}

	if (priv->is_group) {
		g_object_set (cell,
			      "visible", TRUE,
			      "weight", PANGO_WEIGHT_BOLD,
			      "text", priv->name,
			      "attributes", NULL,
			      "xpad", 1,
			      "ypad", 1,
			      NULL);

		priv->is_selected = selected;
		priv->is_valid = TRUE;
		return;
	}

	style = gtk_widget_get_style_context (widget);

	attr_list = pango_attr_list_new ();

	gtk_style_context_save (style);

	gtk_style_context_set_state (style, GTK_STATE_FLAG_NORMAL);
	gtk_style_context_get (style, GTK_STATE_FLAG_NORMAL,
		"font", &font_desc,
		NULL);

	font_size = pango_font_description_get_size (font_desc);
	pango_font_description_free (font_desc);
	attr_size = pango_attr_size_new (font_size / 1.2);
	attr_size->start_index = strlen (priv->name) + 1;
	attr_size->end_index = -1;
	pango_attr_list_insert (attr_list, attr_size);

	if (!selected) {
		GdkRGBA color;

		gtk_style_context_get_color (style, GTK_STATE_FLAG_NORMAL, &color);

		attr_color = pango_attr_foreground_new (color.red * 0xffff,
							color.green * 0xffff,
							color.blue * 0xffff);
		attr_color->start_index = attr_size->start_index;
		attr_color->end_index = -1;
		pango_attr_list_insert (attr_list, attr_color);
	}

	gtk_style_context_restore (style);

	if (priv->compact) {
		if (TPAW_STR_EMPTY (priv->status)) {
			str = g_strdup (priv->name);
		} else {
			str = g_strdup_printf ("%s %s", priv->name, priv->status);
		}
	} else {
		const gchar *status = priv->status;
		gboolean on_a_phone = FALSE;

		if (TPAW_STR_EMPTY (priv->status)) {
			status = empathy_presence_get_default_message (priv->presence_type);
		}

		if (!priv->is_group &&
				empathy_client_types_contains_mobile_device (priv->types)) {
			on_a_phone = TRUE;
			/* We want the phone black. */
			if (attr_color)
				attr_color->start_index += 3;
		}

		if (status == NULL)
			str = g_strdup (priv->name);
		else
			str = g_strdup_printf ("%s\n%s%s", priv->name,
					       on_a_phone ? "☎  " : "",
					       status);
	}

	g_object_set (cell,
		      "visible", TRUE,
		      "weight", PANGO_WEIGHT_NORMAL,
		      "text", str,
		      "attributes", attr_list,
		      "xpad", 0,
		      "ypad", 1,
		      NULL);

	g_free (str);
	pango_attr_list_unref (attr_list);

	priv->is_selected = selected;
	priv->is_valid = TRUE;
}

GtkCellRenderer *
empathy_cell_renderer_text_new (void)
{
	return g_object_new (EMPATHY_TYPE_CELL_RENDERER_TEXT, NULL);
}
