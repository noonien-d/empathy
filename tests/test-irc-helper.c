#include "config.h"
#include "test-irc-helper.h"

void
check_server (TpawIrcServer *server,
              const gchar *_address,
              guint _port,
              gboolean _ssl)
{
  gchar *address;
  guint port;
  gboolean ssl;

  g_assert (server != NULL);

  g_object_get (server,
      "address", &address,
      "port", &port,
      "ssl", &ssl,
      NULL);

  g_assert (address != NULL && strcmp (address, _address) == 0);
  g_assert (port == _port);
  g_assert (ssl == _ssl);

  g_free (address);
}

void
check_network (TpawIrcNetwork *network,
              const gchar *_name,
              const gchar *_charset,
              struct server_t *_servers,
              guint nb_servers)
{
  gchar  *name, *charset;
  GSList *servers, *l;
  guint i;

  g_assert (network != NULL);

  g_object_get (network,
      "name", &name,
      "charset", &charset,
      NULL);

  g_assert (name != NULL && strcmp (name, _name) == 0);
  g_assert (charset != NULL && strcmp (charset, _charset) == 0);

  servers = tpaw_irc_network_get_servers (network);
  g_assert (g_slist_length (servers) == nb_servers);

  /* Is that the right servers ? */
  for (l = servers, i = 0; l != NULL; l = g_slist_next (l), i++)
    {
      TpawIrcServer *server;
      gchar *address;
      guint port;
      gboolean ssl;

      server = l->data;

      g_object_get (server,
          "address", &address,
          "port", &port,
          "ssl", &ssl,
          NULL);

      g_assert (address != NULL && strcmp (address, _servers[i].address)
          == 0);
      g_assert (port == _servers[i].port);
      g_assert (ssl == _servers[i].ssl);

      g_free (address);
    }

  g_slist_foreach (servers, (GFunc) g_object_unref, NULL);
  g_slist_free (servers);
  g_free (name);
  g_free (charset);
}
