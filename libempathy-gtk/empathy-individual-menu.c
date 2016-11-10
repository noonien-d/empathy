/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2008-2010 Collabora Ltd.
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
 *          Travis Reitter <travis.reitter@collabora.co.uk>
 */

#include "config.h"
#include "empathy-individual-menu.h"

#include <glib/gi18n-lib.h>
#include <tp-account-widgets/tpaw-camera-monitor.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-account-selector-dialog.h"
#include "empathy-call-utils.h"
#include "empathy-chatroom-manager.h"
#include "empathy-gtk-enum-types.h"
#include "empathy-images.h"
#include "empathy-individual-dialogs.h"
#include "empathy-individual-dialogs.h"
#include "empathy-individual-edit-dialog.h"
#include "empathy-individual-information-dialog.h"
#include "empathy-individual-manager.h"
#include "empathy-individual-store-channel.h"
#include "empathy-log-window.h"
#include "empathy-request-util.h"
#include "empathy-share-my-desktop.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CONTACT
#include "empathy-debug.h"

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyIndividualMenu)

typedef struct {
  gchar *active_group; /* may be NULL */
  FolksIndividual *individual; /* owned */
  EmpathyIndividualFeatureFlags features;
  EmpathyIndividualStore *store; /* may be NULL */
} EmpathyIndividualMenuPriv;

enum {
  PROP_ACTIVE_GROUP = 1,
  PROP_INDIVIDUAL,
  PROP_FEATURES,
  PROP_STORE,
};

enum {
  MENU_ITEM_ACTIVATED,
  LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (EmpathyIndividualMenu, empathy_individual_menu, GTK_TYPE_MENU);

static GtkWidget * chat_menu_item_new_individual (EmpathyIndividualMenu *self,
    FolksIndividual *individual);
static GtkWidget * chat_menu_item_new_contact (EmpathyIndividualMenu *self,
    EmpathyContact *contact);
static GtkWidget * sms_menu_item_new_individual (EmpathyIndividualMenu *self,
    FolksIndividual *individual);
static GtkWidget * sms_menu_item_new_contact (EmpathyIndividualMenu *self,
    EmpathyContact *contact);
static GtkWidget * audio_call_menu_item_new_contact (
    EmpathyIndividualMenu *self,
    EmpathyContact *contact);
static GtkWidget * video_call_menu_item_new_contact (
    EmpathyIndividualMenu *self,
    EmpathyContact *contact);
static GtkWidget * log_menu_item_new_individual  (FolksIndividual *individual);
static GtkWidget * log_menu_item_new_contact (EmpathyContact *contact);
static GtkWidget * info_menu_item_new_individual (FolksIndividual *individual);
static GtkWidget * edit_menu_item_new_individual (FolksIndividual *individual);
static GtkWidget * invite_menu_item_new (FolksIndividual *individual,
    EmpathyContact *contact);
static GtkWidget * file_transfer_menu_item_new_individual (EmpathyIndividualMenu *self,
    FolksIndividual *individual);
static GtkWidget * file_transfer_menu_item_new_contact (
    EmpathyIndividualMenu *self,
    EmpathyContact *contact);
static GtkWidget * share_my_desktop_menu_item_new_individual (EmpathyIndividualMenu *self,
    FolksIndividual *individual);
static GtkWidget * share_my_desktop_menu_item_new_contact (
    EmpathyIndividualMenu *self,
    EmpathyContact *contact);
static GtkWidget * favourite_menu_item_new_individual (FolksIndividual *individual);
static GtkWidget * add_menu_item_new_individual (EmpathyIndividualMenu *self,
    FolksIndividual *individual);
static GtkWidget * block_menu_item_new_individual (FolksIndividual *individual);
static GtkWidget * remove_menu_item_new_individual (EmpathyIndividualMenu *self);

static void
individual_menu_add_personas (EmpathyIndividualMenu *self,
    GtkMenuShell *menu,
    FolksIndividual *individual,
    EmpathyIndividualFeatureFlags features)
{
  GtkWidget *item;
  GeeSet *personas;
  GeeIterator *iter;
  guint persona_count = 0;

  g_return_if_fail (GTK_IS_MENU (menu));
  g_return_if_fail (FOLKS_IS_INDIVIDUAL (individual));
  g_return_if_fail (empathy_folks_individual_contains_contact (individual));

  personas = folks_individual_get_personas (individual);
  /* we'll re-use this iterator throughout */
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));

  /* Make sure we've got enough valid entries for these menu items to add
   * functionality */
  while (gee_iterator_next (iter))
    {
      FolksPersona *persona = gee_iterator_get (iter);
      if (empathy_folks_persona_is_interesting (persona))
        persona_count++;

      g_clear_object (&persona);
    }

  g_clear_object (&iter);

  /* return early if these entries would add nothing beyond the "quick" items */
  if (persona_count <= 1)
    goto out;

  /* add a separator before the list of personas */
  item = gtk_separator_menu_item_new ();
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  gtk_widget_show (item);

  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (gee_iterator_next (iter))
    {
      GtkWidget *image;
      GtkWidget *contact_item;
      GtkWidget *contact_submenu;
      TpContact *tp_contact;
      EmpathyContact *contact;
      TpfPersona *persona = gee_iterator_get (iter);
      gchar *label;
      FolksPersonaStore *store;
      const gchar *account;
      GtkWidget *action;

      if (!empathy_folks_persona_is_interesting (FOLKS_PERSONA (persona)))
        goto while_finish;

      tp_contact = tpf_persona_get_contact (persona);
      if (tp_contact == NULL)
        goto while_finish;

      contact = empathy_contact_dup_from_tp_contact (tp_contact);

      store = folks_persona_get_store (FOLKS_PERSONA (persona));
      account = folks_persona_store_get_display_name (store);

      /* Translators: this is used in the context menu for a contact. The first
       * parameter is a contact ID (e.g. foo@jabber.org) and the second is one
       * of the user's account IDs (e.g. me@hotmail.com). */
      label = g_strdup_printf (_("%s (%s)"),
          folks_persona_get_display_id (FOLKS_PERSONA (persona)), account);

      contact_item = gtk_image_menu_item_new_with_label (label);
      gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (contact_item),
                                                 TRUE);
      contact_submenu = gtk_menu_new ();
      gtk_menu_item_set_submenu (GTK_MENU_ITEM (contact_item), contact_submenu);
      image = gtk_image_new_from_icon_name (
          empathy_icon_name_for_contact (contact), GTK_ICON_SIZE_MENU);
      gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (contact_item), image);
      gtk_widget_show (image);

      /* Chat */
      if (features & EMPATHY_INDIVIDUAL_FEATURE_CHAT)
        {
          action = chat_menu_item_new_contact (self, contact);
          gtk_menu_shell_append (GTK_MENU_SHELL (contact_submenu), action);
          gtk_widget_show (action);
        }

      /* SMS */
      if (features & EMPATHY_INDIVIDUAL_FEATURE_SMS)
        {
          action = sms_menu_item_new_contact (self, contact);
          gtk_menu_shell_append (GTK_MENU_SHELL (contact_submenu), action);
          gtk_widget_show (action);
        }

      if (features & EMPATHY_INDIVIDUAL_FEATURE_CALL)
        {
          /* Audio Call */
          action = audio_call_menu_item_new_contact (self, contact);
          gtk_menu_shell_append (GTK_MENU_SHELL (contact_submenu), action);
          gtk_widget_show (action);

          /* Video Call */
          action = video_call_menu_item_new_contact (self, contact);
          gtk_menu_shell_append (GTK_MENU_SHELL (contact_submenu), action);
          gtk_widget_show (action);
        }

      /* Log */
      if (features & EMPATHY_INDIVIDUAL_FEATURE_LOG)
        {
          action = log_menu_item_new_contact (contact);
          gtk_menu_shell_append (GTK_MENU_SHELL (contact_submenu), action);
          gtk_widget_show (action);
        }

      /* Invite */
      action = invite_menu_item_new (NULL, contact);
      gtk_menu_shell_append (GTK_MENU_SHELL (contact_submenu), action);
      gtk_widget_show (action);

      /* File transfer */
      if (features & EMPATHY_INDIVIDUAL_FEATURE_FILE_TRANSFER)
        {
          action = file_transfer_menu_item_new_contact (self, contact);
          gtk_menu_shell_append (GTK_MENU_SHELL (contact_submenu), action);
          gtk_widget_show (action);
        }

      /* Share my desktop */
      action = share_my_desktop_menu_item_new_contact (self, contact);
      gtk_menu_shell_append (GTK_MENU_SHELL (contact_submenu), action);
      gtk_widget_show (action);

      gtk_menu_shell_append (GTK_MENU_SHELL (menu), contact_item);
      gtk_widget_show (contact_item);

      g_free (label);
      g_object_unref (contact);

