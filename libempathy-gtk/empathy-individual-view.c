/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2005-2007 Imendio AB
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
 *          Martyn Russell <martyn@imendio.com>
 *          Xavier Claessens <xclaesse@gmail.com>
 *          Travis Reitter <travis.reitter@collabora.co.uk>
 */

#include "config.h"
#include "empathy-individual-view.h"

#include <glib/gi18n-lib.h>
#include <tp-account-widgets/tpaw-pixbuf-utils.h>
#include <tp-account-widgets/tpaw-utils.h>

#include "empathy-cell-renderer-activatable.h"
#include "empathy-cell-renderer-expander.h"
#include "empathy-cell-renderer-text.h"
#include "empathy-connection-aggregator.h"
#include "empathy-contact-groups.h"
#include "empathy-gtk-enum-types.h"
#include "empathy-images.h"
#include "empathy-individual-edit-dialog.h"
#include "empathy-individual-manager.h"
#include "empathy-request-util.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CONTACT
#include "empathy-debug.h"

/* Active users are those which have recently changed state
 * (e.g. online, offline or from normal to a busy state).
 */

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyIndividualView)
typedef struct
{
  EmpathyIndividualStore *store;
  GtkTreeRowReference *drag_row;
  EmpathyIndividualViewFeatureFlags view_features;
  EmpathyIndividualFeatureFlags individual_features;
  GtkWidget *tooltip_widget;

  gboolean show_offline;
  gboolean show_untrusted;
  gboolean show_uninteresting;

  GtkTreeModelFilter *filter;
  GtkWidget *search_widget;

  guint expand_groups_idle_handler;
  /* owned string (group name) -> bool (whether to expand/contract) */
  GHashTable *expand_groups;

  /* Auto scroll */
  guint auto_scroll_timeout_id;
  /* Distance between mouse pointer and the nearby border. Negative when
     scrolling updards.*/
  gint distance;

  GtkTreeModelFilterVisibleFunc custom_filter;
  gpointer custom_filter_data;

  GtkCellRenderer *text_renderer;
} EmpathyIndividualViewPriv;

typedef struct
{
  EmpathyIndividualView *view;
  GtkTreePath *path;
  guint timeout_id;
} DragMotionData;

typedef struct
{
  EmpathyIndividualView *view;
  FolksIndividual *individual;
  gboolean remove;
} ShowActiveData;

enum
{
  PROP_0,
  PROP_STORE,
  PROP_VIEW_FEATURES,
  PROP_INDIVIDUAL_FEATURES,
  PROP_SHOW_OFFLINE,
  PROP_SHOW_UNTRUSTED,
  PROP_SHOW_UNINTERESTING,
};

/* TODO: re-add DRAG_TYPE_CONTACT_ID, for the case that we're dragging around
 * specific EmpathyContacts (between/in/out of Individuals) */
typedef enum
{
  DND_DRAG_TYPE_UNKNOWN = -1,
  DND_DRAG_TYPE_INDIVIDUAL_ID = 0,
  DND_DRAG_TYPE_PERSONA_ID,
  DND_DRAG_TYPE_URI_LIST,
  DND_DRAG_TYPE_STRING,
} DndDragType;

#define DRAG_TYPE(T,I) \
  { (gchar *) T, 0, I }

static const GtkTargetEntry drag_types_dest[] = {
  DRAG_TYPE ("text/x-individual-id", DND_DRAG_TYPE_INDIVIDUAL_ID),
  DRAG_TYPE ("text/x-persona-id", DND_DRAG_TYPE_PERSONA_ID),
  DRAG_TYPE ("text/path-list", DND_DRAG_TYPE_URI_LIST),
  DRAG_TYPE ("text/uri-list", DND_DRAG_TYPE_URI_LIST),
  DRAG_TYPE ("text/plain", DND_DRAG_TYPE_STRING),
  DRAG_TYPE ("STRING", DND_DRAG_TYPE_STRING),
};

static const GtkTargetEntry drag_types_source[] = {
  DRAG_TYPE ("text/x-individual-id", DND_DRAG_TYPE_INDIVIDUAL_ID),
};

#undef DRAG_TYPE

static GdkAtom drag_atoms_dest[G_N_ELEMENTS (drag_types_dest)];

enum
{
  DRAG_INDIVIDUAL_RECEIVED,
  DRAG_PERSONA_RECEIVED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (EmpathyIndividualView, empathy_individual_view,
    GTK_TYPE_TREE_VIEW);

static void
individual_view_tooltip_destroy_cb (GtkWidget *widget,
    EmpathyIndividualView *view)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);

  tp_clear_object (&priv->tooltip_widget);
}

static gboolean
individual_view_query_tooltip_cb (EmpathyIndividualView *view,
    gint x,
    gint y,
    gboolean keyboard_mode,
    GtkTooltip *tooltip,
    gpointer user_data)
{
  EmpathyIndividualViewPriv *priv;
  FolksIndividual *individual;
  GtkTreeModel *model;
  GtkTreeIter iter;
  GtkTreePath *path;
  static gint running = 0;
  gboolean ret = FALSE;

  priv = GET_PRIV (view);

  /* Avoid an infinite loop. See GNOME bug #574377 */
  if (running > 0)
    return FALSE;

  running++;

  /* Don't show the tooltip if there's already a popup menu */
  if (gtk_menu_get_for_attach_widget (GTK_WIDGET (view)) != NULL)
    goto OUT;

  if (!gtk_tree_view_get_tooltip_context (GTK_TREE_VIEW (view), &x, &y,
          keyboard_mode, &model, &path, &iter))
    goto OUT;

  gtk_tree_view_set_tooltip_row (GTK_TREE_VIEW (view), tooltip, path);
  gtk_tree_path_free (path);

  gtk_tree_model_get (model, &iter,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual,
      -1);
  if (individual == NULL)
    goto OUT;

  if (priv->tooltip_widget == NULL)
    {
      priv->tooltip_widget = empathy_individual_widget_new (individual,
          EMPATHY_INDIVIDUAL_WIDGET_FOR_TOOLTIP |
          EMPATHY_INDIVIDUAL_WIDGET_SHOW_LOCATION |
          EMPATHY_INDIVIDUAL_WIDGET_SHOW_CLIENT_TYPES);
      gtk_container_set_border_width (GTK_CONTAINER (priv->tooltip_widget), 8);
      g_object_ref (priv->tooltip_widget);

      tp_g_signal_connect_object (priv->tooltip_widget, "destroy",
          G_CALLBACK (individual_view_tooltip_destroy_cb), view, 0);

      gtk_widget_show (priv->tooltip_widget);
    }
  else
    {
      empathy_individual_widget_set_individual (
        EMPATHY_INDIVIDUAL_WIDGET (priv->tooltip_widget), individual);
    }

  gtk_tooltip_set_custom (tooltip, priv->tooltip_widget);
  ret = TRUE;

  g_object_unref (individual);
OUT:
  running--;

  return ret;
}

static void
groups_change_group_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  FolksGroupDetails *group_details = FOLKS_GROUP_DETAILS (source);
  GError *error = NULL;

  folks_group_details_change_group_finish (group_details, result, &error);
  if (error != NULL)
    {
      g_warning ("failed to change group: %s", error->message);
      g_clear_error (&error);
    }
}

static gboolean
group_can_be_modified (const gchar *name,
    gboolean is_fake_group,
    gboolean adding)
{
  /* Real groups can always be modified */
  if (!is_fake_group)
    return TRUE;

  /* The favorite fake group can be modified so users can
   * add/remove favorites using DnD */
  if (!tp_strdiff (name, EMPATHY_INDIVIDUAL_STORE_FAVORITE))
    return TRUE;

  /* We can remove contacts from the 'ungrouped' fake group */
  if (!adding && !tp_strdiff (name, EMPATHY_INDIVIDUAL_STORE_UNGROUPED))
    return TRUE;

  return FALSE;
}

static gboolean
individual_view_individual_drag_received (GtkWidget *self,
    GdkDragContext *context,
    GtkTreeModel *model,
    GtkTreePath *path,
    GtkSelectionData *selection)
{
  EmpathyIndividualViewPriv *priv;
  EmpathyIndividualManager *manager = NULL;
  FolksIndividual *individual;
  GtkTreePath *source_path;
  const gchar *sel_data;
  gchar *new_group = NULL;
  gchar *old_group = NULL;
  gboolean new_group_is_fake, old_group_is_fake = TRUE, retval = FALSE;

  priv = GET_PRIV (self);

  sel_data = (const gchar *) gtk_selection_data_get_data (selection);
  new_group = empathy_individual_store_get_parent_group (model, path,
      NULL, &new_group_is_fake);

  if (!group_can_be_modified (new_group, new_group_is_fake, TRUE))
    goto finished;

  /* Get source group information iff the view has the FEATURE_GROUPS_CHANGE
   * feature. Otherwise, we just add the dropped contact to whichever group
   * they were dropped in, and don't remove them from their old group. This
   * allows for Individual views which shouldn't allow Individuals to have
   * their groups changed, and also for dragging Individuals between Individual
   * views. */
  if ((priv->view_features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_GROUPS_CHANGE) &&
      priv->drag_row != NULL)
    {
      source_path = gtk_tree_row_reference_get_path (priv->drag_row);
      if (source_path)
        {
          old_group =
              empathy_individual_store_get_parent_group (model, source_path,
              NULL, &old_group_is_fake);
          gtk_tree_path_free (source_path);
        }

      if (!group_can_be_modified (old_group, old_group_is_fake, FALSE))
        goto finished;

      if (!tp_strdiff (old_group, new_group))
        goto finished;
    }
  else if (priv->drag_row != NULL)
    {
      /* We don't allow changing Individuals' groups, and this Individual was
       * dragged from another group in *this* Individual view, so we disallow
       * the drop. */
      goto finished;
    }

  /* XXX: for contacts, we used to ensure the account, create the contact
   * factory, and then wait on the contacts. But they should already be
   * created by this point */

  manager = empathy_individual_manager_dup_singleton ();
  individual = empathy_individual_manager_lookup_member (manager, sel_data);

  if (individual == NULL)
    {
      DEBUG ("failed to find drag event individual with ID '%s'", sel_data);
      goto finished;
    }

  /* FIXME: We should probably wait for the cb before calling
   * gtk_drag_finish */

  /* Emit a signal notifying of the drag. We change the Individual's groups in
   * the default signal handler. */
  g_signal_emit (self, signals[DRAG_INDIVIDUAL_RECEIVED], 0,
      gdk_drag_context_get_selected_action (context), individual, new_group,
      old_group);

  retval = TRUE;

finished:
  tp_clear_object (&manager);
  g_free (old_group);
  g_free (new_group);

  return retval;
}

