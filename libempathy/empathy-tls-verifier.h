/*
 * empathy-tls-verifier.h - Header for EmpathyTLSVerifier
 * Copyright (C) 2010 Collabora Ltd.
 * @author Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
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
 */

#ifndef __EMPATHY_TLS_VERIFIER_H__
#define __EMPATHY_TLS_VERIFIER_H__

#include <glib-object.h>
#include <gio/gio.h>
#include <telepathy-glib/telepathy-glib.h>

G_BEGIN_DECLS

typedef struct _EmpathyTLSVerifier EmpathyTLSVerifier;
typedef struct _EmpathyTLSVerifierClass EmpathyTLSVerifierClass;

struct _EmpathyTLSVerifierClass {
    GObjectClass parent_class;
};

struct _EmpathyTLSVerifier {
    GObject parent;
    gpointer priv;
};

GType empathy_tls_verifier_get_type (void);

#define EMPATHY_TYPE_TLS_VERIFIER \
  (empathy_tls_verifier_get_type ())
#define EMPATHY_TLS_VERIFIER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), EMPATHY_TYPE_TLS_VERIFIER, \
    EmpathyTLSVerifier))
#define EMPATHY_TLS_VERIFIER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), EMPATHY_TYPE_TLS_VERIFIER, \
  EmpathyTLSVerifierClass))
#define EMPATHY_IS_TLS_VERIFIER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), EMPATHY_TYPE_TLS_VERIFIER))
#define EMPATHY_IS_TLS_VERIFIER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), EMPATHY_TYPE_TLS_VERIFIER))
#define EMPATHY_TLS_VERIFIER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), EMPATHY_TYPE_TLS_VERIFIER, \
  EmpathyTLSVerifierClass))

EmpathyTLSVerifier * empathy_tls_verifier_new (
    TpTLSCertificate *certificate,
    const gchar *hostname,
    const gchar **reference_identities);

void empathy_tls_verifier_verify_async (EmpathyTLSVerifier *self,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean empathy_tls_verifier_verify_finish (EmpathyTLSVerifier *self,
    GAsyncResult *res,
    TpTLSCertificateRejectReason *reason,
    GHashTable **details,
    GError **error);

void empathy_tls_verifier_set_database (EmpathyTLSVerifier *self,
    GTlsDatabase *database);

void empathy_tls_verifier_store_exception (EmpathyTLSVerifier *self);

G_END_DECLS

#endif /* #ifndef __EMPATHY_TLS_VERIFIER_H__*/
