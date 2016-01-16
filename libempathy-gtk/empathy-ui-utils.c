/*
 * Copyright (C) 2002-2007 Imendio AB
 * Copyright (C) 2007-2010 Collabora Ltd.
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
 *          Xavier Claessens <xclaesse@gmail.com>
 *          Jonny Lamb <jonny.lamb@collabora.co.uk>
 *          Travis Reitter <travis.reitter@collabora.co.uk>
 *
 *          Part of this file is copied from GtkSourceView (gtksourceiter.c):
 *          Paolo Maggi
 *          Jeroen Zwartepoorte
 */

#include "config.h"
#include "empathy-ui-utils.h"

#include <X11/Xatom.h>
#include <gdk/gdkx.h>
#include <glib/gi18n-lib.h>
#include <gio/gdesktopappinfo.h>
#include <tp-account-widgets/tpaw-live-search.h>
#include <tp-account-widgets/tpaw-pixbuf-utils.h>
#include <tp-account-widgets/tpaw-utils.h>

#include "empathy-ft-factory.h"
#include "empathy-images.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

void
empathy_gtk_init (void)
{
  static gboolean initialized = FALSE;

  if (initialized)
    return;

  empathy_init ();

  gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
      PKGDATADIR G_DIR_SEPARATOR_S "icons");

  /* Add icons from source dir if available */
  if (g_getenv ("EMPATHY_SRCDIR") != NULL)
    {
      gchar *path;

      path = g_build_filename (g_getenv ("EMPATHY_SRCDIR"), "data",
          "icons", "local-copy", NULL);

      if (g_file_test (path, G_FILE_TEST_EXISTS))
        gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (), path);

      g_free (path);
    }

  initialized = TRUE;
}

const gchar *
empathy_icon_name_for_presence (TpConnectionPresenceType presence)
{
  switch (presence)
    {
      case TP_CONNECTION_PRESENCE_TYPE_AVAILABLE:
        return EMPATHY_IMAGE_AVAILABLE;
      case TP_CONNECTION_PRESENCE_TYPE_BUSY:
        return EMPATHY_IMAGE_BUSY;
      case TP_CONNECTION_PRESENCE_TYPE_AWAY:
        return EMPATHY_IMAGE_AWAY;
      case TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY:
        if (gtk_icon_theme_has_icon (gtk_icon_theme_get_default (),
                   EMPATHY_IMAGE_EXT_AWAY))
          return EMPATHY_IMAGE_EXT_AWAY;

        /* The 'extended-away' icon is not an official one so we fallback to
         * idle if it's not implemented */
        return EMPATHY_IMAGE_IDLE;
      case TP_CONNECTION_PRESENCE_TYPE_HIDDEN:
        if (gtk_icon_theme_has_icon (gtk_icon_theme_get_default (),
                   EMPATHY_IMAGE_HIDDEN))
          return EMPATHY_IMAGE_HIDDEN;

        /* The 'hidden' icon is not an official one so we fallback to offline if
         * it's not implemented */
        return EMPATHY_IMAGE_OFFLINE;
      case TP_CONNECTION_PRESENCE_TYPE_OFFLINE:
      case TP_CONNECTION_PRESENCE_TYPE_ERROR:
        return EMPATHY_IMAGE_OFFLINE;
      case TP_CONNECTION_PRESENCE_TYPE_UNKNOWN:
        return EMPATHY_IMAGE_PENDING;
      case TP_CONNECTION_PRESENCE_TYPE_UNSET:
      default:
        return NULL;
    }

  return NULL;
}

const gchar *
empathy_icon_name_for_contact (EmpathyContact *contact)
{
  TpConnectionPresenceType presence;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact),
      EMPATHY_IMAGE_OFFLINE);

  presence = empathy_contact_get_presence (contact);
  return empathy_icon_name_for_presence (presence);
}

const gchar *
empathy_icon_name_for_individual (FolksIndividual *individual)
{
  FolksPresenceType folks_presence;
  TpConnectionPresenceType presence;

  folks_presence = folks_presence_details_get_presence_type (
      FOLKS_PRESENCE_DETAILS (individual));
  presence = empathy_folks_presence_type_to_tp (folks_presence);

  return empathy_icon_name_for_presence (presence);
}

const gchar *
empathy_protocol_name_for_contact (EmpathyContact *contact)
{
  TpAccount *account;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  account = empathy_contact_get_account (contact);
  if (account == NULL)
    return NULL;

  return tp_account_get_icon_name (account);
}

struct SizeData
{
  gint width;
  gint height;
  gboolean preserve_aspect_ratio;
};