while_finish:
      g_clear_object (&persona);
    }

out:
  g_clear_object (&iter);
}

static void
empathy_individual_menu_init (EmpathyIndividualMenu *self)
{
  EmpathyIndividualMenuPriv *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_INDIVIDUAL_MENU, EmpathyIndividualMenuPriv);

  self->priv = priv;
}

static GList *
find_phone_accounts (void)
{
  TpAccountManager *am;
  GList *accounts, *l;
  GList *found = NULL;

  am = tp_account_manager_dup ();
  g_return_val_if_fail (am != NULL, NULL);

  accounts = tp_account_manager_dup_valid_accounts (am);
  for (l = accounts; l != NULL; l = g_list_next (l))
    {
      TpAccount *account = l->data;

      if (tp_account_get_connection_status (account, NULL) !=
          TP_CONNECTION_STATUS_CONNECTED)
        continue;

      if (!tp_account_associated_with_uri_scheme (account, "tel"))
        continue;

      found = g_list_prepend (found, g_object_ref (account));
    }

  g_list_free_full (accounts, g_object_unref);
  g_object_unref (am);

  return found;
}

static gboolean
has_phone_account (void)
{
  GList *accounts;
  gboolean result;

  accounts = find_phone_accounts ();
  result = (accounts != NULL);

  g_list_free_full (accounts, (GDestroyNotify) g_object_unref);

  return result;
}

static void
call_phone_number (FolksPhoneFieldDetails *details,
    TpAccount *account)
{
  gchar *number;

  number = folks_phone_field_details_get_normalised (details);
  DEBUG ("Try to call %s", number);

  empathy_call_new_with_streams (number,
      account, FALSE, empathy_get_current_action_time ());

  g_free (number);
}

static void
display_call_phone_dialog (FolksPhoneFieldDetails *details,
    GList *accounts)
{
  GtkWidget *dialog;
  gint response;

  dialog = empathy_account_selector_dialog_new (accounts);

  gtk_window_set_title (GTK_WINDOW (dialog),
      _("Select account to use to place the call"));

  gtk_dialog_add_buttons (GTK_DIALOG (dialog),
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      _("Call"), GTK_RESPONSE_OK,
      NULL);

  response = gtk_dialog_run (GTK_DIALOG (dialog));

  if (response == GTK_RESPONSE_OK)
    {
      TpAccount *account;

      account = empathy_account_selector_dialog_dup_selected (
           EMPATHY_ACCOUNT_SELECTOR_DIALOG (dialog));

      if (account != NULL)
        {
          call_phone_number (details, account);

          g_object_unref (account);
        }
    }

  gtk_widget_destroy (dialog);
}

static void
call_phone_number_cb (GtkMenuItem *item,
      FolksPhoneFieldDetails *details)
{
  GList *accounts;

  accounts = find_phone_accounts ();
  if (accounts == NULL)
    {
      DEBUG ("No phone aware account connected; can't call");
    }
  else if (g_list_length (accounts) == 1)
    {
      call_phone_number (details, accounts->data);
    }
  else
    {
      /* Ask which account to use */
      display_call_phone_dialog (details, accounts);
    }

  g_list_free_full (accounts, (GDestroyNotify) g_object_unref);
}

static const gchar *
find_phone_type (FolksPhoneFieldDetails *details)
{
  GeeCollection *types;
  GeeIterator *iter;
  const gchar *retval = NULL;

  types = folks_abstract_field_details_get_parameter_values (
      FOLKS_ABSTRACT_FIELD_DETAILS (details), "type");

  if (types == NULL)
    return NULL;

  iter = gee_iterable_iterator (GEE_ITERABLE (types));
  while (gee_iterator_next (iter))
    {
      gchar *type = gee_iterator_get (iter);

      if (!tp_strdiff (type, "CELL"))
        retval = _("Mobile");
      else if (!tp_strdiff (type, "WORK"))
        retval = _("Work");
      else if (!tp_strdiff (type, "HOME"))
        retval = _("HOME");

      g_free (type);

      if (retval != NULL)
        break;
    }

  g_object_unref (iter);

  return retval;
}

static void
add_phone_numbers (EmpathyIndividualMenu *self)
{
  EmpathyIndividualMenuPriv *priv = GET_PRIV (self);
  GeeSet *all_numbers;
  GeeIterator *iter;
  gboolean sensitive;

  all_numbers = folks_phone_details_get_phone_numbers (
      FOLKS_PHONE_DETAILS (priv->individual));

  sensitive = has_phone_account ();

  iter = gee_iterable_iterator (GEE_ITERABLE (all_numbers));
  while (gee_iterator_next (iter))
    {
      FolksPhoneFieldDetails *details = gee_iterator_get (iter);
      GtkWidget *item, *image;
      gchar *tmp, *number;
      const gchar *type;

      type = find_phone_type (details);
      number = folks_phone_field_details_get_normalised (details);

      if (type != NULL)
        {
          /* translators: first argument is a phone number like +32123456 and
           * the second one is something like 'home' or 'work'. */
          tmp = g_strdup_printf (_("Call %s (%s)"), number, type);
        }
      else
        {
          /* translators: argument is a phone number like +32123456 */
          tmp = g_strdup_printf (_("Call %s"), number);
        }

      g_free (number);

      item = gtk_image_menu_item_new_with_mnemonic (tmp);
      g_free (tmp);

      g_signal_connect_data (item, "activate",
          G_CALLBACK (call_phone_number_cb), g_object_ref (details),
          (GClosureNotify) g_object_unref, 0);

      gtk_widget_set_sensitive (item, sensitive);

      image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_CALL,
          GTK_ICON_SIZE_MENU);
      gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
      gtk_widget_show (image);

      gtk_menu_shell_append (GTK_MENU_SHELL (self), item);
      gtk_widget_show (item);

      g_object_unref (details);
    }

  g_object_unref (iter);
}

/* return a list of TpContact supporting the blocking iface */
static GList *
get_contacts_supporting_blocking (FolksIndividual *individual)
{
  GeeSet *personas;
  GeeIterator *iter;
  GList *result = NULL;

  personas = folks_individual_get_personas (individual);

  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (gee_iterator_next (iter))
    {
      TpfPersona *persona = gee_iterator_get (iter);
      TpContact *contact;
      TpConnection *conn;

      if (!TPF_IS_PERSONA (persona))
        goto while_next;

      contact = tpf_persona_get_contact (persona);
      if (contact == NULL)
        goto while_next;

      conn = tp_contact_get_connection (contact);

      if (tp_proxy_has_interface_by_id (conn,
        TP_IFACE_QUARK_CONNECTION_INTERFACE_CONTACT_BLOCKING))
        result = g_list_prepend (result, contact);

while_next:
      g_clear_object (&persona);
    }

  g_clear_object (&iter);

  return result;
}

typedef struct
{
  gboolean blocked;
  GtkWidget *parent;
} GotAvatarCtx;

