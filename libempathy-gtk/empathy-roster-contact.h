#ifndef __EMPATHY_ROSTER_CONTACT_H__
#define __EMPATHY_ROSTER_CONTACT_H__

#include <gtk/gtk.h>
#include <folks/folks.h>
#include "empathy-contact.h"

G_BEGIN_DECLS

typedef struct _EmpathyRosterContact EmpathyRosterContact;
typedef struct _EmpathyRosterContactClass EmpathyRosterContactClass;
typedef struct _EmpathyRosterContactPriv EmpathyRosterContactPriv;

struct _EmpathyRosterContactClass
{
  /*<private>*/
  GtkListBoxRowClass parent_class;
};

struct _EmpathyRosterContact
{
  /*<private>*/
  GtkListBoxRow parent;
  EmpathyRosterContactPriv *priv;
};

GType empathy_roster_contact_get_type (void);

/* TYPE MACROS */
#define EMPATHY_TYPE_ROSTER_CONTACT \
  (empathy_roster_contact_get_type ())
#define EMPATHY_ROSTER_CONTACT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), \
    EMPATHY_TYPE_ROSTER_CONTACT, \
    EmpathyRosterContact))
#define EMPATHY_ROSTER_CONTACT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), \
    EMPATHY_TYPE_ROSTER_CONTACT, \
    EmpathyRosterContactClass))
#define EMPATHY_IS_ROSTER_CONTACT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), \
    EMPATHY_TYPE_ROSTER_CONTACT))
#define EMPATHY_IS_ROSTER_CONTACT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), \
    EMPATHY_TYPE_ROSTER_CONTACT))
#define EMPATHY_ROSTER_CONTACT_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
    EMPATHY_TYPE_ROSTER_CONTACT, \
    EmpathyRosterContactClass))

GtkWidget * empathy_roster_contact_new (FolksIndividual *individual,
    const gchar *group);

FolksIndividual * empathy_roster_contact_get_individual (EmpathyRosterContact *self);

EmpathyContact * empathy_roster_contact_get_contact (EmpathyRosterContact *self);

const gchar * empathy_roster_contact_get_group (EmpathyRosterContact *self);

gboolean empathy_roster_contact_is_online (EmpathyRosterContact *self);

void empathy_roster_contact_set_event_icon (EmpathyRosterContact *self,
    const gchar *icon);

GdkPixbuf * empathy_roster_contact_get_avatar_pixbuf (
    EmpathyRosterContact *self);

gint64 empathy_roster_contact_get_most_recent_timestamp (EmpathyRosterContact *contact);

G_END_DECLS

#endif /* #ifndef __EMPATHY_ROSTER_CONTACT_H__*/
