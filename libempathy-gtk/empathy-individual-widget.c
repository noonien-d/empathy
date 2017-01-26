/*
 * Copyright (C) 2007-2010 Collabora Ltd.
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
 *          Philip Withnall <philip.withnall@collabora.co.uk>
 */

#include "config.h"
#include "empathy-individual-widget.h"

#include <glib/gi18n-lib.h>
#include <tp-account-widgets/tpaw-builder.h>
#include <tp-account-widgets/tpaw-contactinfo-utils.h>
#include <tp-account-widgets/tpaw-time.h>
#include <tp-account-widgets/tpaw-utils.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#ifdef HAVE_LIBCHAMPLAIN
#include <champlain/champlain.h>
#include <champlain-gtk/champlain-gtk.h>
#endif

#include "empathy-avatar-image.h"
#include "empathy-groups-widget.h"
#include "empathy-gtk-enum-types.h"
#include "empathy-location.h"
#include "empathy-request-util.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CONTACT
#include "empathy-debug.h"

/**
 * SECTION:empathy-individual-widget
 * @title:EmpathyIndividualWidget
 * @short_description: A widget used to display and edit details about an
 * individual
 * @include: libempathy-empathy-individual-widget.h
 *
 * #EmpathyIndividualWidget is a widget which displays appropriate widgets
 * with details about an individual, also allowing changing these details,
 * if desired.
 */

/**
 * EmpathyIndividualWidget:
 * @parent: parent object
 *
 * Widget which displays appropriate widgets with details about an individual,
 * also allowing changing these details, if desired.
 */

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyIndividualWidget)

typedef struct {
  FolksIndividual *individual; /* owned */
  EmpathyIndividualWidgetFlags flags;

  /* weak pointer to the contact whose contact details we're displaying */
  TpContact *contact;

  /* unowned Persona (borrowed from priv->individual) -> GtkGrid child */
  GHashTable *persona_grids;
  /* Table containing the information for the individual as whole, or NULL */
  GtkGrid *individual_grid;

  /* Individual */
  GtkWidget *hbox_presence;
  GtkWidget *vbox_individual_widget;
  GtkWidget *scrolled_window_individual;
  GtkWidget *viewport_individual;
  GtkWidget *vbox_individual;

  /* Location */
  GtkWidget *vbox_location;
  GtkWidget *subvbox_location;
  GtkWidget *grid_location;
  GtkWidget *label_location;
#ifdef HAVE_LIBCHAMPLAIN
  GtkWidget *viewport_map;
  GtkWidget *map_view_embed;
  ChamplainView *map_view;
#endif

  /* Groups */
  GtkWidget *groups_widget;

  /* Client types */
  GtkWidget *hbox_client_types;

  /* Details */
  GtkWidget *vbox_details;
  GtkWidget *grid_details;
  GtkWidget *hbox_details_requested;
  GtkWidget *details_spinner;
  GCancellable *details_cancellable; /* owned */
} EmpathyIndividualWidgetPriv;

G_DEFINE_TYPE (EmpathyIndividualWidget, empathy_individual_widget,
    GTK_TYPE_BOX);

enum {
  PROP_INDIVIDUAL = 1,
  PROP_FLAGS
};

static void client_types_update (EmpathyIndividualWidget *self);
static void remove_weak_contact (EmpathyIndividualWidget *self);

static void
details_set_up (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);

  gtk_widget_hide (priv->vbox_details);

  priv->details_spinner = gtk_spinner_new ();
  gtk_box_pack_end (GTK_BOX (priv->hbox_details_requested),
      priv->details_spinner, TRUE, TRUE, 0);
  gtk_widget_show (priv->details_spinner);
}

static void
client_types_notify_cb (TpContact *contact,
    GParamSpec *pspec,
    EmpathyIndividualWidget *self)
{
  client_types_update (self);
}

typedef struct {
  EmpathyIndividualWidget *widget; /* weak */
  TpContact *contact; /* owned */
} DetailsData;

static void
update_weak_contact (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  TpContact *tp_contact = NULL;

  remove_weak_contact (self);

  if (priv->individual != NULL)
    {
      /* FIXME: We take the most available TpContact we find and only
       * use its details. It would be a lot better if we would get the
       * details for every TpContact in the Individual and merge them
       * all, but that requires vCard support in libfolks for it to
       * not be hideously complex.  (bgo#627399) */
      GeeSet *personas;
      GeeIterator *iter;
      FolksPresenceType presence_type = FOLKS_PRESENCE_TYPE_UNSET;

      personas = folks_individual_get_personas (priv->individual);
      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (gee_iterator_next (iter))
        {
          FolksPersona *persona = gee_iterator_get (iter);

          /* We only want personas which have presence and a TpContact */
          if (empathy_folks_persona_is_interesting (FOLKS_PERSONA (persona)))
            {
              FolksPresenceDetails *presence;
              FolksPresenceType presence_type_cur;

              presence = FOLKS_PRESENCE_DETAILS (persona);
              presence_type_cur = folks_presence_details_get_presence_type (
                  presence);

              if (tp_contact == NULL || folks_presence_details_typecmp (
                    presence_type_cur, presence_type) > 0)
                {
                  presence_type = presence_type_cur;
                  tp_contact = tpf_persona_get_contact (TPF_PERSONA (persona));
                }
            }

          g_clear_object (&persona);
        }
      g_clear_object (&iter);
    }

  if (tp_contact != NULL)
    {
      priv->contact = tp_contact;
      g_object_add_weak_pointer (G_OBJECT (tp_contact),
          (gpointer *) &priv->contact);

      g_signal_connect (priv->contact, "notify::client-types",
          (GCallback) client_types_notify_cb, self);
    }
}

static void
add_row (GtkGrid *grid,
    guint row,
    GtkWidget *title,
    GtkWidget *value)
{
  gtk_grid_attach (grid, title, 0, row, 1, 1);
  gtk_misc_set_alignment (GTK_MISC (title), 0, 0.5);
  gtk_widget_show (title);

  gtk_grid_attach (grid, value, 1, row, 1, 1);
  gtk_misc_set_alignment (GTK_MISC (value), 0, 0.5);
  gtk_widget_show (value);
}

static gboolean
channel_name_activated_cb (
    GtkLabel *label,
    gchar *uri,
    TpAccount *account)
{
  empathy_join_muc (account, uri, empathy_get_current_action_time ());
  return TRUE;
}

static GtkWidget *
create_channel_list_label (TpAccount *account,
    GList *info,
    guint row)
{
  GtkWidget *label = NULL;
  GString *label_markup = g_string_new ("");
  guint i;
  GPtrArray *channels;
  GList *l;

  /* Is there channels? */
  channels = g_ptr_array_new ();

  for (l = info; l != NULL; l = l->next)
    {
      TpContactInfoField *field = l->data;

      if (!tp_strdiff (field->field_name, "x-irc-channel"))
        g_ptr_array_add (channels, (gpointer) field->field_value[0]);
    }

  if (channels->len == 0)
    goto out;

  for (i = 0; i < channels->len; i++)
    {
      const gchar *channel_name = g_ptr_array_index (channels, i);
      /* We abuse the URI of the link to hold the channel name. It seems to
       * be okay to just use it essentially verbatim, rather than trying to
       * ensure it's actually a valid URI. */
      gchar *escaped = g_markup_escape_text (channel_name, -1);

      if (i > 0)
        g_string_append (label_markup, ", ");

      g_string_append_printf (label_markup, "<a href='%s'>%s</a>",
          escaped, escaped);
      g_free (escaped);
    }

  label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (label), label_markup->str);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);

  g_signal_connect (label, "activate-link",
      (GCallback) channel_name_activated_cb, account);