static GotAvatarCtx *
got_avatar_ctx_new (gboolean blocked,
    GtkWidget *parent)
{
  GotAvatarCtx *ctx = g_slice_new0 (GotAvatarCtx);

  ctx->blocked = blocked;
  ctx->parent = parent != NULL ? g_object_ref (parent) : NULL;
  return ctx;
}

static void
got_avatar_ctx_free (GotAvatarCtx *ctx)
{
  g_clear_object (&ctx->parent);
  g_slice_free (GotAvatarCtx, ctx);
}

static void
got_avatar (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  FolksIndividual *individual = FOLKS_INDIVIDUAL (source_object);
  GotAvatarCtx *ctx = user_data;
  GdkPixbuf *avatar;
  GError *error = NULL;
  gboolean abusive = FALSE;
  EmpathyIndividualManager *manager;

  avatar = empathy_pixbuf_avatar_from_individual_scaled_finish (individual,
      result, &error);

  if (error != NULL)
    {
      DEBUG ("Could not get avatar: %s", error->message);
      g_error_free (error);
    }

  if (ctx->blocked) {
    /* confirm the user really wishes to block the contact */
    if (!empathy_block_individual_dialog_show (GTK_WINDOW (ctx->parent),
          individual, avatar, &abusive))
      goto out;
  }

  manager = empathy_individual_manager_dup_singleton ();

  empathy_individual_manager_set_blocked (manager, individual,
      ctx->blocked, abusive);

  g_object_unref (manager);

out:
  g_clear_object (&avatar);
  got_avatar_ctx_free (ctx);
}

static void
empathy_individual_block_menu_item_toggled (GtkCheckMenuItem *item,
    FolksIndividual *individual)
{
  GotAvatarCtx *ctx;
  gboolean blocked;
  GtkWidget *parent;

  /* @item may be destroyed while the async call is running to get the things
   * we need from it right now. */
  blocked = gtk_check_menu_item_get_active (item);

  parent = g_object_get_data (
    G_OBJECT (gtk_widget_get_parent (GTK_WIDGET (item))),
    "window");

  ctx = got_avatar_ctx_new (blocked, parent);

  empathy_pixbuf_avatar_from_individual_scaled_async (individual,
      48, 48, NULL, got_avatar, ctx);
}

static void
update_block_menu_item (GtkWidget *item,
    FolksIndividual *individual)
{
  GList *contacts, *l;
  gboolean is_blocked = TRUE;

  contacts = get_contacts_supporting_blocking (individual);

  if (contacts == NULL)
    is_blocked = FALSE;

  /* Check the menu item if all his personas are blocked */
  for (l = contacts; l != NULL; l = g_list_next (l))
    {
      TpContact *contact = l->data;

      if (!tp_contact_is_blocked (contact))
        {
          is_blocked = FALSE;
          break;
        }
    }

  g_signal_handlers_block_by_func (item,
      empathy_individual_block_menu_item_toggled, individual);

  gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), is_blocked);

  g_signal_handlers_unblock_by_func (item,
      empathy_individual_block_menu_item_toggled, individual);

  g_list_free (contacts);
}

static void
contact_blocked_changed_cb (TpContact *contact,
    GParamSpec *spec,
    GtkWidget *item)
{
  FolksIndividual *individual;

  individual = g_object_get_data (G_OBJECT (item), "individual");

  update_block_menu_item (item, individual);
}

static GtkWidget *
block_menu_item_new_individual (FolksIndividual *individual)
{
  GtkWidget *item;
  GList *contacts, *l;

  contacts = get_contacts_supporting_blocking (individual);

  /* Can't block, no persona supports blocking */
  if (contacts == NULL)
    return NULL;

  item = gtk_check_menu_item_new_with_mnemonic (_("_Block Contact"));

  g_object_set_data_full (G_OBJECT (item), "individual",
      g_object_ref (individual), g_object_unref);

  for (l = contacts; l != NULL; l = g_list_next (l))
    {
      TpContact *contact = l->data;

      tp_g_signal_connect_object (contact, "notify::is-blocked",
          G_CALLBACK (contact_blocked_changed_cb), item, 0);
    }

  g_signal_connect (item, "toggled",
      G_CALLBACK (empathy_individual_block_menu_item_toggled), individual);

  update_block_menu_item (item, individual);

  g_list_free (contacts);

  return item;
}

enum
{
  REMOVE_DIALOG_RESPONSE_CANCEL = 0,
  REMOVE_DIALOG_RESPONSE_DELETE,
  REMOVE_DIALOG_RESPONSE_DELETE_AND_BLOCK,
  REMOVE_DIALOG_RESPONSE_REMOVE_FROM_GROUP
};

static int
remove_dialog_show (const gchar *message,
    const gchar *secondary_text,
    gboolean show_remove_from_group,
    gboolean block_button,
    GdkPixbuf *avatar,
    const gchar *active_group)
{
  GtkWidget *dialog;
  gboolean res;

  dialog = gtk_message_dialog_new (NULL, GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", message);

  if (avatar != NULL)
    {
      GtkWidget *image = gtk_image_new_from_pixbuf (avatar);
      gtk_message_dialog_set_image (GTK_MESSAGE_DIALOG (dialog), image);
      gtk_widget_show (image);
    }

  if (show_remove_from_group)
    {
      GtkWidget *button;
      gchar *button_text = g_strdup_printf (_("Remove from _Group “%s”"),
          active_group);

      /* gtk_dialog_add_button() doesn't allow us to pass a string with a
       * mnemonic so we have to create the button manually. */
      button = gtk_button_new_with_mnemonic (button_text);
      g_free (button_text);

      gtk_dialog_add_action_widget (GTK_DIALOG (dialog), button,
          REMOVE_DIALOG_RESPONSE_REMOVE_FROM_GROUP);

      gtk_widget_show (button);
    }

  if (block_button)
    {
      GtkWidget *button;

      /* gtk_dialog_add_button() doesn't allow us to pass a string with a
       * mnemonic so we have to create the button manually. */
      button = gtk_button_new_with_mnemonic (
          _("Delete and _Block"));

      gtk_dialog_add_action_widget (GTK_DIALOG (dialog), button,
          REMOVE_DIALOG_RESPONSE_DELETE_AND_BLOCK);

      gtk_widget_show (button);
    }

  gtk_dialog_add_buttons (GTK_DIALOG (dialog),
      GTK_STOCK_CANCEL, REMOVE_DIALOG_RESPONSE_CANCEL,
      GTK_STOCK_DELETE, REMOVE_DIALOG_RESPONSE_DELETE, NULL);
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
      "%s", secondary_text);

  gtk_widget_show (dialog);

  res = gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);

  return res;
}

static void
individual_removed_from_group_cb (GObject *source_object,
    GAsyncResult *res,
    gpointer user_data)
{
  GError *error = NULL;
  FolksIndividual *individual = FOLKS_INDIVIDUAL (source_object);

  folks_group_details_change_group_finish (
      FOLKS_GROUP_DETAILS (individual), res, &error);
  if (error != NULL)
    {
      DEBUG ("Individual could not be removed from group: %s",
          error->message);
      g_error_free (error);
    }
}

