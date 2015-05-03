#include "config.h"
#include "empathy-roster-view.h"

#include <glib/gi18n-lib.h>

#include "empathy-contact-groups.h"
#include "empathy-roster-contact.h"
#include "empathy-roster-group.h"
#include "empathy-ui-utils.h"

G_DEFINE_TYPE (EmpathyRosterView, empathy_roster_view, GTK_TYPE_LIST_BOX)

/* Flashing delay for icons (milliseconds). */
#define FLASH_TIMEOUT 500

/* Delay in milliseconds between the last stroke on the keyboard and the start
 * of the live search. */
#define SEARCH_TIMEOUT 500

enum
{
  PROP_MODEL = 1,
  PROP_SHOW_OFFLINE,
  PROP_SHOW_GROUPS,
  PROP_EMPTY,
  N_PROPS
};

enum
{
  SIG_INDIVIDUAL_ACTIVATED,
  SIG_POPUP_INDIVIDUAL_MENU,
  SIG_EVENT_ACTIVATED,
  SIG_INDIVIDUAL_TOOLTIP,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

#define NO_GROUP "X-no-group"

struct _EmpathyRosterViewPriv
{
  /* FolksIndividual (borrowed) -> GHashTable (
   * (gchar * group_name) -> EmpathyRosterContact (borrowed))
   *
   * When not using groups, this hash just have one element mapped
   * from the special NO_GROUP key. We could use it as a set but
   * I prefer to stay coherent in the way this hash is managed.
   */
  GHashTable *roster_contacts;
  /* (gchar *group_name) -> EmpathyRosterGroup (borrowed) */
  GHashTable *roster_groups;
  /* Hash of the EmpathyRosterContact currently displayed */
  GHashTable *displayed_contacts;

  guint last_event_id;
  /* queue of (Event *). The most recent events are in the head of the queue
   * so we always display the icon of the oldest one. */
  GQueue *events;
  guint flash_id;
  gboolean display_flash_event;

  guint search_id;

  gboolean show_offline;
  gboolean show_groups;
  gboolean empty;

  TpawLiveSearch *search;

  EmpathyRosterModel *model;
};

/* Prototypes to break cycles */
static void remove_from_group (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *group);

typedef struct
{
  guint id;
  FolksIndividual *individual;
  gchar *icon;
  gpointer user_data;
} Event;

static Event *
event_new (guint id,
    FolksIndividual *individual,
    const gchar *icon,
    gpointer user_data)
{
  Event *event = g_slice_new (Event);

  event->id = id;
  event->individual = g_object_ref (individual);
  event->icon = g_strdup (icon);
  event->user_data = user_data;
  return event;
}

static void
event_free (gpointer data)
{
  Event *event = data;
  g_object_unref (event->individual);
  g_free (event->icon);

  g_slice_free (Event, event);
}

static void
empathy_roster_view_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (object);