static void
pixbuf_from_avatar_size_prepared_cb (GdkPixbufLoader *loader,
    int width,
    int height,
    struct SizeData *data)
{
  g_return_if_fail (width > 0 && height > 0);

  if (data->preserve_aspect_ratio && (data->width > 0 || data->height > 0))
    {
      if (data->width < 0)
        {
          width = width * (double) data->height / (gdouble) height;
          height = data->height;
        }
      else if (data->height < 0)
        {
          height = height * (double) data->width / (double) width;
          width = data->width;
        }
      else if ((double) height * (double) data->width >
           (double) width * (double) data->height)
        {
          width = 0.5 + (double) width * (double) data->height / (double) height;
          height = data->height;
        }
      else
        {
          height = 0.5 + (double) height * (double) data->width / (double) width;
          width = data->width;
        }
    }
  else
    {
      if (data->width > 0)
        width = data->width;

      if (data->height > 0)
        height = data->height;
    }

  gdk_pixbuf_loader_set_size (loader, width, height);
}

static void
empathy_avatar_pixbuf_roundify (GdkPixbuf *pixbuf)
{
  gint width, height, rowstride;
  guchar *pixels;

  width = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);
  rowstride = gdk_pixbuf_get_rowstride (pixbuf);
  pixels = gdk_pixbuf_get_pixels (pixbuf);

  if (width < 6 || height < 6)
    return;

  /* Top left */
  pixels[3] = 0;
  pixels[7] = 0x80;
  pixels[11] = 0xC0;
  pixels[rowstride + 3] = 0x80;
  pixels[rowstride * 2 + 3] = 0xC0;

  /* Top right */
  pixels[width * 4 - 1] = 0;
  pixels[width * 4 - 5] = 0x80;
  pixels[width * 4 - 9] = 0xC0;
  pixels[rowstride + (width * 4) - 1] = 0x80;
  pixels[(2 * rowstride) + (width * 4) - 1] = 0xC0;

  /* Bottom left */
  pixels[(height - 1) * rowstride + 3] = 0;
  pixels[(height - 1) * rowstride + 7] = 0x80;
  pixels[(height - 1) * rowstride + 11] = 0xC0;
  pixels[(height - 2) * rowstride + 3] = 0x80;
  pixels[(height - 3) * rowstride + 3] = 0xC0;

  /* Bottom right */
  pixels[height * rowstride - 1] = 0;
  pixels[(height - 1) * rowstride - 1] = 0x80;
  pixels[(height - 2) * rowstride - 1] = 0xC0;
  pixels[height * rowstride - 5] = 0x80;
  pixels[height * rowstride - 9] = 0xC0;
}

static gboolean
empathy_gdk_pixbuf_is_opaque (GdkPixbuf *pixbuf)
{
  gint height, rowstride, i;
  guchar *pixels;
  guchar *row;

  height = gdk_pixbuf_get_height (pixbuf);
  rowstride = gdk_pixbuf_get_rowstride (pixbuf);
  pixels = gdk_pixbuf_get_pixels (pixbuf);

  row = pixels;
  for (i = 3; i < rowstride; i+=4)
    if (row[i] < 0xfe)
      return FALSE;

  for (i = 1; i < height - 1; i++)
    {
      row = pixels + (i*rowstride);
      if (row[3] < 0xfe || row[rowstride-1] < 0xfe)
        return FALSE;
    }

  row = pixels + ((height-1) * rowstride);
  for (i = 3; i < rowstride; i+=4)
    if (row[i] < 0xfe)
      return FALSE;

  return TRUE;
}

static GdkPixbuf *
pixbuf_round_corners (GdkPixbuf *pixbuf)
{
  GdkPixbuf *result;

  if (!gdk_pixbuf_get_has_alpha (pixbuf))
    {
      result = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8,
          gdk_pixbuf_get_width (pixbuf),
          gdk_pixbuf_get_height (pixbuf));

      gdk_pixbuf_copy_area (pixbuf, 0, 0,
          gdk_pixbuf_get_width (pixbuf),
          gdk_pixbuf_get_height (pixbuf),
          result,
          0, 0);
    }
  else
    {
      result = g_object_ref (pixbuf);
    }

  if (empathy_gdk_pixbuf_is_opaque (result))
    empathy_avatar_pixbuf_roundify (result);

  return result;
}

static GdkPixbuf *
avatar_pixbuf_from_loader (GdkPixbufLoader *loader)
{
  GdkPixbuf *pixbuf;

  pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);

  return pixbuf_round_corners (pixbuf);
}