static void
remove_got_avatar (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  FolksIndividual *individual = FOLKS_INDIVIDUAL (source_object);
  EmpathyIndividualMenu *self = EMPATHY_INDIVIDUAL_MENU (user_data);
  EmpathyIndividualMenuPriv *priv = GET_PRIV (self);
  GdkPixbuf *avatar;
  EmpathyIndividualManager *manager;
  gchar *text;
  GeeSet *personas;
  guint persona_count = 0;
  gboolean can_block;
  GError *error = NULL;
  gint res;
  gboolean show_remove_from_group;
  GeeSet *groups;

  avatar = empathy_pixbuf_avatar_from_individual_scaled_finish (individual,
      result, &error);

  if (error != NULL)
    {
      DEBUG ("Could not get avatar: %s", error->message);
      g_error_free (error);
    }

  /* We couldn't retrieve the avatar, but that isn't a fatal error,
   * so we still display the remove dialog. */

  groups = folks_group_details_get_groups (FOLKS_GROUP_DETAILS (individual));
  show_remove_from_group =
      gee_collection_get_size (GEE_COLLECTION (groups)) > 1;

  personas = folks_individual_get_personas (individual);

  persona_count = gee_collection_get_size (GEE_COLLECTION (personas));

  /* If we have more than one TpfPersona, display a different message
   * ensuring the user knows that *all* of the meta-contacts' personas will
   * be removed. */

  if (persona_count < 2)
    {
      /* Not a meta-contact */
      text =
          g_strdup_printf (
              _("Do you really want to remove the contact “%s”?"),
              folks_alias_details_get_alias (
                  FOLKS_ALIAS_DETAILS (individual)));
    }
  else
    {
      /* Meta-contact */
      text =
          g_strdup_printf (
              _("Do you really want to remove the linked contact “%s”? "
                "Note that this will remove all the contacts which make up "
                "this linked contact."),
              folks_alias_details_get_alias (
                  FOLKS_ALIAS_DETAILS (individual)));
    }


  manager = empathy_individual_manager_dup_singleton ();
  can_block = empathy_individual_manager_supports_blocking (manager,
      individual);
  res = remove_dialog_show (_("Removing contact"), text,
      show_remove_from_group, can_block, avatar, priv->active_group);

  if (res == REMOVE_DIALOG_RESPONSE_REMOVE_FROM_GROUP)
    {
      folks_group_details_change_group (FOLKS_GROUP_DETAILS (individual),
          priv->active_group, false, individual_removed_from_group_cb, NULL);
      goto finally;
    }

  if (res == REMOVE_DIALOG_RESPONSE_DELETE ||
      res == REMOVE_DIALOG_RESPONSE_DELETE_AND_BLOCK)
    {
      gboolean abusive;

      if (res == REMOVE_DIALOG_RESPONSE_DELETE_AND_BLOCK)
        {
          if (!empathy_block_individual_dialog_show (NULL, individual,
                avatar, &abusive))
            goto finally;

          empathy_individual_manager_set_blocked (manager, individual,
              TRUE, abusive);
        }

      empathy_individual_manager_remove (manager, individual, "");
    }

 finally:
  g_free (text);
  g_object_unref (manager);
  g_object_unref (self);
}

static void
remove_activate_cb (GtkMenuItem *menuitem,
    EmpathyIndividualMenu *self)
{
  EmpathyIndividualMenuPriv *priv = GET_PRIV (self);

  empathy_pixbuf_avatar_from_individual_scaled_async (priv->individual,
      48, 48, NULL, remove_got_avatar, g_object_ref (self));
}

