#include "config.h"
#include "empathy-roster-contact.h"

#include <glib/gi18n-lib.h>
#include <tp-account-widgets/tpaw-images.h>
#include <tp-account-widgets/tpaw-pixbuf-utils.h>

#include "empathy-ui-utils.h"
#include "empathy-utils.h"

G_DEFINE_TYPE (EmpathyRosterContact, empathy_roster_contact, GTK_TYPE_LIST_BOX_ROW)

#define AVATAR_SIZE 48

enum
{
  PROP_INDIVIDIUAL = 1,
  PROP_GROUP,
  PROP_ONLINE,
  PROP_ALIAS,
  PROP_MOST_RECENT_EVENT,
  N_PROPS
};

/*
enum
{
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];
*/

struct _EmpathyRosterContactPriv
{
  FolksIndividual *individual;
  EmpathyContact *contact;
  gchar *group;

  TplLogManager *log_manager;
  TplEvent *most_recent_event;

  GtkWidget *avatar;
  GtkWidget *first_line_alig;
  GtkWidget *alias;
  GtkWidget *presence_msg;
  GtkWidget *most_recent_msg;
  GtkWidget *presence_icon;
  GtkWidget *phone_icon;

  /* If not NULL, used instead of the individual's presence icon */
  gchar *event_icon;

  gboolean online;
};

static const gchar *
get_alias (EmpathyRosterContact *self)
{
  return folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (
        self->priv->individual));
}

