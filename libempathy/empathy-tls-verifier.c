/*
 * empathy-tls-verifier.c - Source for EmpathyTLSVerifier
 * Copyright (C) 2010 Collabora Ltd.
 * Copyright (C) 2017 Red Hat, Inc.
 * @author Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
 * @author Debarshi Ray <debarshir@gnome.org>
 * @author Stef Walter <stefw@collabora.co.uk>
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

#include "config.h"
#include "empathy-tls-verifier.h"

#include <gcr/gcr.h>

#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_TLS
#include "empathy-debug.h"

G_DEFINE_TYPE (EmpathyTLSVerifier, empathy_tls_verifier,
    G_TYPE_OBJECT)

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyTLSVerifier);

enum {
  PROP_TLS_CERTIFICATE = 1,
  PROP_HOSTNAME,
  PROP_REFERENCE_IDENTITIES,

  LAST_PROPERTY,
};

typedef struct {
  GTlsCertificate *g_certificate;
  GTlsDatabase *database;
  TpTLSCertificate *certificate;
  gchar *hostname;
  gchar **reference_identities;

  GSimpleAsyncResult *verify_result;
  GHashTable *details;

  gboolean dispose_run;
} EmpathyTLSVerifierPriv;

static GTlsCertificate *
tls_certificate_new_from_der (GPtrArray *data, GError **error)
{
  GTlsBackend *tls_backend;
  GTlsCertificate *cert = NULL;
  GTlsCertificate *issuer = NULL;
  GTlsCertificate *retval = NULL;
  GType tls_certificate_type;
  gint i;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  tls_backend = g_tls_backend_get_default ();
  tls_certificate_type = g_tls_backend_get_certificate_type (tls_backend);

  for (i = (gint) data->len - 1; i >= 0; --i)
    {
      GArray *cert_data;

      cert_data = g_ptr_array_index (data, i);
      cert = g_initable_new (tls_certificate_type,
          NULL,
          error,
          "certificate", (GByteArray *) cert_data,
          "issuer", issuer,
          NULL);

      if (cert == NULL)
        goto out;

      g_clear_object (&issuer);
      issuer = g_object_ref (cert);
      g_clear_object (&cert);
    }

  g_assert_null (cert);
  g_assert_true (G_IS_TLS_CERTIFICATE (issuer));

  retval = g_object_ref (issuer);

 out:
  g_clear_object (&cert);
  g_clear_object (&issuer);
  return retval;
}

static TpTLSCertificateRejectReason
verification_output_to_reason (GTlsCertificateFlags flags)
{
  TpTLSCertificateRejectReason retval;

  g_assert (flags != 0);

  switch (flags)
    {
      case G_TLS_CERTIFICATE_UNKNOWN_CA:
        retval = TP_TLS_CERTIFICATE_REJECT_REASON_UNTRUSTED;
        break;
      case G_TLS_CERTIFICATE_BAD_IDENTITY:
        retval = TP_TLS_CERTIFICATE_REJECT_REASON_HOSTNAME_MISMATCH;
        break;
      case G_TLS_CERTIFICATE_NOT_ACTIVATED:
        retval = TP_TLS_CERTIFICATE_REJECT_REASON_NOT_ACTIVATED;
        break;
      case G_TLS_CERTIFICATE_EXPIRED:
        retval = TP_TLS_CERTIFICATE_REJECT_REASON_EXPIRED;
        break;
      case G_TLS_CERTIFICATE_REVOKED:
        retval = TP_TLS_CERTIFICATE_REJECT_REASON_REVOKED;
        break;
      case G_TLS_CERTIFICATE_INSECURE:
        retval = TP_TLS_CERTIFICATE_REJECT_REASON_INSECURE;
        break;
      case G_TLS_CERTIFICATE_GENERIC_ERROR:
      default:
        retval = TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN;
        break;
    }

  return retval;
}

static void
complete_verification (EmpathyTLSVerifier *self)
{
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  DEBUG ("Verification successful, completing...");

  g_simple_async_result_complete_in_idle (priv->verify_result);

  g_clear_object (&priv->g_certificate);
  tp_clear_object (&priv->verify_result);
}

static void
abort_verification (EmpathyTLSVerifier *self,
    TpTLSCertificateRejectReason reason)
{
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  DEBUG ("Verification error %u, aborting...", reason);

  g_simple_async_result_set_error (priv->verify_result,
      G_IO_ERROR, reason, "TLS verification failed with reason %u",
      reason);
  g_simple_async_result_complete_in_idle (priv->verify_result);

  g_clear_object (&priv->g_certificate);
  tp_clear_object (&priv->verify_result);
}

static void
debug_certificate (GcrCertificate *cert)
{
    gchar *subject = gcr_certificate_get_subject_dn (cert);
    DEBUG ("Certificate: %s", subject);
    g_free (subject);
}

static void
verify_chain_cb (GObject *object,
        GAsyncResult *res,
        gpointer user_data)
{
  GError *error = NULL;

  GTlsCertificateFlags flags;
  GTlsDatabase *tls_database = G_TLS_DATABASE (object);
  gint i;
  EmpathyTLSVerifier *self = EMPATHY_TLS_VERIFIER (user_data);
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  /* FIXME: g_tls_database_verify_chain doesn't set the GError if the
   * certificate chain couldn't be verified. See:
   * https://bugzilla.gnome.org/show_bug.cgi?id=780310
   */
  flags = g_tls_database_verify_chain_finish (tls_database, res, &error);
  if (flags != 0)
    {
      TpTLSCertificateRejectReason reason;

      /* We don't pass the identity to g_tls_database_verify. */
      g_assert_false (flags & G_TLS_CERTIFICATE_BAD_IDENTITY);

      reason = verification_output_to_reason (flags);
      DEBUG ("Certificate verification gave flags %d with reason %u",
          (gint) flags,
          reason);

      abort_verification (self, reason);
      g_clear_error (&error);
      goto out;
    }

  for (i = 0; priv->reference_identities[i] != NULL; i++)
    {
      GSocketConnectable *identity = NULL;

      identity = g_network_address_new (priv->reference_identities[i], 0);
      flags = g_tls_certificate_verify (priv->g_certificate, identity, NULL);

      g_object_unref (identity);
      if (flags == 0)
        break;
    }

  if (flags != 0)
    {
      TpTLSCertificateRejectReason reason;

      g_assert_cmpint (flags, ==, G_TLS_CERTIFICATE_BAD_IDENTITY);

      reason = verification_output_to_reason (flags);
      DEBUG ("Certificate verification gave flags %d with reason %u",
          (gint) flags,
          reason);

      /* FIXME: We don't set "certificate-hostname" because
       * GTlsCertificate doesn't expose the hostname used in the
       * certificate. We will temporarily lose some verbosity in
       * EmpathyTLSDialog, but that's balanced by no longer
       * relying on a specific encryption library.
       */
      tp_asv_set_string (priv->details, "expected-hostname", priv->hostname);

      DEBUG ("Hostname mismatch: expected %s", priv->hostname);

      abort_verification (self, reason);
      goto out;
    }

  DEBUG ("Verified certificate chain");
  complete_verification (self);