static void
real_drag_individual_received_cb (EmpathyIndividualView *self,
    GdkDragAction action,
    FolksIndividual *individual,
    const gchar *new_group,
    const gchar *old_group)
{
  DEBUG ("individual %s dragged from '%s' to '%s'",
      folks_individual_get_id (individual), old_group, new_group);

  if (!tp_strdiff (new_group, EMPATHY_INDIVIDUAL_STORE_FAVORITE))
    {
      /* Mark contact as favourite */
      folks_favourite_details_set_is_favourite (
          FOLKS_FAVOURITE_DETAILS (individual), TRUE);
      return;
    }

  if (!tp_strdiff (old_group, EMPATHY_INDIVIDUAL_STORE_FAVORITE))
    {
      /* Remove contact as favourite */
      folks_favourite_details_set_is_favourite (
          FOLKS_FAVOURITE_DETAILS (individual), FALSE);

      /* Don't try to remove it */
      old_group = NULL;
    }

  if (new_group != NULL)
    {
      folks_group_details_change_group (FOLKS_GROUP_DETAILS (individual),
          new_group, TRUE, groups_change_group_cb, NULL);
    }

  if (old_group != NULL && action == GDK_ACTION_MOVE)
    {
      folks_group_details_change_group (FOLKS_GROUP_DETAILS (individual),
          old_group, FALSE, groups_change_group_cb, NULL);
    }
}

static gboolean
individual_view_persona_drag_received (GtkWidget *self,
    GdkDragContext *context,
    GtkTreeModel *model,
    GtkTreePath *path,
    GtkSelectionData *selection)
{
  EmpathyIndividualManager *manager = NULL;
  FolksIndividual *individual = NULL;
  FolksPersona *persona = NULL;
  const gchar *persona_uid;
  GList *individuals, *l;
  GeeIterator *iter = NULL;
  gboolean retval = FALSE;

  persona_uid = (const gchar *) gtk_selection_data_get_data (selection);

  /* FIXME: This is slow, but the only way to find the Persona we're having
   * dropped on us. */
  manager = empathy_individual_manager_dup_singleton ();
  individuals = empathy_individual_manager_get_members (manager);

  for (l = individuals; l != NULL; l = l->next)
    {
      GeeSet *personas;

      personas = folks_individual_get_personas (FOLKS_INDIVIDUAL (l->data));
      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (gee_iterator_next (iter))
        {
          FolksPersona *persona_cur = gee_iterator_get (iter);

          if (!tp_strdiff (folks_persona_get_uid (persona), persona_uid))
            {
              /* takes ownership of the ref */
              persona = persona_cur;
              individual = g_object_ref (l->data);
              goto got_persona;
            }
          g_clear_object (&persona_cur);
        }
      g_clear_object (&iter);
    }

got_persona:
  g_clear_object (&iter);
  g_list_free (individuals);

  if (persona == NULL || individual == NULL)
    {
      DEBUG ("Failed to find drag event persona with UID '%s'", persona_uid);
    }
  else
    {
      /* Emit a signal notifying of the drag. We change the Individual's groups in
       * the default signal handler. */
      g_signal_emit (self, signals[DRAG_PERSONA_RECEIVED], 0,
          gdk_drag_context_get_selected_action (context), persona, individual,
          &retval);
    }

  tp_clear_object (&manager);
  tp_clear_object (&persona);
  tp_clear_object (&individual);

  return retval;
}

static gboolean
individual_view_file_drag_received (GtkWidget *view,
    GdkDragContext *context,
    GtkTreeModel *model,
    GtkTreePath *path,
    GtkSelectionData *selection)
{
  GtkTreeIter iter;
  const gchar *sel_data;
  FolksIndividual *individual;
  EmpathyContact *contact;

  sel_data = (const gchar *) gtk_selection_data_get_data (selection);

  gtk_tree_model_get_iter (model, &iter, path);
  gtk_tree_model_get (model, &iter,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual, -1);
  if (individual == NULL)
    return FALSE;

  contact = empathy_contact_dup_from_folks_individual (individual);
  empathy_send_file_from_uri_list (contact, sel_data);

  g_object_unref (individual);
  tp_clear_object (&contact);

  return TRUE;
}

static void
individual_view_drag_data_received (GtkWidget *view,
    GdkDragContext *context,
    gint x,
    gint y,
    GtkSelectionData *selection,
    guint info,
    guint time_)
{
  GtkTreeModel *model;
  gboolean is_row;
  GtkTreeViewDropPosition position;
  GtkTreePath *path;
  gboolean success = TRUE;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (view));

  /* Get destination group information. */
  is_row = gtk_tree_view_get_dest_row_at_pos (GTK_TREE_VIEW (view),
      x, y, &path, &position);
  if (!is_row)
    {
      success = FALSE;
    }
  else if (info == DND_DRAG_TYPE_INDIVIDUAL_ID)
    {
      success = individual_view_individual_drag_received (view,
          context, model, path, selection);
    }
  else if (info == DND_DRAG_TYPE_PERSONA_ID)
    {
      success = individual_view_persona_drag_received (view, context, model,
          path, selection);
    }
  else if (info == DND_DRAG_TYPE_URI_LIST || info == DND_DRAG_TYPE_STRING)
    {
      success = individual_view_file_drag_received (view,
          context, model, path, selection);
    }

  gtk_tree_path_free (path);
  gtk_drag_finish (context, success, FALSE, GDK_CURRENT_TIME);
}

static gboolean
individual_view_drag_motion_cb (DragMotionData *data)
{
  if (data->view != NULL)
    {
      gtk_tree_view_expand_row (GTK_TREE_VIEW (data->view), data->path, FALSE);
      g_object_remove_weak_pointer (G_OBJECT (data->view),
          (gpointer *) &data->view);
    }

  data->timeout_id = 0;

  return FALSE;
}

/* Minimum distance between the mouse pointer and a horizontal border when we
   start auto scrolling. */
#define AUTO_SCROLL_MARGIN_SIZE 20
/* How far to scroll per one tick. */
#define AUTO_SCROLL_PITCH       10

static gboolean
individual_view_auto_scroll_cb (EmpathyIndividualView *self)
{
	EmpathyIndividualViewPriv *priv = GET_PRIV (self);
	GtkAdjustment         *adj;
	gdouble                new_value;

	adj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (self));

	if (priv->distance < 0)
		new_value = gtk_adjustment_get_value (adj) - AUTO_SCROLL_PITCH;
	else
		new_value = gtk_adjustment_get_value (adj) + AUTO_SCROLL_PITCH;

	new_value = CLAMP (new_value, gtk_adjustment_get_lower (adj),
		gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));

	gtk_adjustment_set_value (adj, new_value);

	return TRUE;
}