out:
  g_ptr_array_unref (channels);
  g_string_free (label_markup, TRUE);

  return label;
}
static guint
details_update_show (EmpathyIndividualWidget *self,
    TpContact *contact)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  GList *info, *l;
  guint n_rows = 0;
  GtkWidget *channels_label;
  TpConnection *conn;
  TpAccount *account;

  info = tp_contact_dup_contact_info (contact);
  info = g_list_sort (info, (GCompareFunc) tpaw_contact_info_field_cmp);
  for (l = info; l != NULL; l = l->next)
    {
      TpContactInfoField *field = l->data;
      gchar *title;
      const gchar *value;
      TpawContactInfoFormatFunc format;
      GtkWidget *title_widget, *value_widget;

      if (field->field_value == NULL || field->field_value[0] == NULL)
        continue;

      value = field->field_value[0];

      if (!tpaw_contact_info_lookup_field (field->field_name,
          NULL, &format))
        {
          DEBUG ("Unhandled ContactInfo field: %s", field->field_name);
          continue;
        }

      /* Skip empty field */
      if (tp_str_empty (value))
        continue;

      /* Add Title */
      title = tpaw_contact_info_field_label (field->field_name,
          field->parameters, TRUE);
      title_widget = gtk_label_new (title);

      /* Add Value */
      value_widget = gtk_label_new (value);

      if (format != NULL)
        {
          gchar *markup;

          markup = format (field->field_value);
          gtk_label_set_markup (GTK_LABEL (value_widget), markup);
          g_free (markup);
        }

      gtk_label_set_selectable (GTK_LABEL (value_widget),
          (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_FOR_TOOLTIP) ? FALSE : TRUE);

      add_row (GTK_GRID (priv->grid_details), n_rows, title_widget,
          value_widget);

      n_rows++;
    }

  conn = tp_contact_get_connection (contact);
  account = tp_connection_get_account (conn);

  channels_label = create_channel_list_label (account, info, n_rows);

  if (channels_label != NULL)
    {
      GtkWidget *title_widget;

      title_widget =  gtk_label_new (_("Channels:"));

      add_row (GTK_GRID (priv->grid_details), n_rows, title_widget,
          channels_label);

      n_rows++;
    }

  tp_contact_info_list_free (info);

  return n_rows;
}

static void
details_notify_cb (TpContact *contact,
    GParamSpec *pspec,
    EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  guint n_rows;

  gtk_container_foreach (GTK_CONTAINER (priv->grid_details),
      (GtkCallback) gtk_widget_destroy, NULL);

  n_rows = details_update_show (self, contact);

  if (n_rows > 0)
    {
      gtk_widget_show (priv->vbox_details);
      gtk_widget_show (priv->grid_details);
    }
  else
    {
      gtk_widget_hide (priv->vbox_details);
    }

  gtk_widget_hide (priv->hbox_details_requested);
  gtk_spinner_stop (GTK_SPINNER (priv->details_spinner));
}

static void
details_request_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data)
{
  EmpathyIndividualWidget *self = user_data;
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  TpContact *contact = (TpContact *) source;
  gboolean hide_widget = FALSE;
  GError *error = NULL;

  if (tp_contact_request_contact_info_finish (contact, res, &error) == TRUE)
    {
      details_notify_cb (contact, NULL, self);
    }
  else
    {
      /* If the request got cancelled it could mean the contact widget is
       * destroyed, so we should not dereference information */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            g_error_free (error);
            return;
        }

      hide_widget = TRUE;
      g_error_free (error);
    }

  if (hide_widget == TRUE)
    gtk_widget_hide (GET_PRIV (self)->vbox_details);

  tp_clear_object (&priv->details_cancellable);

  tp_g_signal_connect_object (contact, "notify::contact-info",
      (GCallback) details_notify_cb, self, 0);
}

static void
fetch_contact_information (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  TpConnection *connection;

  connection = tp_contact_get_connection (priv->contact);

  if (!tp_proxy_has_interface_by_id (connection,
          TP_IFACE_QUARK_CONNECTION_INTERFACE_CONTACT_INFO))
    {
      gtk_widget_hide (GET_PRIV (self)->vbox_details);
      return;
    }

  /* Request the Individual's info */
  gtk_widget_show (priv->vbox_details);
  gtk_widget_show (priv->hbox_details_requested);
  gtk_widget_hide (priv->grid_details);
  gtk_spinner_start (GTK_SPINNER (priv->details_spinner));

  if (priv->details_cancellable == NULL)
    {
      priv->details_cancellable = g_cancellable_new ();

      tp_contact_request_contact_info_async (priv->contact,
          priv->details_cancellable, details_request_cb, self);
    }
}

static void
details_update (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);

  if (!(priv->flags & EMPATHY_INDIVIDUAL_WIDGET_SHOW_DETAILS))
    return;

  gtk_widget_hide (priv->vbox_details);

  if (priv->contact == NULL)
    update_weak_contact (self);

  if (priv->contact != NULL)
    {
      fetch_contact_information (self);
    }
}

static void
groups_update (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);

  if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_EDIT_GROUPS &&
      priv->individual != NULL)
    {
      empathy_groups_widget_set_group_details (
          EMPATHY_GROUPS_WIDGET (priv->groups_widget),
          FOLKS_GROUP_DETAILS (priv->individual));
      gtk_widget_show (priv->groups_widget);
    }
  else
    {
      gtk_widget_hide (priv->groups_widget);
    }
}