out:
  /* Matches ref when starting verify chain */
  g_object_unref (self);
}

static void
is_certificate_pinned_cb (GObject *object,
    GAsyncResult *res,
    gpointer user_data)
{
  GError *error = NULL;
  GPtrArray *cert_data;
  EmpathyTLSVerifier *self = EMPATHY_TLS_VERIFIER (user_data);
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  if (gcr_trust_is_certificate_pinned_finish (res, &error))
    {
      DEBUG ("Found pinned certificate for %s", priv->hostname);
      complete_verification (self);
      goto out;
    }

  /* error is set only when there is an actual failure. It won't be
   * set, if it successfully determined that the ceritificate was not
   * pinned. */
  if (error != NULL)
    {
      DEBUG ("Failed to determine if certificate is pinned: %s",
          error->message);
      g_clear_error (&error);
    }

  cert_data = tp_tls_certificate_get_cert_data (priv->certificate);
  priv->g_certificate = tls_certificate_new_from_der (cert_data, &error);
  if (error != NULL)
    {
      DEBUG ("Verification of certificate chain failed: %s", error->message);

      abort_verification (self, TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN);
      g_clear_error (&error);
      goto out;
    }

  DEBUG ("Performing verification");

  g_tls_database_verify_chain_async (priv->database,
      priv->g_certificate,
      G_TLS_DATABASE_PURPOSE_AUTHENTICATE_SERVER,
      NULL,
      NULL,
      G_TLS_DATABASE_VERIFY_NONE,
      NULL,
      verify_chain_cb,
      g_object_ref (self));

out:
  /* Matches ref when starting is certificate pinned */
  g_object_unref (self);
}