static gboolean
individual_view_drag_motion (GtkWidget *widget,
    GdkDragContext *context,
    gint x,
    gint y,
    guint time_)
{
  EmpathyIndividualViewPriv *priv;
  GtkTreeModel *model;
  GdkAtom target;
  GtkTreeIter iter;
  static DragMotionData *dm = NULL;
  GtkTreePath *path;
  gboolean is_row;
  gboolean is_different = FALSE;
  gboolean cleanup = TRUE;
  gboolean retval = TRUE;
  GtkAllocation allocation;
  guint i;
  DndDragType drag_type = DND_DRAG_TYPE_UNKNOWN;

  priv = GET_PRIV (EMPATHY_INDIVIDUAL_VIEW (widget));
  model = gtk_tree_view_get_model (GTK_TREE_VIEW (widget));


  if (priv->auto_scroll_timeout_id != 0)
    {
      g_source_remove (priv->auto_scroll_timeout_id);
      priv->auto_scroll_timeout_id = 0;
    }

  gtk_widget_get_allocation (widget, &allocation);

  if (y < AUTO_SCROLL_MARGIN_SIZE ||
      y > (allocation.height - AUTO_SCROLL_MARGIN_SIZE))
    {
      if (y < AUTO_SCROLL_MARGIN_SIZE)
        priv->distance = MIN (-y, -1);
      else
        priv->distance = MAX (allocation.height - y, 1);

      priv->auto_scroll_timeout_id = g_timeout_add (10 * ABS (priv->distance),
          (GSourceFunc) individual_view_auto_scroll_cb, widget);
    }

  is_row = gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (widget),
      x, y, &path, NULL, NULL, NULL);

  cleanup &= (dm == NULL);

  if (is_row)
    {
      cleanup &= (dm && gtk_tree_path_compare (dm->path, path) != 0);
      is_different = ((dm == NULL) || ((dm != NULL)
              && gtk_tree_path_compare (dm->path, path) != 0));
    }
  else
    cleanup &= FALSE;

  if (path == NULL)
    {
      /* Coordinates don't point to an actual row, so make sure the pointer
         and highlighting don't indicate that a drag is possible.
       */
      gdk_drag_status (context, GDK_ACTION_DEFAULT, time_);
      gtk_tree_view_set_drag_dest_row (GTK_TREE_VIEW (widget), NULL, 0);
      return FALSE;
    }
  target = gtk_drag_dest_find_target (widget, context, NULL);
  gtk_tree_model_get_iter (model, &iter, path);

  /* Determine the DndDragType of the data */
  for (i = 0; i < G_N_ELEMENTS (drag_atoms_dest); i++)
    {
      if (target == drag_atoms_dest[i])
        {
          drag_type = drag_types_dest[i].info;
          break;
        }
    }

  if (drag_type == DND_DRAG_TYPE_URI_LIST ||
      drag_type == DND_DRAG_TYPE_STRING)
    {
      /* This is a file drag, and it can only be dropped on contacts,
       * not groups.
       * If we don't have FEATURE_FILE_DROP, disallow the drop completely,
       * even if we have a valid target. */
      FolksIndividual *individual = NULL;
      EmpathyCapabilities caps = EMPATHY_CAPABILITIES_NONE;

      if (priv->view_features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_FILE_DROP)
        {
          gtk_tree_model_get (model, &iter,
              EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual,
              -1);
        }

      if (individual != NULL)
        {
          EmpathyContact *contact = NULL;

          contact = empathy_contact_dup_from_folks_individual (individual);
          if (contact != NULL)
            caps = empathy_contact_get_capabilities (contact);

          tp_clear_object (&contact);
        }

      if (individual != NULL &&
          folks_presence_details_is_online (
              FOLKS_PRESENCE_DETAILS (individual)) &&
          (caps & EMPATHY_CAPABILITIES_FT))
        {
          gdk_drag_status (context, GDK_ACTION_COPY, time_);
          gtk_tree_view_set_drag_dest_row (GTK_TREE_VIEW (widget),
              path, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
        }
      else
        {
          gdk_drag_status (context, 0, time_);
          gtk_tree_view_set_drag_dest_row (GTK_TREE_VIEW (widget), NULL, 0);
          retval = FALSE;
        }

      if (individual != NULL)
        g_object_unref (individual);
    }
  else if ((drag_type == DND_DRAG_TYPE_INDIVIDUAL_ID &&
      (priv->view_features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_GROUPS_CHANGE ||
       priv->drag_row == NULL)) ||
      (drag_type == DND_DRAG_TYPE_PERSONA_ID &&
       priv->view_features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_PERSONA_DROP))
    {
      /* If target != GDK_NONE, then we have a contact (individual or persona)
         drag.  If we're pointing to a group, highlight it.  Otherwise, if the
         contact we're pointing to is in a group, highlight that.  Otherwise,
         set the drag position to before the first row for a drag into
         the "non-group" at the top.
         If it's an Individual:
           We only highlight things if the contact is from a different
           Individual view, or if this Individual view has
           FEATURE_GROUPS_CHANGE. This prevents highlighting in Individual views
           which don't have FEATURE_GROUPS_CHANGE, but do have
           FEATURE_INDIVIDUAL_DRAG and FEATURE_INDIVIDUAL_DROP.
         If it's a Persona:
           We only highlight things if we have FEATURE_PERSONA_DROP.
       */
      GtkTreeIter group_iter;
      gboolean is_group;
      GtkTreePath *group_path;
      gtk_tree_model_get (model, &iter,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group, -1);
      if (is_group)
        {
          group_iter = iter;
        }
      else
        {
          if (gtk_tree_model_iter_parent (model, &group_iter, &iter))
            gtk_tree_model_get (model, &group_iter,
                EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group, -1);
        }
      if (is_group)
        {
          gdk_drag_status (context, GDK_ACTION_MOVE, time_);
          group_path = gtk_tree_model_get_path (model, &group_iter);
          gtk_tree_view_set_drag_dest_row (GTK_TREE_VIEW (widget),
              group_path, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
          gtk_tree_path_free (group_path);
        }
      else
        {
          group_path = gtk_tree_path_new_first ();
          gdk_drag_status (context, GDK_ACTION_MOVE, time_);
          gtk_tree_view_set_drag_dest_row (GTK_TREE_VIEW (widget),
              group_path, GTK_TREE_VIEW_DROP_BEFORE);
        }
    }

  if (!is_different && !cleanup)
    return retval;

  if (dm)
    {
      gtk_tree_path_free (dm->path);
      if (dm->timeout_id)
        {
          g_source_remove (dm->timeout_id);
        }

      g_free (dm);

      dm = NULL;
    }

  if (!gtk_tree_view_row_expanded (GTK_TREE_VIEW (widget), path))
    {
      dm = g_new0 (DragMotionData, 1);

      dm->view = EMPATHY_INDIVIDUAL_VIEW (widget);
      g_object_add_weak_pointer (G_OBJECT (widget), (gpointer *) &dm->view);
      dm->path = gtk_tree_path_copy (path);

      dm->timeout_id = g_timeout_add_seconds (1,
          (GSourceFunc) individual_view_drag_motion_cb, dm);
    }

  return retval;
}

static void
individual_view_drag_begin (GtkWidget *widget,
    GdkDragContext *context)
{
  EmpathyIndividualViewPriv *priv;
  GtkTreeSelection *selection;
  GtkTreeModel *model;
  GtkTreePath *path;
  GtkTreeIter iter;

  priv = GET_PRIV (widget);

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
  if (!gtk_tree_selection_get_selected (selection, &model, &iter))
    return;

  GTK_WIDGET_CLASS (empathy_individual_view_parent_class)->drag_begin (widget,
      context);

  path = gtk_tree_model_get_path (model, &iter);
  priv->drag_row = gtk_tree_row_reference_new (model, path);
  gtk_tree_path_free (path);
}

static void
individual_view_drag_data_get (GtkWidget *widget,
    GdkDragContext *context,
    GtkSelectionData *selection,
    guint info,
    guint time_)
{
  EmpathyIndividualViewPriv *priv;
  GtkTreePath *src_path;
  GtkTreeIter iter;
  GtkTreeModel *model;
  FolksIndividual *individual;
  const gchar *individual_id;

  priv = GET_PRIV (widget);

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (widget));
  if (priv->drag_row == NULL)
    return;

  src_path = gtk_tree_row_reference_get_path (priv->drag_row);
  if (src_path == NULL)
    return;

  if (!gtk_tree_model_get_iter (model, &iter, src_path))
    {
      gtk_tree_path_free (src_path);
      return;
    }

  gtk_tree_path_free (src_path);

  individual =
      empathy_individual_view_dup_selected (EMPATHY_INDIVIDUAL_VIEW (widget));
  if (individual == NULL)
    return;

  individual_id = folks_individual_get_id (individual);

  if (info == DND_DRAG_TYPE_INDIVIDUAL_ID)
    {
      gtk_selection_data_set (selection,
          gdk_atom_intern ("text/x-individual-id", FALSE), 8,
          (guchar *) individual_id, strlen (individual_id) + 1);
    }

  g_object_unref (individual);
}

static void
individual_view_drag_end (GtkWidget *widget,
    GdkDragContext *context)
{
  EmpathyIndividualViewPriv *priv;

  priv = GET_PRIV (widget);

  GTK_WIDGET_CLASS (empathy_individual_view_parent_class)->drag_end (widget,
      context);

  if (priv->drag_row)
    {
      gtk_tree_row_reference_free (priv->drag_row);
      priv->drag_row = NULL;
    }

  if (priv->auto_scroll_timeout_id != 0)
    {
      g_source_remove (priv->auto_scroll_timeout_id);
      priv->auto_scroll_timeout_id = 0;
    }
}

static gboolean
individual_view_drag_drop (GtkWidget *widget,
    GdkDragContext *drag_context,
    gint x,
    gint y,
    guint time_)
{
  return FALSE;
}

typedef struct
{
  EmpathyIndividualView *view;
  guint button;
  guint32 time;
} MenuPopupData;

static void
menu_deactivate_cb (GtkMenuShell *menushell,
    gpointer user_data)
{
  /* FIXME: we shouldn't have to disconnec the signal (bgo #641327) */
  g_signal_handlers_disconnect_by_func (menushell,
      menu_deactivate_cb, user_data);

  gtk_menu_detach (GTK_MENU (menushell));
}

static gboolean
individual_view_popup_menu_idle_cb (gpointer user_data)
{
  MenuPopupData *data = user_data;
  GtkWidget *menu;

  menu = empathy_individual_view_get_individual_menu (data->view);
  if (menu == NULL)
    menu = empathy_individual_view_get_group_menu (data->view);

  if (menu != NULL)
    {
      gtk_menu_attach_to_widget (GTK_MENU (menu), GTK_WIDGET (data->view),
          NULL);
      gtk_widget_show (menu);
      gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL, data->button,
          data->time);

      /* menu is initially unowned but gtk_menu_attach_to_widget() taked its
       * floating ref. We can either wait that the treeview releases its ref
       * when it will be destroyed (when leaving Empathy) or explicitely
       * detach the menu when it's not displayed any more.
       * We go for the latter as we don't want to keep useless menus in memory
       * during the whole lifetime of Empathy. */
      g_signal_connect (menu, "deactivate", G_CALLBACK (menu_deactivate_cb),
          NULL);
    }

  g_slice_free (MenuPopupData, data);

  return FALSE;
}

static gboolean
individual_view_button_press_event_cb (EmpathyIndividualView *view,
    GdkEventButton *event,
    gpointer user_data)
{
  if (event->button == 3)
    {
      MenuPopupData *data;

      data = g_slice_new (MenuPopupData);
      data->view = view;
      data->button = event->button;
      data->time = event->time;
      g_idle_add (individual_view_popup_menu_idle_cb, data);
    }

  return FALSE;
}

static gboolean
individual_view_key_press_event_cb (EmpathyIndividualView *view,
    GdkEventKey *event,
    gpointer user_data)
{
  if (event->keyval == GDK_KEY_Menu)
    {
      MenuPopupData *data;

      data = g_slice_new (MenuPopupData);
      data->view = view;
      data->button = 0;
      data->time = event->time;
      g_idle_add (individual_view_popup_menu_idle_cb, data);
    } else if (event->keyval == GDK_KEY_F2) {
        FolksIndividual *individual;

        g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (view), FALSE);

        individual = empathy_individual_view_dup_selected (view);
        if (individual == NULL)
            return FALSE;

        empathy_individual_edit_dialog_show (individual, NULL);

        g_object_unref (individual);
    }

  return FALSE;
}