static GdkPixbuf *
empathy_pixbuf_from_avatar_scaled (EmpathyAvatar *avatar,
    gint width,
    gint height)
{
  GdkPixbuf *pixbuf;
  GdkPixbufLoader *loader;
  struct SizeData data;
  GError *error = NULL;

  if (!avatar)
    return NULL;

  data.width = width;
  data.height = height;
  data.preserve_aspect_ratio = TRUE;

  loader = gdk_pixbuf_loader_new ();

  g_signal_connect (loader, "size-prepared",
      G_CALLBACK (pixbuf_from_avatar_size_prepared_cb), &data);

  if (avatar->len == 0)
    {
      g_warning ("Avatar has 0 length");
      return NULL;
    }
  else if (!gdk_pixbuf_loader_write (loader, avatar->data, avatar->len, &error))
    {
      g_warning ("Couldn't write avatar image:%p with "
          "length:%" G_GSIZE_FORMAT " to pixbuf loader: %s",
          avatar->data, avatar->len, error->message);

      g_error_free (error);
      return NULL;
    }

  gdk_pixbuf_loader_close (loader, NULL);
  pixbuf = avatar_pixbuf_from_loader (loader);

  g_object_unref (loader);

  return pixbuf;
}

GdkPixbuf *
empathy_pixbuf_avatar_from_contact_scaled (EmpathyContact *contact,
    gint width,
    gint height)
{
  EmpathyAvatar *avatar;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  avatar = empathy_contact_get_avatar (contact);

  return empathy_pixbuf_from_avatar_scaled (avatar, width, height);
}

typedef struct
{
  GSimpleAsyncResult *result;
  guint width;
  guint height;
  GCancellable *cancellable;
} PixbufAvatarFromIndividualClosure;

static PixbufAvatarFromIndividualClosure *
pixbuf_avatar_from_individual_closure_new (FolksIndividual *individual,
    GSimpleAsyncResult *result,
    gint width,
    gint height,
    GCancellable *cancellable)
{
  PixbufAvatarFromIndividualClosure *closure;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), NULL);

  closure = g_slice_new0 (PixbufAvatarFromIndividualClosure);
  closure->result = g_object_ref (result);
  closure->width = width;
  closure->height = height;

  if (cancellable != NULL)
    closure->cancellable = g_object_ref (cancellable);

  return closure;
}

static void
pixbuf_avatar_from_individual_closure_free (
    PixbufAvatarFromIndividualClosure *closure)
{
  g_clear_object (&closure->cancellable);
  g_object_unref (closure->result);
  g_slice_free (PixbufAvatarFromIndividualClosure, closure);
}

/**
 * @pixbuf: (transfer all)
 *
 * Return: (transfer all)
 */
static GdkPixbuf *
transform_pixbuf (GdkPixbuf *pixbuf)
{
  GdkPixbuf *result;

  result = pixbuf_round_corners (pixbuf);
  g_object_unref (pixbuf);

  return result;
}

static void
avatar_icon_load_cb (GObject *object,
    GAsyncResult *result,
    gpointer user_data)
{
  GLoadableIcon *icon = G_LOADABLE_ICON (object);
  PixbufAvatarFromIndividualClosure *closure = user_data;
  GInputStream *stream;
  GError *error = NULL;
  GdkPixbuf *pixbuf;
  GdkPixbuf *final_pixbuf;

  stream = g_loadable_icon_load_finish (icon, result, NULL, &error);
  if (error != NULL)
    {
      DEBUG ("Failed to open avatar stream: %s", error->message);
      g_simple_async_result_set_from_error (closure->result, error);
      goto out;
    }

  pixbuf = gdk_pixbuf_new_from_stream_at_scale (stream,
      closure->width, closure->height, TRUE, closure->cancellable, &error);

  g_object_unref (stream);

  if (pixbuf == NULL)
    {
      DEBUG ("Failed to read avatar: %s", error->message);
      g_simple_async_result_set_from_error (closure->result, error);
      goto out;
    }

  final_pixbuf = transform_pixbuf (pixbuf);

  /* Pass ownership of final_pixbuf to the result */
  g_simple_async_result_set_op_res_gpointer (closure->result,
      final_pixbuf, g_object_unref);

out:
  g_simple_async_result_complete (closure->result);

  g_clear_error (&error);
  pixbuf_avatar_from_individual_closure_free (closure);
}

