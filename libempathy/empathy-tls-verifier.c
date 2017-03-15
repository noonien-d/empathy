/*
 * empathy-tls-verifier.c - Source for EmpathyTLSVerifier
 * Copyright (C) 2010 Collabora Ltd.
 * @author Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
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
  TpTLSCertificate *certificate;
  gchar *hostname;
  gchar **reference_identities;

  GSimpleAsyncResult *verify_result;
  GHashTable *details;

  gboolean dispose_run;
} EmpathyTLSVerifierPriv;

static gboolean
verification_output_to_reason (gint res,
    guint verify_output,
    TpTLSCertificateRejectReason *reason)
{
  gboolean retval = TRUE;

  g_assert (reason != NULL);

  if (res != GNUTLS_E_SUCCESS)
    {
      retval = FALSE;

      /* the certificate is not structurally valid */
      switch (res)
        {
        case GNUTLS_E_INSUFFICIENT_CREDENTIALS:
          *reason = TP_TLS_CERTIFICATE_REJECT_REASON_UNTRUSTED;
          break;
        case GNUTLS_E_CONSTRAINT_ERROR:
          *reason = TP_TLS_CERTIFICATE_REJECT_REASON_LIMIT_EXCEEDED;
          break;
        default:
          *reason = TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN;
          break;
        }

      goto out;
    }

  /* the certificate is structurally valid, check for other errors. */
  if (verify_output & GNUTLS_CERT_INVALID)
    {
      retval = FALSE;

      if (verify_output & GNUTLS_CERT_SIGNER_NOT_FOUND)
        *reason = TP_TLS_CERTIFICATE_REJECT_REASON_SELF_SIGNED;
      else if (verify_output & GNUTLS_CERT_SIGNER_NOT_CA)
        *reason = TP_TLS_CERTIFICATE_REJECT_REASON_UNTRUSTED;
      else if (verify_output & GNUTLS_CERT_INSECURE_ALGORITHM)
        *reason = TP_TLS_CERTIFICATE_REJECT_REASON_INSECURE;
      else if (verify_output & GNUTLS_CERT_NOT_ACTIVATED)
        *reason = TP_TLS_CERTIFICATE_REJECT_REASON_NOT_ACTIVATED;
      else if (verify_output & GNUTLS_CERT_EXPIRED)
        *reason = TP_TLS_CERTIFICATE_REJECT_REASON_EXPIRED;
      else if (verify_output & GNUTLS_CERT_REVOKED)
        *reason = TP_TLS_CERTIFICATE_REJECT_REASON_REVOKED;
      else
        *reason = TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN;

      goto out;
    }

 out:
  return retval;
}

static void
build_certificate_list_for_gnutls (GcrCertificateChain *chain,
        gnutls_x509_crt_t **list,
        guint *n_list,
        gnutls_x509_crt_t **anchors,
        guint *n_anchors)
{
  GcrCertificate *cert;
  guint idx, length;
  gnutls_x509_crt_t *retval;
  gnutls_x509_crt_t gcert;
  gnutls_datum_t datum;
  gsize n_data;

  g_assert (list);
  g_assert (n_list);
  g_assert (anchors);
  g_assert (n_anchors);

  *list = *anchors = NULL;
  *n_list = *n_anchors = 0;

  length = gcr_certificate_chain_get_length (chain);
  retval = g_malloc0 (sizeof (gnutls_x509_crt_t) * length);

  /* Convert the main body of the chain to gnutls */
  for (idx = 0; idx < length; ++idx)
    {
      cert = gcr_certificate_chain_get_certificate (chain, idx);
      datum.data = (gpointer)gcr_certificate_get_der_data (cert, &n_data);
      datum.size = n_data;

      gnutls_x509_crt_init (&gcert);
      if (gnutls_x509_crt_import (gcert, &datum, GNUTLS_X509_FMT_DER) < 0)
        g_return_if_reached ();

      retval[idx] = gcert;
    }

  *list = retval;
  *n_list = length;

  /* See if we have an anchor */
  if (gcr_certificate_chain_get_status (chain) ==
          GCR_CERTIFICATE_CHAIN_ANCHORED)
    {
      cert = gcr_certificate_chain_get_anchor (chain);
      g_return_if_fail (cert);

      datum.data = (gpointer)gcr_certificate_get_der_data (cert, &n_data);
      datum.size = n_data;

      gnutls_x509_crt_init (&gcert);
      if (gnutls_x509_crt_import (gcert, &datum, GNUTLS_X509_FMT_DER) < 0)
        g_return_if_reached ();

      retval = g_malloc0 (sizeof (gnutls_x509_crt_t) * 1);
      retval[0] = gcert;
      *anchors = retval;
      *n_anchors = 1;
    }
}

static void
free_certificate_list_for_gnutls (gnutls_x509_crt_t *list,
        guint n_list)
{
  guint idx;

  for (idx = 0; idx < n_list; idx++)
    gnutls_x509_crt_deinit (list[idx]);
  g_free (list);
}