  switch (property_id)
    {
      case PROP_MODEL:
        g_value_set_object (value, self->priv->model);
        break;
      case PROP_SHOW_OFFLINE:
        g_value_set_boolean (value, self->priv->show_offline);
        break;
      case PROP_SHOW_GROUPS:
        g_value_set_boolean (value, self->priv->show_groups);
        break;
      case PROP_EMPTY:
        g_value_set_boolean (value, self->priv->empty);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
empathy_roster_view_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (object);

  switch (property_id)
    {
      case PROP_MODEL:
        g_assert (self->priv->model == NULL);
        self->priv->model = g_value_dup_object (value);
        break;
      case PROP_SHOW_OFFLINE:
        empathy_roster_view_show_offline (self, g_value_get_boolean (value));
        break;
      case PROP_SHOW_GROUPS:
        empathy_roster_view_show_groups (self, g_value_get_boolean (value));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
roster_contact_changed_cb (GtkListBoxRow *child,
    GParamSpec *spec,
    EmpathyRosterView *self)
{
  gtk_list_box_row_changed (child);
}

static GtkWidget *
add_roster_contact (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *group)
{
  GtkWidget *contact;

  contact = empathy_roster_contact_new (individual, group);

  /* Need to refilter if online is changed */
  g_signal_connect (contact, "notify::online",
      G_CALLBACK (roster_contact_changed_cb), self);

  /* Need to resort if alias is changed */
  g_signal_connect (contact, "notify::alias",
      G_CALLBACK (roster_contact_changed_cb), self);

  /* Need to resort if most recent event changed */
  g_signal_connect (contact, "notify::most-recent-event",
      G_CALLBACK (roster_contact_changed_cb), self);

  gtk_widget_show (contact);
  gtk_container_add (GTK_CONTAINER (self), contact);

  return contact;
}

static void
group_expanded_cb (GtkWidget *expander,
    GParamSpec *spec,
    EmpathyRosterGroup *group)
{
  GList *widgets, *l;

  widgets = empathy_roster_group_get_widgets (group);
  for (l = widgets; l != NULL; l = g_list_next (l))
    {
      gtk_list_box_row_changed (l->data);
    }

  g_list_free (widgets);

  empathy_contact_group_set_expanded (empathy_roster_group_get_name (group),
      gtk_expander_get_expanded (group->expander));
}

static EmpathyRosterGroup *
lookup_roster_group (EmpathyRosterView *self,
    const gchar *group)
{
  return g_hash_table_lookup (self->priv->roster_groups, group);
}

static EmpathyRosterGroup *
ensure_roster_group (EmpathyRosterView *self,
    const gchar *group)
{
  GtkWidget *roster_group;

  roster_group = (GtkWidget *) lookup_roster_group (self, group);
  if (roster_group != NULL)
    return EMPATHY_ROSTER_GROUP (roster_group);

  if (!tp_strdiff (group, EMPATHY_ROSTER_MODEL_GROUP_TOP_GROUP))
    roster_group = empathy_roster_group_new (group, "emblem-favorite-symbolic");
  else if (!tp_strdiff (group, EMPATHY_ROSTER_MODEL_GROUP_PEOPLE_NEARBY))
    roster_group = empathy_roster_group_new (group, "im-local-xmpp");
  else
    roster_group = empathy_roster_group_new (group, NULL);

  gtk_expander_set_expanded (EMPATHY_ROSTER_GROUP (roster_group)->expander,
      empathy_contact_group_get_expanded (group));

  g_signal_connect (EMPATHY_ROSTER_GROUP (roster_group)->expander,
      "notify::expanded", G_CALLBACK (group_expanded_cb), roster_group);

  gtk_widget_show (roster_group);
  gtk_container_add (GTK_CONTAINER (self), roster_group);

  g_hash_table_insert (self->priv->roster_groups, g_strdup (group),
      roster_group);

  return EMPATHY_ROSTER_GROUP (roster_group);
}

static void
update_empty (EmpathyRosterView *self,
    gboolean empty)
{
  if (self->priv->empty == empty)
    return;

  self->priv->empty = empty;
  g_object_notify (G_OBJECT (self), "empty");
}

static gboolean filter_group (EmpathyRosterView *self,
    EmpathyRosterGroup *group);

static gboolean
at_least_one_group_displayed (EmpathyRosterView *self)
{
  GHashTableIter iter;
  gpointer v;

  g_hash_table_iter_init (&iter, self->priv->roster_groups);
  while (g_hash_table_iter_next (&iter, NULL, &v))
    {
      EmpathyRosterGroup *group = EMPATHY_ROSTER_GROUP (v);

      if (filter_group (self, group))
        return TRUE;
    }

  return FALSE;
}

static void
check_if_empty (EmpathyRosterView *self)
{
  /* Roster is considered as empty if there is no contact *and* no group
   * currently displayed. */
  if (g_hash_table_size (self->priv->displayed_contacts) != 0 ||
      at_least_one_group_displayed (self))
    {
      update_empty (self, FALSE);
      return;
    }

  update_empty (self, TRUE);
}

static void
update_group_widgets (EmpathyRosterView *self,
    EmpathyRosterGroup *group,
    EmpathyRosterContact *contact,
    gboolean add)
{
  guint old_count, count;

  old_count = empathy_roster_group_get_widgets_count (group);

  if (add)
    count = empathy_roster_group_add_widget (group, GTK_WIDGET (contact));
  else
    count = empathy_roster_group_remove_widget (group, GTK_WIDGET (contact));

  if (count != old_count)
    {
      gtk_list_box_row_changed (GTK_LIST_BOX_ROW (group));

      check_if_empty (self);
    }
}

static void
add_to_group (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *group)
{
  GtkWidget *contact;
  GHashTable *contacts;
  EmpathyRosterGroup *roster_group = NULL;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  if (g_hash_table_lookup (contacts, group) != NULL)
    return;

  if (tp_strdiff (group, NO_GROUP))
    roster_group = ensure_roster_group (self, group);

  contact = add_roster_contact (self, individual, group);
  g_hash_table_insert (contacts, g_strdup (group), contact);

  if (roster_group != NULL)
    {
      update_group_widgets (self, roster_group,
          EMPATHY_ROSTER_CONTACT (contact), TRUE);
    }

  if (tp_strdiff (group, NO_GROUP) &&
      tp_strdiff (group, EMPATHY_ROSTER_MODEL_GROUP_UNGROUPED) &&
      g_hash_table_size (contacts) == 2 /* 1:Ungrouped and 2:first group */)
    {
      remove_from_group (self, individual,
          EMPATHY_ROSTER_MODEL_GROUP_UNGROUPED);
    }
}

static void
individual_favourite_change_cb (FolksIndividual *individual,
    GParamSpec *spec,
    EmpathyRosterView *self)
{
  /* We may have to refilter the contact as only favorite contacts are always
   * displayed regardless of their presence. */
  GHashTable *contacts;
  GtkWidget *contact;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  if (self->priv->show_groups)
    contact = g_hash_table_lookup (contacts,
        EMPATHY_ROSTER_MODEL_GROUP_TOP_GROUP);
  else
    contact = g_hash_table_lookup (contacts, NO_GROUP);

  if (contact == NULL)
    return;

  gtk_list_box_row_changed (GTK_LIST_BOX_ROW (contact));
}

static void
individual_added (EmpathyRosterView *self,
    FolksIndividual *individual)
{
  GHashTable *contacts;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts != NULL)
    return;

  contacts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_hash_table_insert (self->priv->roster_contacts, individual, contacts);

  if (!self->priv->show_groups)
    {
      add_to_group (self, individual, NO_GROUP);
    }
  else
    {
      GList *groups, *l;

      groups = empathy_roster_model_dup_groups_for_individual (self->priv->model,
          individual);

      if (g_list_length (groups) > 0)
        {
          for (l = groups; l != NULL; l = g_list_next (l))
            {
              add_to_group (self, individual, l->data);
            }
        }
      else
        {
          /* No group, adds to Ungrouped */
          add_to_group (self, individual, EMPATHY_ROSTER_MODEL_GROUP_UNGROUPED);
        }

      g_list_free_full (groups, g_free);
    }

  tp_g_signal_connect_object (individual, "notify::is-favourite",
      G_CALLBACK (individual_favourite_change_cb), self, 0);
}

static void
set_event_icon_on_individual (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *icon)
{
  GHashTable *contacts;
  GHashTableIter iter;
  gpointer v;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  g_hash_table_iter_init (&iter, contacts);
  while (g_hash_table_iter_next (&iter, NULL, &v))
    {
      EmpathyRosterContact *contact =v;

      empathy_roster_contact_set_event_icon (contact, icon);
    }
}

static void
flash_event (Event *event,
    EmpathyRosterView *self)
{
  set_event_icon_on_individual (self, event->individual, event->icon);
}

static void
unflash_event (Event *event,
    EmpathyRosterView *self)
{
  set_event_icon_on_individual (self, event->individual, NULL);
}

static gboolean
flash_cb (gpointer data)
{
  EmpathyRosterView *self = data;

  if (self->priv->display_flash_event)
    {
      g_queue_foreach (self->priv->events, (GFunc) flash_event, self);
      self->priv->display_flash_event = FALSE;
    }
  else
    {
      g_queue_foreach (self->priv->events, (GFunc) unflash_event, self);
      self->priv->display_flash_event = TRUE;
    }

  return TRUE;
}

static void
start_flashing (EmpathyRosterView *self)
{
  if (self->priv->flash_id != 0)
    return;

  self->priv->display_flash_event = TRUE;

  self->priv->flash_id = g_timeout_add (FLASH_TIMEOUT,
      flash_cb, self);
}

static void
stop_flashing (EmpathyRosterView *self)
{
  if (self->priv->flash_id == 0)
    return;

  g_source_remove (self->priv->flash_id);
  self->priv->flash_id = 0;
}

static void
remove_event (EmpathyRosterView *self,
    Event *event)
{
  unflash_event (event, self);
  g_queue_remove (self->priv->events, event);

  if (g_queue_get_length (self->priv->events) == 0)
    {
      stop_flashing (self);
    }
}

static void
remove_all_individual_event (EmpathyRosterView *self,
    FolksIndividual *individual)
{
  GList *l;

  for (l = g_queue_peek_head_link (self->priv->events); l != NULL;
      l = g_list_next (l))
    {
      Event *event = l->data;

      if (event->individual == individual)
        {
          remove_event (self, event);
          return;
        }
    }
}

static void
individual_removed (EmpathyRosterView *self,
    FolksIndividual *individual)
{
  GHashTable *contacts;
  GHashTableIter iter;
  gpointer key, value;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  remove_all_individual_event (self, individual);

  g_hash_table_iter_init (&iter, contacts);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const gchar *group_name = key;
      GtkWidget *contact = value;
      EmpathyRosterGroup *group;

      group = lookup_roster_group (self, group_name);
      if (group != NULL)
        {
          update_group_widgets (self, group,
              EMPATHY_ROSTER_CONTACT (contact), FALSE);
        }

      gtk_container_remove (GTK_CONTAINER (self), contact);
    }

  g_hash_table_remove (self->priv->roster_contacts, individual);
}

static void
individual_added_cb (EmpathyRosterModel *model,
    FolksIndividual *individual,
    EmpathyRosterView *self)
{
  individual_added (self, individual);
}

static void
individual_removed_cb (EmpathyRosterModel *model,
    FolksIndividual *individual,
    EmpathyRosterView *self)
{
  individual_removed (self, individual);
}

static gboolean
contact_in_top (EmpathyRosterView *self,
    EmpathyRosterContact *contact)
{
  if (!self->priv->show_groups)
    {
      /* Always display top contacts in non-group mode. */
      GList *groups;
      FolksIndividual *individual;
      gboolean result = FALSE;

      individual = empathy_roster_contact_get_individual (contact);

      groups = empathy_roster_model_dup_groups_for_individual (
          self->priv->model, individual);

      if (g_list_find_custom (groups, EMPATHY_ROSTER_MODEL_GROUP_TOP_GROUP,
            (GCompareFunc) g_strcmp0) != NULL)
        result = TRUE;

      g_list_free_full (groups, g_free);

      return result;
    }

  if (!tp_strdiff (empathy_roster_contact_get_group (contact),
          EMPATHY_ROSTER_MODEL_GROUP_TOP_GROUP))
    /* If we are displaying contacts, we only want to *always* display the
     * RosterContact which is displayed at the top; not the ones displayed in
     * the 'normal' group sections */
    return TRUE;

  return FALSE;
}

static gint
compare_roster_contacts_by_conversation_time (EmpathyRosterContact *a,
    EmpathyRosterContact *b)
{
  gint64 ts_a, ts_b;

  ts_a = empathy_roster_contact_get_most_recent_timestamp (a);
  ts_b = empathy_roster_contact_get_most_recent_timestamp (b);

  if (ts_a == ts_b) return 0;
  if (ts_a > ts_b) return -1;
  return 1;
}

static gint
compare_roster_contacts_by_alias (EmpathyRosterContact *a,
    EmpathyRosterContact *b)
{
  FolksIndividual *ind_a, *ind_b;
  const gchar *alias_a, *alias_b;

  ind_a = empathy_roster_contact_get_individual (a);
  ind_b = empathy_roster_contact_get_individual (b);

  alias_a = folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (ind_a));
  alias_b = folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (ind_b));

  return g_utf8_collate (alias_a, alias_b);
}