static void
empathy_roster_contact_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyRosterContact *self = EMPATHY_ROSTER_CONTACT (object);

  switch (property_id)
    {
      case PROP_INDIVIDIUAL:
        g_value_set_object (value, self->priv->individual);
        break;
      case PROP_GROUP:
        g_value_set_string (value, self->priv->group);
        break;
      case PROP_ONLINE:
        g_value_set_boolean (value, self->priv->online);
        break;
      case PROP_ALIAS:
        g_value_set_string (value, get_alias (self));
        break;
      case PROP_MOST_RECENT_EVENT:
        g_value_set_object (value, self->priv->most_recent_event);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
empathy_roster_contact_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyRosterContact *self = EMPATHY_ROSTER_CONTACT (object);

  switch (property_id)
    {
      case PROP_INDIVIDIUAL:
        g_assert (self->priv->individual == NULL); /* construct only */
        self->priv->individual = g_value_dup_object (value);
        break;
      case PROP_GROUP:
        g_assert (self->priv->group == NULL); /* construct only */
        self->priv->group = g_value_dup_string (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

gint64
empathy_roster_contact_get_most_recent_timestamp (EmpathyRosterContact *contact)
{
  if (contact->priv->most_recent_event) {
    return tpl_event_get_timestamp (contact->priv->most_recent_event);
  }
  return 0;
}

static const gchar*
get_most_recent_message (EmpathyRosterContact *contact)
{
  if (contact->priv->most_recent_event) {
    return tpl_text_event_get_message (TPL_TEXT_EVENT(contact->priv->most_recent_event));
  }
  return NULL;
}

static void
avatar_loaded_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  TpWeakRef *wr = user_data;
  EmpathyRosterContact *self;
  GdkPixbuf *pixbuf;

  self = tp_weak_ref_dup_object (wr);
  if (self == NULL)
    goto out;

  pixbuf = empathy_pixbuf_avatar_from_individual_scaled_finish (
      FOLKS_INDIVIDUAL (source), result, NULL);

  if (pixbuf == NULL)
    {
      pixbuf = tpaw_pixbuf_from_icon_name_sized (
          TPAW_IMAGE_AVATAR_DEFAULT, AVATAR_SIZE);
    }

  gtk_image_set_from_pixbuf (GTK_IMAGE (self->priv->avatar), pixbuf);
  g_object_unref (pixbuf);

  g_object_unref (self);

out:
  tp_weak_ref_destroy (wr);
}

static void
update_most_recent_msg (EmpathyRosterContact *self)
{
  const gchar* msg = get_most_recent_message (self);

  if (tp_str_empty (msg))
    {
      gtk_alignment_set (GTK_ALIGNMENT (self->priv->first_line_alig),
          0, 0.5, 1, 1);
      gtk_widget_hide (self->priv->most_recent_msg);
    }
  else
    {
      gchar *tmp = g_strdup (msg);
      if (strchr(tmp, '\n')) strchr(tmp, '\n')[0] = 0;
      gtk_label_set_text (GTK_LABEL (self->priv->most_recent_msg), tmp);
      gtk_alignment_set (GTK_ALIGNMENT (self->priv->first_line_alig),
          0, 0.75, 1, 1);
      gtk_misc_set_alignment (GTK_MISC (self->priv->most_recent_msg), 0, 0.25);
      g_free (tmp);
    }
}

static void
update_avatar (EmpathyRosterContact *self)
{
  empathy_pixbuf_avatar_from_individual_scaled_async (self->priv->individual,
      AVATAR_SIZE, AVATAR_SIZE, NULL, avatar_loaded_cb,
      tp_weak_ref_new (self, NULL, NULL));
}

static void
avatar_changed_cb (FolksIndividual *individual,
    GParamSpec *spec,
    EmpathyRosterContact *self)
{
  update_avatar (self);
}

static void
update_alias (EmpathyRosterContact *self)
{
  gtk_label_set_text (GTK_LABEL (self->priv->alias), get_alias (self));

  g_object_notify (G_OBJECT (self), "alias");
}

static void
alias_changed_cb (FolksIndividual *individual,
    GParamSpec *spec,
    EmpathyRosterContact *self)
{
  update_alias (self);
}

static void
update_presence_msg (EmpathyRosterContact *self)
{
  const gchar *msg;
  GStrv types;

  msg = folks_presence_details_get_presence_message (
      FOLKS_PRESENCE_DETAILS (self->priv->individual));

  if (tp_str_empty (msg))
    {
      /* Just display the alias in the center of the row */
      gtk_alignment_set (GTK_ALIGNMENT (self->priv->first_line_alig),
          0, 0.5, 1, 1);

      gtk_widget_hide (self->priv->presence_msg);
    }
  else
    {
      FolksPresenceType type;

      type = folks_presence_details_get_presence_type (
          FOLKS_PRESENCE_DETAILS (self->priv->individual));
      if (type == FOLKS_PRESENCE_TYPE_ERROR)
        {
          gchar *tmp;

          /* Add a prefix explaining that something goes wrong when trying to
           * fetch contact's presence. */
          tmp = g_strdup_printf (_("Server cannot find contact: %s"), msg);
          gtk_label_set_text (GTK_LABEL (self->priv->presence_msg), tmp);

          g_free (tmp);
        }
      else
        {
          gtk_label_set_text (GTK_LABEL (self->priv->presence_msg), msg);
        }

      gtk_alignment_set (GTK_ALIGNMENT (self->priv->first_line_alig),
          0, 0.75, 1, 1);
      gtk_misc_set_alignment (GTK_MISC (self->priv->presence_msg), 0, 0.25);

      gtk_widget_show (self->priv->presence_msg);
    }

  types = (GStrv) empathy_individual_get_client_types (self->priv->individual);

  gtk_widget_set_visible (self->priv->phone_icon,
      empathy_client_types_contains_mobile_device (types));
}

static void
presence_message_changed_cb (FolksIndividual *individual,
    GParamSpec *spec,
    EmpathyRosterContact *self)
{
  update_presence_msg (self);
}

static void
update_presence_icon (EmpathyRosterContact *self)
{
  const gchar *icon;

  if (self->priv->event_icon == NULL)
    icon = empathy_icon_name_for_individual (self->priv->individual);
  else
    icon = self->priv->event_icon;

  gtk_image_set_from_icon_name (GTK_IMAGE (self->priv->presence_icon), icon,
      GTK_ICON_SIZE_MENU);
}

static void
update_online (EmpathyRosterContact *self)
{
  FolksPresenceType presence;
  gboolean online;

  presence = folks_presence_details_get_presence_type (
      FOLKS_PRESENCE_DETAILS (self->priv->individual));

  switch (presence)
    {
      case FOLKS_PRESENCE_TYPE_UNSET:
      case FOLKS_PRESENCE_TYPE_OFFLINE:
      case FOLKS_PRESENCE_TYPE_UNKNOWN:
      case FOLKS_PRESENCE_TYPE_ERROR:
        online = FALSE;
        break;

      case FOLKS_PRESENCE_TYPE_AVAILABLE:
      case FOLKS_PRESENCE_TYPE_AWAY:
      case FOLKS_PRESENCE_TYPE_EXTENDED_AWAY:
      case FOLKS_PRESENCE_TYPE_HIDDEN:
      case FOLKS_PRESENCE_TYPE_BUSY:
        online = TRUE;
        break;

      default:
        g_warning ("Unknown FolksPresenceType: %d", presence);
        online = FALSE;
    }

  if (self->priv->online == online)
    return;

  self->priv->online = online;
  g_object_notify (G_OBJECT (self), "online");
}

static void
presence_status_changed_cb (FolksIndividual *individual,
    GParamSpec *spec,
    EmpathyRosterContact *self)
{
  update_presence_icon (self);
  update_online (self);
}

static void
get_filtered_events (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  EmpathyRosterContact *contact = EMPATHY_ROSTER_CONTACT (user_data);
  GError *error;
  GList *events;

  error = NULL;
  if (!tpl_log_manager_get_filtered_events_finish (contact->priv->log_manager, res, &events, &error))
    {
      g_warning ("Unable to get events: %s", error->message);
      g_error_free (error);
      goto out;
    }

  if (events) {
    contact->priv->most_recent_event = TPL_EVENT (events->data);
    g_object_notify (G_OBJECT (contact), "most-recent-event");
    update_most_recent_msg (contact);
  }

 out:
  return;
}

static void
empathy_roster_contact_constructed (GObject *object)
{
  EmpathyRosterContact *self = EMPATHY_ROSTER_CONTACT (object);
  TplEntity *tpl_entity;
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_roster_contact_parent_class)->constructed;

  if (chain_up != NULL)
    chain_up (object);

  g_assert (FOLKS_IS_INDIVIDUAL (self->priv->individual));

  self->priv->contact = empathy_contact_dup_best_for_action (
                  self->priv->individual,
                  EMPATHY_ACTION_CHAT);

  self->priv->log_manager = tpl_log_manager_dup_singleton ();

  tpl_entity = tpl_entity_new_from_tp_contact (
                  empathy_contact_get_tp_contact (self->priv->contact),
                  TPL_ENTITY_CONTACT);
  tpl_log_manager_get_filtered_events_async(
                  self->priv->log_manager,
                  empathy_contact_get_account (self->priv->contact),
                  tpl_entity,
                  TPL_EVENT_MASK_TEXT,
                  1,
                  NULL,
                  NULL,
                  get_filtered_events,
                  object);

  tp_g_signal_connect_object (self->priv->individual, "notify::avatar",
      G_CALLBACK (avatar_changed_cb), self, 0);
  tp_g_signal_connect_object (self->priv->individual, "notify::alias",
      G_CALLBACK (alias_changed_cb), self, 0);
  tp_g_signal_connect_object (self->priv->individual,
      "notify::presence-message",
      G_CALLBACK (presence_message_changed_cb), self, 0);
  tp_g_signal_connect_object (self->priv->individual, "notify::presence-status",
      G_CALLBACK (presence_status_changed_cb), self, 0);

  update_avatar (self);
  update_alias (self);
  update_presence_msg (self);
  update_presence_icon (self);

  update_online (self);
}

static void
empathy_roster_contact_dispose (GObject *object)
{
  EmpathyRosterContact *self = EMPATHY_ROSTER_CONTACT (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_roster_contact_parent_class)->dispose;

  g_clear_object (&self->priv->individual);

  if (chain_up != NULL)
    chain_up (object);
}

static void
empathy_roster_contact_finalize (GObject *object)
{
  EmpathyRosterContact *self = EMPATHY_ROSTER_CONTACT (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_roster_contact_parent_class)->finalize;

  g_free (self->priv->group);
  g_free (self->priv->event_icon);
  g_object_unref (self->priv->log_manager);

  if (chain_up != NULL)
    chain_up (object);
}

static void
empathy_roster_contact_class_init (
    EmpathyRosterContactClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GParamSpec *spec;

  oclass->get_property = empathy_roster_contact_get_property;
  oclass->set_property = empathy_roster_contact_set_property;
  oclass->constructed = empathy_roster_contact_constructed;
  oclass->dispose = empathy_roster_contact_dispose;
  oclass->finalize = empathy_roster_contact_finalize;

  spec = g_param_spec_object ("individual", "Individual",
      "FolksIndividual",
      FOLKS_TYPE_INDIVIDUAL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_INDIVIDIUAL, spec);

  spec = g_param_spec_string ("group", "Group",
      "Group of this widget, or NULL",
      NULL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_GROUP, spec);

  spec = g_param_spec_boolean ("online", "Online",
      "TRUE if Individual is online",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_ONLINE, spec);

  spec = g_param_spec_string ("alias", "Alias",
      "The Alias of the individual displayed in the widget",
      NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_ALIAS, spec);

  spec = g_param_spec_object ("most-recent-event", "Most recent event",
      "Most recent event",
      TPL_TYPE_EVENT,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_MOST_RECENT_EVENT, spec);

  g_type_class_add_private (klass, sizeof (EmpathyRosterContactPriv));
}

static void
empathy_roster_contact_init (EmpathyRosterContact *self)
{
  GtkWidget *alig, *main_box, *box, *first_line_box;
  GtkStyleContext *context;

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_ROSTER_CONTACT, EmpathyRosterContactPriv);

  alig = gtk_alignment_new (0.5, 0.5, 1, 1);
  gtk_widget_show (alig);
  gtk_alignment_set_padding (GTK_ALIGNMENT (alig), 4, 4, 4, 12);

  main_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  /* Avatar */
  self->priv->avatar = gtk_image_new ();

  gtk_widget_set_size_request (self->priv->avatar, AVATAR_SIZE, AVATAR_SIZE);

  gtk_box_pack_start (GTK_BOX (main_box), self->priv->avatar, FALSE, FALSE, 0);
  gtk_widget_show (self->priv->avatar);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* Alias and phone icon */
  self->priv->first_line_alig = gtk_alignment_new (0, 0.5, 1, 1);
  first_line_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  self->priv->alias = gtk_label_new (NULL);
  gtk_label_set_ellipsize (GTK_LABEL (self->priv->alias), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start (GTK_BOX (first_line_box), self->priv->alias,
      FALSE, FALSE, 0);
  gtk_misc_set_alignment (GTK_MISC (self->priv->alias), 0, 0.5);
  gtk_widget_show (self->priv->alias);

  self->priv->phone_icon = gtk_image_new_from_icon_name ("phone-symbolic",
      GTK_ICON_SIZE_MENU);
  gtk_misc_set_alignment (GTK_MISC (self->priv->phone_icon), 0, 0.5);
  gtk_box_pack_start (GTK_BOX (first_line_box), self->priv->phone_icon,
      TRUE, TRUE, 0);

  gtk_container_add (GTK_CONTAINER (self->priv->first_line_alig),
      first_line_box);
  gtk_widget_show (self->priv->first_line_alig);

  gtk_box_pack_start (GTK_BOX (box), self->priv->first_line_alig,
      TRUE, TRUE, 0);
  gtk_widget_show (first_line_box);

  gtk_box_pack_start (GTK_BOX (main_box), box, TRUE, TRUE, 0);
  gtk_widget_show (box);

  /* Presence */
  self->priv->presence_msg = gtk_label_new (NULL);
  gtk_label_set_ellipsize (GTK_LABEL (self->priv->presence_msg),
      PANGO_ELLIPSIZE_END);
  /*
  gtk_box_pack_start (GTK_BOX (box), self->priv->presence_msg, TRUE, TRUE, 0);
  gtk_widget_show (self->priv->presence_msg);
  */

  context = gtk_widget_get_style_context (self->priv->presence_msg);
  gtk_style_context_add_class (context, GTK_STYLE_CLASS_DIM_LABEL);

  /* Most recent message */
  self->priv->most_recent_msg = gtk_label_new (NULL);
  gtk_label_set_ellipsize (GTK_LABEL (self->priv->most_recent_msg),
      PANGO_ELLIPSIZE_END);
  gtk_box_pack_start (GTK_BOX (box), self->priv->most_recent_msg, TRUE, TRUE, 0);
  gtk_widget_show (self->priv->most_recent_msg);

  context = gtk_widget_get_style_context (self->priv->most_recent_msg);
  gtk_style_context_add_class (context, GTK_STYLE_CLASS_DIM_LABEL);

  /* Presence icon */
  self->priv->presence_icon = gtk_image_new ();

  gtk_box_pack_start (GTK_BOX (main_box), self->priv->presence_icon,
      FALSE, FALSE, 0);
  gtk_widget_show (self->priv->presence_icon);

  gtk_container_add (GTK_CONTAINER (self), alig);
  gtk_container_add (GTK_CONTAINER (alig), main_box);
  gtk_widget_show (main_box);
}

GtkWidget *
empathy_roster_contact_new (FolksIndividual *individual,
    const gchar *group)
{
  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);

  return g_object_new (EMPATHY_TYPE_ROSTER_CONTACT,
      "individual", individual,
      "group", group,
      NULL);
}

FolksIndividual *
empathy_roster_contact_get_individual (EmpathyRosterContact *self)
{
  return self->priv->individual;
}

EmpathyContact *
empathy_roster_contact_get_contact (EmpathyRosterContact *self)
{
  return self->priv->contact;
}

gboolean
empathy_roster_contact_is_online (EmpathyRosterContact *self)
{
  return self->priv->online;
}

const gchar *
empathy_roster_contact_get_group (EmpathyRosterContact *self)
{
  return self->priv->group;
}

void
empathy_roster_contact_set_event_icon (EmpathyRosterContact *self,
    const gchar *icon)
{
  if (!tp_strdiff (self->priv->event_icon, icon))
    return;

  g_free (self->priv->event_icon);
  self->priv->event_icon = g_strdup (icon);

  update_presence_icon (self);
}

GdkPixbuf *
empathy_roster_contact_get_avatar_pixbuf (EmpathyRosterContact *self)
{
  return gtk_image_get_pixbuf (GTK_IMAGE (self->priv->avatar));
}