static void
complete_verification (EmpathyTLSVerifier *self)
{
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  DEBUG ("Verification successful, completing...");

  g_simple_async_result_complete_in_idle (priv->verify_result);

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
debug_certificate_chain (GcrCertificateChain *chain)
{
    GEnumClass *enum_class;
    GEnumValue *enum_value;
    gint idx, length;
    GcrCertificate *cert;

    enum_class = G_ENUM_CLASS
            (g_type_class_peek (GCR_TYPE_CERTIFICATE_CHAIN_STATUS));
    enum_value = g_enum_get_value (enum_class,
            gcr_certificate_chain_get_status (chain));
    length = gcr_certificate_chain_get_length (chain);
    DEBUG ("Certificate chain: length %u status %s",
            length, enum_value ? enum_value->value_nick : "XXX");

    for (idx = 0; idx < length; ++idx)
      {
        cert = gcr_certificate_chain_get_certificate (chain, idx);
        debug_certificate (cert);
      }
}

static void
perform_verification (EmpathyTLSVerifier *self,
        GcrCertificateChain *chain)
{
  gboolean ret = FALSE;
  TpTLSCertificateRejectReason reason =
    TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN;
  gnutls_x509_crt_t *list, *anchors;
  guint n_list, n_anchors;
  guint verify_output;
  gint res;
  gint i;
  gboolean matched = FALSE;
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  DEBUG ("Performing verification");
  debug_certificate_chain (chain);

  list = anchors = NULL;
  n_list = n_anchors = 0;

  /*
   * If the first certificate is an pinned certificate then we completely
   * ignore the rest of the verification process.
   */
  if (gcr_certificate_chain_get_status (chain) == GCR_CERTIFICATE_CHAIN_PINNED)
    {
      DEBUG ("Found pinned certificate for %s", priv->hostname);
      complete_verification (self);
      goto out;
  }

  build_certificate_list_for_gnutls (chain, &list, &n_list,
          &anchors, &n_anchors);
  if (list == NULL || n_list == 0) {
      g_warn_if_reached ();
      abort_verification (self, TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN);
      goto out;
  }

  verify_output = 0;
  res = gnutls_x509_crt_list_verify (list, n_list, anchors, n_anchors,
           NULL, 0, 0, &verify_output);
  ret = verification_output_to_reason (res, verify_output, &reason);

  DEBUG ("Certificate verification gave result %d with reason %u", ret,
          reason);

  if (!ret) {
      abort_verification (self, reason);
      goto out;
  }

  /* now check if the certificate matches one of the reference identities. */
  if (priv->reference_identities != NULL)
    {
      for (i = 0, matched = FALSE; priv->reference_identities[i] != NULL; ++i)
        {
          if (gnutls_x509_crt_check_hostname (list[0],
                  priv->reference_identities[i]) == 1)
            {
              matched = TRUE;
              break;
            }
        }
    }

  if (!matched)
    {
      gchar *certified_hostname;

      certified_hostname = empathy_get_x509_certificate_hostname (list[0]);
      tp_asv_set_string (priv->details,
          "expected-hostname", priv->hostname);
      tp_asv_set_string (priv->details,
          "certificate-hostname", certified_hostname);

      DEBUG ("Hostname mismatch: got %s but expected %s",
          certified_hostname, priv->hostname);

      g_free (certified_hostname);
      abort_verification (self,
              TP_TLS_CERTIFICATE_REJECT_REASON_HOSTNAME_MISMATCH);
      goto out;
    }

  DEBUG ("Hostname matched");
  complete_verification (self);

 out:
  free_certificate_list_for_gnutls (list, n_list);
  free_certificate_list_for_gnutls (anchors, n_anchors);
}

static void
perform_verification_cb (GObject *object,
        GAsyncResult *res,
        gpointer user_data)
{
  GError *error = NULL;

  GcrCertificateChain *chain = GCR_CERTIFICATE_CHAIN (object);
  EmpathyTLSVerifier *self = EMPATHY_TLS_VERIFIER (user_data);

  /* Even if building the chain fails, try verifying what we have */
  if (!gcr_certificate_chain_build_finish (chain, res, &error))
    {
      DEBUG ("Building of certificate chain failed: %s", error->message);
      g_clear_error (&error);
    }

  perform_verification (self, chain);

  /* Matches ref when staring chain build */
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

  priv = self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_TLS_VERIFIER, EmpathyTLSVerifierPriv);
  priv->details = tp_asv_new (NULL, NULL);
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
  GcrCertificateChain *chain;
  GcrCertificate *cert;
  GPtrArray *cert_data;
  GArray *data;
  guint idx;
  EmpathyTLSVerifierPriv *priv = GET_PRIV (self);

  DEBUG ("Starting verification");

  g_return_if_fail (priv->verify_result == NULL);

  cert_data = tp_tls_certificate_get_cert_data (priv->certificate);
  g_return_if_fail (cert_data);

  priv->verify_result = g_simple_async_result_new (G_OBJECT (self),
      callback, user_data, NULL);

  /* Create a certificate chain */
  chain = gcr_certificate_chain_new ();
  for (idx = 0; idx < cert_data->len; ++idx) {
    data = g_ptr_array_index (cert_data, idx);
    cert = gcr_simple_certificate_new ((guchar *) data->data, data->len);
    gcr_certificate_chain_add (chain, cert);
    g_object_unref (cert);
  }

  gcr_certificate_chain_build_async (chain, GCR_PURPOSE_SERVER_AUTH, priv->hostname, 0,
          NULL, perform_verification_cb, g_object_ref (self));

  g_object_unref (chain);
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