void
empathy_pixbuf_avatar_from_individual_scaled_async (
    FolksIndividual *individual,
    gint width,
    gint height,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GLoadableIcon *avatar_icon;
  GSimpleAsyncResult *result;
  PixbufAvatarFromIndividualClosure *closure;

  result = g_simple_async_result_new (G_OBJECT (individual),
      callback, user_data, empathy_pixbuf_avatar_from_individual_scaled_async);

  avatar_icon = folks_avatar_details_get_avatar (
      FOLKS_AVATAR_DETAILS (individual));

  if (avatar_icon == NULL)
    {
      g_simple_async_result_set_error (result, G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND, "no avatar found");

      g_simple_async_result_complete (result);
      g_object_unref (result);
      return;
    }

  closure = pixbuf_avatar_from_individual_closure_new (individual, result,
      width, height, cancellable);

  g_return_if_fail (closure != NULL);

  g_loadable_icon_load_async (avatar_icon, width, cancellable,
      avatar_icon_load_cb, closure);

  g_object_unref (result);
}

/* Return a ref on the GdkPixbuf */
GdkPixbuf *
empathy_pixbuf_avatar_from_individual_scaled_finish (
    FolksIndividual *individual,
    GAsyncResult *result,
    GError **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  gboolean result_valid;
  GdkPixbuf *pixbuf;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);
  g_return_val_if_fail (G_IS_SIMPLE_ASYNC_RESULT (simple), NULL);

  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;

  result_valid = g_simple_async_result_is_valid (result,
      G_OBJECT (individual),
      empathy_pixbuf_avatar_from_individual_scaled_async);

  g_return_val_if_fail (result_valid, NULL);

  pixbuf = g_simple_async_result_get_op_res_gpointer (simple);
  return pixbuf != NULL ? g_object_ref (pixbuf) : NULL;
}

GdkPixbuf *
empathy_pixbuf_contact_status_icon (EmpathyContact *contact,
    gboolean show_protocol)
{
  const gchar *icon_name;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  icon_name = empathy_icon_name_for_contact (contact);

  if (icon_name == NULL)
    return NULL;

  return empathy_pixbuf_contact_status_icon_with_icon_name (contact,
      icon_name, show_protocol);
}

static GdkPixbuf * empathy_pixbuf_protocol_from_contact_scaled (
    EmpathyContact *contact,
    gint width,
    gint height);

GdkPixbuf *
empathy_pixbuf_contact_status_icon_with_icon_name (EmpathyContact *contact,
    const gchar *icon_name,
    gboolean show_protocol)
{
  GdkPixbuf *pix_status;
  GdkPixbuf *pix_protocol;
  gchar *icon_filename;
  gint height, width;
  gint numerator, denominator;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact) ||
      (show_protocol == FALSE), NULL);
  g_return_val_if_fail (icon_name != NULL, NULL);

  numerator = 3;
  denominator = 4;

  icon_filename = tpaw_filename_from_icon_name (icon_name,
      GTK_ICON_SIZE_MENU);

  if (icon_filename == NULL)
    {
      DEBUG ("icon name: %s could not be found\n", icon_name);
      return NULL;
    }

  pix_status = gdk_pixbuf_new_from_file (icon_filename, NULL);

  if (pix_status == NULL)
    {
      DEBUG ("Could not open icon %s\n", icon_filename);
      g_free (icon_filename);
      return NULL;
    }

  g_free (icon_filename);

  if (!show_protocol)
    return pix_status;

  height = gdk_pixbuf_get_height (pix_status);
  width = gdk_pixbuf_get_width (pix_status);

  pix_protocol = empathy_pixbuf_protocol_from_contact_scaled (contact,
      width * numerator / denominator,
      height * numerator / denominator);

  if (pix_protocol == NULL)
    return pix_status;

  gdk_pixbuf_composite (pix_protocol, pix_status,
      0, height - height * numerator / denominator,
      width * numerator / denominator, height * numerator / denominator,
      0, height - height * numerator / denominator,
      1, 1,
      GDK_INTERP_BILINEAR, 255);

  g_object_unref (pix_protocol);

  return pix_status;
}

static GdkPixbuf *
empathy_pixbuf_protocol_from_contact_scaled (EmpathyContact *contact,
    gint width,
    gint height)
{
  TpAccount *account;
  gchar *filename;
  GdkPixbuf *pixbuf = NULL;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  account = empathy_contact_get_account (contact);
  filename = tpaw_filename_from_icon_name (
      tp_account_get_icon_name (account), GTK_ICON_SIZE_MENU);

  if (filename != NULL)
    {
      pixbuf = gdk_pixbuf_new_from_file_at_size (filename, width, height, NULL);
      g_free (filename);
    }

  return pixbuf;
}