static void
individual_view_row_activated (GtkTreeView *view,
    GtkTreePath *path,
    GtkTreeViewColumn *column)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);
  FolksIndividual *individual;
  EmpathyContact *contact;
  GtkTreeModel *model;
  GtkTreeIter iter;

  if (!(priv->individual_features & EMPATHY_INDIVIDUAL_FEATURE_CHAT))
    return;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (view));
  gtk_tree_model_get_iter (model, &iter, path);
  gtk_tree_model_get (model, &iter,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual, -1);

  if (individual == NULL)
    return;

  /* Determine which Persona to chat to, by choosing the most available one. */
  contact = empathy_contact_dup_best_for_action (individual,
      EMPATHY_ACTION_CHAT);

  if (contact != NULL)
    {
      DEBUG ("Starting a chat");

      empathy_chat_with_contact (contact,
          gtk_get_current_event_time ());
    }

  g_object_unref (individual);
  tp_clear_object (&contact);
}

static void
individual_view_call_activated_cb (EmpathyCellRendererActivatable *cell,
    const gchar *path_string,
    EmpathyIndividualView *view)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);
  GtkWidget *menu;
  GtkTreeModel *model;
  GtkTreeIter iter;
  FolksIndividual *individual;
  GdkEventButton *event;
  GtkMenuShell *shell;
  GtkWidget *item;

  if (!(priv->view_features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_INDIVIDUAL_CALL))
    return;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (view));
  if (!gtk_tree_model_get_iter_from_string (model, &iter, path_string))
    return;

  gtk_tree_model_get (model, &iter,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual, -1);
  if (individual == NULL)
    return;

  event = (GdkEventButton *) gtk_get_current_event ();

  menu = empathy_context_menu_new (GTK_WIDGET (view));
  shell = GTK_MENU_SHELL (menu);

  /* audio */
  item = empathy_individual_audio_call_menu_item_new_individual (NULL,
      individual);
  gtk_menu_shell_append (shell, item);
  gtk_widget_show (item);

  /* video */
  item = empathy_individual_video_call_menu_item_new_individual (NULL,
      individual);
  gtk_menu_shell_append (shell, item);
  gtk_widget_show (item);

  gtk_widget_show (menu);
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL,
      event->button, event->time);

  g_object_unref (individual);
}

static void
individual_view_cell_set_background (EmpathyIndividualView *view,
    GtkCellRenderer *cell,
    gboolean is_group,
    gboolean is_active)
{
  if (!is_group && is_active)
    {
      GtkStyleContext *style;
      GdkRGBA color;

      style = gtk_widget_get_style_context (GTK_WIDGET (view));

      gtk_style_context_save (style);
      gtk_style_context_set_state (style, GTK_STATE_FLAG_SELECTED);
      gtk_style_context_get_background_color (style, GTK_STATE_FLAG_SELECTED,
          &color);
      gtk_style_context_restore (style);

      /* Here we take the current theme colour and add it to
       * the colour for white and average the two. This
       * gives a colour which is inline with the theme but
       * slightly whiter.
       */
      empathy_make_color_whiter (&color);

      g_object_set (cell, "cell-background-rgba", &color, NULL);
    }
  else
    g_object_set (cell, "cell-background-rgba", NULL, NULL);
}

static void
individual_view_pixbuf_cell_data_func (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *model,
    GtkTreeIter *iter,
    EmpathyIndividualView *view)
{
  GdkPixbuf *pixbuf;
  gboolean is_group;
  gboolean is_active;

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_ACTIVE, &is_active,
      EMPATHY_INDIVIDUAL_STORE_COL_ICON_STATUS, &pixbuf, -1);

  g_object_set (cell,
      "visible", !is_group,
      "pixbuf", pixbuf,
      NULL);

  tp_clear_object (&pixbuf);

  individual_view_cell_set_background (view, cell, is_group, is_active);
}

static void
individual_view_group_icon_cell_data_func (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *model,
    GtkTreeIter *iter,
    EmpathyIndividualView *view)
{
  GdkPixbuf *pixbuf = NULL;
  gboolean is_group;
  gchar *name;

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name, -1);

  if (!is_group)
    goto out;

  if (!tp_strdiff (name, EMPATHY_INDIVIDUAL_STORE_FAVORITE))
    {
      pixbuf = tpaw_pixbuf_from_icon_name ("emblem-favorite",
          GTK_ICON_SIZE_MENU);
    }
  else if (!tp_strdiff (name, EMPATHY_INDIVIDUAL_STORE_PEOPLE_NEARBY))
    {
      pixbuf = tpaw_pixbuf_from_icon_name ("im-local-xmpp",
          GTK_ICON_SIZE_MENU);
    }

out:
  g_object_set (cell,
      "visible", pixbuf != NULL,
      "pixbuf", pixbuf,
      NULL);

  tp_clear_object (&pixbuf);

  g_free (name);
}

static void
individual_view_audio_call_cell_data_func (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *model,
    GtkTreeIter *iter,
    EmpathyIndividualView *view)
{
  gboolean is_group;
  gboolean is_active;
  gboolean can_audio, can_video;

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_ACTIVE, &is_active,
      EMPATHY_INDIVIDUAL_STORE_COL_CAN_AUDIO_CALL, &can_audio,
      EMPATHY_INDIVIDUAL_STORE_COL_CAN_VIDEO_CALL, &can_video, -1);

  g_object_set (cell,
      "visible", !is_group && (can_audio || can_video),
      "icon-name", can_video ? EMPATHY_IMAGE_VIDEO_CALL : EMPATHY_IMAGE_VOIP,
      NULL);

  individual_view_cell_set_background (view, cell, is_group, is_active);
}

static void
individual_view_avatar_cell_data_func (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *model,
    GtkTreeIter *iter,
    EmpathyIndividualView *view)
{
  GdkPixbuf *pixbuf;
  gboolean show_avatar;
  gboolean is_group;
  gboolean is_active;

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_PIXBUF_AVATAR, &pixbuf,
      EMPATHY_INDIVIDUAL_STORE_COL_PIXBUF_AVATAR_VISIBLE, &show_avatar,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_ACTIVE, &is_active, -1);

  g_object_set (cell,
      "visible", !is_group && show_avatar,
      "pixbuf", pixbuf,
      NULL);

  tp_clear_object (&pixbuf);

  individual_view_cell_set_background (view, cell, is_group, is_active);
}

static void
individual_view_text_cell_data_func (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *model,
    GtkTreeIter *iter,
    EmpathyIndividualView *view)
{
  gboolean is_group;
  gboolean is_active;

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_ACTIVE, &is_active, -1);

  individual_view_cell_set_background (view, cell, is_group, is_active);
}

static void
individual_view_expander_cell_data_func (GtkTreeViewColumn *column,
    GtkCellRenderer *cell,
    GtkTreeModel *model,
    GtkTreeIter *iter,
    EmpathyIndividualView *view)
{
  gboolean is_group;
  gboolean is_active;

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_ACTIVE, &is_active, -1);

  if (gtk_tree_model_iter_has_child (model, iter))
    {
      GtkTreePath *path;
      gboolean row_expanded;

      path = gtk_tree_model_get_path (model, iter);
      row_expanded =
          gtk_tree_view_row_expanded (GTK_TREE_VIEW
          (gtk_tree_view_column_get_tree_view (column)), path);
      gtk_tree_path_free (path);

      g_object_set (cell,
          "visible", TRUE,
          "expander-style",
          row_expanded ? GTK_EXPANDER_EXPANDED : GTK_EXPANDER_COLLAPSED,
          NULL);
    }
  else
    g_object_set (cell, "visible", FALSE, NULL);

  individual_view_cell_set_background (view, cell, is_group, is_active);
}

static void
individual_view_row_expand_or_collapse_cb (EmpathyIndividualView *view,
    GtkTreeIter *iter,
    GtkTreePath *path,
    gpointer user_data)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);
  GtkTreeModel *model;
  gchar *name;
  gboolean expanded;

  if (!(priv->view_features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_GROUPS_SAVE))
    return;

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (view));

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name, -1);

  expanded = GPOINTER_TO_INT (user_data);
  empathy_contact_group_set_expanded (name, expanded);

  g_free (name);
}

static gboolean
individual_view_start_search_cb (EmpathyIndividualView *view,
    gpointer data)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);

  if (priv->search_widget == NULL)
    return FALSE;

  empathy_individual_view_start_search (view);

  return TRUE;
}

static void
individual_view_search_text_notify_cb (TpawLiveSearch *search,
    GParamSpec *pspec,
    EmpathyIndividualView *view)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);
  GtkTreePath *path;
  GtkTreeViewColumn *focus_column;
  GtkTreeModel *model;
  GtkTreeIter iter;
  gboolean set_cursor = FALSE;

  gtk_tree_model_filter_refilter (priv->filter);

  /* Set cursor on the first contact. If it is already set on a group,
   * set it on its first child contact. Note that first child of a group
   * is its separator, that's why we actually set to the 2nd
   */

  model = gtk_tree_view_get_model (GTK_TREE_VIEW (view));
  gtk_tree_view_get_cursor (GTK_TREE_VIEW (view), &path, &focus_column);

  if (path == NULL)
    {
      path = gtk_tree_path_new_from_string ("0:1");
      set_cursor = TRUE;
    }
  else if (gtk_tree_path_get_depth (path) < 2)
    {
      gboolean is_group;

      gtk_tree_model_get_iter (model, &iter, path);
      gtk_tree_model_get (model, &iter,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
          -1);

      if (is_group)
        {
          gtk_tree_path_down (path);
          gtk_tree_path_next (path);
          set_cursor = TRUE;
        }
    }

  if (set_cursor)
    {
      /* FIXME: Workaround for GTK bug #621651, we have to make sure the path is
       * valid. */
      if (gtk_tree_model_get_iter (model, &iter, path))
        {
          gtk_tree_view_set_cursor (GTK_TREE_VIEW (view), path, focus_column,
              FALSE);
        }
    }

  gtk_tree_path_free (path);
}