/* Converts the Location's GHashTable's key to a user readable string */
static const gchar *
location_key_to_label (const gchar *key)
{
  if (tp_strdiff (key, EMPATHY_LOCATION_COUNTRY_CODE) == FALSE)
    return _("Country ISO Code:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_COUNTRY) == FALSE)
    return _("Country:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_REGION) == FALSE)
    return _("State:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_LOCALITY) == FALSE)
    return _("City:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_AREA) == FALSE)
    return _("Area:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_POSTAL_CODE) == FALSE)
    return _("Postal Code:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_STREET) == FALSE)
    return _("Street:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_BUILDING) == FALSE)
    return _("Building:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_FLOOR) == FALSE)
    return _("Floor:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_ROOM) == FALSE)
    return _("Room:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_TEXT) == FALSE)
    return _("Text:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_DESCRIPTION) == FALSE)
    return _("Description:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_URI) == FALSE)
    return _("URI:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_ACCURACY_LEVEL) == FALSE)
    return _("Accuracy Level:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_ERROR) == FALSE)
    return _("Error:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_VERTICAL_ERROR_M) == FALSE)
    return _("Vertical Error (meters):");
  else if (tp_strdiff (key, EMPATHY_LOCATION_HORIZONTAL_ERROR_M) == FALSE)
    return _("Horizontal Error (meters):");
  else if (tp_strdiff (key, EMPATHY_LOCATION_SPEED) == FALSE)
    return _("Speed:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_BEARING) == FALSE)
    return _("Bearing:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_CLIMB) == FALSE)
    return _("Climb Speed:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_TIMESTAMP) == FALSE)
    return _("Last Updated on:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_LON) == FALSE)
    return _("Longitude:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_LAT) == FALSE)
    return _("Latitude:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_ALT) == FALSE)
    return _("Altitude:");
  else
  {
    DEBUG ("Unexpected Location key: %s", key);
    return key;
  }
}

static void
location_update (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  EmpathyContact *contact = NULL;
  GHashTable *location = NULL;
  GValue *value;
  GtkWidget *label;
  guint row = 0;
  static const gchar* ordered_geolocation_keys[] = {
    EMPATHY_LOCATION_TEXT,
    EMPATHY_LOCATION_URI,
    EMPATHY_LOCATION_DESCRIPTION,
    EMPATHY_LOCATION_BUILDING,
    EMPATHY_LOCATION_FLOOR,
    EMPATHY_LOCATION_ROOM,
    EMPATHY_LOCATION_STREET,
    EMPATHY_LOCATION_AREA,
    EMPATHY_LOCATION_LOCALITY,
    EMPATHY_LOCATION_REGION,
    EMPATHY_LOCATION_COUNTRY,
    NULL
  };
  int i;
  const gchar *skey;
  gboolean display_map = FALSE;
  GeeSet *personas;
  GeeIterator *iter;

  if (!(priv->flags & EMPATHY_INDIVIDUAL_WIDGET_SHOW_LOCATION) ||
      priv->individual == NULL)
    {
      gtk_widget_hide (priv->vbox_location);
      return;
    }

  /* FIXME: For the moment, we just display the first location data we can
   * find amongst the Individual's Personas. Once libfolks grows a location
   * interface, we can use that. (bgo#627400) */

  personas = folks_individual_get_personas (priv->individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (location == NULL && gee_iterator_next (iter))
    {
      FolksPersona *persona = gee_iterator_get (iter);

      if (empathy_folks_persona_is_interesting (persona))
        {
          TpContact *tp_contact;

          /* Get the contact. If it turns out to have location information, we
           * have to keep it alive for the duration of the function, since we're
           * accessing its private data. */
          tp_contact = tpf_persona_get_contact (TPF_PERSONA (persona));
          if (tp_contact != NULL)
            {
              contact = empathy_contact_dup_from_tp_contact (tp_contact);
              empathy_contact_set_persona (contact, persona);

              /* Try and get a location */
              location = empathy_contact_get_location (contact);
              /* if location isn't fully valid, treat the contact as
               * insufficient */
              if (location != NULL && g_hash_table_size (location) <= 0)
                {
                  location = NULL;
                  g_clear_object (&contact);
                }
            }
        }
      g_clear_object (&persona);
    }
  g_clear_object (&iter);

  if (contact == NULL || location == NULL)
    {
      gtk_widget_hide (priv->vbox_location);
      tp_clear_object (&contact);
      return;
    }

  value = g_hash_table_lookup (location, EMPATHY_LOCATION_TIMESTAMP);
  if (value == NULL)
    {
      gchar *loc = g_strdup_printf ("<b>%s</b>", _("Location"));
      gtk_label_set_markup (GTK_LABEL (priv->label_location), loc);
      g_free (loc);
    }
  else
    {
      gchar *user_date;
      gchar *text;
      gint64 stamp;
      gchar *tmp;

      stamp = g_value_get_int64 (value);

      user_date = tpaw_time_to_string_relative (stamp);

      tmp = g_strdup_printf ("<b>%s</b>", _("Location"));
      /* translators: format is "Location, $date" */
      text = g_strdup_printf (_("%s, %s"), tmp, user_date);
      g_free (tmp);
      gtk_label_set_markup (GTK_LABEL (priv->label_location), text);
      g_free (user_date);
      g_free (text);
    }

  /* Prepare the location information table */
  if (priv->grid_location != NULL)
    gtk_widget_destroy (priv->grid_location);

  priv->grid_location = gtk_grid_new ();
  gtk_box_pack_start (GTK_BOX (priv->subvbox_location),
      priv->grid_location, FALSE, FALSE, 5);


  for (i = 0; (skey = ordered_geolocation_keys[i]); i++)
    {
      const gchar* user_label;
      GValue *gvalue;
      char *svalue = NULL;

      gvalue = g_hash_table_lookup (location, (gpointer) skey);
      if (gvalue == NULL)
        continue;

      user_label = location_key_to_label (skey);

      label = gtk_label_new (user_label);
      gtk_misc_set_alignment (GTK_MISC (label), 0, 0.5);
      gtk_grid_attach (GTK_GRID (priv->grid_location),
          label, 0, row, 1, 1);
      gtk_widget_show (label);

      if (G_VALUE_TYPE (gvalue) == G_TYPE_DOUBLE)
        {
          gdouble dvalue;
          dvalue = g_value_get_double (gvalue);
          svalue = g_strdup_printf ("%f", dvalue);
        }
      else if (G_VALUE_TYPE (gvalue) == G_TYPE_STRING)
        {
          svalue = g_value_dup_string (gvalue);
        }
      else if (G_VALUE_TYPE (gvalue) == G_TYPE_INT64)
        {
          gint64 time_;

          time_ = g_value_get_int64 (value);
          svalue = tpaw_time_to_string_utc (time_, _("%B %e, %Y at %R UTC"));
        }

      if (svalue != NULL)
        {
          label = gtk_label_new (svalue);
          gtk_grid_attach (GTK_GRID (priv->grid_location),
              label, 1, row, 1, 1);
          gtk_misc_set_alignment (GTK_MISC (label), 0, 0);
          gtk_widget_show (label);

          gtk_label_set_selectable (GTK_LABEL (label),
              (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_FOR_TOOLTIP) ? FALSE :
                  TRUE);
        }

      g_free (svalue);
      row++;
    }

  tp_clear_object (&contact);

#ifdef HAVE_LIBCHAMPLAIN
  if ((g_hash_table_lookup (location, EMPATHY_LOCATION_LAT) != NULL) &&
      (g_hash_table_lookup (location, EMPATHY_LOCATION_LON) != NULL))
    {
      /* Cannot be displayed in tooltips until Clutter-Gtk can deal with such
       * windows */
      display_map = TRUE;
    }
#endif

  if (row > 0)
    {
      /* We can display some fields */
      gtk_widget_show (priv->grid_location);
    }
  else if (display_map == FALSE)
    {
      /* Can't display either fields or map */
      gtk_widget_hide (priv->vbox_location);
      return;
    }

#ifdef HAVE_LIBCHAMPLAIN
  if (display_map == TRUE)
    {
      ChamplainMarkerLayer *layer;

      priv->map_view_embed = gtk_champlain_embed_new ();
      priv->map_view = gtk_champlain_embed_get_view (
          GTK_CHAMPLAIN_EMBED (priv->map_view_embed));

      gtk_container_add (GTK_CONTAINER (priv->viewport_map),
          priv->map_view_embed);
      g_object_set (G_OBJECT (priv->map_view),
          "kinetic-mode", TRUE,
          "zoom-level", 10,
          NULL);

      layer = champlain_marker_layer_new ();
      champlain_view_add_layer (priv->map_view, CHAMPLAIN_LAYER (layer));

      /* FIXME: For now, we have to do this manually. Once libfolks grows a
       * location interface, we can use that. (bgo#627400) */

      personas = folks_individual_get_personas (priv->individual);
      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (gee_iterator_next (iter))
        {
          FolksPersona *persona = gee_iterator_get (iter);

          if (empathy_folks_persona_is_interesting (persona))
            {
              gdouble lat = 0.0, lon = 0.0;
              ClutterActor *marker;
              TpContact *tp_contact;

              /* Get the contact */
              tp_contact = tpf_persona_get_contact (TPF_PERSONA (persona));
              if (tp_contact == NULL)
                goto while_finish;

              contact = empathy_contact_dup_from_tp_contact (tp_contact);
              empathy_contact_set_persona (contact, persona);

              /* Try and get a location */
              location = empathy_contact_get_location (contact);
              if (location == NULL || g_hash_table_size (location) == 0)
                goto while_finish;

              /* Get this persona's latitude and longitude */
              value = g_hash_table_lookup (location, EMPATHY_LOCATION_LAT);
              if (value == NULL)
                goto while_finish;

              lat = g_value_get_double (value);

              value = g_hash_table_lookup (location, EMPATHY_LOCATION_LON);
              if (value == NULL)
                goto while_finish;

              lon = g_value_get_double (value);

              /* Add a marker to the map */
              marker = champlain_label_new_with_text (
                  folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (persona)),
                  NULL, NULL, NULL);
              champlain_location_set_location (CHAMPLAIN_LOCATION (marker),
                  lat, lon);
              champlain_marker_layer_add_marker (layer,
                  CHAMPLAIN_MARKER (marker));
            }

while_finish:
          g_clear_object (&persona);
          g_clear_object (&contact);
        }
      g_clear_object (&iter);

      /* Zoom to show all of the markers */
      champlain_view_ensure_layers_visible (priv->map_view, FALSE);

      gtk_widget_show_all (priv->viewport_map);
    }
#endif

    gtk_widget_show (priv->vbox_location);
}

static void
client_types_update (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  const gchar * const *types;

  if (!(priv->flags & EMPATHY_INDIVIDUAL_WIDGET_SHOW_CLIENT_TYPES) ||
      priv->individual == NULL)
    {
      gtk_widget_hide (priv->hbox_client_types);
      return;
    }

  if (priv->contact == NULL)
    update_weak_contact (self);

  /* let's try that again... */
  if (priv->contact == NULL)
    return;

  types = tp_contact_get_client_types (priv->contact);

  if (empathy_client_types_contains_mobile_device ((GStrv) types))
    {
      gtk_widget_show (priv->hbox_client_types);
    }
  else
    {
      gtk_widget_hide (priv->hbox_client_types);
    }

}

static void
remove_weak_contact (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);

  if (priv->contact == NULL)
    return;

  g_signal_handlers_disconnect_by_func (priv->contact, client_types_notify_cb,
      self);

  g_object_remove_weak_pointer (G_OBJECT (priv->contact),
      (gpointer *) &priv->contact);
  priv->contact = NULL;
}

static EmpathyAvatar *
persona_dup_avatar (FolksPersona *persona)
{
  TpContact *tp_contact;
  EmpathyContact *contact;
  EmpathyAvatar *avatar;

  if (!empathy_folks_persona_is_interesting (persona))
    return NULL;

  tp_contact = tpf_persona_get_contact (TPF_PERSONA (persona));
  if (tp_contact == NULL)
    return NULL;

  contact = empathy_contact_dup_from_tp_contact (tp_contact);
  empathy_contact_set_persona (contact, persona);

  avatar = empathy_contact_get_avatar (contact);
  if (avatar != NULL)
    empathy_avatar_ref (avatar);
  g_object_unref (contact);

  return avatar;
}

static EmpathyAvatar *
individual_dup_avatar (FolksIndividual *individual)
{
  GeeSet *personas;
  GeeIterator *iter;
  EmpathyAvatar *avatar = NULL;

  /* FIXME: We just choose the first Persona which has an avatar, and save that.
   * The avatar handling in EmpathyContact needs to be moved into libfolks as
   * much as possible, and this code rewritten to use FolksHasAvatar.
   * (bgo#627401) */

  personas = folks_individual_get_personas (individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (avatar == NULL && gee_iterator_next (iter))
    {
      FolksPersona *persona = gee_iterator_get (iter);
      avatar = persona_dup_avatar (persona);

      g_clear_object (&persona);
    }
  g_clear_object (&iter);

  return avatar;
}

static void
save_avatar_menu_activate_cb (GtkWidget *widget,
    EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  GtkWidget *dialog;
  EmpathyAvatar *avatar;
  gchar *ext = NULL, *filename;

  dialog = gtk_file_chooser_dialog_new (_("Save Avatar"),
      NULL,
      GTK_FILE_CHOOSER_ACTION_SAVE,
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
      NULL);

  gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog),
      TRUE);

  avatar = individual_dup_avatar (priv->individual);
  if (avatar == NULL)
    return;

  /* look for the avatar extension */
  if (avatar->format != NULL)
    {
      gchar **splitted;

      splitted = g_strsplit (avatar->format, "/", 2);
      if (splitted[0] != NULL && splitted[1] != NULL)
          ext = g_strdup (splitted[1]);

      g_strfreev (splitted);
    }
  else
    {
      /* Avatar was loaded from the cache so was converted to PNG */
      ext = g_strdup ("png");
    }

  if (ext != NULL)
    {
      gchar *id;

      id = tp_escape_as_identifier (folks_individual_get_id (priv->individual));

      filename = g_strdup_printf ("%s.%s", id, ext);
      gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), filename);

      g_free (id);
      g_free (ext);
      g_free (filename);
    }

  if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
    {
      GError *error = NULL;

      filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));

      if (empathy_avatar_save_to_file (avatar, filename, &error) == FALSE)
        {
          /* Save error */
          GtkWidget *error_dialog;

          error_dialog = gtk_message_dialog_new (NULL, 0,
              GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
              _("Unable to save avatar"));

          gtk_message_dialog_format_secondary_text (
              GTK_MESSAGE_DIALOG (error_dialog), "%s", error->message);

          g_signal_connect (error_dialog, "response",
              (GCallback) gtk_widget_destroy, NULL);

          gtk_window_present (GTK_WINDOW (error_dialog));

          g_clear_error (&error);
        }

      g_free (filename);
    }

  gtk_widget_destroy (dialog);
  empathy_avatar_unref (avatar);
}

static gboolean
popup_avatar_menu (EmpathyIndividualWidget *self,
    GtkWidget *parent,
    GdkEventButton *event)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  GtkWidget *menu, *item;
  EmpathyAvatar *avatar;
  gint button, event_time;

  if (priv->individual == NULL)
    return FALSE;

  avatar = individual_dup_avatar (priv->individual);
  if (avatar == NULL)
    return FALSE;
  empathy_avatar_unref (avatar);

  menu = empathy_context_menu_new (parent);

  /* Add "Save as..." entry */
  item = gtk_image_menu_item_new_from_stock (GTK_STOCK_SAVE_AS, NULL);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  gtk_widget_show (item);

  g_signal_connect (item, "activate",
      (GCallback) save_avatar_menu_activate_cb, self);

  if (event != NULL)
    {
      button = event->button;
      event_time = event->time;
    }
  else
    {
      button = 0;
      event_time = gtk_get_current_event_time ();
    }

  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL, button, event_time);

  return TRUE;
}

static gboolean
avatar_widget_popup_menu_cb (GtkWidget *widget,
    EmpathyIndividualWidget *self)
{
  return popup_avatar_menu (self, widget, NULL);
}

static gboolean
avatar_widget_button_press_event_cb (GtkWidget *widget,
    GdkEventButton *event,
    EmpathyIndividualWidget *self)
{
  /* Ignore double-clicks and triple-clicks */
  if (event->button == 3 && event->type == GDK_BUTTON_PRESS)
    return popup_avatar_menu (self, widget, event);

  return FALSE;
}

/* Returns the TpAccount for the user as a convenience. Note that it has a ref
 * added. */
static TpAccount *
individual_is_user (FolksIndividual *individual)
{
  GeeSet *personas;
  GeeIterator *iter;
  TpAccount *retval = NULL;

  /* FIXME: This should move into libfolks when libfolks grows a way of
   * determining "self". (bgo#627402) */
  personas = folks_individual_get_personas (individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (gee_iterator_next (iter))
    {
      FolksPersona *persona = gee_iterator_get (iter);

      if (TPF_IS_PERSONA (persona))
        {
          TpContact *tp_contact;
          EmpathyContact *contact = NULL;

          /* Get the contact */
          tp_contact = tpf_persona_get_contact (TPF_PERSONA (persona));
          if (tp_contact != NULL)
            {
              contact = empathy_contact_dup_from_tp_contact (tp_contact);
              empathy_contact_set_persona (contact, persona);

              /* Determine if the contact is the user */
              if (empathy_contact_is_user (contact))
                retval = g_object_ref (empathy_contact_get_account (contact));
            }

          g_object_unref (contact);
        }
      g_clear_object (&persona);
    }
  g_clear_object (&iter);

  return retval;
}

static void
set_nickname_cb (TpAccount *account,
    GAsyncResult *result,
    gpointer user_data)
{
  GError *error = NULL;

  if (tp_account_set_nickname_finish (account, result, &error) == FALSE)
    {
      DEBUG ("Failed to set Account.Nickname: %s", error->message);
      g_error_free (error);
    }
}

static gboolean
entry_alias_focus_event_cb (GtkEditable *editable,
    GdkEventFocus *event,
    EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);

  if (priv->individual != NULL)
    {
      const gchar *alias;
      TpAccount *account;

      alias = gtk_entry_get_text (GTK_ENTRY (editable));
      account = individual_is_user (priv->individual);

      if (account != NULL)
        {
          DEBUG ("Set Account.Nickname to %s", alias);
          tp_account_set_nickname_async (account, alias,
              (GAsyncReadyCallback) set_nickname_cb, NULL);
          g_object_unref (account);
        }
      else
        {
          folks_alias_details_set_alias (FOLKS_ALIAS_DETAILS (priv->individual),
              alias);
        }
    }

  return FALSE;
}

static void
favourite_toggled_cb (GtkToggleButton *button,
    EmpathyIndividualWidget *self)
{
  gboolean active = gtk_toggle_button_get_active (button);
  folks_favourite_details_set_is_favourite (
      FOLKS_FAVOURITE_DETAILS (GET_PRIV (self)->individual), active);
}

static void
notify_avatar_cb (gpointer folks_object,
    GParamSpec *pspec,
    EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  EmpathyAvatar *avatar = NULL;
  GObject *grid;
  GtkWidget *avatar_widget;

  if (FOLKS_IS_INDIVIDUAL (folks_object))
    {
      avatar = individual_dup_avatar (FOLKS_INDIVIDUAL (folks_object));
      grid = G_OBJECT (priv->individual_grid);
    }
  else if (FOLKS_IS_PERSONA (folks_object))
    {
      avatar = persona_dup_avatar (FOLKS_PERSONA (folks_object));
      grid = g_hash_table_lookup (priv->persona_grids, folks_object);
    }
  else
    {
      g_assert_not_reached ();
    }

  if (grid == NULL)
    return;

  avatar_widget = g_object_get_data (grid, "avatar-widget");
  empathy_avatar_image_set (EMPATHY_AVATAR_IMAGE (avatar_widget), avatar);

  if (avatar != NULL)
    empathy_avatar_unref (avatar);
}

static void
notify_alias_cb (gpointer folks_object,
    GParamSpec *pspec,
    EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  GObject *grid;
  GtkWidget *alias_widget;

  if (FOLKS_IS_INDIVIDUAL (folks_object))
    grid = G_OBJECT (priv->individual_grid);
  else if (FOLKS_IS_PERSONA (folks_object))
    grid = g_hash_table_lookup (priv->persona_grids, folks_object);
  else
    g_assert_not_reached ();

  if (grid == NULL)
    return;

  alias_widget = g_object_get_data (grid, "alias-widget");

  if (GTK_IS_ENTRY (alias_widget))
    {
      gtk_entry_set_text (GTK_ENTRY (alias_widget),
          folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (folks_object)));
    }
  else
    {
      gtk_label_set_label (GTK_LABEL (alias_widget),
          folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (folks_object)));
    }
}

static void
notify_presence_cb (gpointer folks_object,
    GParamSpec *pspec,
    EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  GObject *grid;
  GtkWidget *status_label, *state_image;
  const gchar *message;
  gchar *markup_text = NULL;
  FolksPresenceType presence;
  gboolean visible = TRUE;

  if (FOLKS_IS_INDIVIDUAL (folks_object))
    grid = G_OBJECT (priv->individual_grid);
  else if (FOLKS_IS_PERSONA (folks_object))
    grid = g_hash_table_lookup (priv->persona_grids, folks_object);
  else
    g_assert_not_reached ();

  if (grid == NULL)
    return;

  status_label = g_object_get_data (grid, "status-label");
  state_image = g_object_get_data (grid, "state-image");

  presence = folks_presence_details_get_presence_type (
      FOLKS_PRESENCE_DETAILS (folks_object));
  if (presence == FOLKS_PRESENCE_TYPE_UNKNOWN ||
      presence == FOLKS_PRESENCE_TYPE_ERROR)
    {
      /* Don't display anything if we don't know the presence */
      visible = FALSE;
      goto out;
    }

  message = folks_presence_details_get_presence_message (
      FOLKS_PRESENCE_DETAILS (folks_object));
  if (TPAW_STR_EMPTY (message))
    {
      message = folks_presence_details_get_default_message_from_type (presence);
    }

  if (message != NULL)
    markup_text = tpaw_add_link_markup (message);
  gtk_label_set_markup (GTK_LABEL (status_label), markup_text);
  g_free (markup_text);

  gtk_image_set_from_icon_name (GTK_IMAGE (state_image),
      empathy_icon_name_for_presence (
        empathy_folks_presence_type_to_tp (presence)),
      GTK_ICON_SIZE_BUTTON);

out:
  gtk_widget_set_visible (status_label, visible);
  gtk_widget_set_visible (state_image, visible);
}

static void
notify_is_favourite_cb (gpointer folks_object,
    GParamSpec *pspec,
    EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  GObject *grid;
  GtkWidget *favourite_widget;

  if (FOLKS_IS_INDIVIDUAL (folks_object))
    grid = G_OBJECT (priv->individual_grid);
  else if (FOLKS_IS_PERSONA (folks_object))
    grid = g_hash_table_lookup (priv->persona_grids, folks_object);
  else
    g_assert_not_reached ();

  if (grid == NULL)
    return;

  favourite_widget = g_object_get_data (grid, "favourite-widget");

  if (GTK_IS_TOGGLE_BUTTON (favourite_widget))
    {
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (favourite_widget),
          folks_favourite_details_get_is_favourite (
              FOLKS_FAVOURITE_DETAILS (folks_object)));
    }
}

static void
alias_presence_avatar_favourite_set_up (EmpathyIndividualWidget *self,
    GtkGrid *grid,
    guint starting_row)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  GtkWidget *label, *alias, *image, *avatar;
  guint current_row = starting_row;

  /* Alias */
  label = gtk_label_new (_("Alias:"));
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
  gtk_grid_attach (grid, label, 0, current_row, 1, 1);
  gtk_widget_show (label);

  /* Set up alias label/entry */
  if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_EDIT_ALIAS)
    {
      alias = gtk_entry_new ();

      g_signal_connect (alias, "focus-out-event",
          (GCallback) entry_alias_focus_event_cb, self);

      /* Make return activate the window default (the Close button) */
      gtk_entry_set_activates_default (GTK_ENTRY (alias), TRUE);
    }
  else
    {
      alias = gtk_label_new (NULL);
      gtk_label_set_selectable (GTK_LABEL (alias),
          (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_FOR_TOOLTIP) ? FALSE : TRUE);
      gtk_misc_set_alignment (GTK_MISC (alias), 0.0, 0.5);
    }

  g_object_set_data (G_OBJECT (grid), "alias-widget", alias);
  gtk_grid_attach_next_to (grid, alias, label,
                           GTK_POS_RIGHT, 1, 1);
  gtk_widget_show (alias);

  current_row++;

  /* Presence */
  priv->hbox_presence = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

  /* Presence image */
  image = gtk_image_new_from_stock (GTK_STOCK_MISSING_IMAGE,
      GTK_ICON_SIZE_BUTTON);
  g_object_set_data (G_OBJECT (grid), "state-image", image);
  gtk_box_pack_start (GTK_BOX (priv->hbox_presence), image, FALSE,
      FALSE, 0);
  gtk_widget_show (image);

  label = gtk_label_new ("");
  gtk_label_set_line_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_misc_set_alignment (GTK_MISC (label), 0, 0.5);

  gtk_label_set_selectable (GTK_LABEL (label),
      (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_FOR_TOOLTIP) ? FALSE : TRUE);

  g_object_set_data (G_OBJECT (grid), "status-label", label);
  gtk_box_pack_start (GTK_BOX (priv->hbox_presence), label, FALSE,
      FALSE, 0);
  gtk_widget_show (label);

  gtk_grid_attach (grid, priv->hbox_presence,
                   0, current_row, 2, 1);
  gtk_widget_show (priv->hbox_presence);

  current_row++;

  /* Set up favourite toggle button */
  if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_EDIT_FAVOURITE)
    {
      GtkWidget *favourite = gtk_check_button_new_with_label (_("Favorite"));

      g_signal_connect (favourite, "toggled",
          (GCallback) favourite_toggled_cb, self);

      g_object_set_data (G_OBJECT (grid), "favourite-widget", favourite);
      gtk_grid_attach (grid, favourite,
                       0, current_row, 2, 1);
      gtk_widget_show (favourite);

      current_row++;
    }

  /* Set up avatar display */
  avatar = empathy_avatar_image_new ();

  if (!(priv->flags & EMPATHY_INDIVIDUAL_WIDGET_FOR_TOOLTIP))
    {
      g_signal_connect (avatar, "popup-menu",
          (GCallback) avatar_widget_popup_menu_cb, self);
      g_signal_connect (avatar, "button-press-event",
          (GCallback) avatar_widget_button_press_event_cb, self);
    }

  g_object_set_data (G_OBJECT (grid), "avatar-widget", avatar);
  g_object_set (avatar,
                "valign", GTK_ALIGN_START,
                "margin-left", 6,
                "margin-right", 6,
                "margin-top", 6,
                "margin-bottom", 6,
                NULL);

  gtk_grid_attach (grid, avatar,
                   2, 0, 1, current_row);
  gtk_widget_show (avatar);
}

static void
update_persona (EmpathyIndividualWidget *self, FolksPersona *persona)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  TpContact *tp_contact;
  EmpathyContact *contact;
  TpAccount *account;
  GtkGrid *grid;
  GtkLabel *label;
  GtkImage *image;
  const gchar *id;

  grid = g_hash_table_lookup (priv->persona_grids, persona);

  g_assert (grid != NULL);

  tp_contact = tpf_persona_get_contact (TPF_PERSONA (persona));
  if (tp_contact == NULL)
    return;

  contact = empathy_contact_dup_from_tp_contact (tp_contact);
  empathy_contact_set_persona (contact, persona);

  account = empathy_contact_get_account (contact);

  /* Update account widget */
  if (account != NULL)
    {
      const gchar *name;

      label = g_object_get_data (G_OBJECT (grid), "account-label");
      image = g_object_get_data (G_OBJECT (grid), "account-image");

      name = tp_account_get_display_name (account);
      gtk_label_set_label (label, name);

      name = tp_account_get_icon_name (account);
      gtk_image_set_from_icon_name (image, name, GTK_ICON_SIZE_MENU);
    }

  /* Update id widget */
  label = g_object_get_data (G_OBJECT (grid), "id-widget");
  id = folks_persona_get_display_id (persona);
  gtk_label_set_label (label, (id != NULL) ? id : "");

  /* Update other widgets */
  notify_alias_cb (persona, NULL, self);
  notify_presence_cb (persona, NULL, self);
  notify_avatar_cb (persona, NULL, self);

  if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_EDIT_FAVOURITE)
    notify_is_favourite_cb (persona, NULL, self);

  g_object_unref (contact);
}

static void
add_persona (EmpathyIndividualWidget *self,
    FolksPersona *persona)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  GtkBox *hbox;
  GtkGrid *grid;
  GtkWidget *label, *account_label, *account_image, *separator;
  guint current_row = 0;

  if (!empathy_folks_persona_is_interesting (persona))
    return;

  if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_EDIT_FAVOURITE)
    grid = GTK_GRID (gtk_grid_new ());
  else
    grid = GTK_GRID (gtk_grid_new ());

  gtk_orientable_set_orientation (GTK_ORIENTABLE (grid), GTK_ORIENTATION_VERTICAL);
  gtk_grid_set_row_spacing (grid, 6);
  gtk_grid_set_column_spacing (grid, 6);

  /* Account and Identifier */
  label = gtk_label_new (_("Account:"));
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
  gtk_grid_attach (grid, label, 0, current_row, 1, 1);
  gtk_widget_show (label);

  /* Pack the protocol icon with the account name in an hbox */
  hbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));

  account_label = gtk_label_new (NULL);
  gtk_label_set_selectable (GTK_LABEL (account_label),
      (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_FOR_TOOLTIP) ? FALSE : TRUE);
  gtk_misc_set_alignment (GTK_MISC (account_label), 0.0, 0.5);
  gtk_widget_show (account_label);

  account_image = gtk_image_new ();
  gtk_widget_show (account_image);

  gtk_box_pack_start (hbox, account_image, FALSE, FALSE, 0);
  gtk_box_pack_start (hbox, account_label, FALSE, TRUE, 0);

  g_object_set_data (G_OBJECT (grid), "account-image", account_image);
  g_object_set_data (G_OBJECT (grid), "account-label", account_label);
  gtk_grid_attach_next_to (grid, GTK_WIDGET (hbox), label, GTK_POS_RIGHT, 1, 1);
  gtk_widget_show (GTK_WIDGET (hbox));

  current_row++;

  /* Translators: Identifier to connect to Instant Messaging network */
  label = gtk_label_new (_("Identifier:"));
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
  gtk_grid_attach (grid, label, 0, current_row, 1, 1);
  gtk_widget_show (label);

  /* Set up ID label */
  label = gtk_label_new (NULL);
  gtk_label_set_selectable (GTK_LABEL (label),
      (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_FOR_TOOLTIP) ? FALSE : TRUE);
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);

  g_object_set_data (G_OBJECT (grid), "id-widget", label);
  gtk_grid_attach (grid, label, 1, current_row, 1, 1);
  gtk_widget_show (label);

  current_row++;

  alias_presence_avatar_favourite_set_up (self, grid, current_row);

  /* Connect to signals and display the grid */
  g_signal_connect (persona, "notify::alias",
      (GCallback) notify_alias_cb, self);
  g_signal_connect (persona, "notify::avatar",
      (GCallback) notify_avatar_cb, self);
  g_signal_connect (persona, "notify::presence-type",
      (GCallback) notify_presence_cb, self);
  g_signal_connect (persona, "notify::presence-message",
      (GCallback) notify_presence_cb, self);

  if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_EDIT_FAVOURITE)
    {
      g_signal_connect (persona, "notify::is-favourite",
          (GCallback) notify_is_favourite_cb, self);
    }

  gtk_box_pack_start (GTK_BOX (priv->vbox_individual),
      GTK_WIDGET (grid), FALSE, TRUE, 0);
  gtk_widget_show (GTK_WIDGET (grid));

  /* Pack a separator after the grid */
  separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  g_object_set_data (G_OBJECT (grid), "separator", separator);
  gtk_box_pack_start (GTK_BOX (priv->vbox_individual), separator, FALSE, FALSE,
      0);
  gtk_widget_show (separator);

  g_hash_table_replace (priv->persona_grids, persona, grid);

  /* Update the new widgets */
  update_persona (self, persona);
}

