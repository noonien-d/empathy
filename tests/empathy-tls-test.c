#include "config.h"

#include <telepathy-glib/telepathy-glib.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "empathy-tls-verifier.h"
#include "mock-pkcs11.h"
#include "test-helper.h"

#define MOCK_TLS_CERTIFICATE_PATH "/mock/certificate"

/* Forward decl */
GType mock_tls_certificate_get_type (void);

#define MOCK_TLS_CERTIFICATE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), mock_tls_certificate_get_type (), \
    MockTLSCertificate))

typedef struct _MockTLSCertificate {
  GObject parent;
  guint state;
  GPtrArray *rejections;
  gchar *cert_type;
  GPtrArray *cert_data;
} MockTLSCertificate;

typedef struct _MockTLSCertificateClass {
  GObjectClass parent;
  TpDBusPropertiesMixinClass dbus_props_class;
} MockTLSCertificateClass;

enum {
  PROP_0,
  PROP_STATE,
  PROP_REJECTIONS,
  PROP_CERTIFICATE_TYPE,
  PROP_CERTIFICATE_CHAIN_DATA
};

static void mock_tls_certificate_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE(MockTLSCertificate, mock_tls_certificate, G_TYPE_OBJECT,
        G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_AUTHENTICATION_TLS_CERTIFICATE,
                mock_tls_certificate_iface_init)
        G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
                tp_dbus_properties_mixin_iface_init)
)

static void
mock_tls_certificate_init (MockTLSCertificate *self)
{
  self->state = TP_TLS_CERTIFICATE_STATE_PENDING;
  self->cert_type = g_strdup ("x509");
  self->cert_data = g_ptr_array_new_with_free_func ((GDestroyNotify)
      g_array_unref);
  self->rejections = g_ptr_array_new ();
}

