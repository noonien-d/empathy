/*
 * Copyright (C) 2010 Collabora Ltd.
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
 * Authors: Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gnutls/gnutls.h>

#include "empathy-auth-factory.h"
#include "empathy-bad-password-dialog.h"
#include "empathy-password-dialog.h"
#include "empathy-sanity-cleaning.h"
#include "empathy-server-tls-handler.h"
#include "empathy-tls-dialog.h"
#include "empathy-tls-verifier.h"
#include "empathy-ui-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_TLS
#include "empathy-debug.h"

#define TIMEOUT 60

static gboolean use_timer = TRUE;
static guint timeout_id = 0;
static guint num_windows = 0;

static gboolean
timeout_cb (gpointer p)
{
  DEBUG ("Timeout reached; exiting...");

  gtk_main_quit ();
  return FALSE;
}

static void
start_timer (void)
{
  if (!use_timer)
    return;

  if (timeout_id != 0)
    return;

  DEBUG ("Start timer");

  timeout_id = g_timeout_add_seconds (TIMEOUT, timeout_cb, NULL);
}

static void
stop_timer (void)
{
  if (timeout_id == 0)
    return;

  DEBUG ("Stop timer");

  g_source_remove (timeout_id);
  timeout_id = 0;
}

static void
tls_dialog_response_cb (GtkDialog *dialog,
    gint response_id,
    gpointer user_data)
{
  TpTLSCertificate *certificate = NULL;
  TpTLSCertificateRejectReason reason = 0;
  GHashTable *details = NULL;
  EmpathyTLSDialog *tls_dialog = EMPATHY_TLS_DIALOG (dialog);
  gboolean remember = FALSE;
  EmpathyTLSVerifier *verifier = EMPATHY_TLS_VERIFIER (user_data);

  g_object_get (tls_dialog,
      "certificate", &certificate,
      "reason", &reason,
      "remember", &remember,
      "details", &details,
      NULL);

  DEBUG ("Response %d (remember: %d)", response_id, remember);

  gtk_widget_destroy (GTK_WIDGET (dialog));

  if (response_id == GTK_RESPONSE_YES)
    {
      tp_tls_certificate_accept_async (certificate, NULL, NULL);
    }
  else
    {
      tp_asv_set_boolean (details, "user-requested", TRUE);
      tp_tls_certificate_add_rejection (certificate, reason, NULL,
          g_variant_new_parsed ("{ 'user-requested': <%b> }", TRUE));

      tp_tls_certificate_reject_async (certificate, NULL, NULL);
    }

  if (remember)
    empathy_tls_verifier_store_exception (verifier);

  g_object_unref (certificate);
  g_hash_table_unref (details);

  /* restart the timeout */
  num_windows--;

  if (num_windows > 0)
    return;

  start_timer ();
}

static void
display_interactive_dialog (TpTLSCertificate *certificate,
    EmpathyTLSVerifier *verifier,
    TpTLSCertificateRejectReason reason,
    GHashTable *details)
{
  GtkWidget *tls_dialog;

  /* stop the timeout */
  num_windows++;
  stop_timer ();

  tls_dialog = empathy_tls_dialog_new (certificate, reason, details);
  g_signal_connect_data (tls_dialog, "response",
      G_CALLBACK (tls_dialog_response_cb), g_object_ref (verifier),
      (GClosureNotify)g_object_unref, 0);

  gtk_widget_show (tls_dialog);
}

static void
verifier_verify_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  TpTLSCertificateRejectReason reason;
  GError *error = NULL;
  TpTLSCertificate *certificate = NULL;
  GHashTable *details = NULL;
  gchar *hostname = NULL;

  g_object_get (source,
      "certificate", &certificate,
      NULL);

  empathy_tls_verifier_verify_finish (EMPATHY_TLS_VERIFIER (source),
      result, &reason, &details, &error);

  if (error != NULL)
    {
      DEBUG ("Error: %s", error->message);
      display_interactive_dialog (certificate, EMPATHY_TLS_VERIFIER (source),
              reason, details);

      g_error_free (error);
    }
  else
    {
      tp_tls_certificate_accept_async (certificate, NULL, NULL);
    }

  g_free (hostname);
  g_object_unref (certificate);
}

static void
auth_factory_new_tls_handler_cb (EmpathyAuthFactory *factory,
    EmpathyServerTLSHandler *handler,
    gpointer user_data)
{
  TpTLSCertificate *certificate = NULL;
  gchar *hostname = NULL;
  gchar **reference_identities = NULL;
  EmpathyTLSVerifier *verifier;

  DEBUG ("New TLS server handler received from the factory");

  g_object_get (handler,
      "certificate", &certificate,
      "hostname", &hostname,
      "reference-identities", &reference_identities,
      NULL);

  verifier = empathy_tls_verifier_new (certificate, hostname,
      (const gchar **) reference_identities);
  empathy_tls_verifier_verify_async (verifier,
      verifier_verify_cb, NULL);

  g_object_unref (verifier);
  g_object_unref (certificate);
  g_free (hostname);
  g_strfreev (reference_identities);
}