static void
remove_persona (EmpathyIndividualWidget *self,
    FolksPersona *persona)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  GtkWidget *separator;
  GtkGrid *grid;

  if (!empathy_folks_persona_is_interesting (persona))
    return;

  grid = g_hash_table_lookup (priv->persona_grids, persona);
  if (grid == NULL)
    return;

  g_signal_handlers_disconnect_by_func (persona, notify_alias_cb, self);
  g_signal_handlers_disconnect_by_func (persona, notify_avatar_cb, self);
  g_signal_handlers_disconnect_by_func (persona, notify_presence_cb, self);

  if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_EDIT_FAVOURITE)
    {
      g_signal_handlers_disconnect_by_func (persona, notify_is_favourite_cb,
          self);
    }

  /* Remove the separator */
  separator = g_object_get_data (G_OBJECT (grid), "separator");
  if (separator != NULL)
    gtk_container_remove (GTK_CONTAINER (priv->vbox_individual), separator);

  /* Remove the widget */
  gtk_container_remove (GTK_CONTAINER (priv->vbox_individual),
      GTK_WIDGET (grid));

  g_hash_table_remove (priv->persona_grids, persona);
}

static void
update_individual_grid (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);

  notify_alias_cb (priv->individual, NULL, self);
  notify_presence_cb (priv->individual, NULL, self);
  notify_avatar_cb (priv->individual, NULL, self);

  if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_EDIT_FAVOURITE)
    notify_is_favourite_cb (priv->individual, NULL, self);
}