static GtkWidget *
remove_menu_item_new_individual (EmpathyIndividualMenu *self)
{
  GeeSet *personas;
  GeeIterator *iter;
  gboolean can_remove = FALSE;
  GtkWidget *item, *image;
  EmpathyIndividualMenuPriv *priv = GET_PRIV (self);

  /* If any of the Individual's personas can be removed, add an option to
   * remove. This will act as a best-effort option. If any Personas cannot be
   * removed from the server, then this option will just be inactive upon
   * subsequent menu openings */
  personas = folks_individual_get_personas (priv->individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (!can_remove && gee_iterator_next (iter))
    {
      FolksPersona *persona = gee_iterator_get (iter);
      FolksPersonaStore *store = folks_persona_get_store (persona);
      FolksMaybeBool maybe_can_remove =
          folks_persona_store_get_can_remove_personas (store);

      if (maybe_can_remove == FOLKS_MAYBE_BOOL_TRUE)
        can_remove = TRUE;

      g_clear_object (&persona);
    }
  g_clear_object (&iter);

  if (!can_remove)
    return NULL;

  item = gtk_image_menu_item_new_with_mnemonic (_("_Remove"));
  image = gtk_image_new_from_icon_name (GTK_STOCK_REMOVE,
      GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);

  g_signal_connect (item, "activate",
      G_CALLBACK (remove_activate_cb), self);

  return item;
}

static void
constructed (GObject *object)
{
  EmpathyIndividualMenu *self = EMPATHY_INDIVIDUAL_MENU (object);
  EmpathyIndividualMenuPriv *priv = GET_PRIV (object);
  GtkMenuShell *shell;
  GtkWidget *item;
  FolksIndividual *individual;
  EmpathyIndividualFeatureFlags features;

  /* Build the menu */
  shell = GTK_MENU_SHELL (object);
  individual = priv->individual;
  features = priv->features;

  /* Add contact */
  if (features & EMPATHY_INDIVIDUAL_FEATURE_ADD_CONTACT)
    {
      item = add_menu_item_new_individual (self, individual);
      if (item != NULL)
        {
          gtk_menu_shell_append (GTK_MENU_SHELL (shell), item);
          gtk_widget_show (item);
        }
    }

  /* Chat */
  if (features & EMPATHY_INDIVIDUAL_FEATURE_CHAT)
    {
      item = chat_menu_item_new_individual (self, individual);
      if (item != NULL)
        {
          gtk_menu_shell_append (shell, item);
          gtk_widget_show (item);
        }
    }

  /* SMS */
  if (features & EMPATHY_INDIVIDUAL_FEATURE_SMS)
    {
      item = sms_menu_item_new_individual (self, individual);
      if (item != NULL)
        {
          gtk_menu_shell_append (shell, item);
          gtk_widget_show (item);
        }
    }

  if (features & EMPATHY_INDIVIDUAL_FEATURE_CALL)
    {
      /* Audio Call */
      item = empathy_individual_audio_call_menu_item_new_individual (self,
          individual);
      gtk_menu_shell_append (shell, item);
      gtk_widget_show (item);

      /* Video Call */
      item = empathy_individual_video_call_menu_item_new_individual (self,
          individual);
      gtk_menu_shell_append (shell, item);
      gtk_widget_show (item);
    }

  if (features & EMPATHY_INDIVIDUAL_FEATURE_CALL_PHONE)
    add_phone_numbers (self);

  /* Invite */
  item = invite_menu_item_new (individual, NULL);
  gtk_menu_shell_append (shell, item);
  gtk_widget_show (item);

  /* File transfer */
  if (features & EMPATHY_INDIVIDUAL_FEATURE_FILE_TRANSFER)
    {
      item = file_transfer_menu_item_new_individual (self, individual);
      gtk_menu_shell_append (shell, item);
      gtk_widget_show (item);
    }

  /* Share my desktop */
  /* FIXME we should add the "Share my desktop" menu item if Vino is
  a registered handler in MC5 */
  item = share_my_desktop_menu_item_new_individual (self, individual);
  gtk_menu_shell_append (shell, item);
  gtk_widget_show (item);

  /* Menu items to target specific contacts */
  individual_menu_add_personas (self, GTK_MENU_SHELL (object),
      individual, features);

  /* Separator */
  if (features & (EMPATHY_INDIVIDUAL_FEATURE_EDIT |
      EMPATHY_INDIVIDUAL_FEATURE_INFO |
      EMPATHY_INDIVIDUAL_FEATURE_FAVOURITE))
    {
      item = gtk_separator_menu_item_new ();
      gtk_menu_shell_append (shell, item);
      gtk_widget_show (item);
    }

  /* Edit */
  if (features & EMPATHY_INDIVIDUAL_FEATURE_EDIT)
    {
      item = edit_menu_item_new_individual (individual);
      gtk_menu_shell_append (shell, item);
      gtk_widget_show (item);
    }

  /* Log */
  if (features & EMPATHY_INDIVIDUAL_FEATURE_LOG)
    {
      item = log_menu_item_new_individual (individual);
      gtk_menu_shell_append (shell, item);
      gtk_widget_show (item);
    }

  /* Info */
  if (features & EMPATHY_INDIVIDUAL_FEATURE_INFO)
    {
      item = info_menu_item_new_individual (individual);
      gtk_menu_shell_append (shell, item);
      gtk_widget_show (item);
    }

  /* Favorite checkbox */
  if (features & EMPATHY_INDIVIDUAL_FEATURE_FAVOURITE)
    {
      item = favourite_menu_item_new_individual (individual);
      gtk_menu_shell_append (shell, item);
      gtk_widget_show (item);
    }

  /* Separator & Block */
  if (features & EMPATHY_INDIVIDUAL_FEATURE_BLOCK &&
      (item = block_menu_item_new_individual (individual)) != NULL) {
    GtkWidget *sep;

    sep = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (shell, sep);
    gtk_widget_show (sep);

    gtk_menu_shell_append (shell, item);
    gtk_widget_show (item);
  }

  /* Separator & Remove */
  if (features & EMPATHY_INDIVIDUAL_FEATURE_REMOVE &&
      (item = remove_menu_item_new_individual (self)) != NULL) {
    GtkWidget *sep;

    sep = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (shell, sep);
    gtk_widget_show (sep);

    gtk_menu_shell_append (shell, item);
    gtk_widget_show (item);
  }
}

static void
get_property (GObject *object,
    guint param_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyIndividualMenuPriv *priv;

  priv = GET_PRIV (object);

  switch (param_id)
    {
      case PROP_ACTIVE_GROUP:
        g_value_set_string (value, priv->active_group);
        break;
      case PROP_INDIVIDUAL:
        g_value_set_object (value, priv->individual);
        break;
      case PROP_FEATURES:
        g_value_set_flags (value, priv->features);
        break;
      case PROP_STORE:
        g_value_set_object (value, priv->store);
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
  EmpathyIndividualMenuPriv *priv;

  priv = GET_PRIV (object);

  switch (param_id)
    {
      case PROP_ACTIVE_GROUP:
        g_assert (priv->active_group == NULL); /* construct only */
        priv->active_group = g_value_dup_string (value);
        break;
      case PROP_INDIVIDUAL:
        priv->individual = g_value_dup_object (value);
        break;
      case PROP_FEATURES:
        priv->features = g_value_get_flags (value);
        break;
      case PROP_STORE:
        priv->store = g_value_dup_object (value); /* read only */
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        break;
    }
}

static void
dispose (GObject *object)
{
  EmpathyIndividualMenuPriv *priv = GET_PRIV (object);

  tp_clear_object (&priv->individual);
  tp_clear_object (&priv->store);

  G_OBJECT_CLASS (empathy_individual_menu_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
  EmpathyIndividualMenuPriv *priv = GET_PRIV (object);

  g_free (priv->active_group);

  G_OBJECT_CLASS (empathy_individual_menu_parent_class)->finalize (object);
}

static void
empathy_individual_menu_class_init (EmpathyIndividualMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = constructed;
  object_class->get_property = get_property;
  object_class->set_property = set_property;
  object_class->dispose = dispose;
  object_class->finalize = finalize;

  /**
   * gchar *:active-group:
   *
   * The group the selected roster-contact widget belongs, or NULL.
   */
  g_object_class_install_property (object_class, PROP_ACTIVE_GROUP,
      g_param_spec_string ("active-group",
          "Active group",
          "The group the selected roster-contact widget belongs, or NULL",
          NULL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  /**
   * EmpathyIndividualMenu:individual:
   *
   * The #FolksIndividual the menu is for.
   */
  g_object_class_install_property (object_class, PROP_INDIVIDUAL,
      g_param_spec_object ("individual",
          "Individual",
          "The #FolksIndividual the menu is for.",
          FOLKS_TYPE_INDIVIDUAL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  /**
   * EmpathyIndividualMenu:features:
   *
   * A set of feature flags controlling which entries are shown.
   */
  g_object_class_install_property (object_class, PROP_FEATURES,
      g_param_spec_flags ("features",
          "Features",
          "A set of feature flags controlling which entries are shown.",
          EMPATHY_TYPE_INDIVIDUAL_FEATURE_FLAGS,
          EMPATHY_INDIVIDUAL_FEATURE_NONE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_STORE,
      g_param_spec_object ("store",
          "Store",
          "The EmpathyIndividualStore to use to get contact owner",
          EMPATHY_TYPE_INDIVIDUAL_STORE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  signals[MENU_ITEM_ACTIVATED] =
      g_signal_new ("menu-item-activated",
          G_TYPE_FROM_CLASS (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL, NULL,
          g_cclosure_marshal_generic,
          G_TYPE_NONE,
          0);

  g_type_class_add_private (object_class, sizeof (EmpathyIndividualMenuPriv));
}

GtkWidget *
empathy_individual_menu_new (FolksIndividual *individual,
    const gchar *active_group,
    EmpathyIndividualFeatureFlags features,
    EmpathyIndividualStore *store)
{
  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);
  g_return_val_if_fail (store == NULL ||
      EMPATHY_IS_INDIVIDUAL_STORE (store), NULL);
  g_return_val_if_fail (features != EMPATHY_INDIVIDUAL_FEATURE_NONE, NULL);

  return g_object_new (EMPATHY_TYPE_INDIVIDUAL_MENU,
      "active-group", active_group,
      "individual", individual,
      "features", features,
      "store", store,
      NULL);
}

/* Like menu_item_set_first_contact(), but always operates upon the given
 * contact. If the contact is non-NULL, it is assumed that the menu entry should
 * be sensitive. */
static gboolean
menu_item_set_contact (GtkWidget *item,
    EmpathyContact *contact,
    GCallback activate_callback,
    EmpathyActionType action_type)
{
  gboolean can_do_action = FALSE;

  if (contact != NULL)
    can_do_action = empathy_contact_can_do_action (contact, action_type);
  gtk_widget_set_sensitive (item, can_do_action);

  if (can_do_action == TRUE)
    {
      /* We want to make sure that the EmpathyContact stays alive while the
       * signal is connected. */
      g_signal_connect_data (item, "activate", G_CALLBACK (activate_callback),
          g_object_ref (contact), (GClosureNotify) g_object_unref, 0);
    }

  return can_do_action;
}

/**
 * Set the given menu @item to call @activate_callback using the TpContact
 * (associated with @individual) with the highest availability who is also valid
 * whenever @item is activated.
 *
 * @action_type is the type of action performed by the menu entry; this is used
 * so that only contacts which can perform that action (e.g. are capable of
 * receiving video calls) are selected, as appropriate.
 */
static GtkWidget *
menu_item_set_first_contact (GtkWidget *item,
    FolksIndividual *individual,
    GCallback activate_callback,
    EmpathyActionType action_type)
{
  EmpathyContact *best_contact;

  best_contact = empathy_contact_dup_best_for_action (individual, action_type);
  menu_item_set_contact (item, best_contact, G_CALLBACK (activate_callback),
      action_type);
  tp_clear_object (&best_contact);

  return item;
}

static void
emit_menu_item_activated (GtkMenuItem *item)
{
  EmpathyIndividualMenu *self;

  self = EMPATHY_INDIVIDUAL_MENU (g_object_get_data (G_OBJECT (item),
      "individual-menu"));
  g_signal_emit (self, signals [MENU_ITEM_ACTIVATED], 0);
}

static void
empathy_individual_chat_menu_item_activated (GtkMenuItem *item,
  EmpathyContact *contact)
{
  g_return_if_fail (EMPATHY_IS_CONTACT (contact));

  empathy_chat_with_contact (contact, empathy_get_current_action_time ());

  emit_menu_item_activated (item);
}

static GtkWidget *
chat_menu_item_new (EmpathyIndividualMenu *self)
{
  GtkWidget *item;
  GtkWidget *image;

  item = gtk_image_menu_item_new_with_mnemonic (_("_Chat"));
  image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_MESSAGE,
      GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
  gtk_widget_show (image);

  g_object_set_data (G_OBJECT (item), "individual-menu", self);

  return item;
}

static GtkWidget *
chat_menu_item_new_individual (EmpathyIndividualMenu *self,
    FolksIndividual *individual)
{
  GtkWidget *item;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual) &&
      empathy_folks_individual_contains_contact (individual), NULL);

  item = chat_menu_item_new (self);

  menu_item_set_first_contact (item, individual,
      G_CALLBACK (empathy_individual_chat_menu_item_activated),
      EMPATHY_ACTION_CHAT);

  return item;
}

static GtkWidget *
chat_menu_item_new_contact (EmpathyIndividualMenu *self,
    EmpathyContact *contact)
{
  GtkWidget *item;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  item = chat_menu_item_new (self);

  menu_item_set_contact (item, contact,
      G_CALLBACK (empathy_individual_chat_menu_item_activated),
      EMPATHY_ACTION_CHAT);

  return item;
}

static void
empathy_individual_sms_menu_item_activated (GtkMenuItem *item,
  EmpathyContact *contact)
{
  g_return_if_fail (EMPATHY_IS_CONTACT (contact));

  empathy_sms_contact_id (
      empathy_contact_get_account (contact),
      empathy_contact_get_id (contact),
      empathy_get_current_action_time (),
      NULL, NULL);

  emit_menu_item_activated (item);
}

static GtkWidget *
sms_menu_item_new (EmpathyIndividualMenu *self)
{
  GtkWidget *item;
  GtkWidget *image;

  item = gtk_image_menu_item_new_with_mnemonic (_("_SMS"));
  image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_SMS,
      GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
  gtk_widget_show (image);

  g_object_set_data (G_OBJECT (item), "individual-menu", self);

  return item;
}

static GtkWidget *
sms_menu_item_new_individual (EmpathyIndividualMenu *self,
    FolksIndividual *individual)
{
  GtkWidget *item;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual) &&
      empathy_folks_individual_contains_contact (individual), NULL);

  item = sms_menu_item_new (self);

  menu_item_set_first_contact (item, individual,
      G_CALLBACK (empathy_individual_sms_menu_item_activated),
      EMPATHY_ACTION_SMS);

  return item;
}

static GtkWidget *
sms_menu_item_new_contact (EmpathyIndividualMenu *self,
    EmpathyContact *contact)
{
  GtkWidget *item;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  item = sms_menu_item_new (self);

  menu_item_set_contact (item, contact,
      G_CALLBACK (empathy_individual_sms_menu_item_activated),
      EMPATHY_ACTION_SMS);

  return item;
}


static void
empathy_individual_audio_call_menu_item_activated (GtkMenuItem *item,
  EmpathyContact *contact)
{
  g_return_if_fail (EMPATHY_IS_CONTACT (contact));

  empathy_call_new_with_streams (empathy_contact_get_id (contact),
      empathy_contact_get_account (contact),
      FALSE, empathy_get_current_action_time ());

  emit_menu_item_activated (item);
}

static GtkWidget *
audio_call_menu_item_new (EmpathyIndividualMenu *self)
{
  GtkWidget *item;
  GtkWidget *image;

  item = gtk_image_menu_item_new_with_mnemonic (C_("menu item", "_Audio Call"));
  image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_VOIP, GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
  gtk_widget_show (image);

  g_object_set_data (G_OBJECT (item), "individual-menu", self);

  return item;
}

GtkWidget *
empathy_individual_audio_call_menu_item_new_individual (
    EmpathyIndividualMenu *self,
    FolksIndividual *individual)
{
  GtkWidget *item;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);

  item = audio_call_menu_item_new (self);

  menu_item_set_first_contact (item, individual,
      G_CALLBACK (empathy_individual_audio_call_menu_item_activated),
      EMPATHY_ACTION_AUDIO_CALL);

  return item;
}

static GtkWidget *
audio_call_menu_item_new_contact (
    EmpathyIndividualMenu *self,
    EmpathyContact *contact)
{
  GtkWidget *item;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  item = audio_call_menu_item_new (self);

  menu_item_set_contact (item, contact,
      G_CALLBACK (empathy_individual_audio_call_menu_item_activated),
      EMPATHY_ACTION_AUDIO_CALL);

  return item;
}


static void
empathy_individual_video_call_menu_item_activated (GtkMenuItem *item,
  EmpathyContact *contact)
{
  g_return_if_fail (EMPATHY_IS_CONTACT (contact));

  empathy_call_new_with_streams (empathy_contact_get_id (contact),
      empathy_contact_get_account (contact),
      TRUE, empathy_get_current_action_time ());

  emit_menu_item_activated (item);
}

static GtkWidget *
video_call_menu_item_new (EmpathyIndividualMenu *self)
{
  GtkWidget *item;
  GtkWidget *image;

  item = gtk_image_menu_item_new_with_mnemonic (C_("menu item", "_Video Call"));
  image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_VIDEO_CALL,
      GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
  gtk_widget_show (image);

  g_object_set_data (G_OBJECT (item), "individual-menu", self);

  return item;
}

static void
check_camera_available (GtkWidget *item)
{
  TpawCameraMonitor *monitor;

  /* Only follow available cameras if the contact can do Video calls */
  if (gtk_widget_get_sensitive (item))
    {
      monitor = tpaw_camera_monitor_dup_singleton ();
      g_object_set_data_full (G_OBJECT (item),
          "monitor", monitor, g_object_unref);
      g_object_bind_property (monitor, "available", item, "sensitive",
          G_BINDING_SYNC_CREATE);
    }
}

GtkWidget *
empathy_individual_video_call_menu_item_new_individual (
    EmpathyIndividualMenu *self,
    FolksIndividual *individual)
{
  GtkWidget *item;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);

  item = video_call_menu_item_new (self);

  menu_item_set_first_contact (item, individual,
      G_CALLBACK (empathy_individual_video_call_menu_item_activated),
      EMPATHY_ACTION_VIDEO_CALL);

  check_camera_available (item);

  return item;
}

GtkWidget *
video_call_menu_item_new_contact (EmpathyIndividualMenu *self,
    EmpathyContact *contact)
{
  GtkWidget *item;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  item = video_call_menu_item_new (self);

  menu_item_set_contact (item, contact,
      G_CALLBACK (empathy_individual_video_call_menu_item_activated),
      EMPATHY_ACTION_VIDEO_CALL);

  check_camera_available (item);

  return item;
}

static void
empathy_individual_log_menu_item_activated (GtkMenuItem *item,
  EmpathyContact *contact)
{
  g_return_if_fail (EMPATHY_IS_CONTACT (contact));

  empathy_log_window_show (empathy_contact_get_account (contact),
      empathy_contact_get_id (contact), FALSE, NULL);
}

static GtkWidget *
log_menu_item_new (void)
{
  GtkWidget *item;
  GtkWidget *image;

  item = gtk_image_menu_item_new_with_mnemonic (_("_Previous Conversations"));
  image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_LOG, GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
  gtk_widget_show (image);

  return item;
}

static GtkWidget *
log_menu_item_new_individual (FolksIndividual *individual)
{
  GtkWidget *item;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);

  item = log_menu_item_new ();

  menu_item_set_first_contact (item, individual,
      G_CALLBACK (empathy_individual_log_menu_item_activated),
      EMPATHY_ACTION_VIEW_LOGS);

  return item;
}

static GtkWidget *
log_menu_item_new_contact (EmpathyContact *contact)
{
  GtkWidget *item;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  item = log_menu_item_new ();

  menu_item_set_contact (item, contact,
      G_CALLBACK (empathy_individual_log_menu_item_activated),
      EMPATHY_ACTION_VIEW_LOGS);

  return item;
}

static void
empathy_individual_file_transfer_menu_item_activated (GtkMenuItem *item,
    EmpathyContact *contact)
{
  g_return_if_fail (EMPATHY_IS_CONTACT (contact));

  empathy_send_file_with_file_chooser (contact);

  emit_menu_item_activated (item);
}

static GtkWidget *
file_transfer_menu_item_new (EmpathyIndividualMenu *self)
{
  GtkWidget *item;
  GtkWidget *image;

  item = gtk_image_menu_item_new_with_mnemonic (_("Send File"));
  image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_DOCUMENT_SEND,
      GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
  gtk_widget_show (image);

  g_object_set_data (G_OBJECT (item), "individual-menu", self);

  return item;
}

static GtkWidget *
file_transfer_menu_item_new_individual (EmpathyIndividualMenu *self,
    FolksIndividual *individual)
{
  GtkWidget *item;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);

  item = file_transfer_menu_item_new (self);

  menu_item_set_first_contact (item, individual,
      G_CALLBACK (empathy_individual_file_transfer_menu_item_activated),
      EMPATHY_ACTION_SEND_FILE);

  return item;
}

static GtkWidget *
file_transfer_menu_item_new_contact (EmpathyIndividualMenu *self,
    EmpathyContact *contact)
{
  GtkWidget *item;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  item = file_transfer_menu_item_new (self);

  menu_item_set_contact (item, contact,
      G_CALLBACK (empathy_individual_file_transfer_menu_item_activated),
      EMPATHY_ACTION_SEND_FILE);

  return item;
}

static void
empathy_individual_share_my_desktop_menu_item_activated (GtkMenuItem *item,
    EmpathyContact *contact)
{
  g_return_if_fail (EMPATHY_IS_CONTACT (contact));

  empathy_share_my_desktop_share_with_contact (contact);

  emit_menu_item_activated (item);
}

static GtkWidget *
share_my_desktop_menu_item_new (EmpathyIndividualMenu *self)
{
  GtkWidget *item;
  GtkWidget *image;

  item = gtk_image_menu_item_new_with_mnemonic (_("Share My Desktop"));
  image = gtk_image_new_from_icon_name (GTK_STOCK_NETWORK, GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
  gtk_widget_show (image);

  g_object_set_data (G_OBJECT (item), "individual-menu", self);

  return item;
}

static GtkWidget *
share_my_desktop_menu_item_new_individual (EmpathyIndividualMenu *self,
    FolksIndividual *individual)
{
  GtkWidget *item;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);

  item = share_my_desktop_menu_item_new (self);

  menu_item_set_first_contact (item, individual,
      G_CALLBACK (empathy_individual_share_my_desktop_menu_item_activated),
      EMPATHY_ACTION_SHARE_MY_DESKTOP);

  return item;
}

static GtkWidget *
share_my_desktop_menu_item_new_contact (EmpathyIndividualMenu *self,
    EmpathyContact *contact)
{
  GtkWidget *item;

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  item = share_my_desktop_menu_item_new (self);

  menu_item_set_contact (item, contact,
      G_CALLBACK (empathy_individual_share_my_desktop_menu_item_activated),
      EMPATHY_ACTION_SHARE_MY_DESKTOP);

  return item;
}

static void
favourite_menu_item_toggled_cb (GtkCheckMenuItem *item,
  FolksIndividual *individual)
{
  folks_favourite_details_set_is_favourite (
      FOLKS_FAVOURITE_DETAILS (individual),
      gtk_check_menu_item_get_active (item));
}

static GtkWidget *
favourite_menu_item_new_individual (FolksIndividual *individual)
{
  GtkWidget *item;

  item = gtk_check_menu_item_new_with_label (_("Favorite"));

  gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item),
      folks_favourite_details_get_is_favourite (
          FOLKS_FAVOURITE_DETAILS (individual)));

  g_signal_connect (item, "toggled",
      G_CALLBACK (favourite_menu_item_toggled_cb), individual);

  return item;
}