static void
individual_view_search_activate_cb (GtkWidget *search,
  EmpathyIndividualView *view)
{
  GtkTreePath *path;
  GtkTreeViewColumn *focus_column;

  gtk_tree_view_get_cursor (GTK_TREE_VIEW (view), &path, &focus_column);
  if (path != NULL)
    {
      gtk_tree_view_row_activated (GTK_TREE_VIEW (view), path, focus_column);
      gtk_tree_path_free (path);

      gtk_widget_hide (search);
    }
}

static gboolean
individual_view_search_key_navigation_cb (GtkWidget *search,
  GdkEvent *event,
  EmpathyIndividualView *view)
{
  GdkEvent *new_event;
  gboolean ret = FALSE;

  new_event = gdk_event_copy (event);
  gtk_widget_grab_focus (GTK_WIDGET (view));
  ret = gtk_widget_event (GTK_WIDGET (view), new_event);
  gtk_widget_grab_focus (search);

  gdk_event_free (new_event);

  return ret;
}

static void
individual_view_search_hide_cb (TpawLiveSearch *search,
    EmpathyIndividualView *view)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);
  GtkTreeModel *model;
  GtkTreePath *cursor_path;
  GtkTreeIter iter;
  gboolean valid = FALSE;

  /* block expand or collapse handlers, they would write the
   * expand or collapsed setting to file otherwise */
  g_signal_handlers_block_by_func (view,
      individual_view_row_expand_or_collapse_cb, GINT_TO_POINTER (TRUE));
  g_signal_handlers_block_by_func (view,
    individual_view_row_expand_or_collapse_cb, GINT_TO_POINTER (FALSE));

  /* restore which groups are expanded and which are not */
  model = gtk_tree_view_get_model (GTK_TREE_VIEW (view));
  for (valid = gtk_tree_model_get_iter_first (model, &iter);
       valid; valid = gtk_tree_model_iter_next (model, &iter))
    {
      gboolean is_group;
      gchar *name = NULL;
      GtkTreePath *path;

      gtk_tree_model_get (model, &iter,
          EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
          -1);

      if (!is_group)
        {
          g_free (name);
          continue;
        }

      path = gtk_tree_model_get_path (model, &iter);
      if ((priv->view_features &
            EMPATHY_INDIVIDUAL_VIEW_FEATURE_GROUPS_SAVE) == 0 ||
          empathy_contact_group_get_expanded (name))
        {
          gtk_tree_view_expand_row (GTK_TREE_VIEW (view), path, TRUE);
        }
      else
        {
          gtk_tree_view_collapse_row (GTK_TREE_VIEW (view), path);
        }

      gtk_tree_path_free (path);
      g_free (name);
    }

  /* unblock expand or collapse handlers */
  g_signal_handlers_unblock_by_func (view,
      individual_view_row_expand_or_collapse_cb, GINT_TO_POINTER (TRUE));
  g_signal_handlers_unblock_by_func (view,
      individual_view_row_expand_or_collapse_cb, GINT_TO_POINTER (FALSE));

  /* keep the selected contact visible */
  gtk_tree_view_get_cursor (GTK_TREE_VIEW (view), &cursor_path, NULL);

  if (cursor_path != NULL)
    gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (view), cursor_path, NULL,
        FALSE, 0, 0);

  gtk_tree_path_free (cursor_path);
}

static void
individual_view_search_show_cb (TpawLiveSearch *search,
    EmpathyIndividualView *view)
{
  /* block expand or collapse handlers during expand all, they would
   * write the expand or collapsed setting to file otherwise */
  g_signal_handlers_block_by_func (view,
      individual_view_row_expand_or_collapse_cb, GINT_TO_POINTER (TRUE));

  gtk_tree_view_expand_all (GTK_TREE_VIEW (view));

  g_signal_handlers_unblock_by_func (view,
      individual_view_row_expand_or_collapse_cb, GINT_TO_POINTER (TRUE));
}

static gboolean
expand_idle_foreach_cb (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    EmpathyIndividualView *self)
{
  EmpathyIndividualViewPriv *priv;
  gboolean is_group;
  gpointer should_expand;
  gchar *name;

  /* We only want groups */
  if (gtk_tree_path_get_depth (path) > 1)
    return FALSE;

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name,
      -1);

  if (!is_group)
    {
      g_free (name);
      return FALSE;
    }

  priv = GET_PRIV (self);

  if (g_hash_table_lookup_extended (priv->expand_groups, name, NULL,
      &should_expand))
    {
      if (GPOINTER_TO_INT (should_expand))
        gtk_tree_view_expand_row (GTK_TREE_VIEW (self), path, FALSE);
      else
        gtk_tree_view_collapse_row (GTK_TREE_VIEW (self), path);

      g_hash_table_remove (priv->expand_groups, name);
    }

  g_free (name);

  return FALSE;
}

static gboolean
individual_view_expand_idle_cb (EmpathyIndividualView *self)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (self);

  g_signal_handlers_block_by_func (self,
    individual_view_row_expand_or_collapse_cb, GINT_TO_POINTER (TRUE));
  g_signal_handlers_block_by_func (self,
    individual_view_row_expand_or_collapse_cb, GINT_TO_POINTER (FALSE));

  /* The store/filter could've been removed while we were in the idle queue */
  if (priv->filter != NULL)
    {
      gtk_tree_model_foreach (GTK_TREE_MODEL (priv->filter),
          (GtkTreeModelForeachFunc) expand_idle_foreach_cb, self);
    }

  g_signal_handlers_unblock_by_func (self,
      individual_view_row_expand_or_collapse_cb, GINT_TO_POINTER (FALSE));
  g_signal_handlers_unblock_by_func (self,
      individual_view_row_expand_or_collapse_cb, GINT_TO_POINTER (TRUE));

  /* Empty the table of groups to expand/contract, since it may contain groups
   * which no longer exist in the tree view. This can happen after going
   * offline, for example. */
  g_hash_table_remove_all (priv->expand_groups);
  priv->expand_groups_idle_handler = 0;
  g_object_unref (self);

  return FALSE;
}

static void
individual_view_row_has_child_toggled_cb (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    EmpathyIndividualView *view)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);
  gboolean should_expand, is_group = FALSE;
  gchar *name = NULL;
  gpointer will_expand;

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name,
      -1);

  if (!is_group || TPAW_STR_EMPTY (name))
    {
      g_free (name);
      return;
    }

  should_expand = (priv->view_features &
          EMPATHY_INDIVIDUAL_VIEW_FEATURE_GROUPS_SAVE) == 0 ||
      (priv->search_widget != NULL &&
          gtk_widget_get_visible (priv->search_widget)) ||
      empathy_contact_group_get_expanded (name);

  /* FIXME: It doesn't work to call gtk_tree_view_expand_row () from within
   * gtk_tree_model_filter_refilter (). We add the rows to expand/contract to
   * a hash table, and expand or contract them as appropriate all at once in
   * an idle handler which iterates over all the group rows. */
  if (!g_hash_table_lookup_extended (priv->expand_groups, name, NULL,
      &will_expand) ||
      GPOINTER_TO_INT (will_expand) != should_expand)
    {
      g_hash_table_insert (priv->expand_groups, g_strdup (name),
          GINT_TO_POINTER (should_expand));

      if (priv->expand_groups_idle_handler == 0)
        {
          priv->expand_groups_idle_handler =
              g_idle_add ((GSourceFunc) individual_view_expand_idle_cb,
                  g_object_ref (view));
        }
    }

  g_free (name);
}

static gboolean
individual_view_is_visible_individual (EmpathyIndividualView *self,
    FolksIndividual *individual,
    gboolean is_online,
    gboolean is_searching,
    const gchar *group,
    gboolean is_fake_group,
    guint event_count)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (self);
  TpawLiveSearch *live = TPAW_LIVE_SEARCH (priv->search_widget);
  GeeSet *personas;
  GeeIterator *iter;
  gboolean is_favorite;

  /* Always display individuals having pending events */
  if (event_count > 0)
    return TRUE;

  /* We're only giving the visibility wrt filtering here, not things like
   * presence. */
  if (!priv->show_untrusted &&
      folks_individual_get_trust_level (individual) == FOLKS_TRUST_LEVEL_NONE)
    {
      return FALSE;
    }

  if (!priv->show_uninteresting)
    {
      gboolean contains_interesting_persona = FALSE;

      /* Hide all individuals which consist entirely of uninteresting
       * personas */
      personas = folks_individual_get_personas (individual);
      iter = gee_iterable_iterator (GEE_ITERABLE (personas));
      while (!contains_interesting_persona && gee_iterator_next (iter))
        {
          FolksPersona *persona = gee_iterator_get (iter);

          if (empathy_folks_persona_is_interesting (persona))
            contains_interesting_persona = TRUE;

          g_clear_object (&persona);
        }
      g_clear_object (&iter);

      if (!contains_interesting_persona)
        return FALSE;
    }

  is_favorite = folks_favourite_details_get_is_favourite (
      FOLKS_FAVOURITE_DETAILS (individual));
  if (!is_searching) {
    if (is_favorite && is_fake_group &&
        !tp_strdiff (group, EMPATHY_INDIVIDUAL_STORE_FAVORITE))
        /* Always display favorite contacts in the favorite group */
        return TRUE;

    return (priv->show_offline || is_online);
  }

  return empathy_individual_match_string (individual,
      tpaw_live_search_get_text (live),
      tpaw_live_search_get_words (live));
}

static gchar *
get_group (GtkTreeModel *model,
    GtkTreeIter *iter,
    gboolean *is_fake)
{
  GtkTreeIter parent_iter;
  gchar *name = NULL;

  *is_fake = FALSE;

  if (!gtk_tree_model_iter_parent (model, &parent_iter, iter))
    return NULL;

  gtk_tree_model_get (model, &parent_iter,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_FAKE_GROUP, is_fake,
      -1);

  return name;
}