void
empathy_url_show (GtkWidget *parent,
      const char *url)
{
  gchar  *real_url;
  GError *error = NULL;

  g_return_if_fail (parent == NULL || GTK_IS_WIDGET (parent));
  g_return_if_fail (url != NULL);

  real_url = tpaw_make_absolute_url (url);

  gtk_show_uri (parent ? gtk_widget_get_screen (parent) : NULL, real_url,
      gtk_get_current_event_time (), &error);

  if (error)
    {
      GtkWidget *dialog;

      dialog = gtk_message_dialog_new (NULL, 0,
          GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
          _("Unable to open URI"));

      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
          "%s", error->message);

      g_signal_connect (dialog, "response",
            G_CALLBACK (gtk_widget_destroy), NULL);

      gtk_window_present (GTK_WINDOW (dialog));

      g_clear_error (&error);
    }

  g_free (real_url);
}

void
empathy_send_file (EmpathyContact *contact,
    GFile *file)
{
  EmpathyFTFactory *factory;
  GtkRecentManager *manager;
  gchar *uri;

  g_return_if_fail (EMPATHY_IS_CONTACT (contact));
  g_return_if_fail (G_IS_FILE (file));

  factory = empathy_ft_factory_dup_singleton ();

  empathy_ft_factory_new_transfer_outgoing (factory, contact, file,
      empathy_get_current_action_time ());

  uri = g_file_get_uri (file);
  manager = gtk_recent_manager_get_default ();
  gtk_recent_manager_add_item (manager, uri);
  g_free (uri);

  g_object_unref (factory);
}

void
empathy_send_file_from_uri_list (EmpathyContact *contact,
    const gchar *uri_list)
{
  const gchar *nl;
  GFile *file;

  /* Only handle a single file for now. It would be wicked cool to be
     able to do multiple files, offering to zip them or whatever like
     nautilus-sendto does. Note that text/uri-list is defined to have
     each line terminated by \r\n, but we can be tolerant of applications
     that only use \n or don't terminate single-line entries.
  */
  nl = strstr (uri_list, "\r\n");
  if (!nl)
    nl = strchr (uri_list, '\n');

  if (nl)
    {
      gchar *uri = g_strndup (uri_list, nl - uri_list);
      file = g_file_new_for_uri (uri);
      g_free (uri);
    }
  else
    {
      file = g_file_new_for_uri (uri_list);
    }

  empathy_send_file (contact, file);

  g_object_unref (file);
}

static void
file_manager_send_file_response_cb (GtkDialog *widget,
    gint response_id,
    EmpathyContact *contact)
{
  GFile *file;

  if (response_id == GTK_RESPONSE_OK)
    {
      file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (widget));

      empathy_send_file (contact, file);

      g_object_unref (file);
    }

  g_object_unref (contact);
  gtk_widget_destroy (GTK_WIDGET (widget));
}

static gboolean
filter_cb (const GtkFileFilterInfo *filter_info,
    gpointer data)
{
  /* filter out socket files */
  return tp_strdiff (filter_info->mime_type, "inode/socket");
}

static GtkFileFilter *
create_file_filter (void)
{
  GtkFileFilter *filter;

  filter = gtk_file_filter_new ();

  gtk_file_filter_add_custom (filter, GTK_FILE_FILTER_MIME_TYPE, filter_cb,
    NULL, NULL);

  return filter;
}

void
empathy_send_file_with_file_chooser (EmpathyContact *contact)
{
  GtkWidget *widget;
  GtkWidget *button;

  g_return_if_fail (EMPATHY_IS_CONTACT (contact));

  DEBUG ("Creating selection file chooser");

  widget = gtk_file_chooser_dialog_new (_("Select a file"), NULL,
      GTK_FILE_CHOOSER_ACTION_OPEN,
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      NULL);

  /* send button */
  button = gtk_button_new_with_mnemonic (_("_Send"));
  gtk_button_set_image (GTK_BUTTON (button),
      gtk_image_new_from_icon_name (EMPATHY_IMAGE_DOCUMENT_SEND,
        GTK_ICON_SIZE_BUTTON));
  gtk_widget_show (button);

  gtk_dialog_add_action_widget (GTK_DIALOG (widget), button,
      GTK_RESPONSE_OK);

  gtk_widget_set_can_default (button, TRUE);
  gtk_dialog_set_default_response (GTK_DIALOG (widget),
      GTK_RESPONSE_OK);

  gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (widget), FALSE);

  gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (widget),
      g_get_home_dir ());

  gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (widget),
      create_file_filter ());

  g_signal_connect (widget, "response",
      G_CALLBACK (file_manager_send_file_response_cb), g_object_ref (contact));

  gtk_widget_show (widget);
}