static gint
compare_roster_contacts_no_group (EmpathyRosterView *self,
    EmpathyRosterContact *a,
    EmpathyRosterContact *b)
{
  gboolean top_a, top_b;

  top_a = contact_in_top (self, a);
  top_b = contact_in_top (self, b);

  if (top_a == top_b)
    /* Both contacts are in the top of the roster (or not). Sort them
     * alphabetically */
    return compare_roster_contacts_by_conversation_time (a, b);
  else if (top_a)
    return -1;
  else
    return 1;
}

static gint
compare_group_names (const gchar *group_a,
    const gchar *group_b)
{
  if (!tp_strdiff (group_a, EMPATHY_ROSTER_MODEL_GROUP_TOP_GROUP))
    return -1;

  if (!tp_strdiff (group_b, EMPATHY_ROSTER_MODEL_GROUP_TOP_GROUP))
    return 1;

  if (!tp_strdiff (group_a, EMPATHY_ROSTER_MODEL_GROUP_UNGROUPED))
    return 1;
  else if (!tp_strdiff (group_b, EMPATHY_ROSTER_MODEL_GROUP_UNGROUPED))
    return -1;

  return g_utf8_collate (group_a, group_b);
}

static gint
compare_roster_contacts_with_groups (EmpathyRosterView *self,
    EmpathyRosterContact *a,
    EmpathyRosterContact *b)
{
  const gchar *group_a, *group_b;

  group_a = empathy_roster_contact_get_group (a);
  group_b = empathy_roster_contact_get_group (b);

  if (!tp_strdiff (group_a, group_b))
    /* Same group, compare the contacts */
    return compare_roster_contacts_by_conversation_time (a, b);

  /* Sort by group */
  return compare_group_names (group_a, group_b);
}