static void
empathy_tls_verifier_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyTLSVerifierPriv *priv = GET_PRIV (object);

  switch (property_id)
    {
    case PROP_TLS_CERTIFICATE:
      g_value_set_object (value, priv->certificate);
      break;
    case PROP_HOSTNAME:
      g_value_set_string (value, priv->hostname);
      break;
    case PROP_REFERENCE_IDENTITIES:
      g_value_set_boxed (value, priv->reference_identities);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
empathy_tls_verifier_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyTLSVerifierPriv *priv = GET_PRIV (object);

  switch (property_id)
    {
    case PROP_TLS_CERTIFICATE:
      priv->certificate = g_value_dup_object (value);
      break;
    case PROP_HOSTNAME:
      priv->hostname = g_value_dup_string (value);
      break;
    case PROP_REFERENCE_IDENTITIES:
      priv->reference_identities = g_value_dup_boxed (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
empathy_tls_verifier_dispose (GObject *object)
{
  EmpathyTLSVerifierPriv *priv = GET_PRIV (object);

  if (priv->dispose_run)
    return;

  priv->dispose_run = TRUE;

  g_clear_object (&priv->g_certificate);
  g_clear_object (&priv->database);
  tp_clear_object (&priv->certificate);

  G_OBJECT_CLASS (empathy_tls_verifier_parent_class)->dispose (object);
}

static void
empathy_tls_verifier_finalize (GObject *object)
{
  EmpathyTLSVerifierPriv *priv = GET_PRIV (object);

  DEBUG ("%p", object);

  tp_clear_boxed (G_TYPE_HASH_TABLE, &priv->details);
  g_free (priv->hostname);
  g_strfreev (priv->reference_identities);

  G_OBJECT_CLASS (empathy_tls_verifier_parent_class)->finalize (object);
}

static void
empathy_tls_verifier_init (EmpathyTLSVerifier *self)
{
  EmpathyTLSVerifierPriv *priv;
  GTlsBackend *tls_backend;

  priv = self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_TLS_VERIFIER, EmpathyTLSVerifierPriv);
  priv->details = tp_asv_new (NULL, NULL);

  tls_backend = g_tls_backend_get_default ();
  priv->database = g_tls_backend_get_default_database (tls_backend);
}

static void
empathy_tls_verifier_class_init (EmpathyTLSVerifierClass *klass)
{
  GParamSpec *pspec;
  GObjectClass *oclass = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (EmpathyTLSVerifierPriv));

  oclass->set_property = empathy_tls_verifier_set_property;
  oclass->get_property = empathy_tls_verifier_get_property;
  oclass->finalize = empathy_tls_verifier_finalize;
  oclass->dispose = empathy_tls_verifier_dispose;

  pspec = g_param_spec_object ("certificate", "The TpTLSCertificate",
      "The TpTLSCertificate to be verified.",
      TP_TYPE_TLS_CERTIFICATE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_TLS_CERTIFICATE, pspec);

  pspec = g_param_spec_string ("hostname", "The hostname",
      "The hostname which is certified by the certificate.",
      NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_HOSTNAME, pspec);

  pspec = g_param_spec_boxed ("reference-identities",
      "The reference identities",
      "The certificate should certify one of these identities.",
      G_TYPE_STRV,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_REFERENCE_IDENTITIES, pspec);
}

EmpathyTLSVerifier *
empathy_tls_verifier_new (TpTLSCertificate *certificate,
    const gchar *hostname,
    const gchar **reference_identities)
{
  g_assert (TP_IS_TLS_CERTIFICATE (certificate));
  g_assert (hostname != NULL);
  g_assert (reference_identities != NULL);

  return g_object_new (EMPATHY_TYPE_TLS_VERIFIER,
      "certificate", certificate,
      "hostname", hostname,
      "reference-identities", reference_identities,
      NULL);
}

void
empathy_tls_verifier_verify_async (EmpathyTLSVerifier *self,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GcrCertificate *cert;
  GPtrArray *cert_data;
  GArray *data;
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  DEBUG ("Starting verification");

  g_return_if_fail (priv->verify_result == NULL);
  g_return_if_fail (priv->g_certificate == NULL);

  cert_data = tp_tls_certificate_get_cert_data (priv->certificate);
  g_return_if_fail (cert_data);

  priv->verify_result = g_simple_async_result_new (G_OBJECT (self),
      callback, user_data, NULL);

  /* The first certificate in the chain is for the host */
  data = g_ptr_array_index (cert_data, 0);
  cert = gcr_simple_certificate_new ((gpointer) data->data,
      (gsize) data->len);

  DEBUG ("Checking if certificate is pinned:");
  debug_certificate (cert);

  gcr_trust_is_certificate_pinned_async (cert,
      GCR_PURPOSE_SERVER_AUTH,
      priv->hostname,
      NULL,
      is_certificate_pinned_cb,
      g_object_ref (self));

  g_object_unref (cert);
}

gboolean
empathy_tls_verifier_verify_finish (EmpathyTLSVerifier *self,
    GAsyncResult *res,
    TpTLSCertificateRejectReason *reason,
    GHashTable **details,
    GError **error)
{
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res),
          error))
    {
      if (reason != NULL)
        *reason = (*error)->code;

      if (details != NULL)
        {
          *details = tp_asv_new (NULL, NULL);
          tp_g_hash_table_update (*details, priv->details,
              (GBoxedCopyFunc) g_strdup,
              (GBoxedCopyFunc) tp_g_value_slice_dup);
        }

      return FALSE;
    }

  if (reason != NULL)
    *reason = TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN;

  return TRUE;
}

