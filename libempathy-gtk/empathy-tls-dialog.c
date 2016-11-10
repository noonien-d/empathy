/*
 * empathy-tls-dialog.c - Source for EmpathyTLSDialog
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

#include "config.h"
#include "empathy-tls-dialog.h"

#include <glib/gi18n-lib.h>
#include <gcr/gcr.h>

#include "empathy-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_TLS
#include "empathy-debug.h"

G_DEFINE_TYPE (EmpathyTLSDialog, empathy_tls_dialog,
    GTK_TYPE_MESSAGE_DIALOG)

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyTLSDialog);

enum {
  PROP_TLS_CERTIFICATE = 1,
  PROP_REASON,
  PROP_REMEMBER,
  PROP_DETAILS,

  LAST_PROPERTY,
};

typedef struct {
  TpTLSCertificate *certificate;
  TpTLSCertificateRejectReason reason;
  GHashTable *details;

  gboolean remember;

  gboolean dispose_run;
} EmpathyTLSDialogPriv;

static void
empathy_tls_dialog_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyTLSDialogPriv *priv = GET_PRIV (object);

  switch (property_id)
    {
    case PROP_TLS_CERTIFICATE:
      g_value_set_object (value, priv->certificate);
      break;
    case PROP_REASON:
      g_value_set_uint (value, priv->reason);
      break;
    case PROP_REMEMBER:
      g_value_set_boolean (value, priv->remember);
      break;
    case PROP_DETAILS:
      g_value_set_boxed (value, priv->details);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
empathy_tls_dialog_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyTLSDialogPriv *priv = GET_PRIV (object);

  switch (property_id)
    {
    case PROP_TLS_CERTIFICATE:
      priv->certificate = g_value_dup_object (value);
      break;
    case PROP_REASON:
      priv->reason = g_value_get_uint (value);
      break;
    case PROP_DETAILS:
      priv->details = g_value_dup_boxed (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
empathy_tls_dialog_dispose (GObject *object)
{
  EmpathyTLSDialogPriv *priv = GET_PRIV (object);

  if (priv->dispose_run)
    return;

  priv->dispose_run = TRUE;

  tp_clear_object (&priv->certificate);

  G_OBJECT_CLASS (empathy_tls_dialog_parent_class)->dispose (object);
}

static void
empathy_tls_dialog_finalize (GObject *object)
{
  EmpathyTLSDialogPriv *priv = GET_PRIV (object);

  tp_clear_boxed (G_TYPE_HASH_TABLE, &priv->details);

  G_OBJECT_CLASS (empathy_tls_dialog_parent_class)->finalize (object);
}

static gchar *
reason_to_string (EmpathyTLSDialog *self)
{
  GString *str;
  const gchar *reason_str;
  TpTLSCertificateRejectReason reason;
  GHashTable *details;
  EmpathyTLSDialogPriv *priv = GET_PRIV (self);

  str = g_string_new (NULL);
  reason = priv->reason;
  details = priv->details;

  g_string_append (str, _("The identity provided by the chat server cannot be "
          "verified."));
  g_string_append (str, "\n\n");

  switch (reason)
    {
    case TP_TLS_CERTIFICATE_REJECT_REASON_UNTRUSTED:
      reason_str = _("The certificate is not signed by a Certification "
          "Authority.");
      break;
    case TP_TLS_CERTIFICATE_REJECT_REASON_EXPIRED:
      reason_str = _("The certificate has expired.");
      break;
    case TP_TLS_CERTIFICATE_REJECT_REASON_NOT_ACTIVATED:
      reason_str = _("The certificate hasn’t yet been activated.");
      break;
    case TP_TLS_CERTIFICATE_REJECT_REASON_FINGERPRINT_MISMATCH:
      reason_str = _("The certificate does not have the expected fingerprint.");
      break;
    case TP_TLS_CERTIFICATE_REJECT_REASON_HOSTNAME_MISMATCH:
      reason_str = _("The hostname verified by the certificate doesn’t match "
          "the server name.");
      break;
    case TP_TLS_CERTIFICATE_REJECT_REASON_SELF_SIGNED:
      reason_str = _("The certificate is self-signed.");
      break;
    case TP_TLS_CERTIFICATE_REJECT_REASON_REVOKED:
      reason_str = _("The certificate has been revoked by the issuing "
          "Certification Authority.");
      break;
    case TP_TLS_CERTIFICATE_REJECT_REASON_INSECURE:
      reason_str = _("The certificate is cryptographically weak.");
      break;
    case TP_TLS_CERTIFICATE_REJECT_REASON_LIMIT_EXCEEDED:
      reason_str = _("The certificate length exceeds verifiable limits.");
      break;
    case TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN:
    default:
      reason_str = _("The certificate is malformed.");
      break;
    }

  g_string_append (str, reason_str);

  /* add more information in case of HOSTNAME_MISMATCH */
  if (reason == TP_TLS_CERTIFICATE_REJECT_REASON_HOSTNAME_MISMATCH)
    {
      const gchar *expected_hostname, *certificate_hostname;

      expected_hostname = tp_asv_get_string (details, "expected-hostname");
      certificate_hostname = tp_asv_get_string (details,
          "certificate-hostname");

      if (expected_hostname != NULL && certificate_hostname != NULL)
        {
          g_string_append (str, "\n\n");
          g_string_append_printf (str, _("Expected hostname: %s"),
              expected_hostname);
          g_string_append (str, "\n");
          g_string_append_printf (str, _("Certificate hostname: %s"),
              certificate_hostname);
        }
    }

  return g_string_free (str, FALSE);
}