static void
file_manager_receive_file_response_cb (GtkDialog *dialog,
    GtkResponseType response,
    EmpathyFTHandler *handler)
{
  EmpathyFTFactory *factory;
  GFile *file;

  if (response == GTK_RESPONSE_OK)
    {
      GFile *parent;
      GFileInfo *info;
      guint64 free_space, file_size;
      GError *error = NULL;

      file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));
      parent = g_file_get_parent (file);
      info = g_file_query_filesystem_info (parent,
          G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, &error);

      g_object_unref (parent);

      if (error != NULL)
        {
          g_warning ("Error: %s", error->message);

          g_object_unref (file);
          return;
        }

      free_space = g_file_info_get_attribute_uint64 (info,
          G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
      file_size = empathy_ft_handler_get_total_bytes (handler);

      g_object_unref (info);

      if (file_size > free_space)
        {
          GtkWidget *message = gtk_message_dialog_new (GTK_WINDOW (dialog),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE,
            _("Insufficient free space to save file"));
          char *file_size_str, *free_space_str;

          file_size_str = g_format_size (file_size);
          free_space_str = g_format_size (free_space);

          gtk_message_dialog_format_secondary_text (
              GTK_MESSAGE_DIALOG (message),
              _("%s of free space are required to save this "
                "file, but only %s is available. Please "
                "choose another location."),
            file_size_str, free_space_str);

          gtk_dialog_run (GTK_DIALOG (message));

          g_free (file_size_str);
          g_free (free_space_str);
          gtk_widget_destroy (message);

          g_object_unref (file);

          return;
        }

      factory = empathy_ft_factory_dup_singleton ();

      empathy_ft_factory_set_destination_for_incoming_handler (
          factory, handler, file);

      g_object_unref (factory);
      g_object_unref (file);
    }
  else
    {
      /* unref the handler, as we dismissed the file chooser,
       * and refused the transfer.
       */
      g_object_unref (handler);
    }

  gtk_widget_destroy (GTK_WIDGET (dialog));
}

void
empathy_receive_file_with_file_chooser (EmpathyFTHandler *handler)
{
  GtkWidget *widget;
  const gchar *dir;
  EmpathyContact *contact;
  gchar *title;

  contact = empathy_ft_handler_get_contact (handler);
  g_assert (contact != NULL);

  title = g_strdup_printf (_("Incoming file from %s"),
    empathy_contact_get_alias (contact));

  widget = gtk_file_chooser_dialog_new (title,
      NULL, GTK_FILE_CHOOSER_ACTION_SAVE,
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      GTK_STOCK_SAVE, GTK_RESPONSE_OK,
      NULL);

  gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (widget),
    empathy_ft_handler_get_filename (handler));

  gtk_file_chooser_set_do_overwrite_confirmation
    (GTK_FILE_CHOOSER (widget), TRUE);

  dir = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
  if (dir == NULL)
    /* Fallback to $HOME if $XDG_DOWNLOAD_DIR is not set */
    dir = g_get_home_dir ();

  gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (widget), dir);

  g_signal_connect (widget, "response",
      G_CALLBACK (file_manager_receive_file_response_cb), handler);

  gtk_widget_show (widget);
  g_free (title);
}

void
empathy_make_color_whiter (GdkRGBA *color)
{
  const GdkRGBA white = { 1.0, 1.0, 1.0, 1.0 };

  color->red = (color->red + white.red) / 2;
  color->green = (color->green + white.green) / 2;
  color->blue = (color->blue + white.blue) / 2;
}

static void
menu_deactivate_cb (GtkMenu *menu,
  gpointer user_data)
{
  /* FIXME: we shouldn't have to disconnect the signal (bgo #641327) */
  g_signal_handlers_disconnect_by_func (menu,
         menu_deactivate_cb, user_data);

  gtk_menu_detach (menu);
}

/* Convenient function to create a GtkMenu attached to @attach_to and detach
 * it when the menu is not displayed any more. This is useful when creating a
 * context menu that we want to get rid as soon as it as been displayed. */
GtkWidget *
empathy_context_menu_new (GtkWidget *attach_to)
{
  GtkWidget *menu;

  menu = gtk_menu_new ();

  gtk_menu_attach_to_widget (GTK_MENU (menu), attach_to, NULL);

  /* menu is initially unowned but gtk_menu_attach_to_widget () taked its
   * floating ref. We can either wait that @attach_to releases its ref when
   * it will be destroyed (when leaving Empathy most of the time) or explicitely
   * detach the menu when it's not displayed any more.
   * We go for the latter as we don't want to keep useless menus in memory
   * during the whole lifetime of Empathy. */
  g_signal_connect (menu, "deactivate", G_CALLBACK (menu_deactivate_cb), NULL);

  return menu;
}