static void
individual_grid_set_up (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  guint current_row = 0;
  GtkGrid *grid;

  grid = GTK_GRID (gtk_grid_new ());
  gtk_orientable_set_orientation (GTK_ORIENTABLE (grid), GTK_ORIENTATION_VERTICAL);
  gtk_grid_set_row_spacing (grid, 6);
  gtk_grid_set_column_spacing (grid, 6);

  /* We only display the number of personas in tooltips */
  if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_FOR_TOOLTIP)
    {
      gchar *message;
      GtkWidget *label;
      GeeSet *personas;
      GeeIterator *iter;
      guint num_personas = 0;

      /* Meta-contacts message displaying how many Telepathy personas we have */
      personas = folks_individual_get_personas (priv->individual);
      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (gee_iterator_next (iter))
        {
          FolksPersona *persona = gee_iterator_get (iter);
          if (empathy_folks_persona_is_interesting (persona))
            num_personas++;

          g_clear_object (&persona);
        }
      g_clear_object (&iter);

      message = g_strdup_printf (
          /* Translators: the plurality applies to both instances of the word
           * "contact" */
          ngettext ("Linked contact containing %u contact",
              "Linked contacts containing %u contacts", num_personas),
          num_personas);
      label = gtk_label_new (message);
      gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
      g_free (message);

      gtk_grid_attach (grid, label, 0, current_row, 2, 1);
      gtk_widget_show (label);

      current_row++;
    }

  alias_presence_avatar_favourite_set_up (self, grid, current_row);

  /* Display the grid */
  gtk_box_pack_start (GTK_BOX (priv->vbox_individual), GTK_WIDGET (grid),
      FALSE, TRUE, 0);
  gtk_widget_show (GTK_WIDGET (grid));

  priv->individual_grid = grid;

  /* Update the grid */
  update_individual_grid (self);
}