static void
mock_tls_certificate_get_property (GObject *object,
        guint property_id,
        GValue *value,
        GParamSpec *pspec)
{
  MockTLSCertificate *self = MOCK_TLS_CERTIFICATE (object);

  switch (property_id)
    {
    case PROP_STATE:
      g_value_set_uint (value, self->state);
      break;
    case PROP_REJECTIONS:
      g_value_set_boxed (value, self->rejections);
      break;
    case PROP_CERTIFICATE_TYPE:
      g_value_set_string (value, self->cert_type);
      break;
    case PROP_CERTIFICATE_CHAIN_DATA:
      g_value_set_boxed (value, self->cert_data);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
mock_tls_certificate_finalize (GObject *object)
{
  MockTLSCertificate *self = MOCK_TLS_CERTIFICATE (object);

  tp_clear_boxed (TP_ARRAY_TYPE_TLS_CERTIFICATE_REJECTION_LIST,
      &self->rejections);
  g_free (self->cert_type);
  self->cert_type = NULL;
  g_ptr_array_unref (self->cert_data);
  self->cert_data = NULL;

  G_OBJECT_CLASS (mock_tls_certificate_parent_class)->finalize (object);
}

static void
mock_tls_certificate_class_init (MockTLSCertificateClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GParamSpec *pspec;

  static TpDBusPropertiesMixinPropImpl object_props[] = {
          { "State", "state", NULL },
          { "Rejections", "rejections", NULL },
          { "CertificateType", "certificate-type", NULL },
          { "CertificateChainData", "certificate-chain-data", NULL },
          { NULL }
  };

  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
    { TP_IFACE_AUTHENTICATION_TLS_CERTIFICATE,
      tp_dbus_properties_mixin_getter_gobject_properties,
      NULL,
      object_props,
    },
    { NULL }
  };

  oclass->get_property = mock_tls_certificate_get_property;
  oclass->finalize = mock_tls_certificate_finalize;

  pspec = g_param_spec_uint ("state",
      "State of this certificate",
      "The state of this TLS certificate.",
      0, TP_NUM_TLS_CERTIFICATE_STATES - 1,
      TP_TLS_CERTIFICATE_STATE_PENDING,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_STATE, pspec);

  pspec = g_param_spec_boxed ("rejections",
      "The reject reasons",
      "The reasons why this TLS certificate has been rejected",
      TP_ARRAY_TYPE_TLS_CERTIFICATE_REJECTION_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_REJECTIONS, pspec);

  pspec = g_param_spec_string ("certificate-type",
      "The certificate type",
      "The type of this certificate.",
      NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_CERTIFICATE_TYPE, pspec);

  pspec = g_param_spec_boxed ("certificate-chain-data",
      "The certificate chain data",
      "The raw PEM-encoded trust chain of this certificate.",
      TP_ARRAY_TYPE_UCHAR_ARRAY_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_CERTIFICATE_CHAIN_DATA, pspec);

  klass->dbus_props_class.interfaces = prop_interfaces;
  tp_dbus_properties_mixin_class_init (oclass,
      G_STRUCT_OFFSET (MockTLSCertificateClass, dbus_props_class));
}

static void
mock_tls_certificate_accept (TpSvcAuthenticationTLSCertificate *base,
        DBusGMethodInvocation *context)
{
  MockTLSCertificate *self = MOCK_TLS_CERTIFICATE (base);
  self->state = TP_TLS_CERTIFICATE_STATE_ACCEPTED;
  tp_svc_authentication_tls_certificate_emit_accepted (self);
  tp_svc_authentication_tls_certificate_return_from_accept (context);
}

static void
mock_tls_certificate_reject (TpSvcAuthenticationTLSCertificate *base,
        const GPtrArray *in_Rejections,
        DBusGMethodInvocation *context)
{
  MockTLSCertificate *self = MOCK_TLS_CERTIFICATE (base);
  self->state = TP_TLS_CERTIFICATE_STATE_REJECTED;
  tp_svc_authentication_tls_certificate_emit_rejected (self, in_Rejections);
  tp_svc_authentication_tls_certificate_return_from_reject (context);
}

static void
mock_tls_certificate_iface_init (gpointer g_iface,
        gpointer iface_data)
{
  TpSvcAuthenticationTLSCertificateClass *klass =
    (TpSvcAuthenticationTLSCertificateClass *) g_iface;

  tp_svc_authentication_tls_certificate_implement_accept (klass,
      mock_tls_certificate_accept);
  tp_svc_authentication_tls_certificate_implement_reject (klass,
      mock_tls_certificate_reject);
}

#if 0
static void
mock_tls_certificate_assert_rejected (MockTLSCertificate *self,
        TpTLSCertificateRejectReason reason)
{
  GValueArray *rejection;
  TpTLSCertificateRejectReason rejection_reason;
  gchar *rejection_error;
  GHashTable *rejection_details;
  guint i;

  g_assert (self->state == TP_TLS_CERTIFICATE_STATE_REJECTED);
  g_assert (self->rejections);
  g_assert (self->rejections->len > 0);

  for (i = 0; i < self->rejections->len; ++i)
    {
      rejection = g_ptr_array_index (self->rejections, i);
      tp_value_array_unpack (rejection, 3,
              G_TYPE_UINT, &rejection_reason,
              G_TYPE_STRING, &rejection_error,
              TP_HASH_TYPE_STRING_VARIANT_MAP, &rejection_details,
              NULL);
      g_free (rejection_error);
      g_hash_table_unref (rejection_details);

      if (rejection_reason == reason)
        return;
    }

  g_assert ("Certificate was not rejected for right reason" && 0);
}
#endif

static MockTLSCertificate *
mock_tls_certificate_new_and_register (TpDBusDaemon *dbus,
        const gchar *path,
        ...)
{
  MockTLSCertificate *cert;
  GError *error = NULL;
  gchar *filename, *contents;
  GArray *der;
  gsize length;
  va_list va;

  cert = g_object_new (mock_tls_certificate_get_type (), NULL);

  va_start (va, path);
  while (path != NULL) {
      filename = g_build_filename (g_getenv ("EMPATHY_SRCDIR"),
              "tests", "certificates", path, NULL);
      g_file_get_contents (filename, &contents, &length, &error);
      g_assert_no_error (error);

      der = g_array_sized_new (TRUE, TRUE, sizeof (guchar), length);
      g_array_append_vals (der, contents, length);
      g_ptr_array_add (cert->cert_data, der);

      g_free (contents);
      g_free (filename);

      path = va_arg (va, gchar*);
  }
  va_end (va);

  tp_dbus_daemon_register_object (dbus, MOCK_TLS_CERTIFICATE_PATH, cert);
  return cert;
}

/* ----------------------------------------------------------------------------
 * TESTS
 */

typedef struct {
  GMainLoop *loop;
  GTlsDatabase *database;
  TpDBusDaemon *dbus;
  const gchar *dbus_name;
  MockTLSCertificate *mock;
  TpTLSCertificate *cert;
  GAsyncResult *result;
} Test;

static void
setup (Test *test, gconstpointer data)
{
  GError *error = NULL;
  GckModule *module;
  const gchar *trust_uris[2] = { MOCK_SLOT_ONE_URI, NULL };
  gchar *path = NULL;

  test->loop = g_main_loop_new (NULL, FALSE);

  path = g_build_filename (g_getenv ("EMPATHY_SRCDIR"),
      "tests",
      "certificates",
      "certificate-authority.pem",
      NULL);
  test->database = g_tls_file_database_new (path, &error);
  g_assert_no_error (error);

  test->dbus = tp_dbus_daemon_dup (&error);
  g_assert_no_error (error);

  test->dbus_name = tp_dbus_daemon_get_unique_name (test->dbus);

  test->result = NULL;
  test->cert = NULL;

  /* Add our mock module as the only PKCS#11 module */
  module = gck_module_new (&mock_default_functions);
  mock_C_Initialize (NULL);

  gcr_pkcs11_set_modules (NULL);
  gcr_pkcs11_add_module (module);
  gcr_pkcs11_set_trust_lookup_uris (trust_uris);

  g_free (path);
}

static void
teardown (Test *test, gconstpointer data)
{
  mock_C_Finalize (NULL);

  test->dbus_name = NULL;

  if (test->mock)
    {
      tp_dbus_daemon_unregister_object (test->dbus, test->mock);
      g_object_unref (test->mock);
      test->mock = NULL;
    }

  if (test->result)
    g_object_unref (test->result);
  test->result = NULL;

  if (test->cert)
    g_object_unref (test->cert);
  test->cert = NULL;

  g_clear_object (&test->database);

  g_main_loop_unref (test->loop);
  test->loop = NULL;

  g_object_unref (test->dbus);
  test->dbus = NULL;
}

static void
add_certificate_to_mock (Test *test,
        const gchar *certificate,
        const gchar *peer)
{
  GError *error = NULL;
  GcrCertificate *cert;
  gchar *contents;
  gsize length;
  gchar *path;

  path = g_build_filename (g_getenv ("EMPATHY_SRCDIR"),
                           "tests", "certificates", certificate, NULL);

  g_file_get_contents (path, &contents, &length, &error);
  g_assert_no_error (error);

  cert = gcr_simple_certificate_new ((const guchar *)contents, length);
  mock_module_add_certificate (cert);
  mock_module_add_assertion (cert,
          peer ? CKT_X_PINNED_CERTIFICATE : CKT_X_ANCHORED_CERTIFICATE,
          GCR_PURPOSE_SERVER_AUTH, peer);
  g_object_unref (cert);

  g_free (contents);
  g_free (path);
}

static void
fetch_callback_result (GObject *object,
        GAsyncResult *res,
        gpointer user_data)
{
  Test *test = user_data;
  g_assert (!test->result);
  test->result = g_object_ref (res);
  g_main_loop_quit (test->loop);
}

static void
ensure_certificate_proxy (Test *test)
{
  GError *error = NULL;
  GQuark features[] = { TP_TLS_CERTIFICATE_FEATURE_CORE, 0 };

  if (test->cert)
    return;

  /* Create and prepare a certificate */
  /* We don't use tp_tls_certificate_new() as we don't pass a parent */
  test->cert = g_object_new (TP_TYPE_TLS_CERTIFICATE,
      "dbus-daemon", test->dbus,
      "bus-name", test->dbus_name,
      "object-path", MOCK_TLS_CERTIFICATE_PATH,
      NULL);

  tp_proxy_prepare_async (test->cert, features, fetch_callback_result, test);
  g_main_loop_run (test->loop);
  tp_proxy_prepare_finish (test->cert, test->result, &error);
  g_assert_no_error (error);

  /* Clear for any future async stuff */
  g_object_unref (test->result);
  test->result = NULL;
}

/* A simple test to make sure the test infrastructure is working */
static void
test_certificate_mock_basics (Test *test,
        gconstpointer data G_GNUC_UNUSED)
{
  GError *error = NULL;

  test->mock = mock_tls_certificate_new_and_register (test->dbus,
          "server-cert.cer", NULL);

  ensure_certificate_proxy (test);

  tp_tls_certificate_accept_async (test->cert, fetch_callback_result, test);
  g_main_loop_run (test->loop);
  tp_tls_certificate_accept_finish (test->cert, test->result, &error);
  g_assert_no_error (error);

  g_assert (test->mock->state == TP_TLS_CERTIFICATE_STATE_ACCEPTED);
}

#if 0

static void
test_certificate_verify_success_with_pkcs11_lookup (Test *test,
        gconstpointer data G_GNUC_UNUSED)
{
  TpTLSCertificateRejectReason reason = 0;
  GError *error = NULL;
  EmpathyTLSVerifier *verifier;
  const gchar *reference_identities[] = {
    "test-server.empathy.gnome.org",
    NULL
  };

  /*
   * In this test the mock TLS connection only has one certificate
   * not a full certificat echain. The root anchor certificate is
   * retrieved from PKCS#11 storage.
   */

  test->mock = mock_tls_certificate_new_and_register (test->dbus,
          "server-cert.cer", NULL);

  /* We add the collabora directory with the collabora root */
  add_certificate_to_mock (test, "certificate-authority.cer", NULL);

  ensure_certificate_proxy (test);

  verifier = empathy_tls_verifier_new (test->cert, "test-server.empathy.gnome.org",
      reference_identities);
  empathy_tls_verifier_verify_async (verifier, fetch_callback_result, test);
  g_main_loop_run (test->loop);

  empathy_tls_verifier_verify_finish (verifier, test->result, &reason,
      NULL, &error);
  g_assert_no_error (error);

  /* Yay the verification was a success! */

  g_clear_error (&error);
  g_object_unref (verifier);
}

#endif

static void
test_certificate_verify_success_with_full_chain (Test *test,
        gconstpointer data G_GNUC_UNUSED)
{
  TpTLSCertificateRejectReason reason = 0;
  GError *error = NULL;
  EmpathyTLSVerifier *verifier;
  const gchar *reference_identities[] = {
    "test-server.empathy.gnome.org",
    NULL
  };

  /*
   * In this test the mock TLS connection has a full certificate
   * chain. We look for an anchor certificate in the chain.
   */

  test->mock = mock_tls_certificate_new_and_register (test->dbus,
          "server-cert.cer", "certificate-authority.cer", NULL);

  /* We add the collabora directory with the collabora root */
  add_certificate_to_mock (test, "certificate-authority.cer", NULL);

  ensure_certificate_proxy (test);

  verifier = empathy_tls_verifier_new (test->cert, "test-server.empathy.gnome.org",
      reference_identities);
  empathy_tls_verifier_set_database (verifier, test->database);
  empathy_tls_verifier_verify_async (verifier, fetch_callback_result, test);
  g_main_loop_run (test->loop);
  empathy_tls_verifier_verify_finish (verifier, test->result, &reason,
      NULL, &error);
  g_assert_no_error (error);

  /* Yay the verification was a success! */

  g_clear_error (&error);
  g_object_unref (verifier);
}

static void
test_certificate_verify_root_not_found (Test *test,
        gconstpointer data G_GNUC_UNUSED)
{
  TpTLSCertificateRejectReason reason = 0;
  GError *error = NULL;
  EmpathyTLSVerifier *verifier;
  const gchar *reference_identities[] = {
    "test-server.empathy.gnome.org",
    NULL
  };

  test->mock = mock_tls_certificate_new_and_register (test->dbus,
          "server-cert.cer", NULL);

  /* Note that we're not adding any place to find root certs */

  ensure_certificate_proxy (test);

  verifier = empathy_tls_verifier_new (test->cert, "test-server.empathy.gnome.org",
      reference_identities);
  empathy_tls_verifier_verify_async (verifier, fetch_callback_result, test);
  g_main_loop_run (test->loop);

  empathy_tls_verifier_verify_finish (verifier, test->result, &reason,
      NULL, &error);

  /* And it should say we're untrusted */
  g_assert_error (error, G_IO_ERROR,
      TP_TLS_CERTIFICATE_REJECT_REASON_UNTRUSTED);

  g_clear_error (&error);
  g_object_unref (verifier);
}

static void
test_certificate_verify_root_not_anchored (Test *test,
        gconstpointer data G_GNUC_UNUSED)
{
  TpTLSCertificateRejectReason reason = 0;
  GError *error = NULL;
  EmpathyTLSVerifier *verifier;
  const gchar *reference_identities[] = {
    "test-server.empathy.gnome.org",
    NULL
  };

  test->mock = mock_tls_certificate_new_and_register (test->dbus,
          "server-cert.cer", "certificate-authority.cer", NULL);

  /* Note that we're not adding any place to find root certs */

  ensure_certificate_proxy (test);

  verifier = empathy_tls_verifier_new (test->cert, "test-server.empathy.gnome.org",
      reference_identities);
  empathy_tls_verifier_verify_async (verifier, fetch_callback_result, test);
  g_main_loop_run (test->loop);

  empathy_tls_verifier_verify_finish (verifier, test->result, &reason,
      NULL, &error);

  /* And it should say we're untrusted */
  g_assert_error (error, G_IO_ERROR,
      TP_TLS_CERTIFICATE_REJECT_REASON_UNTRUSTED);

  g_clear_error (&error);
  g_object_unref (verifier);
}

static void
test_certificate_verify_identities_invalid (Test *test,
        gconstpointer data G_GNUC_UNUSED)
{
  TpTLSCertificateRejectReason reason = 0;
  GError *error = NULL;
  EmpathyTLSVerifier *verifier;
  const gchar *reference_identities[] = {
    "invalid.host.name",
    NULL
  };

  test->mock = mock_tls_certificate_new_and_register (test->dbus,
          "server-cert.cer", "certificate-authority.cer", NULL);

  /* We add the collabora directory with the collabora root */
  add_certificate_to_mock (test, "certificate-authority.cer", NULL);

  ensure_certificate_proxy (test);

  verifier = empathy_tls_verifier_new (test->cert, "invalid.host.name",
      reference_identities);
  empathy_tls_verifier_set_database (verifier, test->database);
  empathy_tls_verifier_verify_async (verifier, fetch_callback_result, test);
  g_main_loop_run (test->loop);

  empathy_tls_verifier_verify_finish (verifier, test->result, &reason,
      NULL, &error);

  /* And it should say we're self-signed (oddly enough) */
  g_assert_error (error, G_IO_ERROR,
      TP_TLS_CERTIFICATE_REJECT_REASON_HOSTNAME_MISMATCH);

  g_clear_error (&error);
  g_object_unref (verifier);
}

static void
test_certificate_verify_uses_reference_identities (Test *test,
        gconstpointer data G_GNUC_UNUSED)
{
  TpTLSCertificateRejectReason reason = 0;
  GError *error = NULL;
  EmpathyTLSVerifier *verifier;
  const gchar *reference_identities[] = {
    "invalid.host.name",
    NULL
  };

  test->mock = mock_tls_certificate_new_and_register (test->dbus,
          "server-cert.cer", "certificate-authority.cer", NULL);

  /* We add the collabora directory with the collabora root */
  add_certificate_to_mock (test, "certificate-authority.cer", NULL);

  ensure_certificate_proxy (test);

  /* Should be using the reference_identities and not host name for checks */
  verifier = empathy_tls_verifier_new (test->cert, "test-server.empathy.gnome.org",
      reference_identities);
  empathy_tls_verifier_set_database (verifier, test->database);
  empathy_tls_verifier_verify_async (verifier, fetch_callback_result, test);
  g_main_loop_run (test->loop);

  empathy_tls_verifier_verify_finish (verifier, test->result, &reason,
      NULL, &error);

  /* And it should say we're self-signed (oddly enough) */
  g_assert_error (error, G_IO_ERROR,
      TP_TLS_CERTIFICATE_REJECT_REASON_HOSTNAME_MISMATCH);

  g_clear_error (&error);
  g_object_unref (verifier);
}

static void
test_certificate_verify_success_with_pinned (Test *test,
        gconstpointer data G_GNUC_UNUSED)
{
  TpTLSCertificateRejectReason reason = 0;
  GError *error = NULL;
  EmpathyTLSVerifier *verifier;
  const gchar *reference_identities[] = {
    "test-server.empathy.gnome.org",
    NULL
  };

  /*
   * In this test the mock TLS connection has a certificate that has
   * been pinned for the test-server.empathy.gnome.org peer.
   */

  test->mock = mock_tls_certificate_new_and_register (test->dbus,
          "server-cert.cer", NULL);

  /* We add the collabora directory with the collabora root */
  add_certificate_to_mock (test, "server-cert.cer", "test-server.empathy.gnome.org");

  ensure_certificate_proxy (test);

  verifier = empathy_tls_verifier_new (test->cert, "test-server.empathy.gnome.org",
      reference_identities);
  empathy_tls_verifier_verify_async (verifier, fetch_callback_result, test);
  g_main_loop_run (test->loop);
  empathy_tls_verifier_verify_finish (verifier, test->result, &reason,
      NULL, &error);
  g_assert_no_error (error);

  /* Yay the verification was a success! */

  g_clear_error (&error);
  g_object_unref (verifier);
}

static void
test_certificate_verify_pinned_wrong_host (Test *test,
        gconstpointer data G_GNUC_UNUSED)
{
  TpTLSCertificateRejectReason reason = 0;
  GError *error = NULL;
  EmpathyTLSVerifier *verifier;
  const gchar *reference_identities[] = {
    "test-server.empathy.gnome.org",
    NULL
  };

  test->mock = mock_tls_certificate_new_and_register (test->dbus,
          "server-cert.cer", NULL);

  /* We add the collabora directory with the collabora root */
  add_certificate_to_mock (test, "server-cert.cer", "test-server.empathy.gnome.org");

  ensure_certificate_proxy (test);

  verifier = empathy_tls_verifier_new (test->cert, "another.gnome.org",
      reference_identities);
  empathy_tls_verifier_verify_async (verifier, fetch_callback_result, test);
  g_main_loop_run (test->loop);

  empathy_tls_verifier_verify_finish (verifier, test->result, &reason,
      NULL, &error);

  /* And it should say we're untrusted */
  g_assert_error (error, G_IO_ERROR,
      TP_TLS_CERTIFICATE_REJECT_REASON_UNTRUSTED);

  g_clear_error (&error);
  g_object_unref (verifier);
}

int
main (int argc,
    char **argv)
{
  int result;

  test_init (argc, argv);

  g_test_add ("/tls/certificate_basics", Test, NULL,
          setup, test_certificate_mock_basics, teardown);
#if 0
  g_test_add ("/tls/certificate_verify_success_with_pkcs11_lookup", Test, NULL,
          setup, test_certificate_verify_success_with_pkcs11_lookup, teardown);
#endif
  g_test_add ("/tls/certificate_verify_success_with_full_chain", Test, NULL,
          setup, test_certificate_verify_success_with_full_chain, teardown);
  g_test_add ("/tls/certificate_verify_root_not_found", Test, NULL,
          setup, test_certificate_verify_root_not_found, teardown);
  g_test_add ("/tls/certificate_verify_root_not_anchored", Test, NULL,
          setup, test_certificate_verify_root_not_anchored, teardown);
  g_test_add ("/tls/certificate_verify_identities_invalid", Test, NULL,
          setup, test_certificate_verify_identities_invalid, teardown);
  g_test_add ("/tls/certificate_verify_uses_reference_identities", Test, NULL,
          setup, test_certificate_verify_uses_reference_identities, teardown);
  g_test_add ("/tls/certificate_verify_success_with_pinned", Test, NULL,
          setup, test_certificate_verify_success_with_pinned, teardown);
  g_test_add ("/tls/certificate_verify_pinned_wrong_host", Test, NULL,
          setup, test_certificate_verify_pinned_wrong_host, teardown);

  result = g_test_run ();
  test_deinit ();
  return result;
}