static gint
compare_roster_contacts (EmpathyRosterView *self,
    EmpathyRosterContact *a,
    EmpathyRosterContact *b)
{
  if (!self->priv->show_groups)
    return compare_roster_contacts_no_group (self, a, b);
  else
    return compare_roster_contacts_with_groups (self, a, b);
}

static gint
compare_roster_groups (EmpathyRosterGroup *a,
    EmpathyRosterGroup *b)
{
  const gchar *name_a, *name_b;

  name_a = empathy_roster_group_get_name (a);
  name_b = empathy_roster_group_get_name (b);

  return compare_group_names (name_a, name_b);
}

static gint
compare_contact_group (EmpathyRosterContact *contact,
    EmpathyRosterGroup *group)
{
  const char *contact_group, *group_name;

  contact_group = empathy_roster_contact_get_group (contact);
  group_name = empathy_roster_group_get_name (group);

  if (!tp_strdiff (contact_group, group_name))
    /* @contact is in @group, @group has to be displayed first */
    return 1;

  /* @contact is in a different group, sort by group name */
  return compare_group_names (contact_group, group_name);
}

static gint
roster_view_sort (GtkListBoxRow *a,
    GtkListBoxRow *b,
    gpointer user_data)
{
  EmpathyRosterView *self = user_data;

  if (EMPATHY_IS_ROSTER_CONTACT (a) && EMPATHY_IS_ROSTER_CONTACT (b))
    return compare_roster_contacts (self, EMPATHY_ROSTER_CONTACT (a),
        EMPATHY_ROSTER_CONTACT (b));
  else if (EMPATHY_IS_ROSTER_GROUP (a) && EMPATHY_IS_ROSTER_GROUP (b))
    return compare_roster_groups (EMPATHY_ROSTER_GROUP (a),
        EMPATHY_ROSTER_GROUP (b));
  else if (EMPATHY_IS_ROSTER_CONTACT (a) && EMPATHY_IS_ROSTER_GROUP (b))
    return compare_contact_group (EMPATHY_ROSTER_CONTACT (a),
        EMPATHY_ROSTER_GROUP (b));
  else if (EMPATHY_IS_ROSTER_GROUP (a) && EMPATHY_IS_ROSTER_CONTACT (b))
    return -1 * compare_contact_group (EMPATHY_ROSTER_CONTACT (b),
        EMPATHY_ROSTER_GROUP (a));

  g_return_val_if_reached (0);
}