static void
individual_info_menu_item_activate_cb (GtkMenuItem *item,
    FolksIndividual *individual)
{
  empathy_display_individual_info (individual);
}

static GtkWidget *
info_menu_item_new_individual (FolksIndividual *individual)
{
  GtkWidget *item;
  GtkWidget *image;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);
  g_return_val_if_fail (empathy_folks_individual_contains_contact (individual),
      NULL);

  item = gtk_image_menu_item_new_with_mnemonic (_("Infor_mation"));
  image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_CONTACT_INFORMATION,
                GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
  gtk_widget_show (image);

  g_signal_connect (item, "activate",
          G_CALLBACK (individual_info_menu_item_activate_cb),
          individual);

  return item;
}

static void
individual_edit_menu_item_activate_cb (FolksIndividual *individual)
{
  empathy_individual_edit_dialog_show (individual, NULL);
}

static GtkWidget *
edit_menu_item_new_individual (FolksIndividual *individual)
{
  EmpathyIndividualManager *manager;
  GtkWidget *item;
  GtkWidget *image;
  gboolean enable = FALSE;
  EmpathyContact *contact;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);

  contact = empathy_contact_dup_from_folks_individual (individual);

  g_return_val_if_fail (EMPATHY_IS_CONTACT (contact), NULL);

  if (empathy_individual_manager_initialized ())
    {
      TpConnection *connection;

      manager = empathy_individual_manager_dup_singleton ();
      connection = empathy_contact_get_connection (contact);

      enable = (empathy_connection_can_alias_personas (connection,
						       individual) &&
                empathy_connection_can_group_personas (connection, individual));

      g_object_unref (manager);
    }

  item = gtk_image_menu_item_new_with_mnemonic (
      C_("Edit individual (contextual menu)", "_Edit"));
  image = gtk_image_new_from_icon_name (GTK_STOCK_EDIT, GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
  gtk_widget_show (image);

  gtk_widget_set_sensitive (item, enable);

  g_signal_connect_swapped (item, "activate",
      G_CALLBACK (individual_edit_menu_item_activate_cb), individual);

  g_object_unref (contact);

  return item;
}