static gboolean
individual_view_filter_visible_func (GtkTreeModel *model,
    GtkTreeIter *iter,
    gpointer user_data)
{
  EmpathyIndividualView *self = EMPATHY_INDIVIDUAL_VIEW (user_data);
  EmpathyIndividualViewPriv *priv = GET_PRIV (self);
  FolksIndividual *individual = NULL;
  gboolean is_group, is_separator, valid;
  GtkTreeIter child_iter;
  gboolean visible, is_online;
  gboolean is_searching = TRUE;
  guint event_count;

  if (priv->custom_filter != NULL)
    return priv->custom_filter (model, iter, priv->custom_filter_data);

  if (priv->search_widget == NULL ||
      !gtk_widget_get_visible (priv->search_widget))
     is_searching = FALSE;

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, &is_separator,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_ONLINE, &is_online,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual,
      EMPATHY_INDIVIDUAL_STORE_COL_EVENT_COUNT, &event_count,
      -1);

  if (individual != NULL)
    {
      gchar *group;
      gboolean is_fake_group;

      group = get_group (model, iter, &is_fake_group);

      visible = individual_view_is_visible_individual (self, individual,
          is_online, is_searching, group, is_fake_group, event_count);

      g_object_unref (individual);
      g_free (group);

      return visible;
    }

  if (is_separator)
    return TRUE;

  /* Not a contact, not a separator, must be a group */
  g_return_val_if_fail (is_group, FALSE);

  /* only show groups which are not empty */
  for (valid = gtk_tree_model_iter_children (model, &child_iter, iter);
       valid; valid = gtk_tree_model_iter_next (model, &child_iter))
    {
      gchar *group;
      gboolean is_fake_group;

      gtk_tree_model_get (model, &child_iter,
        EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual,
        EMPATHY_INDIVIDUAL_STORE_COL_IS_ONLINE, &is_online,
        EMPATHY_INDIVIDUAL_STORE_COL_EVENT_COUNT, &event_count,
        -1);

      if (individual == NULL)
        continue;

      group = get_group (model, &child_iter, &is_fake_group);

      visible = individual_view_is_visible_individual (self, individual,
          is_online, is_searching, group, is_fake_group, event_count);

      g_object_unref (individual);
      g_free (group);

      /* show group if it has at least one visible contact in it */
      if (visible)
        return TRUE;
    }

  return FALSE;
}

static gchar * empathy_individual_view_dup_selected_group (
    EmpathyIndividualView *view,
    gboolean *is_fake_group);

static void
text_edited_cb (GtkCellRendererText *cellrenderertext,
    gchar *path,
    gchar *name,
    EmpathyIndividualView *self)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (self);
  gchar *old_name, *new_name;

  g_object_set (priv->text_renderer, "editable", FALSE, NULL);

  new_name = g_strdup (name);
  g_strstrip (new_name);

  if (tp_str_empty (new_name))
    goto out;

  old_name = empathy_individual_view_dup_selected_group (self, NULL);
  g_return_if_fail (old_name != NULL);

  if (tp_strdiff (old_name, new_name))
    {
      EmpathyConnectionAggregator *aggregator;

      DEBUG ("rename group '%s' to '%s'", old_name, new_name);

      aggregator = empathy_connection_aggregator_dup_singleton ();

      empathy_connection_aggregator_rename_group (aggregator, old_name,
          new_name);
      g_object_unref (aggregator);
    }

  g_free (old_name);
out:
  g_free (new_name);
}

static void
text_renderer_editing_cancelled_cb (GtkCellRenderer *renderer,
    EmpathyIndividualView *self)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (self);

  g_object_set (priv->text_renderer, "editable", FALSE, NULL);
}

static void
individual_view_constructed (GObject *object)
{
  EmpathyIndividualView *view = EMPATHY_INDIVIDUAL_VIEW (object);
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);
  GtkCellRenderer *cell;
  GtkTreeViewColumn *col;
  guint i;

  /* Setup view */
  g_object_set (view,
      "headers-visible", FALSE,
      "show-expanders", FALSE,
      NULL);

  col = gtk_tree_view_column_new ();

  /* State */
  cell = gtk_cell_renderer_pixbuf_new ();
  gtk_tree_view_column_pack_start (col, cell, FALSE);
  gtk_tree_view_column_set_cell_data_func (col, cell,
      (GtkTreeCellDataFunc) individual_view_pixbuf_cell_data_func,
      view, NULL);

  g_object_set (cell,
      "xpad", 5,
      "ypad", 1,
      "visible", FALSE,
      NULL);

  /* Group icon */
  cell = gtk_cell_renderer_pixbuf_new ();
  gtk_tree_view_column_pack_start (col, cell, FALSE);
  gtk_tree_view_column_set_cell_data_func (col, cell,
      (GtkTreeCellDataFunc) individual_view_group_icon_cell_data_func,
      view, NULL);

  g_object_set (cell,
      "xpad", 0,
      "ypad", 0,
      "visible", FALSE,
      "width", 16,
      "height", 16,
      NULL);

  /* Name */
  priv->text_renderer = empathy_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (col, priv->text_renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func (col, priv->text_renderer,
      (GtkTreeCellDataFunc) individual_view_text_cell_data_func, view, NULL);

  gtk_tree_view_column_add_attribute (col, priv->text_renderer,
      "name", EMPATHY_INDIVIDUAL_STORE_COL_NAME);
  gtk_tree_view_column_add_attribute (col, priv->text_renderer,
      "text", EMPATHY_INDIVIDUAL_STORE_COL_NAME);
  gtk_tree_view_column_add_attribute (col, priv->text_renderer,
      "presence-type", EMPATHY_INDIVIDUAL_STORE_COL_PRESENCE_TYPE);
  gtk_tree_view_column_add_attribute (col, priv->text_renderer,
      "status", EMPATHY_INDIVIDUAL_STORE_COL_STATUS);
  gtk_tree_view_column_add_attribute (col, priv->text_renderer,
      "is_group", EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP);
  gtk_tree_view_column_add_attribute (col, priv->text_renderer,
      "compact", EMPATHY_INDIVIDUAL_STORE_COL_COMPACT);
  gtk_tree_view_column_add_attribute (col, priv->text_renderer,
      "client-types", EMPATHY_INDIVIDUAL_STORE_COL_CLIENT_TYPES);

  g_signal_connect (priv->text_renderer, "editing-canceled",
      G_CALLBACK (text_renderer_editing_cancelled_cb), view);
  g_signal_connect (priv->text_renderer, "edited",
      G_CALLBACK (text_edited_cb), view);

  /* Audio Call Icon */
  cell = empathy_cell_renderer_activatable_new ();
  gtk_tree_view_column_pack_start (col, cell, FALSE);
  gtk_tree_view_column_set_cell_data_func (col, cell,
      (GtkTreeCellDataFunc) individual_view_audio_call_cell_data_func,
      view, NULL);

  g_object_set (cell, "visible", FALSE, NULL);

  g_signal_connect (cell, "path-activated",
      G_CALLBACK (individual_view_call_activated_cb), view);

  /* Avatar */
  cell = gtk_cell_renderer_pixbuf_new ();
  gtk_tree_view_column_pack_start (col, cell, FALSE);
  gtk_tree_view_column_set_cell_data_func (col, cell,
      (GtkTreeCellDataFunc) individual_view_avatar_cell_data_func,
      view, NULL);

  g_object_set (cell,
      "xpad", 0,
      "ypad", 0,
      "visible", FALSE,
      "width", 32,
      "height", 32,
      NULL);

  /* Expander */
  cell = empathy_cell_renderer_expander_new ();
  gtk_tree_view_column_pack_end (col, cell, FALSE);
  gtk_tree_view_column_set_cell_data_func (col, cell,
      (GtkTreeCellDataFunc) individual_view_expander_cell_data_func,
      view, NULL);

  /* Actually add the column now we have added all cell renderers */
  gtk_tree_view_append_column (GTK_TREE_VIEW (view), col);

  /* Drag & Drop. */
  for (i = 0; i < G_N_ELEMENTS (drag_types_dest); ++i)
    {
      drag_atoms_dest[i] = gdk_atom_intern (drag_types_dest[i].target, FALSE);
    }
}

static void
individual_view_set_view_features (EmpathyIndividualView *view,
    EmpathyIndividualViewFeatureFlags features)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);
  gboolean has_tooltip;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (view));

  priv->view_features = features;

  /* Setting reorderable is a hack that gets us row previews as drag icons
     for free.  We override all the drag handlers.  It's tricky to get the
     position of the drag icon right in drag_begin.  GtkTreeView has special
     voodoo for it, so we let it do the voodoo that he do (but only if dragging
     is enabled).
   */
  gtk_tree_view_set_reorderable (GTK_TREE_VIEW (view),
      (features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_INDIVIDUAL_DRAG));

  /* Update DnD source/dest */
  if (features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_INDIVIDUAL_DRAG)
    {
      gtk_drag_source_set (GTK_WIDGET (view),
          GDK_BUTTON1_MASK,
          drag_types_source,
          G_N_ELEMENTS (drag_types_source),
          GDK_ACTION_MOVE | GDK_ACTION_COPY);
    }
  else
    {
      gtk_drag_source_unset (GTK_WIDGET (view));

    }

  if (features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_INDIVIDUAL_DROP)
    {
      gtk_drag_dest_set (GTK_WIDGET (view),
          GTK_DEST_DEFAULT_ALL,
          drag_types_dest,
          G_N_ELEMENTS (drag_types_dest), GDK_ACTION_MOVE | GDK_ACTION_COPY);
    }
  else
    {
      /* FIXME: URI could still be droped depending on FT feature */
      gtk_drag_dest_unset (GTK_WIDGET (view));
    }

  /* Update has-tooltip */
  has_tooltip =
      (features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_INDIVIDUAL_TOOLTIP) != 0;
  gtk_widget_set_has_tooltip (GTK_WIDGET (view), has_tooltip);
}