static void
update_header (GtkListBoxRow *row,
    GtkListBoxRow *before,
    gpointer user_data)
{
  if (before == NULL)
    {
      /* No separator before the first row */
      gtk_list_box_row_set_header (row, NULL);
      return;
    }

  if (gtk_list_box_row_get_header (row) != NULL)
    return;

  gtk_list_box_row_set_header (row,
      gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
}

static gboolean
is_searching (EmpathyRosterView *self)
{
  if (self->priv->search == NULL)
    return FALSE;

  return gtk_widget_get_visible (GTK_WIDGET (self->priv->search));
}

static void
add_to_displayed (EmpathyRosterView *self,
    EmpathyRosterContact *contact)
{
  FolksIndividual *individual;
  GHashTable *contacts;
  GHashTableIter iter;
  gpointer k;

  if (g_hash_table_lookup (self->priv->displayed_contacts, contact) != NULL)
    return;

  g_hash_table_add (self->priv->displayed_contacts, contact);
  update_empty (self, FALSE);

  /* Groups of this contact may now be displayed if we just displays the first
   * child in this group. */

  if (!self->priv->show_groups)
    return;

  individual = empathy_roster_contact_get_individual (contact);
  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  g_hash_table_iter_init (&iter, contacts);
  while (g_hash_table_iter_next (&iter, &k, NULL))
    {
      const gchar *group_name = k;
      GtkListBoxRow *group;

      group = g_hash_table_lookup (self->priv->roster_groups, group_name);
      if (group == NULL)
        continue;

      gtk_list_box_row_changed (group);
    }
}

static void
remove_from_displayed (EmpathyRosterView *self,
    EmpathyRosterContact *contact)
{
  g_hash_table_remove (self->priv->displayed_contacts, contact);

  check_if_empty (self);
}

static gboolean
contact_is_favorite (EmpathyRosterContact *contact)
{
  FolksIndividual *individual;

  individual = empathy_roster_contact_get_individual (contact);

  return folks_favourite_details_get_is_favourite (
      FOLKS_FAVOURITE_DETAILS (individual));
}

/**
 * check if @contact should be displayed according to @self's current status
 * and without consideration for the state of @contact's groups.
 */
static gboolean
contact_should_be_displayed (EmpathyRosterView *self,
    EmpathyRosterContact *contact)
{
  if (is_searching (self))
    {
      FolksIndividual *individual;

      individual = empathy_roster_contact_get_individual (contact);

      return empathy_individual_match_string (individual,
          tpaw_live_search_get_text (self->priv->search),
          tpaw_live_search_get_words (self->priv->search));
    }

  if (self->priv->show_offline)
      return TRUE;

  if (contact_in_top (self, contact) &&
      contact_is_favorite (contact))
    /* Favorite top contacts are always displayed */
    return TRUE;

  return empathy_roster_contact_is_online (contact);
}


static gboolean
filter_contact (EmpathyRosterView *self,
    EmpathyRosterContact *contact)
{
  gboolean displayed;

  displayed = contact_should_be_displayed (self, contact);

  if (self->priv->show_groups)
    {
      const gchar *group_name;
      EmpathyRosterGroup *group;

      group_name = empathy_roster_contact_get_group (contact);
      group = lookup_roster_group (self, group_name);

      if (group != NULL)
        {
          /* When searching, always display even if the group is closed */
          if (!is_searching (self) &&
              !gtk_expander_get_expanded (group->expander))
            displayed = FALSE;
        }
    }

  if (displayed)
    {
      add_to_displayed (self, contact);
    }
  else
    {
      remove_from_displayed (self, contact);
    }

  return displayed;
}

static gboolean
filter_group (EmpathyRosterView *self,
    EmpathyRosterGroup *group)
{
  GList *widgets, *l;
  gboolean result = FALSE;

  /* Display the group if it contains at least one displayed contact */
  widgets = empathy_roster_group_get_widgets (group);
  for (l = widgets; l != NULL; l = g_list_next (l))
    {
      EmpathyRosterContact *contact = l->data;

      if (contact_should_be_displayed (self, contact))
        {
          result = TRUE;
          break;
        }
    }

  g_list_free (widgets);

  return result;
}

static gboolean
filter_list (GtkListBoxRow *child,
    gpointer user_data)
{
  EmpathyRosterView *self = user_data;

  if (EMPATHY_IS_ROSTER_CONTACT (child))
    return filter_contact (self, EMPATHY_ROSTER_CONTACT (child));

  else if (EMPATHY_IS_ROSTER_GROUP (child))
    return filter_group (self, EMPATHY_ROSTER_GROUP (child));

  g_return_val_if_reached (FALSE);
}

static void
populate_view (EmpathyRosterView *self)
{
  GList *individuals, *l;

  individuals = empathy_roster_model_get_individuals (self->priv->model);
  for (l = individuals; l != NULL; l = g_list_next (l))
    {
      FolksIndividual *individual = l->data;

      individual_added (self, individual);
    }

  g_list_free (individuals);
}

static void
remove_from_group (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *group)
{
  GHashTable *contacts;
  GtkWidget *contact;
  EmpathyRosterGroup *roster_group;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  contact = g_hash_table_lookup (contacts, group);
  if (contact == NULL)
    return;

  g_hash_table_remove (contacts, group);

  if (g_hash_table_size (contacts) == 0)
    {
      add_to_group (self, individual, EMPATHY_ROSTER_MODEL_GROUP_UNGROUPED);
    }

  roster_group = lookup_roster_group (self, group);

  if (roster_group != NULL)
    {
      update_group_widgets (self, roster_group,
          EMPATHY_ROSTER_CONTACT (contact), FALSE);
    }

  gtk_container_remove (GTK_CONTAINER (self), contact);
}

static void
groups_changed_cb (EmpathyRosterModel *model,
    FolksIndividual *individual,
    const gchar *group,
    gboolean is_member,
    EmpathyRosterView *self)
{
  if (!self->priv->show_groups)
    {
      gtk_list_box_invalidate_sort (GTK_LIST_BOX (self));
      return;
    }

  if (is_member)
    {
      add_to_group (self, individual, group);
    }
  else
    {
      remove_from_group (self, individual, group);
    }
}

static void
empathy_roster_view_constructed (GObject *object)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_roster_view_parent_class)->constructed;

  if (chain_up != NULL)
    chain_up (object);

  g_assert (EMPATHY_IS_ROSTER_MODEL (self->priv->model));

  /* Get saved group states. */
  empathy_contact_groups_get_all ();

  populate_view (self);

  tp_g_signal_connect_object (self->priv->model, "individual-added",
      G_CALLBACK (individual_added_cb), self, 0);
  tp_g_signal_connect_object (self->priv->model, "individual-removed",
      G_CALLBACK (individual_removed_cb), self, 0);
  tp_g_signal_connect_object (self->priv->model, "groups-changed",
      G_CALLBACK (groups_changed_cb), self, 0);

  gtk_list_box_set_sort_func (GTK_LIST_BOX (self),
      roster_view_sort, self, NULL);

  gtk_list_box_set_header_func (GTK_LIST_BOX (self), update_header, self, NULL);

  gtk_list_box_set_filter_func (GTK_LIST_BOX (self), filter_list, self, NULL);

  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (self), FALSE);
}