void empathy_tls_verifier_set_database (EmpathyTLSVerifier *self,
    GTlsDatabase *database)
{
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  g_return_if_fail (EMPATHY_IS_TLS_VERIFIER (self));
  g_return_if_fail (G_IS_TLS_DATABASE (database));

  if (database == priv->database)
    return;

  g_clear_object (&priv->database);
  priv->database = g_object_ref (database);
}

void
empathy_tls_verifier_store_exception (EmpathyTLSVerifier *self)
{
  GArray *data;
  GcrCertificate *cert;
  GPtrArray *cert_data;
  GError *error = NULL;
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  cert_data = tp_tls_certificate_get_cert_data (priv->certificate);
  g_return_if_fail (cert_data);

  if (!cert_data->len)
    {
      DEBUG ("No certificate to pin.");
      return;
    }

  /* The first certificate in the chain is for the host */
  data = g_ptr_array_index (cert_data, 0);
  cert = gcr_simple_certificate_new ((gpointer)data->data, data->len);

  DEBUG ("Storing pinned certificate:");
  debug_certificate (cert);

  if (!gcr_trust_add_pinned_certificate (cert, GCR_PURPOSE_SERVER_AUTH,
          priv->hostname, NULL, &error))
      DEBUG ("Can't store the pinned certificate: %s", error->message);

  g_object_unref (cert);
}