static void
individual_view_dispose (GObject *object)
{
  EmpathyIndividualView *view = EMPATHY_INDIVIDUAL_VIEW (object);
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);

  tp_clear_object (&priv->store);
  tp_clear_object (&priv->filter);
  tp_clear_object (&priv->tooltip_widget);

  empathy_individual_view_set_live_search (view, NULL);

  G_OBJECT_CLASS (empathy_individual_view_parent_class)->dispose (object);
}

static void
individual_view_finalize (GObject *object)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (object);

  if (priv->expand_groups_idle_handler != 0)
    g_source_remove (priv->expand_groups_idle_handler);
  g_hash_table_unref (priv->expand_groups);

  G_OBJECT_CLASS (empathy_individual_view_parent_class)->finalize (object);
}

static void
individual_view_get_property (GObject *object,
    guint param_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyIndividualViewPriv *priv;

  priv = GET_PRIV (object);

  switch (param_id)
    {
    case PROP_STORE:
      g_value_set_object (value, priv->store);
      break;
    case PROP_VIEW_FEATURES:
      g_value_set_flags (value, priv->view_features);
      break;
    case PROP_INDIVIDUAL_FEATURES:
      g_value_set_flags (value, priv->individual_features);
      break;
    case PROP_SHOW_OFFLINE:
      g_value_set_boolean (value, priv->show_offline);
      break;
    case PROP_SHOW_UNTRUSTED:
      g_value_set_boolean (value, priv->show_untrusted);
      break;
    case PROP_SHOW_UNINTERESTING:
      g_value_set_boolean (value, priv->show_uninteresting);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
      break;
    };
}

static void
individual_view_set_property (GObject *object,
    guint param_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyIndividualView *view = EMPATHY_INDIVIDUAL_VIEW (object);
  EmpathyIndividualViewPriv *priv = GET_PRIV (object);

  switch (param_id)
    {
    case PROP_STORE:
      empathy_individual_view_set_store (view, g_value_get_object (value));
      break;
    case PROP_VIEW_FEATURES:
      individual_view_set_view_features (view, g_value_get_flags (value));
      break;
    case PROP_INDIVIDUAL_FEATURES:
      priv->individual_features = g_value_get_flags (value);
      break;
    case PROP_SHOW_OFFLINE:
      empathy_individual_view_set_show_offline (view,
          g_value_get_boolean (value));
      break;
    case PROP_SHOW_UNTRUSTED:
      empathy_individual_view_set_show_untrusted (view,
          g_value_get_boolean (value));
      break;
    case PROP_SHOW_UNINTERESTING:
      empathy_individual_view_set_show_uninteresting (view,
          g_value_get_boolean (value));
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
      break;
    };
}

static void
empathy_individual_view_class_init (EmpathyIndividualViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkTreeViewClass *tree_view_class = GTK_TREE_VIEW_CLASS (klass);

  object_class->constructed = individual_view_constructed;
  object_class->dispose = individual_view_dispose;
  object_class->finalize = individual_view_finalize;
  object_class->get_property = individual_view_get_property;
  object_class->set_property = individual_view_set_property;

  widget_class->drag_data_received = individual_view_drag_data_received;
  widget_class->drag_drop = individual_view_drag_drop;
  widget_class->drag_begin = individual_view_drag_begin;
  widget_class->drag_data_get = individual_view_drag_data_get;
  widget_class->drag_end = individual_view_drag_end;
  widget_class->drag_motion = individual_view_drag_motion;

  /* We use the class method to let user of this widget to connect to
   * the signal and stop emission of the signal so the default handler
   * won't be called. */
  tree_view_class->row_activated = individual_view_row_activated;

  klass->drag_individual_received = real_drag_individual_received_cb;

  signals[DRAG_INDIVIDUAL_RECEIVED] =
      g_signal_new ("drag-individual-received",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (EmpathyIndividualViewClass, drag_individual_received),
      NULL, NULL,
      g_cclosure_marshal_generic,
      G_TYPE_NONE, 4, G_TYPE_UINT, FOLKS_TYPE_INDIVIDUAL,
      G_TYPE_STRING, G_TYPE_STRING);

  signals[DRAG_PERSONA_RECEIVED] =
      g_signal_new ("drag-persona-received",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (EmpathyIndividualViewClass, drag_persona_received),
      NULL, NULL,
      g_cclosure_marshal_generic,
      G_TYPE_BOOLEAN, 3, G_TYPE_UINT, FOLKS_TYPE_PERSONA, FOLKS_TYPE_INDIVIDUAL);

  g_object_class_install_property (object_class,
      PROP_STORE,
      g_param_spec_object ("store",
          "The store of the view",
          "The store of the view",
          EMPATHY_TYPE_INDIVIDUAL_STORE,
          G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
      PROP_VIEW_FEATURES,
      g_param_spec_flags ("view-features",
          "Features of the view",
          "Flags for all enabled features",
          EMPATHY_TYPE_INDIVIDUAL_VIEW_FEATURE_FLAGS,
          EMPATHY_INDIVIDUAL_VIEW_FEATURE_NONE, G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
      PROP_INDIVIDUAL_FEATURES,
      g_param_spec_flags ("individual-features",
          "Features of the individual menu",
          "Flags for all enabled features for the menu",
          EMPATHY_TYPE_INDIVIDUAL_FEATURE_FLAGS,
          EMPATHY_INDIVIDUAL_FEATURE_NONE, G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
      PROP_SHOW_OFFLINE,
      g_param_spec_boolean ("show-offline",
          "Show Offline",
          "Whether contact list should display "
          "offline contacts", FALSE, G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
      PROP_SHOW_UNTRUSTED,
      g_param_spec_boolean ("show-untrusted",
          "Show Untrusted Individuals",
          "Whether the view should display untrusted individuals; "
          "those who could not be who they say they are.",
          TRUE, G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
      PROP_SHOW_UNINTERESTING,
      g_param_spec_boolean ("show-uninteresting",
          "Show Uninteresting Individuals",
          "Whether the view should not filter out individuals using "
          "empathy_folks_persona_is_interesting.",
          FALSE, G_PARAM_READWRITE));

  g_type_class_add_private (object_class, sizeof (EmpathyIndividualViewPriv));
}

static void
empathy_individual_view_init (EmpathyIndividualView *view)
{
  EmpathyIndividualViewPriv *priv = G_TYPE_INSTANCE_GET_PRIVATE (view,
      EMPATHY_TYPE_INDIVIDUAL_VIEW, EmpathyIndividualViewPriv);

  view->priv = priv;

  priv->show_untrusted = TRUE;
  priv->show_uninteresting = FALSE;

  /* Get saved group states. */
  empathy_contact_groups_get_all ();

  priv->expand_groups = g_hash_table_new_full (g_str_hash, g_str_equal,
      (GDestroyNotify) g_free, NULL);

  gtk_tree_view_set_row_separator_func (GTK_TREE_VIEW (view),
      empathy_individual_store_row_separator_func, NULL, NULL);

  /* Connect to tree view signals rather than override. */
  g_signal_connect (view, "button-press-event",
      G_CALLBACK (individual_view_button_press_event_cb), NULL);
  g_signal_connect (view, "key-press-event",
      G_CALLBACK (individual_view_key_press_event_cb), NULL);
  g_signal_connect (view, "row-expanded",
      G_CALLBACK (individual_view_row_expand_or_collapse_cb),
      GINT_TO_POINTER (TRUE));
  g_signal_connect (view, "row-collapsed",
      G_CALLBACK (individual_view_row_expand_or_collapse_cb),
      GINT_TO_POINTER (FALSE));
  g_signal_connect (view, "query-tooltip",
      G_CALLBACK (individual_view_query_tooltip_cb), NULL);
}

EmpathyIndividualView *
empathy_individual_view_new (EmpathyIndividualStore *store,
    EmpathyIndividualViewFeatureFlags view_features,
    EmpathyIndividualFeatureFlags individual_features)
{
  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (store), NULL);

  return g_object_new (EMPATHY_TYPE_INDIVIDUAL_VIEW,
      "store", store,
      "individual-features", individual_features,
      "view-features", view_features, NULL);
}

FolksIndividual *
empathy_individual_view_dup_selected (EmpathyIndividualView *view)
{
  GtkTreeSelection *selection;
  GtkTreeIter iter;
  GtkTreeModel *model;
  FolksIndividual *individual;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (view), NULL);

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (view));
  if (!gtk_tree_selection_get_selected (selection, &model, &iter))
    return NULL;

  gtk_tree_model_get (model, &iter,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual, -1);

  return individual;
}

static gchar *
empathy_individual_view_dup_selected_group (EmpathyIndividualView *view,
    gboolean *is_fake_group)
{
  GtkTreeSelection *selection;
  GtkTreeIter iter;
  GtkTreeModel *model;
  gboolean is_group;
  gchar *name;
  gboolean fake;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (view), NULL);

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (view));
  if (!gtk_tree_selection_get_selected (selection, &model, &iter))
    return NULL;

  gtk_tree_model_get (model, &iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_FAKE_GROUP, &fake, -1);

  if (!is_group)
    {
      g_free (name);
      return NULL;
    }

  if (is_fake_group != NULL)
    *is_fake_group = fake;

  return name;
}

enum
{
  REMOVE_DIALOG_RESPONSE_CANCEL = 0,
  REMOVE_DIALOG_RESPONSE_DELETE,
};