static void
individual_grid_destroy (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);

  if (priv->individual_grid == NULL)
    return;

  gtk_container_remove (GTK_CONTAINER (priv->vbox_individual),
      GTK_WIDGET (priv->individual_grid));

#ifdef HAVE_LIBCHAMPLAIN
  if (priv->map_view_embed != NULL)
    {
      gtk_container_remove (GTK_CONTAINER (priv->viewport_map),
          priv->map_view_embed);

      priv->map_view_embed = NULL;
    }
#endif

  priv->individual_grid = NULL;
}

static void
personas_changed_cb (FolksIndividual *individual,
    GeeSet *added,
    GeeSet *removed,
    EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  GList *l, *children;
  GeeSet *personas;
  GeeIterator *iter;
  gboolean show_personas, was_showing_personas, will_show_personas, is_last;
  guint old_num_personas, new_num_personas = 0;

  personas = folks_individual_get_personas (individual);
  /* we'll re-use this iterator throughout */
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));

  /* Note that old_num_personas is the number of persona gridss we were
   * displaying, not the number of Personas which were in the Individual
   * before. */
  old_num_personas = g_hash_table_size (priv->persona_grids);

  while (gee_iterator_next (iter))
    {
      FolksPersona *persona = gee_iterator_get (iter);
      if (empathy_folks_persona_is_interesting (persona))
        new_num_personas++;

      g_clear_object (&persona);
    }
  g_clear_object (&iter);

  /*
   * What we display for various conditions:
   *  - "Personas": display the alias, avatar, presence account and identifier
   *                for each of the Individual's Personas. (i.e. One grid per
   *                Persona.)
   *  - "Individual": display the alias, avatar and presence for the Individual,
   *                  and a label saying "Meta-contact containing x contacts".
   *                  (i.e. One grid in total.)
   *
   *              | SHOW_PERSONAS | !SHOW_PERSONAS
   * -------------+---------------+---------------
   * > 1 Persona  | Personas      | Individual
   * -------------+---------------+---------------
   * == 1 Persona | Personas      | Personas
   */
  show_personas = (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_SHOW_PERSONAS) != 0;
  was_showing_personas = show_personas || old_num_personas == 1;
  will_show_personas = show_personas || new_num_personas == 1;

  /* If both @added and @removed are NULL, we're being called manually, and we
   * need to set up the gridss for the first time. We do this simply by
   * ensuring was_showing_personas and will_show_personas are different so that
   * the code resets the UI.
   */
  if (added == NULL && removed == NULL)
    was_showing_personas = !will_show_personas;

  if (was_showing_personas && will_show_personas)
    {
      GeeIterator *iter_changed;

      /* Remove outdated Personas */
      iter_changed = gee_iterable_iterator (GEE_ITERABLE (removed));
      while (gee_iterator_next (iter_changed))
        {
          FolksPersona *persona = gee_iterator_get (iter_changed);
          remove_persona (self, persona);
          g_clear_object (&persona);
        }
      g_clear_object (&iter_changed);

      /* Add new Personas */
      iter_changed = gee_iterable_iterator (GEE_ITERABLE (added));
      while (gee_iterator_next (iter_changed))
        {
          FolksPersona *persona = gee_iterator_get (iter_changed);
          add_persona (self, persona);
          g_clear_object (&persona);
        }
      g_clear_object (&iter_changed);
    }
  else if (!was_showing_personas && will_show_personas)
    {
      /* Remove the old Individual grid */
      individual_grid_destroy (self);

      /* Set up all the Persona grids instead */
      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (gee_iterator_next (iter))
        {
          FolksPersona *persona = gee_iterator_get (iter);
          add_persona (self, persona);
          g_clear_object (&persona);
        }
      g_clear_object (&iter);
    }
  else if (was_showing_personas && !will_show_personas)
    {
      /* Remove all Personas */
      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (gee_iterator_next (iter))
        {
          FolksPersona *persona = gee_iterator_get (iter);
          remove_persona (self, persona);
          g_clear_object (&persona);
        }
      g_clear_object (&iter);

      if (removed != NULL)
        {
          GeeIterator *iter_changed;

          iter_changed = gee_iterable_iterator (GEE_ITERABLE (removed));
          while (gee_iterator_next (iter_changed))
            {
              FolksPersona *persona = gee_iterator_get (iter_changed);
              remove_persona (self, persona);
              g_clear_object (&persona);
            }
          g_clear_object (&iter_changed);
        }

      /* Set up the Individual grid instead */
      individual_grid_set_up (self);
    }

  /* Hide the last separator and show the others */
  children = gtk_container_get_children (GTK_CONTAINER (priv->vbox_individual));
  children = g_list_reverse (children);
  is_last = TRUE;

  for (l = children; l != NULL; l = l->next)
    {
      if (GTK_IS_SEPARATOR (l->data))
        {
          gtk_widget_set_visible (GTK_WIDGET (l->data), !is_last);
          is_last = FALSE;
        }
    }

  g_list_free (children);
}