typedef struct
{
  FolksIndividual *individual;
  EmpathyContact *contact;
  EmpathyChatroom *chatroom;
} RoomSubMenuData;

static RoomSubMenuData *
room_sub_menu_data_new (FolksIndividual *individual,
    EmpathyContact *contact,
    EmpathyChatroom *chatroom)
{
  RoomSubMenuData *data;

  data = g_slice_new0 (RoomSubMenuData);
  if (individual != NULL)
    data->individual = g_object_ref (individual);
  if (contact != NULL)
    data->contact = g_object_ref (contact);
  data->chatroom = g_object_ref (chatroom);

  return data;
}

static void
room_sub_menu_data_free (RoomSubMenuData *data)
{
  tp_clear_object (&data->individual);
  tp_clear_object (&data->contact);
  g_object_unref (data->chatroom);
  g_slice_free (RoomSubMenuData, data);
}

static void
room_sub_menu_activate_cb (GtkWidget *item,
         RoomSubMenuData *data)
{
  EmpathyTpChat *chat;
  EmpathyChatroomManager *mgr;
  EmpathyContact *contact = NULL;

  chat = empathy_chatroom_get_tp_chat (data->chatroom);
  if (chat == NULL)
    {
      /* channel was invalidated. Ignoring */
      return;
    }

  mgr = empathy_chatroom_manager_dup_singleton (NULL);

  if (data->contact != NULL)
    contact = g_object_ref (data->contact);
  else
    {
      GeeSet *personas;
      GeeIterator *iter;

      /* find the first of this Individual's contacts who can join this room */
      personas = folks_individual_get_personas (data->individual);

      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (gee_iterator_next (iter) && (contact == NULL))
        {
          TpfPersona *persona = gee_iterator_get (iter);
          TpContact *tp_contact;
          GList *rooms;

          if (empathy_folks_persona_is_interesting (FOLKS_PERSONA (persona)))
            {
              tp_contact = tpf_persona_get_contact (persona);
              if (tp_contact != NULL)
                {
                  contact = empathy_contact_dup_from_tp_contact (tp_contact);

                  rooms = empathy_chatroom_manager_get_chatrooms (mgr,
                      empathy_contact_get_account (contact));

                  if (g_list_find (rooms, data->chatroom) == NULL)
                    g_clear_object (&contact);

                  /* if contact != NULL here, we've found our match */

                  g_list_free (rooms);
                }
            }
          g_clear_object (&persona);
        }
      g_clear_object (&iter);
    }

  g_object_unref (mgr);

  if (contact == NULL)
    {
      /* contact disappeared. Ignoring */
      goto out;
    }

  g_return_if_fail (EMPATHY_IS_CONTACT (contact));

  /* send invitation */
  empathy_tp_chat_add (chat, contact, _("Inviting you to this room"));

out:
  g_object_unref (contact);
}