gint64
empathy_get_current_action_time (void)
{
  return (tp_user_action_time_from_x11 (gtk_get_current_event_time ()));
}

/* @words = tpaw_live_search_strip_utf8_string (@text);
 *
 * User has to pass both so we don't have to compute @words ourself each time
 * this function is called. */
gboolean
empathy_individual_match_string (FolksIndividual *individual,
    const char *text,
    GPtrArray *words)
{
  const gchar *str;
  GeeSet *personas;
  GeeIterator *iter;
  gboolean retval = FALSE;

  /* check alias name */
  str = folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (individual));

  if (tpaw_live_search_match_words (str, words))
    return TRUE;

  personas = folks_individual_get_personas (individual);

  /* check contact id, remove the @server.com part */
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (retval == FALSE && gee_iterator_next (iter))
    {
      FolksPersona *persona = gee_iterator_get (iter);
      const gchar *p;

      if (empathy_folks_persona_is_interesting (persona))
        {
          str = folks_persona_get_display_id (persona);

          /* Accept the persona if @text is a full prefix of his ID; that allows
           * user to find, say, a jabber contact by typing his JID. */
          if (g_str_has_prefix (str, text))
            {
              retval = TRUE;
            }
          else
            {
              gchar *dup_str = NULL;
              gboolean visible;

              p = strstr (str, "@");
              if (p != NULL)
                str = dup_str = g_strndup (str, p - str);

              visible = tpaw_live_search_match_words (str, words);
              g_free (dup_str);
              if (visible)
                retval = TRUE;
            }
        }
      g_clear_object (&persona);
    }
  g_clear_object (&iter);

  /* FIXME: Add more rules here, we could check phone numbers in
   * contact's vCard for example. */
  return retval;
}

void
empathy_launch_program (const gchar *dir,
    const gchar *name,
    const gchar *args)
{
  GdkDisplay *display;
  GError *error = NULL;
  gchar *path, *cmd;
  GAppInfo *app_info;
  GdkAppLaunchContext *context = NULL;

  /* Try to run from source directory if possible */
  path = g_build_filename (g_getenv ("EMPATHY_SRCDIR"), "src",
      name, NULL);

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_free (path);
      path = g_build_filename (dir, name, NULL);
    }

  if (args != NULL)
    cmd = g_strconcat (path, " ", args, NULL);
  else
    cmd = g_strdup (path);

  app_info = g_app_info_create_from_commandline (cmd, NULL, 0, &error);
  if (app_info == NULL)
    {
      DEBUG ("Failed to create app info: %s", error->message);
      g_error_free (error);
      goto out;
    }

  display = gdk_display_get_default ();
  context = gdk_display_get_app_launch_context (display);

  if (!g_app_info_launch (app_info, NULL, (GAppLaunchContext *) context,
      &error))
    {
      g_warning ("Failed to launch %s: %s", name, error->message);
      g_error_free (error);
      goto out;
    }

out:
  tp_clear_object (&app_info);
  tp_clear_object (&context);
  g_free (path);
  g_free (cmd);
}

#ifdef GDK_WINDOWING_X11

/* Most of the workspace manipulation code has been copied from libwnck
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2005-2007 Vincent Untz
 */
static void
_wnck_activate_workspace (Screen *screen,
    int new_active_space,
    Time timestamp)
{
  Display *display;
  Window root;
  XEvent xev;

  display = DisplayOfScreen (screen);
  root = RootWindowOfScreen (screen);

  xev.xclient.type = ClientMessage;
  xev.xclient.serial = 0;
  xev.xclient.send_event = True;
  xev.xclient.display = display;
  xev.xclient.window = root;
  xev.xclient.message_type = gdk_x11_get_xatom_by_name ("_NET_CURRENT_DESKTOP");
  xev.xclient.format = 32;
  xev.xclient.data.l[0] = new_active_space;
  xev.xclient.data.l[1] = timestamp;
  xev.xclient.data.l[2] = 0;
  xev.xclient.data.l[3] = 0;
  xev.xclient.data.l[4] = 0;

  gdk_error_trap_push ();
  XSendEvent (display, root, False,
      SubstructureRedirectMask | SubstructureNotifyMask,
      &xev);
  XSync (display, False);
  gdk_error_trap_pop_ignored ();
}