static void
individual_removed_cb (FolksIndividual *individual,
    FolksIndividual *replacement_individual,
    EmpathyIndividualWidget *self)
{
  empathy_individual_widget_set_individual (self, replacement_individual);
}

static void
remove_individual (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);
  if (priv->individual != NULL)
    {
      GeeSet *personas;
      GeeIterator *iter;

      g_signal_handlers_disconnect_by_func (priv->individual,
          notify_alias_cb, self);
      g_signal_handlers_disconnect_by_func (priv->individual,
          notify_presence_cb, self);
      g_signal_handlers_disconnect_by_func (priv->individual,
          notify_avatar_cb, self);
      g_signal_handlers_disconnect_by_func (priv->individual,
          personas_changed_cb, self);
      g_signal_handlers_disconnect_by_func (priv->individual,
          individual_removed_cb, self);

      if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_EDIT_FAVOURITE)
        {
          g_signal_handlers_disconnect_by_func (priv->individual,
              notify_is_favourite_cb, self);
        }

      personas = folks_individual_get_personas (priv->individual);
      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (gee_iterator_next (iter))
        {
          FolksPersona *persona = gee_iterator_get (iter);
          remove_persona (self, persona);
          g_clear_object (&persona);
        }
      g_clear_object (&iter);
      individual_grid_destroy (self);

      if (priv->contact != NULL)
        remove_weak_contact (self);

      tp_clear_object (&priv->individual);
    }

  if (priv->details_cancellable != NULL)
    g_cancellable_cancel (priv->details_cancellable);
}

static void
individual_update (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (self);

  /* Connect and get info from new Individual */
  if (priv->individual != NULL)
    {
      g_signal_connect (priv->individual, "notify::alias",
          (GCallback) notify_alias_cb, self);
      g_signal_connect (priv->individual, "notify::presence-type",
          (GCallback) notify_presence_cb, self);
      g_signal_connect (priv->individual, "notify::presence-message",
          (GCallback) notify_presence_cb, self);
      g_signal_connect (priv->individual, "notify::avatar",
          (GCallback) notify_avatar_cb, self);
      g_signal_connect (priv->individual, "personas-changed",
          (GCallback) personas_changed_cb, self);
      g_signal_connect (priv->individual, "removed",
          (GCallback) individual_removed_cb, self);

      if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_EDIT_FAVOURITE)
        {
          g_signal_connect (priv->individual, "notify::is-favourite",
              (GCallback) notify_is_favourite_cb, self);
        }

      /* Update individual grid */
      personas_changed_cb (priv->individual, NULL, NULL, self);
    }

  if (priv->individual == NULL)
    {
      gtk_widget_hide (priv->vbox_individual);
    }
  else if (priv->individual_grid != NULL)
    {
      /* We only need to update the details for the Individual as a whole */
      update_individual_grid (self);
      gtk_widget_show (priv->vbox_individual);
    }
  else
    {
      /* We need to update the details for every Persona in the Individual */
      GeeSet *personas;
      GeeIterator *iter;

      personas = folks_individual_get_personas (priv->individual);
      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (gee_iterator_next (iter))
        {
          FolksPersona *persona = gee_iterator_get (iter);

          if (empathy_folks_persona_is_interesting (persona))
            update_persona (self, persona);

          g_clear_object (&persona);
        }
      g_clear_object (&iter);

      gtk_widget_show (priv->vbox_individual);
    }
}