static void
clear_view (EmpathyRosterView *self)
{
  g_hash_table_remove_all (self->priv->roster_contacts);
  g_hash_table_remove_all (self->priv->roster_groups);
  g_hash_table_remove_all (self->priv->displayed_contacts);

  gtk_container_foreach (GTK_CONTAINER (self),
      (GtkCallback) gtk_widget_destroy, NULL);
}

static void
empathy_roster_view_dispose (GObject *object)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_roster_view_parent_class)->dispose;

  /* Start by clearing the view so our internal hash tables are cleared from
   * objects being destroyed. */
  clear_view (self);

  stop_flashing (self);

  empathy_roster_view_set_live_search (self, NULL);
  g_clear_object (&self->priv->model);

  if (self->priv->search_id != 0)
    {
      g_source_remove (self->priv->search_id);
      self->priv->search_id = 0;
    }

  if (chain_up != NULL)
    chain_up (object);
}

static void
empathy_roster_view_finalize (GObject *object)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_roster_view_parent_class)->finalize;

  g_hash_table_unref (self->priv->roster_contacts);
  g_hash_table_unref (self->priv->roster_groups);
  g_hash_table_unref (self->priv->displayed_contacts);
  g_queue_free_full (self->priv->events, event_free);

  if (chain_up != NULL)
    chain_up (object);
}

static void
empathy_roster_view_row_activated (GtkListBox *box,
    GtkListBoxRow *row)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (box);
  EmpathyRosterContact *contact;
  FolksIndividual *individual;
  GList *l;

  if (!EMPATHY_IS_ROSTER_CONTACT (row))
    return;

  contact = EMPATHY_ROSTER_CONTACT (row);
  individual = empathy_roster_contact_get_individual (contact);

  /* Activate the oldest event associated with this contact, if any */
  for (l = g_queue_peek_tail_link (self->priv->events); l != NULL;
      l = g_list_previous (l))
    {
      Event *event = l->data;

      if (event->individual == individual)
        {
          g_signal_emit (box, signals[SIG_EVENT_ACTIVATED], 0, individual,
              event->user_data);
          return;
        }
    }

  g_signal_emit (box, signals[SIG_INDIVIDUAL_ACTIVATED], 0, individual);
}

static void
fire_popup_individual_menu (EmpathyRosterView *self,
    GtkListBoxRow *row,
    guint button,
    guint time)
{
  EmpathyRosterContact *contact;
  FolksIndividual *individual;
  const gchar *active_group;

  if (!EMPATHY_IS_ROSTER_CONTACT (row))
    return;

  contact = EMPATHY_ROSTER_CONTACT (row);
  individual = empathy_roster_contact_get_individual (contact);

  active_group = empathy_roster_contact_get_group (contact);
  g_signal_emit (self, signals[SIG_POPUP_INDIVIDUAL_MENU], 0,
      active_group, individual, button, time);
}

static gboolean
empathy_roster_view_button_press_event (GtkWidget *widget,
    GdkEventButton *event)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (widget);
  gboolean (*chain_up) (GtkWidget *, GdkEventButton *) =
      ((GtkWidgetClass *) empathy_roster_view_parent_class)->button_press_event;

  if (event->button == 3)
    {
      GtkListBoxRow *row;

      row = gtk_list_box_get_row_at_y (GTK_LIST_BOX (self), event->y);

      if (row != NULL)
        {
          gtk_list_box_select_row (GTK_LIST_BOX (self), row);

          fire_popup_individual_menu (self, row, event->button, event->time);
        }
    }

  return chain_up (widget, event);
}