static GtkWidget *
create_room_sub_menu (FolksIndividual *individual,
                      EmpathyContact *contact,
                      EmpathyChatroom *chatroom)
{
  GtkWidget *item;
  RoomSubMenuData *data;

  item = gtk_menu_item_new_with_label (empathy_chatroom_get_name (chatroom));
  data = room_sub_menu_data_new (individual, contact, chatroom);
  g_signal_connect_data (item, "activate",
      G_CALLBACK (room_sub_menu_activate_cb), data,
      (GClosureNotify) room_sub_menu_data_free, 0);

  return item;
}

static GtkWidget *
invite_menu_item_new (FolksIndividual *individual,
    EmpathyContact *contact)
{
  GtkWidget *item;
  GtkWidget *image;
  GtkWidget *room_item;
  EmpathyChatroomManager *mgr;
  GList *rooms = NULL;
  GList *names = NULL;
  GList *l;
  GtkWidget *submenu = NULL;
  /* map of chat room names to their objects; just a utility to remove
   * duplicates and to make construction of the alphabetized list easier */
  GHashTable *name_room_map;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual) ||
      EMPATHY_IS_CONTACT (contact),
      NULL);

  name_room_map = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
      g_object_unref);

  item = gtk_image_menu_item_new_with_mnemonic (_("_Invite to Chat Room"));
  image = gtk_image_new_from_icon_name (EMPATHY_IMAGE_GROUP_MESSAGE,
      GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);

  mgr = empathy_chatroom_manager_dup_singleton (NULL);

  if (contact != NULL)
    {
      rooms = empathy_chatroom_manager_get_chatrooms (mgr,
          empathy_contact_get_account (contact));
    }
  else
    {
      GeeSet *personas;
      GeeIterator *iter;

      /* find the first of this Individual's contacts who can join this room */
      personas = folks_individual_get_personas (individual);
      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (gee_iterator_next (iter))
        {
          TpfPersona *persona = gee_iterator_get (iter);
          GList *rooms_cur;
          TpContact *tp_contact;
          EmpathyContact *contact_cur;

          if (empathy_folks_persona_is_interesting (FOLKS_PERSONA (persona)))
            {
              tp_contact = tpf_persona_get_contact (persona);
              if (tp_contact != NULL)
                {
                  contact_cur = empathy_contact_dup_from_tp_contact (
                      tp_contact);

                  rooms_cur = empathy_chatroom_manager_get_chatrooms (mgr,
                      empathy_contact_get_account (contact_cur));
                  rooms = g_list_concat (rooms, rooms_cur);

                  g_object_unref (contact_cur);
                }
            }
          g_clear_object (&persona);
        }
      g_clear_object (&iter);
    }

  /* alphabetize the rooms */
  for (l = rooms; l != NULL; l = g_list_next (l))
    {
      EmpathyChatroom *chatroom = l->data;
      gboolean existed;

      if (empathy_chatroom_get_tp_chat (chatroom) != NULL)
        {
          const gchar *name;

          name = empathy_chatroom_get_name (chatroom);
          existed = (g_hash_table_lookup (name_room_map, name) != NULL);
          g_hash_table_insert (name_room_map, (gpointer) name,
              g_object_ref (chatroom));

          /* this will take care of duplicates in rooms */
          if (!existed)
            {
              names = g_list_insert_sorted (names, (gpointer) name,
                  (GCompareFunc) g_strcmp0);
            }
        }
    }

  for (l = names; l != NULL; l = g_list_next (l))
    {
      const gchar *name = l->data;
      EmpathyChatroom *chatroom;

      if (G_UNLIKELY (submenu == NULL))
        submenu = gtk_menu_new ();

      chatroom = g_hash_table_lookup (name_room_map, name);
      room_item = create_room_sub_menu (individual, contact, chatroom);
      gtk_menu_shell_append ((GtkMenuShell *) submenu, room_item);
      gtk_widget_show (room_item);
    }

  if (submenu)
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), submenu);
  else
    gtk_widget_set_sensitive (item, FALSE);

  gtk_widget_show (image);

  g_hash_table_unref (name_room_map);
  g_object_unref (mgr);
  g_list_free (names);
  g_list_free (rooms);

  return item;
}

static void
add_menu_item_activated (GtkMenuItem *item,
    TpContact *tp_contact)
{
  GtkWidget *toplevel;
  FolksIndividual *individual;

  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (item));
  if (!gtk_widget_is_toplevel (toplevel) || !GTK_IS_WINDOW (toplevel))
    toplevel = NULL;

  individual = empathy_ensure_individual_from_tp_contact (tp_contact);

  empathy_new_individual_dialog_show_with_individual (GTK_WINDOW (toplevel),
      individual);

  g_object_unref (individual);
}

static GtkWidget *
add_menu_item_new_individual (EmpathyIndividualMenu *self,
    FolksIndividual *individual)
{
  EmpathyIndividualMenuPriv *priv = GET_PRIV (self);
  GtkWidget *item, *image;
  GeeSet *personas;
  GeeIterator *iter;
  TpContact *to_add = NULL;

  /* find the first of this Individual's personas which are not in our contact
   * list. */
  personas = folks_individual_get_personas (individual);
  iter = gee_iterable_iterator (GEE_ITERABLE (personas));
  while (gee_iterator_next (iter))
    {
      TpfPersona *persona = gee_iterator_get (iter);
      TpContact *contact;
      TpConnection *conn;

      if (!TPF_IS_PERSONA (persona))
        goto next;

      contact = tpf_persona_get_contact (persona);
      if (contact == NULL)
        goto next;

      /* be sure to use a not channel specific contact.
       * TODO: Ideally tp-glib should do this for us (fdo #42702)*/
      if (EMPATHY_IS_INDIVIDUAL_STORE_CHANNEL (priv->store))
        {
          TpChannel *channel;
          TpChannelGroupFlags flags;

          channel = empathy_individual_store_channel_get_channel (
              EMPATHY_INDIVIDUAL_STORE_CHANNEL (priv->store));

          flags = tp_channel_group_get_flags (channel);
          if ((flags & TP_CHANNEL_GROUP_FLAG_CHANNEL_SPECIFIC_HANDLES) != 0)
            {
              /* Channel uses channel specific handles (thanks XMPP...) */
              contact = tp_channel_group_get_contact_owner (channel, contact);

              /* If we don't know the owner, we can't add the contact */
              if (contact == NULL)
                goto next;
            }
        }

      conn = tp_contact_get_connection (contact);
      if (conn == NULL)
        goto next;

      /* No point to try adding a contact if the CM doesn't support it */
      if (!tp_connection_get_can_change_contact_list (conn))
        goto next;

      /* Can't add ourself */
      if (tp_connection_get_self_contact (conn) == contact)
        goto next;

      if (tp_contact_get_subscribe_state (contact) == TP_SUBSCRIPTION_STATE_YES)
        goto next;

      g_object_unref (persona);
      to_add = contact;
      break;

next:
      g_object_unref (persona);
    }

  g_object_unref (iter);

  if (to_add == NULL)
    return NULL;

  item = gtk_image_menu_item_new_with_mnemonic (_("_Add Contact…"));
  image = gtk_image_new_from_icon_name (GTK_STOCK_ADD, GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);

  g_signal_connect_data (item, "activate",
      G_CALLBACK (add_menu_item_activated),
      g_object_ref (to_add), (GClosureNotify) g_object_unref, 0);

  return item;
}