static GtkWidget *
build_gcr_widget (EmpathyTLSDialog *self)
{
  GcrCertificateWidget *widget;
  GcrCertificate *certificate;
  GPtrArray *cert_chain = NULL;
  GArray *first_cert;
  int height;
  EmpathyTLSDialogPriv *priv = GET_PRIV (self);

  g_object_get (priv->certificate,
      "cert-data", &cert_chain,
      NULL);
  first_cert = g_ptr_array_index (cert_chain, 0);

  certificate = gcr_simple_certificate_new ((const guchar *) first_cert->data,
      first_cert->len);
  widget = gcr_certificate_widget_new (certificate);

  /* FIXME: make this widget bigger by default -- GTK+ should really handle
   * this sort of thing for us */
  gtk_widget_get_preferred_height (GTK_WIDGET (widget), NULL, &height);
  /* force the widget to at least 150 pixels high */
  gtk_widget_set_size_request (GTK_WIDGET (widget), -1, MAX (height, 150));

  g_object_unref (certificate);
  g_ptr_array_unref (cert_chain);

  return GTK_WIDGET (widget);
}

static void
checkbox_toggled_cb (GtkToggleButton *checkbox,
    gpointer user_data)
{
  EmpathyTLSDialog *self = user_data;
  EmpathyTLSDialogPriv *priv = GET_PRIV (self);

  priv->remember = gtk_toggle_button_get_active (checkbox);
  g_object_notify (G_OBJECT (self), "remember");
}

static void
certificate_invalidated_cb (TpTLSCertificate *certificate,
    guint domain,
    gint code,
    gchar *message,
    EmpathyTLSDialog *self)
{
  gtk_widget_destroy (GTK_WIDGET (self));
}

static void
empathy_tls_dialog_constructed (GObject *object)
{
  GtkWidget *content_area, *expander, *details, *checkbox;
  gchar *text;
  EmpathyTLSDialog *self = EMPATHY_TLS_DIALOG (object);
  GtkMessageDialog *message_dialog = GTK_MESSAGE_DIALOG (self);
  GtkDialog *dialog = GTK_DIALOG (self);
  EmpathyTLSDialogPriv *priv = GET_PRIV (self);

  gtk_dialog_add_buttons (dialog,
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      _("C_ontinue"), GTK_RESPONSE_YES,
      NULL);

  text = reason_to_string (self);

  g_object_set (message_dialog,
      "title", _("Untrusted connection"),
      "text", _("This connection is untrusted. Would you like to "
          "continue anyway?"),
      "secondary-text", text,
      NULL);

  g_free (text);

  content_area = gtk_dialog_get_content_area (dialog);

  checkbox = gtk_check_button_new_with_label (
      _("Remember this choice for future connections"));
  gtk_box_pack_end (GTK_BOX (content_area), checkbox, FALSE, FALSE, 0);
  gtk_widget_show (checkbox);
  g_signal_connect (checkbox, "toggled", G_CALLBACK (checkbox_toggled_cb),
      self);

  text = g_strdup_printf ("<b>%s</b>", _("Certificate Details"));
  expander = gtk_expander_new (text);
  gtk_expander_set_use_markup (GTK_EXPANDER (expander), TRUE);
  gtk_box_pack_end (GTK_BOX (content_area), expander, TRUE, TRUE, 0);
  gtk_widget_show (expander);

  g_free (text);

  details = build_gcr_widget (self);
  gtk_container_add (GTK_CONTAINER (expander), details);
  gtk_widget_show (details);

  gtk_window_set_keep_above (GTK_WINDOW (self), TRUE);

  tp_g_signal_connect_object (priv->certificate, "invalidated",
      G_CALLBACK (certificate_invalidated_cb), self, 0);
}

static void
empathy_tls_dialog_init (EmpathyTLSDialog *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_TLS_DIALOG, EmpathyTLSDialogPriv);
}

static void
empathy_tls_dialog_class_init (EmpathyTLSDialogClass *klass)
{
  GParamSpec *pspec;
  GObjectClass *oclass = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (EmpathyTLSDialogPriv));

  oclass->set_property = empathy_tls_dialog_set_property;
  oclass->get_property = empathy_tls_dialog_get_property;
  oclass->dispose = empathy_tls_dialog_dispose;
  oclass->finalize = empathy_tls_dialog_finalize;
  oclass->constructed = empathy_tls_dialog_constructed;

  pspec = g_param_spec_object ("certificate", "The TpTLSCertificate",
      "The TpTLSCertificate to be displayed.",
      TP_TYPE_TLS_CERTIFICATE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_TLS_CERTIFICATE, pspec);

  pspec = g_param_spec_uint ("reason", "The reason",
      "The reason why the certificate is being asked for confirmation.",
      0, TP_NUM_TLS_CERTIFICATE_REJECT_REASONS - 1,
      TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_REASON, pspec);

  pspec = g_param_spec_boolean ("remember", "Whether to remember the decision",
      "Whether we should remember the decision for this certificate.",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_REMEMBER, pspec);

  pspec = g_param_spec_boxed ("details", "Rejection details",
      "Additional details about the rejection of this certificate.",
      G_TYPE_HASH_TABLE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_DETAILS, pspec);
}

GtkWidget *
empathy_tls_dialog_new (TpTLSCertificate *certificate,
    TpTLSCertificateRejectReason reason,
    GHashTable *details)
{
  g_assert (TP_IS_TLS_CERTIFICATE (certificate));

  return g_object_new (EMPATHY_TYPE_TLS_DIALOG,
      "message-type", GTK_MESSAGE_WARNING,
      "certificate", certificate,
      "reason", reason,
      "details", details,
      NULL);
}