static gboolean
empathy_roster_view_key_press_event (GtkWidget *widget,
    GdkEventKey *event)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (widget);
  gboolean (*chain_up) (GtkWidget *, GdkEventKey *) =
      ((GtkWidgetClass *) empathy_roster_view_parent_class)->key_press_event;

  if (event->keyval == GDK_KEY_Menu)
    {
      GtkListBoxRow *row;

      row = gtk_list_box_get_selected_row (GTK_LIST_BOX (self));

      if (row != NULL)
        fire_popup_individual_menu (self, row, 0, event->time);
    }

  return chain_up (widget, event);
}

/**
 * @out_row: (out) (allow-none)
 */
FolksIndividual *
empathy_roster_view_get_individual_at_y (EmpathyRosterView *self,
    gint y,
    GtkListBoxRow **out_row)
{
  GtkListBoxRow *row;

  row = gtk_list_box_get_row_at_y (GTK_LIST_BOX (self), y);

  if (out_row != NULL)
    *out_row = row;

  if (!EMPATHY_IS_ROSTER_CONTACT (row))
    return NULL;

  return empathy_roster_contact_get_individual (EMPATHY_ROSTER_CONTACT (row));
}

const gchar *
empathy_roster_view_get_group_at_y (EmpathyRosterView *self,
    gint y)
{
  GtkListBoxRow *row;

  row = gtk_list_box_get_row_at_y (GTK_LIST_BOX (self), y);

  if (EMPATHY_IS_ROSTER_CONTACT (row))
    return empathy_roster_contact_get_group (EMPATHY_ROSTER_CONTACT (row));
  else if (EMPATHY_IS_ROSTER_GROUP (row))
    return empathy_roster_group_get_name (EMPATHY_ROSTER_GROUP (row));

  return NULL;
}

static gboolean
empathy_roster_view_query_tooltip (GtkWidget *widget,
    gint x,
    gint y,
    gboolean keyboard_mode,
    GtkTooltip *tooltip)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (widget);
  FolksIndividual *individual;
  gboolean result;
  GtkListBoxRow *row;

  individual = empathy_roster_view_get_individual_at_y (self, y, &row);
  if (individual == NULL)
    return FALSE;

  g_signal_emit (self, signals[SIG_INDIVIDUAL_TOOLTIP], 0,
      individual, keyboard_mode, tooltip, &result);

  if (result)
    {
      GtkAllocation allocation;

      gtk_widget_get_allocation (GTK_WIDGET (row), &allocation);
      gtk_tooltip_set_tip_area (tooltip, (GdkRectangle *) &allocation);
    }

  return result;
}

static void
empathy_roster_view_remove (GtkContainer *container,
    GtkWidget *widget)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (container);
  void (*chain_up) (GtkContainer *, GtkWidget *) =
      ((GtkContainerClass *) empathy_roster_view_parent_class)->remove;

  chain_up (container, widget);

  if (EMPATHY_IS_ROSTER_CONTACT (widget))
    remove_from_displayed (self, (EmpathyRosterContact *) widget);
}

static void
empathy_roster_view_class_init (
    EmpathyRosterViewClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GtkListBoxClass *box_class = GTK_LIST_BOX_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkContainerClass *container_class = GTK_CONTAINER_CLASS (klass);
  GParamSpec *spec;

  oclass->get_property = empathy_roster_view_get_property;
  oclass->set_property = empathy_roster_view_set_property;
  oclass->constructed = empathy_roster_view_constructed;
  oclass->dispose = empathy_roster_view_dispose;
  oclass->finalize = empathy_roster_view_finalize;

  widget_class->button_press_event = empathy_roster_view_button_press_event;
  widget_class->key_press_event = empathy_roster_view_key_press_event;
  widget_class->query_tooltip = empathy_roster_view_query_tooltip;

  container_class->remove = empathy_roster_view_remove;

  box_class->row_activated = empathy_roster_view_row_activated;

  spec = g_param_spec_object ("model", "Model",
      "EmpathyRosterModel",
      EMPATHY_TYPE_ROSTER_MODEL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_MODEL, spec);

  spec = g_param_spec_boolean ("show-offline", "Show Offline",
      "Show offline contacts",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_SHOW_OFFLINE, spec);

  spec = g_param_spec_boolean ("show-groups", "Show Groups",
      "Show groups",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_SHOW_GROUPS, spec);

  spec = g_param_spec_boolean ("empty", "Empty",
      "Is the view currently empty?",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_EMPTY, spec);

  signals[SIG_INDIVIDUAL_ACTIVATED] = g_signal_new ("individual-activated",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      1, FOLKS_TYPE_INDIVIDUAL);

  signals[SIG_POPUP_INDIVIDUAL_MENU] = g_signal_new ("popup-individual-menu",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      4, G_TYPE_STRING, FOLKS_TYPE_INDIVIDUAL, G_TYPE_UINT,
          G_TYPE_UINT);

  signals[SIG_EVENT_ACTIVATED] = g_signal_new ("event-activated",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      2, FOLKS_TYPE_INDIVIDUAL, G_TYPE_POINTER);

  signals[SIG_INDIVIDUAL_TOOLTIP] = g_signal_new ("individual-tooltip",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, g_signal_accumulator_true_handled, NULL, NULL,
      G_TYPE_BOOLEAN,
      3, FOLKS_TYPE_INDIVIDUAL, G_TYPE_BOOLEAN, GTK_TYPE_TOOLTIP);

  g_type_class_add_private (klass, sizeof (EmpathyRosterViewPriv));
}