static gboolean
_wnck_get_cardinal (Screen *screen,
    Window xwindow,
    Atom atom,
    int *val)
{
  Display *display;
  Atom type;
  int format;
  gulong nitems;
  gulong bytes_after;
  gulong *num;
  int err, result;

  display = DisplayOfScreen (screen);

  *val = 0;

  gdk_error_trap_push ();
  type = None;
  result = XGetWindowProperty (display, xwindow, atom,
      0, G_MAXLONG, False, XA_CARDINAL, &type, &format, &nitems,
      &bytes_after, (void *) &num);
  err = gdk_error_trap_pop ();
  if (err != Success ||
      result != Success)
    return FALSE;

  if (type != XA_CARDINAL)
    {
      XFree (num);
      return FALSE;
    }

  *val = *num;

  XFree (num);

  return TRUE;
}

static int
window_get_workspace (Screen *xscreen,
    Window win)
{
  int number;

  if (!_wnck_get_cardinal (xscreen, win,
        gdk_x11_get_xatom_by_name ("_NET_WM_DESKTOP"), &number))
    return -1;

  return number;
}

#endif

/* Ask X to move to the desktop on which @window currently is
 * and the present @window. */
void
empathy_move_to_window_desktop (GtkWindow *window,
    guint32 timestamp)
{
#ifdef GDK_WINDOWING_X11
  GdkScreen *screen;
  Screen *xscreen;
  GdkWindow *gdk_window;
  int workspace;

  screen = gtk_window_get_screen (window);
  if (!GDK_IS_X11_SCREEN (screen))
    goto out;

  xscreen = gdk_x11_screen_get_xscreen (screen);
  gdk_window = gtk_widget_get_window (GTK_WIDGET (window));

  workspace = window_get_workspace (xscreen,
      gdk_x11_window_get_xid (gdk_window));
  if (workspace == -1)
    goto out;

  _wnck_activate_workspace (xscreen, workspace, timestamp);

out:
  gtk_window_present_with_time (window, timestamp);
#endif
}

void
empathy_set_css_provider (GtkWidget *widget)
{
  GtkCssProvider *provider;
  gchar *filename;
  GError *error = NULL;
  GdkScreen *screen;

  filename = empathy_file_lookup ("empathy.css", "data");

  provider = gtk_css_provider_new ();

  if (!gtk_css_provider_load_from_path (provider, filename, &error))
    {
      g_warning ("Failed to load css file '%s': %s", filename, error->message);
      g_error_free (error);
      goto out;
    }

  if (widget != NULL)
    screen = gtk_widget_get_screen (widget);
  else
    screen = gdk_screen_get_default ();

  gtk_style_context_add_provider_for_screen (screen,
      GTK_STYLE_PROVIDER (provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

out:
  g_free (filename);
  g_object_unref (provider);
}

static gboolean
launch_app_info (GAppInfo *app_info,
    GError **error)
{
  GdkAppLaunchContext *context = NULL;
  GdkDisplay *display;
  GError *err = NULL;

  display = gdk_display_get_default ();
  context = gdk_display_get_app_launch_context (display);

  if (!g_app_info_launch (app_info, NULL, (GAppLaunchContext *) context,
        &err))
    {
      DEBUG ("Failed to launch %s: %s",
          g_app_info_get_display_name (app_info), err->message);
      g_propagate_error (error, err);
      return FALSE;
    }

  tp_clear_object (&context);
  return TRUE;
}

gboolean
empathy_launch_external_app (const gchar *desktop_file,
    const gchar *args,
    GError **error)
{
  GDesktopAppInfo *desktop_info;
  gboolean result;
  GError *err = NULL;

  desktop_info = g_desktop_app_info_new (desktop_file);
  if (desktop_info == NULL)
    {
      DEBUG ("%s not found", desktop_file);

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "%s not found", desktop_file);
      return FALSE;
    }

  if (args == NULL)
    {
      result = launch_app_info (G_APP_INFO (desktop_info), error);
    }
  else
    {
      gchar *cmd;
      GAppInfo *app_info;

      /* glib doesn't have API to start a desktop file with args... (#637875) */
      cmd = g_strdup_printf ("%s %s", g_app_info_get_commandline (
            (GAppInfo *) desktop_info), args);

      app_info = g_app_info_create_from_commandline (cmd, NULL, 0, &err);
      if (app_info == NULL)
        {
          DEBUG ("Failed to launch '%s': %s", cmd, err->message);
          g_free (cmd);
          g_object_unref (desktop_info);
          g_propagate_error (error, err);
          return FALSE;
        }

      result = launch_app_info (app_info, error);

      g_object_unref (app_info);
      g_free (cmd);
    }

  g_object_unref (desktop_info);
  return result;
}