static int
individual_view_remove_dialog_show (GtkWindow *parent,
    const gchar *message,
    const gchar *secondary_text)
{
  GtkWidget *dialog;
  gboolean res;

  dialog = gtk_message_dialog_new (parent, GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", message);

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
individual_view_group_remove_activate_cb (GtkMenuItem *menuitem,
    EmpathyIndividualView *view)
{
  gchar *group;

  group = empathy_individual_view_dup_selected_group (view, NULL);
  if (group != NULL)
    {
      gchar *text;
      GtkWindow *parent;

      text =
          g_strdup_printf (_("Do you really want to remove the group '%s'?"),
          group);
      parent = tpaw_get_toplevel_window (GTK_WIDGET (view));
      if (individual_view_remove_dialog_show (parent, _("Removing group"),
              text) == REMOVE_DIALOG_RESPONSE_DELETE)
        {
          EmpathyIndividualManager *manager =
              empathy_individual_manager_dup_singleton ();
          empathy_individual_manager_remove_group (manager, group);
          g_object_unref (G_OBJECT (manager));
        }

      g_free (text);
    }

  g_free (group);
}

static void
individual_view_group_rename_activate_cb (GtkMenuItem *menuitem,
    EmpathyIndividualView *self)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (self);
  GtkTreePath *path;
  GtkTreeIter iter;
  GtkTreeSelection *selection;
  GtkTreeModel *model;

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  if (!gtk_tree_selection_get_selected (selection, &model, &iter))
    return;
  path = gtk_tree_model_get_path (model, &iter);

  g_object_set (G_OBJECT (priv->text_renderer), "editable", TRUE, NULL);

  gtk_tree_view_set_enable_search (GTK_TREE_VIEW (self), FALSE);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  gtk_tree_view_set_cursor (GTK_TREE_VIEW (self), path,
      gtk_tree_view_get_column (GTK_TREE_VIEW (self), 0), TRUE);

  gtk_tree_path_free (path);
}

GtkWidget *
empathy_individual_view_get_group_menu (EmpathyIndividualView *view)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);
  gchar *group;
  GtkWidget *menu;
  GtkWidget *item;
  GtkWidget *image;
  gboolean is_fake_group;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (view), NULL);

  if (!(priv->view_features & (EMPATHY_INDIVIDUAL_VIEW_FEATURE_GROUPS_RENAME |
              EMPATHY_INDIVIDUAL_VIEW_FEATURE_GROUPS_REMOVE)))
    return NULL;

  group = empathy_individual_view_dup_selected_group (view, &is_fake_group);
  if (!group || is_fake_group)
    {
      /* We can't alter fake groups */
      g_free (group);
      return NULL;
    }

  menu = gtk_menu_new ();

  if (priv->view_features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_GROUPS_RENAME)
    {
       item = gtk_menu_item_new_with_mnemonic (_("Re_name"));
       gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
       gtk_widget_show (item);
       g_signal_connect (item, "activate",
           G_CALLBACK (individual_view_group_rename_activate_cb), view);
     }

  if (priv->view_features & EMPATHY_INDIVIDUAL_VIEW_FEATURE_GROUPS_REMOVE)
    {
      item = gtk_image_menu_item_new_with_mnemonic (_("_Remove"));
      image = gtk_image_new_from_icon_name (GTK_STOCK_REMOVE,
          GTK_ICON_SIZE_MENU);
      gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
      gtk_widget_show (item);
      g_signal_connect (item, "activate",
          G_CALLBACK (individual_view_group_remove_activate_cb), view);
    }

  g_free (group);

  return menu;
}

GtkWidget *
empathy_individual_view_get_individual_menu (EmpathyIndividualView *view)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);
  FolksIndividual *individual;
  GtkWidget *menu = NULL;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (view), NULL);

  if (priv->individual_features == EMPATHY_INDIVIDUAL_FEATURE_NONE)
    /* No need to create a context menu */
    return NULL;

  individual = empathy_individual_view_dup_selected (view);
  if (individual == NULL)
    return NULL;

  if (!empathy_folks_individual_contains_contact (individual))
    goto out;

  menu = empathy_individual_menu_new (individual, NULL,
      priv->individual_features, priv->store);

out:
  g_object_unref (individual);

  return menu;
}

void
empathy_individual_view_set_live_search (EmpathyIndividualView *view,
    TpawLiveSearch *search)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (view);

  /* remove old handlers if old search was not null */
  if (priv->search_widget != NULL)
    {
      g_signal_handlers_disconnect_by_func (view,
          individual_view_start_search_cb, NULL);

      g_signal_handlers_disconnect_by_func (priv->search_widget,
          individual_view_search_text_notify_cb, view);
      g_signal_handlers_disconnect_by_func (priv->search_widget,
          individual_view_search_activate_cb, view);
      g_signal_handlers_disconnect_by_func (priv->search_widget,
          individual_view_search_key_navigation_cb, view);
      g_signal_handlers_disconnect_by_func (priv->search_widget,
          individual_view_search_hide_cb, view);
      g_signal_handlers_disconnect_by_func (priv->search_widget,
          individual_view_search_show_cb, view);
      g_object_unref (priv->search_widget);
      priv->search_widget = NULL;
    }

  /* connect handlers if new search is not null */
  if (search != NULL)
    {
      priv->search_widget = g_object_ref (search);

      g_signal_connect (view, "start-interactive-search",
          G_CALLBACK (individual_view_start_search_cb), NULL);

      g_signal_connect (priv->search_widget, "notify::text",
          G_CALLBACK (individual_view_search_text_notify_cb), view);
      g_signal_connect (priv->search_widget, "activate",
          G_CALLBACK (individual_view_search_activate_cb), view);
      g_signal_connect (priv->search_widget, "key-navigation",
          G_CALLBACK (individual_view_search_key_navigation_cb), view);
      g_signal_connect (priv->search_widget, "hide",
          G_CALLBACK (individual_view_search_hide_cb), view);
      g_signal_connect (priv->search_widget, "show",
          G_CALLBACK (individual_view_search_show_cb), view);
    }
}

gboolean
empathy_individual_view_is_searching (EmpathyIndividualView *self)
{
  EmpathyIndividualViewPriv *priv;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (self), FALSE);

  priv = GET_PRIV (self);

  return (priv->search_widget != NULL &&
          gtk_widget_get_visible (priv->search_widget));
}

gboolean
empathy_individual_view_get_show_offline (EmpathyIndividualView *self)
{
  EmpathyIndividualViewPriv *priv;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (self), FALSE);

  priv = GET_PRIV (self);

  return priv->show_offline;
}

void
empathy_individual_view_set_show_offline (EmpathyIndividualView *self,
    gboolean show_offline)
{
  EmpathyIndividualViewPriv *priv;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (self));

  priv = GET_PRIV (self);

  priv->show_offline = show_offline;

  g_object_notify (G_OBJECT (self), "show-offline");
  gtk_tree_model_filter_refilter (priv->filter);
}

gboolean
empathy_individual_view_get_show_untrusted (EmpathyIndividualView *self)
{
  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (self), FALSE);

  return GET_PRIV (self)->show_untrusted;
}

void
empathy_individual_view_set_show_untrusted (EmpathyIndividualView *self,
    gboolean show_untrusted)
{
  EmpathyIndividualViewPriv *priv;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (self));

  priv = GET_PRIV (self);

  priv->show_untrusted = show_untrusted;

  g_object_notify (G_OBJECT (self), "show-untrusted");
  gtk_tree_model_filter_refilter (priv->filter);
}

EmpathyIndividualStore *
empathy_individual_view_get_store (EmpathyIndividualView *self)
{
  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (self), NULL);

  return GET_PRIV (self)->store;
}

void
empathy_individual_view_set_store (EmpathyIndividualView *self,
    EmpathyIndividualStore *store)
{
  EmpathyIndividualViewPriv *priv;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (self));
  g_return_if_fail (store == NULL || EMPATHY_IS_INDIVIDUAL_STORE (store));

  priv = GET_PRIV (self);

  /* Destroy the old filter and remove the old store */
  if (priv->store != NULL)
    {
      g_signal_handlers_disconnect_by_func (priv->filter,
          individual_view_row_has_child_toggled_cb, self);

      gtk_tree_view_set_model (GTK_TREE_VIEW (self), NULL);
    }

  tp_clear_object (&priv->filter);
  tp_clear_object (&priv->store);

  /* Set the new store */
  priv->store = store;

  if (store != NULL)
    {
      g_object_ref (store);

      /* Create a new filter */
      priv->filter = GTK_TREE_MODEL_FILTER (gtk_tree_model_filter_new (
          GTK_TREE_MODEL (priv->store), NULL));
      gtk_tree_model_filter_set_visible_func (priv->filter,
          individual_view_filter_visible_func, self, NULL);

      g_signal_connect (priv->filter, "row-has-child-toggled",
          G_CALLBACK (individual_view_row_has_child_toggled_cb), self);
      gtk_tree_view_set_model (GTK_TREE_VIEW (self),
          GTK_TREE_MODEL (priv->filter));
    }
}

void
empathy_individual_view_start_search (EmpathyIndividualView *self)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (self);

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (self));
  g_return_if_fail (priv->search_widget != NULL);

  if (gtk_widget_get_visible (GTK_WIDGET (priv->search_widget)))
    gtk_widget_grab_focus (GTK_WIDGET (priv->search_widget));
  else
    gtk_widget_show (GTK_WIDGET (priv->search_widget));
}

void
empathy_individual_view_set_custom_filter (EmpathyIndividualView *self,
    GtkTreeModelFilterVisibleFunc filter,
    gpointer data)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (self);

  priv->custom_filter = filter;
  priv->custom_filter_data = data;
}

void
empathy_individual_view_refilter (EmpathyIndividualView *self)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (self);

  gtk_tree_model_filter_refilter (priv->filter);
}

void
empathy_individual_view_select_first (EmpathyIndividualView *self)
{
  EmpathyIndividualViewPriv *priv = GET_PRIV (self);
  GtkTreeIter iter;

  gtk_tree_model_filter_refilter (priv->filter);

  if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (priv->filter), &iter))
    {
      GtkTreeSelection *selection = gtk_tree_view_get_selection (
          GTK_TREE_VIEW (self));

      gtk_tree_selection_select_iter (selection, &iter);
    }
}

void
empathy_individual_view_set_show_uninteresting (EmpathyIndividualView *self,
    gboolean show_uninteresting)
{
  EmpathyIndividualViewPriv *priv;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_VIEW (self));

  priv = GET_PRIV (self);

  priv->show_uninteresting = show_uninteresting;

  g_object_notify (G_OBJECT (self), "show-uninteresting");
  gtk_tree_model_filter_refilter (priv->filter);
}