static void
empathy_individual_widget_init (EmpathyIndividualWidget *self)
{
  EmpathyIndividualWidgetPriv *priv;
  GtkBuilder *gui;
  gchar *filename;

  priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_INDIVIDUAL_WIDGET, EmpathyIndividualWidgetPriv);
  self->priv = priv;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
      GTK_ORIENTATION_VERTICAL);

  filename = empathy_file_lookup ("empathy-individual-widget.ui",
      "libempathy-gtk");
  gui = tpaw_builder_get_file (filename,
      "scrolled_window_individual", &priv->scrolled_window_individual,
      "viewport_individual", &priv->viewport_individual,
      "vbox_individual_widget", &priv->vbox_individual_widget,
      "vbox_individual", &priv->vbox_individual,
      "vbox_location", &priv->vbox_location,
      "subvbox_location", &priv->subvbox_location,
      "label_location", &priv->label_location,
#ifdef HAVE_LIBCHAMPLAIN
      "viewport_map", &priv->viewport_map,
#endif
      "groups_widget", &priv->groups_widget,
      "vbox_details", &priv->vbox_details,
      "grid_details", &priv->grid_details,
      "hbox_details_requested", &priv->hbox_details_requested,
      "hbox_client_types", &priv->hbox_client_types,
      NULL);
  g_free (filename);

  priv->grid_location = NULL;

  gtk_box_pack_start (GTK_BOX (self), priv->vbox_individual_widget, TRUE, TRUE,
      0);
  gtk_widget_show (priv->vbox_individual_widget);

  priv->persona_grids = g_hash_table_new (NULL, NULL);
  priv->individual_grid = NULL;

  /* Create widgets */
  details_set_up (self);

  g_object_unref (gui);
}

static void
constructed (GObject *object)
{
  GObjectClass *klass = G_OBJECT_CLASS (empathy_individual_widget_parent_class);
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (object);
  GtkScrolledWindow *scrolled_window =
      GTK_SCROLLED_WINDOW (priv->scrolled_window_individual);

  /* Allow scrolling of the list of Personas if we're showing Personas. */
  if (priv->flags & EMPATHY_INDIVIDUAL_WIDGET_SHOW_PERSONAS)
    {
      gtk_scrolled_window_set_shadow_type (scrolled_window, GTK_SHADOW_IN);
      gtk_scrolled_window_set_policy (scrolled_window,
          GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_box_set_child_packing (GTK_BOX (priv->vbox_individual_widget),
          priv->scrolled_window_individual, TRUE, TRUE, 0, GTK_PACK_START);

      gtk_container_set_border_width (GTK_CONTAINER (priv->viewport_individual),
          6);
      gtk_widget_set_size_request (GTK_WIDGET (scrolled_window), -1, 100);
    }
  else
    {
      gtk_scrolled_window_set_shadow_type (scrolled_window, GTK_SHADOW_NONE);
      gtk_scrolled_window_set_policy (scrolled_window,
          GTK_POLICY_NEVER, GTK_POLICY_NEVER);
      gtk_box_set_child_packing (GTK_BOX (priv->vbox_individual_widget),
          priv->scrolled_window_individual, FALSE, TRUE, 0, GTK_PACK_START);

      gtk_container_set_border_width (GTK_CONTAINER (priv->viewport_individual),
          0);
    }

  /* Chain up */
  if (klass->constructed != NULL)
    klass->constructed (object);
}

static void
get_property (GObject *object,
    guint param_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (object);

  switch (param_id)
    {
      case PROP_INDIVIDUAL:
        g_value_set_object (value, priv->individual);
        break;
      case PROP_FLAGS:
        g_value_set_flags (value, priv->flags);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        break;
    }
}

static void
set_property (GObject *object,
    guint param_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (object);

  switch (param_id)
    {
      case PROP_INDIVIDUAL:
        empathy_individual_widget_set_individual (
            EMPATHY_INDIVIDUAL_WIDGET (object), g_value_get_object (value));
        break;
      case PROP_FLAGS:
        priv->flags = g_value_get_flags (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        break;
    }
}

static void
dispose (GObject *object)
{
  remove_individual (EMPATHY_INDIVIDUAL_WIDGET (object));

  G_OBJECT_CLASS (empathy_individual_widget_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
  EmpathyIndividualWidgetPriv *priv = GET_PRIV (object);

  g_hash_table_unref (priv->persona_grids);

  G_OBJECT_CLASS (empathy_individual_widget_parent_class)->finalize (object);
}

static void
empathy_individual_widget_class_init (EmpathyIndividualWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = constructed;
  object_class->get_property = get_property;
  object_class->set_property = set_property;
  object_class->dispose = dispose;
  object_class->finalize = finalize;

  /**
   * EmpathyIndividualWidget:individual:
   *
   * The #FolksIndividual to display in the widget.
   */
  g_object_class_install_property (object_class, PROP_INDIVIDUAL,
      g_param_spec_object ("individual",
          "Individual",
          "The #FolksIndividual to display in the widget.",
          FOLKS_TYPE_INDIVIDUAL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * EmpathyIndividualWidget:flags:
   *
   * A set of flags which affect the widget's behaviour.
   */
  g_object_class_install_property (object_class, PROP_FLAGS,
      g_param_spec_flags ("flags",
          "Flags",
          "A set of flags which affect the widget's behaviour.",
          EMPATHY_TYPE_INDIVIDUAL_WIDGET_FLAGS,
          EMPATHY_INDIVIDUAL_WIDGET_NONE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT_ONLY));

  g_type_class_add_private (object_class, sizeof (EmpathyIndividualWidgetPriv));
}

/**
 * empathy_individual_widget_new:
 * @individual: the #FolksIndividual to display
 * @flags: flags affecting how the widget behaves and what it displays
 *
 * Creates a new #EmpathyIndividualWidget.
 *
 * Return value: a new #EmpathyIndividualWidget
 */
GtkWidget *
empathy_individual_widget_new (FolksIndividual *individual,
    EmpathyIndividualWidgetFlags flags)
{
  g_return_val_if_fail (individual == NULL || FOLKS_IS_INDIVIDUAL (individual),
      NULL);

  return g_object_new (EMPATHY_TYPE_INDIVIDUAL_WIDGET,
      "individual", individual,
      "flags", flags,
      NULL);
}

/**
 * empathy_individual_widget_get_individual:
 * @self: an #EmpathyIndividualWidget
 *
 * Returns the #FolksIndividual being displayed by the widget.
 *
 * Return value: the #FolksIndividual being displayed, or %NULL
 */
FolksIndividual *
empathy_individual_widget_get_individual (EmpathyIndividualWidget *self)
{
  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_WIDGET (self), NULL);

  return GET_PRIV (self)->individual;
}

/**
 * empathy_individual_widget_set_individual:
 * @self: an #EmpathyIndividualWidget
 * @individual: the #FolksIndividual to display, or %NULL
 *
 * Set the #FolksIndividual to be displayed by the widget:
 * #EmpathyIndividualWidget:individual.
 *
 * The @individual may be %NULL in order to display nothing in the widget.
 */
void
empathy_individual_widget_set_individual (EmpathyIndividualWidget *self,
    FolksIndividual *individual)
{
  EmpathyIndividualWidgetPriv *priv;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_WIDGET (self));
  g_return_if_fail (individual == NULL || FOLKS_IS_INDIVIDUAL (individual));

  priv = GET_PRIV (self);

  if (individual == priv->individual)
    return;

  /* Out with the old… */
  remove_individual (self);

  /* …and in with the new. */
  if (individual != NULL)
    g_object_ref (individual);
  priv->individual = individual;

  /* Update information for widgets */
  individual_update (self);
  groups_update (self);
  details_update (self);
  location_update (self);
  client_types_update (self);
}