static void
auth_factory_new_sasl_handler_cb (EmpathyAuthFactory *factory,
    EmpathyServerSASLHandler *handler,
    gpointer user_data)
{
  GtkWidget *dialog;

  DEBUG ("New SASL server handler received from the factory");

  /* If the handler has the password it will deal with it itself. */
  if (!empathy_server_sasl_handler_has_password (handler))
    {
      DEBUG ("SASL handler doesn't have a password, prompt for one");

      dialog = empathy_password_dialog_new (handler);
      gtk_widget_show (dialog);
    }
}

static void
retry_account_cb (GtkWidget *dialog,
    TpAccount *account,
    const gchar *password,
    EmpathyAuthFactory *factory)
{
  DEBUG ("Try reconnecting to %s", tp_account_get_path_suffix (account));

  empathy_auth_factory_save_retry_password (factory, account, password);

  tp_account_reconnect_async (account, NULL, NULL);
}

static void
auth_factory_auth_passsword_failed (EmpathyAuthFactory *factory,
    TpAccount *account,
    const gchar *password,
    gpointer user_data)
{
  GtkWidget *dialog;

  DEBUG ("Authentication on %s failed, popup password dialog",
      tp_account_get_path_suffix (account));

  dialog = empathy_bad_password_dialog_new (account, password);

  tp_g_signal_connect_object (dialog, "retry",
      G_CALLBACK (retry_account_cb), factory, 0);

  gtk_widget_show (dialog);
}

static void
sanity_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  start_timer ();
}

int
main (int argc,
    char **argv)
{
  GOptionContext *context;
  GError *error = NULL;
  EmpathyAuthFactory *factory;
  TpDebugSender *debug_sender;
  TpSimpleClientFactory *tp_factory;
  TpDBusDaemon *dbus;

  context = g_option_context_new (N_(" - Empathy authentication client"));
  g_option_context_add_group (context, gtk_get_option_group (TRUE));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_print ("%s\nRun '%s --help' to see a full list of available command "
          "line options.\n", error->message, argv[0]);
      g_warning ("Error in empathy-auth-client init: %s", error->message);
      return EXIT_FAILURE;
    }

  g_option_context_free (context);

  empathy_gtk_init ();
  gnutls_global_init ();
  g_set_application_name (_("Empathy authentication client"));

  /* Make empathy and empathy-auth-client appear as the same app in
   * gnome-shell */
  g_set_prgname ("empathy");
  gtk_window_set_default_icon_name ("empathy");
  textdomain (GETTEXT_PACKAGE);

  /* There is no 'main' UI window so just use the default GdkScreen */
  empathy_set_css_provider (NULL);

#ifdef ENABLE_DEBUG
  /* Set up debug sender */
  debug_sender = tp_debug_sender_dup ();
  g_log_set_default_handler (tp_debug_sender_log_handler, G_LOG_DOMAIN);
#endif

  dbus = tp_dbus_daemon_dup (NULL);
  tp_factory = tp_simple_client_factory_new (dbus);
  tp_simple_client_factory_add_account_features_varargs (tp_factory,
      TP_ACCOUNT_FEATURE_STORAGE,
      0);

  factory = empathy_auth_factory_new (tp_factory);
  g_object_unref (tp_factory);
  g_object_unref (dbus);

  g_signal_connect (factory, "new-server-tls-handler",
      G_CALLBACK (auth_factory_new_tls_handler_cb), NULL);

  g_signal_connect (factory, "new-server-sasl-handler",
      G_CALLBACK (auth_factory_new_sasl_handler_cb), NULL);

  g_signal_connect (factory, "auth-password-failed",
      G_CALLBACK (auth_factory_auth_passsword_failed), NULL);

  if (!empathy_auth_factory_register (factory, &error))
    {
      g_critical ("Failed to register the auth factory: %s\n", error->message);
      g_error_free (error);
      g_object_unref (factory);

      return EXIT_FAILURE;
    }

  DEBUG ("Empathy auth client started.");

  if (g_getenv ("EMPATHY_PERSIST") != NULL)
    {
      DEBUG ("Timed-exit disabled");

      use_timer = FALSE;
    }

  /* Wait for the migration code to be done before starting the timer */
  empathy_sanity_checking_run_async (sanity_cb, NULL);

  gtk_main ();

  g_object_unref (factory);
  g_object_unref (debug_sender);

  return EXIT_SUCCESS;
}