static void
empathy_roster_view_init (EmpathyRosterView *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_ROSTER_VIEW, EmpathyRosterViewPriv);

  self->priv->roster_contacts = g_hash_table_new_full (NULL, NULL,
      NULL, (GDestroyNotify) g_hash_table_unref);
  self->priv->roster_groups = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, NULL);
  self->priv->displayed_contacts = g_hash_table_new (NULL, NULL);

  self->priv->events = g_queue_new ();

  self->priv->empty = TRUE;
}

GtkWidget *
empathy_roster_view_new (EmpathyRosterModel *model)
{
  g_return_val_if_fail (EMPATHY_IS_ROSTER_MODEL (model), NULL);

  return g_object_new (EMPATHY_TYPE_ROSTER_VIEW,
      "model", model,
      NULL);
}

void
empathy_roster_view_show_offline (EmpathyRosterView *self,
    gboolean show)
{
  if (self->priv->show_offline == show)
    return;

  self->priv->show_offline = show;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self));

  g_object_notify (G_OBJECT (self), "show-offline");
}

void
empathy_roster_view_show_groups (EmpathyRosterView *self,
    gboolean show)
{
  if (self->priv->show_groups == show)
    return;

  self->priv->show_groups = show;

  /* TODO: block sort/filter? */
  clear_view (self);
  populate_view (self);

  g_object_notify (G_OBJECT (self), "show-groups");
}

static void
select_first_contact (EmpathyRosterView *self)
{
  GList *children, *l;

  children = gtk_container_get_children (GTK_CONTAINER (self));
  for (l = children; l != NULL; l = g_list_next (l))
    {
      GtkWidget *child = l->data;

      if (!gtk_widget_get_child_visible (child))
        continue;

      if (!EMPATHY_IS_ROSTER_CONTACT (child))
        continue;

      gtk_list_box_select_row (GTK_LIST_BOX (self), GTK_LIST_BOX_ROW (child));
      break;
    }

  g_list_free (children);
}

static gboolean
search_timeout_cb (EmpathyRosterView *self)
{
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self));

  select_first_contact (self);

  self->priv->search_id = 0;
  return G_SOURCE_REMOVE;
}

static void
search_text_notify_cb (TpawLiveSearch *search,
    GParamSpec *pspec,
    EmpathyRosterView *self)
{
  if (self->priv->search_id != 0)
    g_source_remove (self->priv->search_id);

  self->priv->search_id = g_timeout_add (SEARCH_TIMEOUT,
      (GSourceFunc) search_timeout_cb, self);
}

static void
search_activate_cb (GtkWidget *search,
  EmpathyRosterView *self)
{
  GtkListBox *box = GTK_LIST_BOX (self);
  GtkListBoxRow *row;

  row = gtk_list_box_get_selected_row (box);
  if (row == NULL)
    return;

  empathy_roster_view_row_activated (box, row);
}

void
empathy_roster_view_set_live_search (EmpathyRosterView *self,
    TpawLiveSearch *search)
{
  if (self->priv->search != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->priv->search,
          search_text_notify_cb, self);
      g_signal_handlers_disconnect_by_func (self->priv->search,
          search_activate_cb, self);

      g_clear_object (&self->priv->search);
    }

  if (search == NULL)
    return;

  self->priv->search = g_object_ref (search);

  g_signal_connect (self->priv->search, "notify::text",
      G_CALLBACK (search_text_notify_cb), self);
  g_signal_connect (self->priv->search, "activate",
      G_CALLBACK (search_activate_cb), self);
}

gboolean
empathy_roster_view_is_empty (EmpathyRosterView *self)
{
  return self->priv->empty;
}

gboolean
empathy_roster_view_is_searching (EmpathyRosterView *self)
{
  return (self->priv->search != NULL &&
      gtk_widget_get_visible (GTK_WIDGET (self->priv->search)));
}

/* Don't use EmpathyEvent as I prefer to keep this object not too specific to
 * Empathy's internals. */
guint
empathy_roster_view_add_event (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *icon,
    gpointer user_data)
{
  GHashTable *contacts;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return 0;

  self->priv->last_event_id++;

  g_queue_push_head (self->priv->events,
      event_new (self->priv->last_event_id, individual, icon, user_data));

  start_flashing (self);

  return self->priv->last_event_id;
}

void
empathy_roster_view_remove_event (EmpathyRosterView *self,
    guint event_id)
{
  GList *l;

  for (l = g_queue_peek_head_link (self->priv->events); l != NULL;
      l = g_list_next (l))
    {
      Event *event = l->data;

      if (event->id == event_id)
        {
          remove_event (self, event);
          return;
        }
    }
}

FolksIndividual *
empathy_roster_view_get_selected_individual (EmpathyRosterView *self)
{
  GtkListBoxRow *row;

  row = gtk_list_box_get_selected_row (GTK_LIST_BOX (self));

  if (!EMPATHY_IS_ROSTER_CONTACT (row))
    return NULL;

  return empathy_roster_contact_get_individual (EMPATHY_ROSTER_CONTACT (row));
}
