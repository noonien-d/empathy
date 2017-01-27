/*
 * Copyright (C) 2008-2012 Collabora Ltd.
 * Copyright (C) 2012 Red Hat, Inc.
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
 *
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 */

#include "config.h"
#include "empathy-theme-adium.h"

#include <glib/gi18n-lib.h>
#include <tp-account-widgets/tpaw-images.h>
#include <tp-account-widgets/tpaw-time.h>
#include <tp-account-widgets/tpaw-pixbuf-utils.h>
#include <tp-account-widgets/tpaw-utils.h>

#include "empathy-gsettings.h"
#include "empathy-images.h"
#include "empathy-plist.h"
#include "empathy-smiley-manager.h"
#include "empathy-ui-utils.h"
#include "empathy-utils.h"
#include "empathy-webkit-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CHAT
#include "empathy-debug.h"

#define BORING_DPI_DEFAULT 96

/* "Join" consecutive messages with timestamps within five minutes */
#define MESSAGE_JOIN_PERIOD 5*60

struct _EmpathyThemeAdiumPriv
{
  EmpathyAdiumData *data;
  EmpathySmileyManager *smiley_manager;
  EmpathyContact *first_contact;
  EmpathyContact *last_contact;
  gint64 first_timestamp;
  gint64 last_timestamp;
  gboolean first_is_backlog;
  gboolean last_is_backlog;
  guint pages_loading;
  /* Queue of QueuedItem*s containing an EmpathyMessage or string */
  GQueue message_queue;
  /* Queue of guint32 of pending message id to remove unread
   * marker for when we lose focus. */
  GQueue acked_messages;
  GtkWidget *inspector_window;

  GSettings *gsettings_chat;
  GSettings *gsettings_desktop;

  gboolean has_focus;
  gboolean has_unread_message;
  gboolean allow_scrolling;
  gchar *variant;
  gboolean in_construction;
  gboolean show_avatars;
};

struct _EmpathyAdiumData
{
  gint ref_count;
  gchar *path;
  gchar *basedir;
  gchar *default_avatar_filename;
  gchar *default_incoming_avatar_filename;
  gchar *default_outgoing_avatar_filename;
  GHashTable *info;
  guint version;
  gboolean custom_template;
  /* gchar* -> gchar* both owned */
  GHashTable *date_format_cache;

  /* HTML bits */
  const gchar *template_html;
  const gchar *content_html;
  const gchar *in_content_html;
  const gchar *in_context_html;
  const gchar *in_nextcontent_html;
  const gchar *in_nextcontext_html;
  const gchar *out_content_html;
  const gchar *out_context_html;
  const gchar *out_nextcontent_html;
  const gchar *out_nextcontext_html;
  const gchar *status_html;

  /* Above html strings are pointers to strings stored in this array.
   * We do this because of fallbacks, some htmls could be pointing the
   * same string. */
  GPtrArray *strings_to_free;
};

static gchar * adium_info_dup_path_for_variant (GHashTable *info,
    const gchar *variant);

enum
{
  PROP_0,
  PROP_ADIUM_DATA,
  PROP_VARIANT,
};

G_DEFINE_TYPE (EmpathyThemeAdium, empathy_theme_adium,
       WEBKIT_TYPE_WEB_VIEW)

enum
{
  QUEUED_EVENT,
  QUEUED_MESSAGE,
  QUEUED_EDIT
};

typedef struct
{
  guint type;
  EmpathyMessage *msg;
  char *str;
  gboolean should_highlight;
} QueuedItem;

static QueuedItem *
queue_item (GQueue *queue,
    guint type,
    EmpathyMessage *msg,
    const char *str,
    gboolean should_highlight,
    gboolean prepend)
{
  QueuedItem *item = g_slice_new0 (QueuedItem);

  item->type = type;
  if (msg != NULL)
    item->msg = g_object_ref (msg);
  item->str = g_strdup (str);
  item->should_highlight = should_highlight;

  if (prepend)
    g_queue_push_head (queue, item);
  else
    g_queue_push_tail (queue, item);

  return item;
}

static void
free_queued_item (QueuedItem *item)
{
  tp_clear_object (&item->msg);
  g_free (item->str);

  g_slice_free (QueuedItem, item);
}

static gboolean
theme_adium_policy_decision_requested_cb (WebKitWebView *view,
    WebKitPolicyDecision *decision,
    WebKitPolicyDecisionType decision_type,
    gpointer data)
{
  if (decision_type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
    return FALSE;

  return empathy_webkit_handle_navigation (view, WEBKIT_NAVIGATION_POLICY_DECISION (decision));
}

/* Replace each %@ in format with string passed in args */
static gchar *
string_with_format (const gchar *format,
    const gchar *first_string,
    ...)
{
  va_list args;
  const gchar *str;
  GString *result;

  va_start (args, first_string);
  result = g_string_sized_new (strlen (format));
  for (str = first_string; str != NULL; str = va_arg (args, const gchar *))
    {
      const gchar *next;

      next = strstr (format, "%@");
      if (next == NULL)
        break;

      g_string_append_len (result, format, next - format);
      g_string_append (result, str);
      format = next + 2;
    }
  g_string_append (result, format);
  va_end (args);

  return g_string_free (result, FALSE);
}

static void
theme_adium_load_template (EmpathyThemeAdium *self)
{
  gchar *basedir_uri;
  gchar *variant_path;
  gchar *template;

  self->priv->pages_loading++;
  basedir_uri = g_strconcat ("file://", self->priv->data->basedir, NULL);

  variant_path = adium_info_dup_path_for_variant (self->priv->data->info,
      self->priv->variant);

  template = string_with_format (self->priv->data->template_html,
    variant_path, NULL);

  webkit_web_view_load_html (WEBKIT_WEB_VIEW (self),
      template, basedir_uri);

  g_free (basedir_uri);
  g_free (variant_path);
  g_free (template);
}

static gchar *
theme_adium_parse_body (EmpathyThemeAdium *self,
  const gchar *text,
  const gchar *token)
{
  TpawStringParser *parsers;
  GString *string;

  /* Check if we have to parse smileys */
  parsers = empathy_webkit_get_string_parser (
    g_settings_get_boolean (self->priv->gsettings_chat,
      EMPATHY_PREFS_CHAT_SHOW_SMILEYS));

  /* Parse text and construct string with links and smileys replaced
   * by html tags. Also escape text to make sure html code is
   * displayed verbatim. */
  string = g_string_sized_new (strlen (text));

  /* wrap this in HTML that allows us to find the message for later
   * editing */
  if (!tp_str_empty (token))
    g_string_append_printf (string,
      "<span id=\"message-token-%s\">",
      token);

  tpaw_string_parser_substr (text, -1, parsers, string);

  if (!tp_str_empty (token))
    g_string_append (string, "</span>");

  /* Wrap body in order to make tabs and multiple spaces displayed
   * properly. See bug #625745. */
  g_string_prepend (string, "<div style=\"display: inline; "
                 "white-space: pre-wrap\"'>");
  g_string_append (string, "</div>");

  return g_string_free (string, FALSE);
}

static void
escape_and_append_len (GString *string, const gchar *str, gint len)
{
  while (str != NULL && *str != '\0' && len != 0)
    {
      switch (*str)
        {
          case '\\':
            /* \ becomes \\ */
            g_string_append (string, "\\\\");
            break;
          case '\"':
            /* " becomes \" */
            g_string_append (string, "\\\"");
            break;
          case '\n':
            /* Remove end of lines */
            break;
          default:
            g_string_append_c (string, *str);
        }

      str++;
      len--;
    }
}

/* If *str starts with match, returns TRUE and move pointer to the end */
static gboolean
theme_adium_match (const gchar **str,
    const gchar *match)
{
  gint len;

  len = strlen (match);
  if (strncmp (*str, match, len) == 0)
    {
      *str += len - 1;
      return TRUE;
    }

  return FALSE;
}

/* Like theme_adium_match() but also return the X part if match is
 * like %foo{X}% */
static gboolean
theme_adium_match_with_format (const gchar **str,
    const gchar *match,
    gchar **format)
{
  const gchar *cur = *str;
  const gchar *end;

  if (!theme_adium_match (&cur, match))
    return FALSE;

  cur++;

  end = strstr (cur, "}%");
  if (!end)
    return FALSE;

  *format = g_strndup (cur , end - cur);
  *str = end + 1;
  return TRUE;
}

/* List of colors used by %senderColor%. Copied from
 * adium/Frameworks/AIUtilities\ Framework/Source/AIColorAdditions.m
 */
static gchar *colors[] = {
  "aqua", "aquamarine", "blue", "blueviolet", "brown", "burlywood", "cadetblue",
  "chartreuse", "chocolate", "coral", "cornflowerblue", "crimson", "cyan",
  "darkblue", "darkcyan", "darkgoldenrod", "darkgreen", "darkgrey", "darkkhaki",
  "darkmagenta", "darkolivegreen", "darkorange", "darkorchid", "darkred",
  "darksalmon", "darkseagreen", "darkslateblue", "darkslategrey",
  "darkturquoise", "darkviolet", "deeppink", "deepskyblue", "dimgrey",
  "dodgerblue", "firebrick", "forestgreen", "fuchsia", "gold", "goldenrod",
  "green", "greenyellow", "grey", "hotpink", "indianred", "indigo", "lawngreen",
  "lightblue", "lightcoral",
  "lightgreen", "lightgrey", "lightpink", "lightsalmon", "lightseagreen",
  "lightskyblue", "lightslategrey", "lightsteelblue", "lime", "limegreen",
  "magenta", "maroon", "mediumaquamarine", "mediumblue", "mediumorchid",
  "mediumpurple", "mediumseagreen", "mediumslateblue", "mediumspringgreen",
  "mediumturquoise", "mediumvioletred", "midnightblue", "navy", "olive",
  "olivedrab", "orange", "orangered", "orchid", "palegreen", "paleturquoise",
  "palevioletred", "peru", "pink", "plum", "powderblue", "purple", "red",
  "rosybrown", "royalblue", "saddlebrown", "salmon", "sandybrown", "seagreen",
  "sienna", "silver", "skyblue", "slateblue", "slategrey", "springgreen",
  "steelblue", "tan", "teal", "thistle", "tomato", "turquoise", "violet",
  "yellowgreen",
};

static const gchar *
nsdate_to_strftime (EmpathyAdiumData *data, const gchar *nsdate)
{
  /* Convert from NSDateFormatter
   * (http://www.stepcase.com/blog/2008/12/02/format-string-for-the-iphone-nsdateformatter/)
   * to strftime supported by g_date_time_format.
   * FIXME: table is incomplete, doc of g_date_time_format has a table of
   *        supported tags.
   * FIXME: g_date_time_format in GLib 2.28 does 0 padding by default, but
   *        in 2.29.x we have to explictely request padding with %0x */
  static const gchar *convert_table[] = {
    "a", "%p", // AM/PM
    "A", NULL, // 0~86399999 (Millisecond of Day)

    "cccc", "%A", // Sunday/Monday/Tuesday/Wednesday/Thursday/Friday/Saturday
    "ccc", "%a", // Sun/Mon/Tue/Wed/Thu/Fri/Sat
    "cc", "%u", // 1~7 (Day of Week)
    "c", "%u", // 1~7 (Day of Week)

    "dd", "%d", // 1~31 (0 padded Day of Month)
    "d", "%d", // 1~31 (0 padded Day of Month)
    "D", "%j", // 1~366 (0 padded Day of Year)

    "e", "%u", // 1~7 (0 padded Day of Week)
    "EEEE", "%A", // Sunday/Monday/Tuesday/Wednesday/Thursday/Friday/Saturday
    "EEE", "%a", // Sun/Mon/Tue/Wed/Thu/Fri/Sat
    "EE", "%a", // Sun/Mon/Tue/Wed/Thu/Fri/Sat
    "E", "%a", // Sun/Mon/Tue/Wed/Thu/Fri/Sat

    "F", NULL, // 1~5 (0 padded Week of Month, first day of week = Monday)

    "g", NULL, // Julian Day Number (number of days since 4713 BC January 1)
    "GGGG", NULL, // Before Christ/Anno Domini
    "GGG", NULL, // BC/AD (Era Designator Abbreviated)
    "GG", NULL, // BC/AD (Era Designator Abbreviated)
    "G", NULL, // BC/AD (Era Designator Abbreviated)

    "h", "%I", // 1~12 (0 padded Hour (12hr))
    "H", "%H", // 0~23 (0 padded Hour (24hr))

    "k", NULL, // 1~24 (0 padded Hour (24hr)
    "K", NULL, // 0~11 (0 padded Hour (12hr))

    "LLLL", "%B", // January/February/March/April/May/June/July/August/September/October/November/December
    "LLL", "%b", // Jan/Feb/Mar/Apr/May/Jun/Jul/Aug/Sep/Oct/Nov/Dec
    "LL", "%m", // 1~12 (0 padded Month)
    "L", "%m", // 1~12 (0 padded Month)

    "m", "%M", // 0~59 (0 padded Minute)
    "MMMM", "%B", // January/February/March/April/May/June/July/August/September/October/November/December
    "MMM", "%b", // Jan/Feb/Mar/Apr/May/Jun/Jul/Aug/Sep/Oct/Nov/Dec
    "MM", "%m", // 1~12 (0 padded Month)
    "M", "%m", // 1~12 (0 padded Month)

    "qqqq", NULL, // 1st quarter/2nd quarter/3rd quarter/4th quarter
    "qqq", NULL, // Q1/Q2/Q3/Q4
    "qq", NULL, // 1~4 (0 padded Quarter)
    "q", NULL, // 1~4 (0 padded Quarter)
    "QQQQ", NULL, // 1st quarter/2nd quarter/3rd quarter/4th quarter
    "QQQ", NULL, // Q1/Q2/Q3/Q4
    "QQ", NULL, // 1~4 (0 padded Quarter)
    "Q", NULL, // 1~4 (0 padded Quarter)

    "s", "%S", // 0~59 (0 padded Second)
    "S", NULL, // (rounded Sub-Second)

    "u", "%Y", // (0 padded Year)

    "vvvv", "%Z", // (General GMT Timezone Name)
    "vvv", "%Z", // (General GMT Timezone Abbreviation)
    "vv", "%Z", // (General GMT Timezone Abbreviation)
    "v", "%Z", // (General GMT Timezone Abbreviation)

    "w", "%W", // 1~53 (0 padded Week of Year, 1st day of week = Sunday, NB, 1st week of year starts from the last Sunday of last year)
    "W", NULL, // 1~5 (0 padded Week of Month, 1st day of week = Sunday)

    "yyyy", "%Y", // (Full Year)
    "yyy", "%y", // (2 Digits Year)
    "yy", "%y", // (2 Digits Year)
    "y", "%Y", // (Full Year)
    "YYYY", NULL, // (Full Year, starting from the Sunday of the 1st week of year)
    "YYY", NULL, // (2 Digits Year, starting from the Sunday of the 1st week of year)
    "YY", NULL, // (2 Digits Year, starting from the Sunday of the 1st week of year)
    "Y", NULL, // (Full Year, starting from the Sunday of the 1st week of year)

    "zzzz", NULL, // (Specific GMT Timezone Name)
    "zzz", NULL, // (Specific GMT Timezone Abbreviation)
    "zz", NULL, // (Specific GMT Timezone Abbreviation)
    "z", NULL, // (Specific GMT Timezone Abbreviation)
    "Z", "%z", // +0000 (RFC 822 Timezone)
  };
  const gchar *str;
  GString *string;
  guint i, j;

  if (nsdate == NULL)
    return NULL;

  str = g_hash_table_lookup (data->date_format_cache, nsdate);
  if (str != NULL) {
    return str;
  }

  /* Copy nsdate into string, replacing occurences of NSDateFormatter tags
   * by corresponding strftime tag. */
  string = g_string_sized_new (strlen (nsdate));
  for (i = 0; nsdate[i] != '\0'; i++)
    {
      gboolean found = FALSE;

      /* even indexes are NSDateFormatter tag, odd indexes are the
       * corresponding strftime tag */
      for (j = 0; j < G_N_ELEMENTS (convert_table); j += 2)
        {
          if (g_str_has_prefix (nsdate + i, convert_table[j]))
            {
              found = TRUE;
              break;
            }
        }

      if (found)
        {
          /* If we don't have a replacement, just ignore that tag */
          if (convert_table[j + 1] != NULL)
            g_string_append (string, convert_table[j + 1]);

          i += strlen (convert_table[j]) - 1;
        }
      else
        {
          g_string_append_c (string, nsdate[i]);
        }
    }

  DEBUG ("Date format converted '%s' → '%s'", nsdate, string->str);

  /* The cache takes ownership of string->str */
  g_hash_table_insert (data->date_format_cache, g_strdup (nsdate), string->str);
  return g_string_free (string, FALSE);
}

static void
theme_adium_add_html (EmpathyThemeAdium *self,
    const gchar *func,
    const gchar *html,
    const gchar *message,
    const gchar *avatar_filename,
    const gchar *name,
    const gchar *contact_id,
    const gchar *service_name,
    const gchar *message_classes,
    gint64 timestamp,
    gboolean is_backlog,
    gboolean outgoing,
    PangoDirection direction)
{
  GBytes *bytes;
  GString *string;
  const gchar *cur = NULL;
  const gchar *js;
  gchar *script;

  /* Make some search-and-replace in the html code */
  string = g_string_sized_new (strlen (html) + strlen (message));
  g_string_append_printf (string, "%s(\"", func);

  for (cur = html; *cur != '\0'; cur++)
    {
      const gchar *replace = NULL;
      gchar *dup_replace = NULL;
      gchar *format = NULL;

      /* Those are all well known keywords that needs replacement in
       * html files. Please keep them in the same order than the adium
       * spec. See http://trac.adium.im/wiki/CreatingMessageStyles */
      if (theme_adium_match (&cur, "%userIconPath%"))
        {
          replace = avatar_filename;
        }
      else if (theme_adium_match (&cur, "%senderScreenName%"))
        {
          replace = contact_id;
        }
      else if (theme_adium_match (&cur, "%sender%"))
        {
          replace = name;
        }
      else if (theme_adium_match (&cur, "%senderColor%"))
        {
          /* A color derived from the user's name.
           * FIXME: If a colon separated list of HTML colors is at
           * Incoming/SenderColors.txt it will be used instead of
           * the default colors.
           */

          /* Ensure we always use the same color when sending messages
           * (bgo #658821) */
          if (outgoing)
            {
              replace = "inherit";
            }
          else if (contact_id != NULL)
            {
              guint hash = g_str_hash (contact_id);
              replace = colors[hash % G_N_ELEMENTS (colors)];
            }
        }
      else if (theme_adium_match (&cur, "%senderStatusIcon%"))
        {
          /* FIXME: The path to the status icon of the sender
           * (available, away, etc...)
           */
        }
      else if (theme_adium_match (&cur, "%messageDirection%"))
        {
          switch (direction)
            {
              case PANGO_DIRECTION_LTR:
              case PANGO_DIRECTION_TTB_LTR:
              case PANGO_DIRECTION_WEAK_LTR:
                replace = "ltr";
                break;
              case PANGO_DIRECTION_RTL:
              case PANGO_DIRECTION_TTB_RTL:
              case PANGO_DIRECTION_WEAK_RTL:
                replace = "rtl";
                break;
              case PANGO_DIRECTION_NEUTRAL:
              default:
                break;
            }
        }
      else if (theme_adium_match (&cur, "%senderDisplayName%"))
        {
          /* FIXME: The serverside (remotely set) name of the
           * sender, such as an MSN display name.
           *
           *  We don't have access to that yet so we use
           * local alias instead.
           */
          replace = name;
        }
      else if (theme_adium_match (&cur, "%senderPrefix%"))
        {
          /* FIXME: If we supported IRC user mode flags, this
           * would be replaced with @ if the user is an op, + if
           * the user has voice, etc. as per
           * http://hg.adium.im/adium/rev/b586b027de42. But we
           * don't, so for now we just strip it. */
        }
      else if (theme_adium_match_with_format (&cur, "%textbackgroundcolor{",
            &format))
        {
          /* FIXME: This keyword is used to represent the
           * highlight background color. "X" is the opacity of the
           * background, ranges from 0 to 1 and can be any decimal
           * between.
           */
        }
      else if (theme_adium_match (&cur, "%message%"))
        {
          replace = message;
        }
      else if (theme_adium_match (&cur, "%time%") ||
           theme_adium_match_with_format (&cur, "%time{", &format))
        {
          const gchar *strftime_format;

          strftime_format = nsdate_to_strftime (self->priv->data, format);
          if (is_backlog)
            dup_replace = tpaw_time_to_string_local (timestamp,
              strftime_format ? strftime_format :
              TPAW_TIME_DATE_FORMAT_DISPLAY_SHORT);
          else
            dup_replace = tpaw_time_to_string_local (timestamp,
              strftime_format ? strftime_format :
              TPAW_TIME_FORMAT_DISPLAY_SHORT);

          replace = dup_replace;
        }
      else if (theme_adium_match (&cur, "%shortTime%"))
        {
          dup_replace = tpaw_time_to_string_local (timestamp,
            TPAW_TIME_FORMAT_DISPLAY_SHORT);
          replace = dup_replace;
        }
      else if (theme_adium_match (&cur, "%service%"))
        {
          replace = service_name;
        }
      else if (theme_adium_match (&cur, "%variant%"))
        {
          /* FIXME: The name of the active message style variant,
           * with all spaces replaced with an underscore.
           * A variant named "Alternating Messages - Blue Red"
           * will become "Alternating_Messages_-_Blue_Red".
           */
        }
      else if (theme_adium_match (&cur, "%userIcons%"))
        {
          replace = self->priv->show_avatars ? "showIcons" : "hideIcons";
        }
      else if (theme_adium_match (&cur, "%messageClasses%"))
        {
          replace = message_classes;
        }
      else if (theme_adium_match (&cur, "%status%"))
        {
          /* FIXME: A description of the status event. This is
           * neither in the user's local language nor expected to
           * be displayed; it may be useful to use a different div
           * class to present different types of status messages.
           * The following is a list of some of the more important
           * status messages; your message style should be able to
           * handle being shown a status message not in this list,
           * as even at present the list is incomplete and is
           * certain to become out of date in the future:
           *  online
           *  offline
           *  away
           *  away_message
           *  return_away
           *  idle
           *  return_idle
           *  date_separator
           *  contact_joined (group chats)
           *  contact_left
           *  error
           *  timed_out
           *  encryption (all OTR messages use this status)
           *  purple (all IRC topic and join/part messages use this status)
           *  fileTransferStarted
           *  fileTransferCompleted
           */
        }
      else
        {
          escape_and_append_len (string, cur, 1);
          continue;
        }

      /* Here we have a replacement to make */
      escape_and_append_len (string, replace, -1);

      g_free (dup_replace);
      g_free (format);
    }
  g_string_append (string, "\")");

  bytes = g_resources_lookup_data ("/org/gnome/Empathy/Chat/empathy-chat.js",
      G_RESOURCE_LOOKUP_FLAGS_NONE,
      NULL);

  if (bytes != NULL)
    {
      js = (const gchar *) g_bytes_get_data (bytes, NULL);
      g_string_prepend (string, js);
      g_bytes_unref (bytes);
    }

  script = g_string_free (string, FALSE);
  webkit_web_view_run_javascript (WEBKIT_WEB_VIEW (self), script, NULL, NULL, NULL);
  g_free (script);
}

static void
theme_adium_append_event_escaped (EmpathyThemeAdium *self,
    const gchar *escaped,
    PangoDirection direction)
{
  theme_adium_add_html (self, "appendMessage",
      self->priv->data->status_html, escaped, NULL, NULL, NULL,
      NULL, "event", tpaw_time_get_current (), FALSE, FALSE, direction);

  /* There is no last contact */
  if (self->priv->last_contact)
    {
      g_object_unref (self->priv->last_contact);
      self->priv->last_contact = NULL;
    }
}
#if 0
/* FIXME: check what this is for and port to WebKit2 */
static void
theme_adium_remove_focus_marks (EmpathyThemeAdium *self,
    WebKitDOMNodeList *nodes)
{
  guint i;

  /* Remove focus and firstFocus class */
  for (i = 0; i < webkit_dom_node_list_get_length (nodes); i++)
    {
      WebKitDOMNode *node = webkit_dom_node_list_item (nodes, i);
      WebKitDOMElement *element = WEBKIT_DOM_ELEMENT (node);
      gchar *class_name;
      gchar **classes, **iter;
      GString *new_class_name;
      gboolean first = TRUE;

      if (element == NULL)
        continue;

      class_name = webkit_dom_element_get_class_name (element);
      classes = g_strsplit (class_name, " ", -1);
      new_class_name = g_string_sized_new (strlen (class_name));

      for (iter = classes; *iter != NULL; iter++)
        {
          if (tp_strdiff (*iter, "focus") &&
              tp_strdiff (*iter, "firstFocus"))
            {
              if (!first)
                g_string_append_c (new_class_name, ' ');

              g_string_append (new_class_name, *iter);
              first = FALSE;
            }
        }

      webkit_dom_element_set_class_name (element, new_class_name->str);

      g_free (class_name);
      g_strfreev (classes);
      g_string_free (new_class_name, TRUE);
    }
}
#endif
static void
theme_adium_remove_all_focus_marks (EmpathyThemeAdium *self)
{
#if 0
  /* FIXME: check what this is for and port to WebKit2 */
  WebKitDOMDocument *dom;
  WebKitDOMNodeList *nodes;
  GError *error = NULL;

  if (!self->priv->has_unread_message)
    return;

  self->priv->has_unread_message = FALSE;

  dom = webkit_web_view_get_dom_document (WEBKIT_WEB_VIEW (self));
  if (dom == NULL)
    return;

  /* Get all nodes with focus class */
  nodes = webkit_dom_document_query_selector_all (dom, ".focus", &error);

  if (nodes == NULL)
    {
      DEBUG ("Error getting focus nodes: %s",
        error ? error->message : "No error");
      g_clear_error (&error);
      return;
    }

  theme_adium_remove_focus_marks (self, nodes);
#endif
}

enum
{
  ADD_CONSECUTIVE_MSG_SCROLL = 0,
  ADD_CONSECUTIVE_MSG_NO_SCROLL = 1,
  ADD_MSG_SCROLL = 2,
  ADD_MSG_NO_SCROLL = 3
};

/*
 * theme_adium_add_message:
 * @self: The #EmpathyThemeAdium used by the view.
 * @msg: An #EmpathyMessage that is to be added to the view.
 * @prev_contact: (out): The #EmpathyContact that sent the previous message.
 * @prev_timestamp: (out): Timestamp of the previous message.
 * @prev_is_backlog: (out): Whether the previous message was fetched
 * from the logs.
 * @should_highlight: Whether the message should be highlighted. eg.,
 * if it matches the user's username in multi-user chat.
 * @js_funcs: An array of JavaScript function names
 *
 * Shows @msg in the chat view by adding to @self. Addition is defined
 * by the JavaScript functions listed in @js_funcs. Common examples
 * are appending new incoming messages or prepending old messages from
 * the logs.
 *
 * @js_funcs should be an array with exactly 4 entries. The entries
 * should be the names of JavaScript functions that take the raw HTML
 * that is to be added to the view as an argument and take the following
 * actions, in this order:
 * - add a new consecutive message and scroll to it if needed,
 * - add a new consecutive message and do not scroll,
 * - add a new non-consecutive message and scroll to it if needed, and
 * - add a new non-consecutive message and do not scroll
 *
 * A message is considered to be consecutive with the previous one if
 * all the following conditions are met:
 * - senders are the same contact,
 * - last message was recieved recently,
 * - last message and this message both are/aren't backlog, and
 * - DisableCombineConsecutive is not set in theme's settings
 */
static void
theme_adium_add_message (EmpathyThemeAdium *self,
    EmpathyMessage *msg,
    EmpathyContact **prev_contact,
    gint64 *prev_timestamp,
    gboolean *prev_is_backlog,
    gboolean should_highlight,
    const gchar *js_funcs[])
{
  EmpathyContact *sender;
  TpMessage *tp_msg;
  TpAccount *account;
  gchar *body_escaped, *name_escaped;
  const gchar *name;
  const gchar *contact_id;
  EmpathyAvatar *avatar;
  const gchar *avatar_filename = NULL;
  gint64 timestamp;
  const gchar *html = NULL;
  const gchar *func;
  const gchar *service_name;
  GString *message_classes = NULL;
  gboolean is_backlog;
  gboolean consecutive;
  gboolean action;
  PangoDirection direction;


  /* Get information */
  sender = empathy_message_get_sender (msg);
  account = empathy_contact_get_account (sender);
  service_name = tpaw_protocol_name_to_display_name
    (tp_account_get_protocol_name (account));
  if (service_name == NULL)
    service_name = tp_account_get_protocol_name (account);
  timestamp = empathy_message_get_timestamp (msg);
  body_escaped = theme_adium_parse_body (self,
    empathy_message_get_body (msg),
    empathy_message_get_token (msg));
  name = empathy_contact_get_logged_alias (sender);
  contact_id = empathy_contact_get_id (sender);
  action = (empathy_message_get_tptype (msg) ==
      TP_CHANNEL_TEXT_MESSAGE_TYPE_ACTION);

  name_escaped = g_markup_escape_text (name, -1);

  /* If this is a /me probably */
  if (action)
    {
      gchar *str;

      if (self->priv->data->version >= 4 || !self->priv->data->custom_template)
        {
          str = g_strdup_printf ("<span class='actionMessageUserName'>%s</span>"
                     "<span class='actionMessageBody'>%s</span>",
                     name_escaped, body_escaped);
        }
      else
        {
          str = g_strdup_printf ("*%s*", body_escaped);
        }

      g_free (body_escaped);
      body_escaped = str;
    }

  /* Get the avatar filename, or a fallback */
  avatar = empathy_contact_get_avatar (sender);
  if (avatar)
    avatar_filename = avatar->filename;

  if (!avatar_filename)
    {
      if (empathy_contact_is_user (sender))
        avatar_filename = self->priv->data->default_outgoing_avatar_filename;
      else
        avatar_filename = self->priv->data->default_incoming_avatar_filename;

      if (!avatar_filename)
        {
          if (!self->priv->data->default_avatar_filename)
            self->priv->data->default_avatar_filename =
              tpaw_filename_from_icon_name (TPAW_IMAGE_AVATAR_DEFAULT,
                       GTK_ICON_SIZE_DIALOG);

          avatar_filename = self->priv->data->default_avatar_filename;
        }
    }

  is_backlog = empathy_message_is_backlog (msg);
  consecutive = empathy_contact_equal (*prev_contact, sender) &&
    (ABS (timestamp - *prev_timestamp) < MESSAGE_JOIN_PERIOD) &&
    (is_backlog == *prev_is_backlog) &&
    !tp_asv_get_boolean (self->priv->data->info,
             "DisableCombineConsecutive", NULL);

  /* Define message classes */
  message_classes = g_string_new ("message");
  if (!self->priv->has_focus && !is_backlog)
    {
      if (!self->priv->has_unread_message)
        {
          g_string_append (message_classes, " firstFocus");
          self->priv->has_unread_message = TRUE;
        }
      g_string_append (message_classes, " focus");
    }

  if (is_backlog)
    g_string_append (message_classes, " history");

  if (consecutive)
    g_string_append (message_classes, " consecutive");

  if (empathy_contact_is_user (sender))
    g_string_append (message_classes, " outgoing");
  else
    g_string_append (message_classes, " incoming");

  if (should_highlight)
    g_string_append (message_classes, " mention");

  if (empathy_message_get_tptype (msg) ==
      TP_CHANNEL_TEXT_MESSAGE_TYPE_AUTO_REPLY)
    g_string_append (message_classes, " autoreply");

  if (action)
    g_string_append (message_classes, " action");

  /* FIXME: other classes:
   * status - the message is a status change
   * event - the message is a notification of something happening
   *         (for example, encryption being turned on)
   * %status% - See %status% in theme_adium_add_html ()
   */

  /* This is slightly a hack, but it's the only way to add
   * arbitrary data to messages in the HTML. We add another
   * class called "x-empathy-message-id-*" to the message. This
   * way, we can remove the unread marker for this specific
   * message later. */
  tp_msg = empathy_message_get_tp_message (msg);
  if (tp_msg != NULL)
    {
      guint32 id;
      gboolean valid;

      id = tp_message_get_pending_message_id (tp_msg, &valid);
      if (valid)
        g_string_append_printf (message_classes,
            " x-empathy-message-id-%u", id);
    }

  /* Define javascript function to use */
  if (consecutive)
    func = self->priv->allow_scrolling ? js_funcs[ADD_CONSECUTIVE_MSG_SCROLL] :
      js_funcs[ADD_CONSECUTIVE_MSG_NO_SCROLL];
  else
    func = self->priv->allow_scrolling ? js_funcs[ADD_MSG_SCROLL] :
      js_funcs[ADD_MSG_NO_SCROLL];

  if (empathy_contact_is_user (sender))
    {
      /* out */
      if (is_backlog)
        /* context */
        html = consecutive ? self->priv->data->out_nextcontext_html :
          self->priv->data->out_context_html;
      else
        /* content */
        html = consecutive ? self->priv->data->out_nextcontent_html :
          self->priv->data->out_content_html;

      /* remove all the unread marks when we are sending a message */
      theme_adium_remove_all_focus_marks (self);
    }
  else
    {
      /* in */
      if (is_backlog)
        /* context */
        html = consecutive ? self->priv->data->in_nextcontext_html :
          self->priv->data->in_context_html;
      else
        /* content */
        html = consecutive ? self->priv->data->in_nextcontent_html :
          self->priv->data->in_content_html;
    }

  direction = pango_find_base_dir (empathy_message_get_body (msg), -1);

  theme_adium_add_html (self, func, html, body_escaped,
      avatar_filename, name_escaped, contact_id,
      service_name, message_classes->str,
      timestamp, is_backlog, empathy_contact_is_user (sender), direction);

  /* Keep the sender of the last displayed message */
  if (*prev_contact)
    g_object_unref (*prev_contact);

  *prev_contact = g_object_ref (sender);
  *prev_timestamp = timestamp;
  *prev_is_backlog = is_backlog;

  g_free (body_escaped);
  g_free (name_escaped);
  g_string_free (message_classes, TRUE);
}

void
empathy_theme_adium_append_message (EmpathyThemeAdium *self,
    EmpathyMessage *msg,
    gboolean should_highlight)
{
  const gchar *js_funcs[] = { "appendNextMessage",
      "appendNextMessageNoScroll",
      "appendMessage",
      "appendMessageNoScroll" };

  if (self->priv->pages_loading != 0)
    {
      queue_item (&self->priv->message_queue, QUEUED_MESSAGE, msg, NULL,
          should_highlight, FALSE);
      return;
    }

  theme_adium_add_message (self, msg, &self->priv->last_contact,
      &self->priv->last_timestamp, &self->priv->last_is_backlog,
      should_highlight, js_funcs);
}

void
empathy_theme_adium_append_event (EmpathyThemeAdium *self,
    const gchar *str)
{
  gchar *str_escaped;
  PangoDirection direction;

  if (self->priv->pages_loading != 0)
    {
      queue_item (&self->priv->message_queue, QUEUED_EVENT, NULL, str, FALSE, FALSE);
      return;
    }

  direction = pango_find_base_dir (str, -1);
  str_escaped = g_markup_escape_text (str, -1);
  theme_adium_append_event_escaped (self, str_escaped, direction);
  g_free (str_escaped);
}

void
empathy_theme_adium_append_event_markup (EmpathyThemeAdium *self,
    const gchar *markup_text,
    const gchar *fallback_text)
{
  PangoDirection direction;

  direction = pango_find_base_dir (fallback_text, -1);
  theme_adium_append_event_escaped (self, markup_text, direction);
}

void
empathy_theme_adium_prepend_message (EmpathyThemeAdium *self,
    EmpathyMessage *msg,
    gboolean should_highlight)
{
  const gchar *js_funcs[] = { "prependPrev",
      "prependPrev",
      "prepend",
      "prepend" };

  if (self->priv->pages_loading != 0)
    {
      queue_item (&self->priv->message_queue, QUEUED_MESSAGE, msg, NULL,
          should_highlight, TRUE);
      return;
    }

  theme_adium_add_message (self, msg, &self->priv->first_contact,
      &self->priv->first_timestamp, &self->priv->first_is_backlog,
      should_highlight, js_funcs);
}

void
empathy_theme_adium_edit_message (EmpathyThemeAdium *self,
    EmpathyMessage *message)
{
#if 0
  /* FIXME: this needs to be ported to WebKit2, but I have no idea what this is for */
  WebKitDOMDocument *doc;
  WebKitDOMElement *span;
  gchar *id, *parsed_body;
  gchar *tooltip, *timestamp;
  GtkIconInfo *icon_info;
  GError *error = NULL;

  if (self->priv->pages_loading != 0)
    {
      queue_item (&self->priv->message_queue, QUEUED_EDIT, message, NULL, FALSE, FALSE);
      return;
    }

  id = g_strdup_printf ("message-token-%s",
    empathy_message_get_supersedes (message));
  /* we don't pass a token here, because doing so will return another
   * <span> element, and we don't want nested <span> elements */
  parsed_body = theme_adium_parse_body (self,
    empathy_message_get_body (message), NULL);

  /* find the element */
  doc = webkit_web_view_get_dom_document (WEBKIT_WEB_VIEW (self));
  span = webkit_dom_document_get_element_by_id (doc, id);

  if (span == NULL)
    {
      DEBUG ("Failed to find id '%s'", id);
      goto except;
    }

  if (!WEBKIT_DOM_IS_HTML_ELEMENT (span))
    {
      DEBUG ("Not a HTML element");
      goto except;
    }

  /* update the HTML */
  webkit_dom_html_element_set_inner_html (WEBKIT_DOM_HTML_ELEMENT (span),
    parsed_body, &error);

  if (error != NULL)
    {
      DEBUG ("Error setting new inner-HTML: %s", error->message);
      g_error_free (error);
      goto except;
    }

  /* set a tooltip */
  timestamp = tpaw_time_to_string_local (
    empathy_message_get_timestamp (message),
    "%H:%M:%S");
  tooltip = g_strdup_printf (_("Message edited at %s"), timestamp);

  webkit_dom_html_element_set_title (WEBKIT_DOM_HTML_ELEMENT (span),
    tooltip);

  g_free (tooltip);
  g_free (timestamp);

  /* mark this message as edited */
  icon_info = gtk_icon_theme_lookup_icon (gtk_icon_theme_get_default (),
    EMPATHY_IMAGE_EDIT_MESSAGE, 16, 0);

  if (icon_info != NULL)
    {
      /* set the icon as a background image using CSS
       * FIXME: the icon won't update in response to theme changes */
      gchar *style = g_strdup_printf (
        "background-image:url('%s');"
        "background-repeat:no-repeat;"
        "background-position:left center;"
        "padding-left:19px;", /* 16px icon + 3px padding */
        gtk_icon_info_get_filename (icon_info));

      webkit_dom_element_set_attribute (span, "style", style, &error);

      if (error != NULL)
        {
          DEBUG ("Error setting element style: %s",
            error->message);
          g_clear_error (&error);
          /* not fatal */
        }

      g_free (style);
      g_object_unref (icon_info);
    }

  goto finally;

except:
  DEBUG ("Could not find message to edit with: %s",
    empathy_message_get_body (message));

finally:
  g_free (id);
  g_free (parsed_body);
#endif
}

void
empathy_theme_adium_scroll (EmpathyThemeAdium *self,
    gboolean allow_scrolling)
{
  self->priv->allow_scrolling = allow_scrolling;

  if (allow_scrolling)
    empathy_theme_adium_scroll_down (self);
}

void
empathy_theme_adium_scroll_down (EmpathyThemeAdium *self)
{
  webkit_web_view_run_javascript (WEBKIT_WEB_VIEW (self), "alignChat(true);", NULL, NULL, NULL);
}

static void
can_copy_cb (WebKitWebView *web_view,
    GAsyncResult *result,
    GTask *task)
{
  g_task_return_boolean (task,
      webkit_web_view_can_execute_editing_command_finish (web_view, result, NULL));
  g_object_unref (task);
}

void
empathy_theme_adium_can_copy (EmpathyThemeAdium *self,
    GCancellable* cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GTask *task;

  task = g_task_new (self, cancellable, callback, user_data);
  webkit_web_view_can_execute_editing_command (WEBKIT_WEB_VIEW (self), WEBKIT_EDITING_COMMAND_COPY,
      cancellable, (GAsyncReadyCallback)can_copy_cb, task);
}

gboolean
empathy_theme_adium_can_copy_finish (EmpathyThemeAdium *self,
    GAsyncResult* result,
    GError** error)
{
  if (!g_task_is_valid (result, self))
    return FALSE;

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
empathy_theme_adium_clear (EmpathyThemeAdium *self)
{
  webkit_web_view_run_javascript (WEBKIT_WEB_VIEW (self), "clearPage()", NULL, NULL, NULL);
  empathy_theme_adium_scroll_down (self);

  /* Clear last contact to avoid trying to add a 'joined'
   * message when we don't have an insertion point. */
  if (self->priv->last_contact)
    {
      g_object_unref (self->priv->last_contact);
      self->priv->last_contact = NULL;
    }
}

void
empathy_theme_adium_find_previous (EmpathyThemeAdium *self)
{
  webkit_find_controller_search_previous (webkit_web_view_get_find_controller (WEBKIT_WEB_VIEW (self)));
}

void
empathy_theme_adium_find_next (EmpathyThemeAdium *self)
{
  webkit_find_controller_search_next (webkit_web_view_get_find_controller (WEBKIT_WEB_VIEW (self)));
}

void
empathy_theme_adium_find_abilities (EmpathyThemeAdium *self,
    const gchar *search_criteria,
    gboolean match_case,
    gboolean *can_do_previous,
    gboolean *can_do_next)
{
  /* FIXME: Does webkit provide an API for that? We have wrap=true in
   * find_next and find_previous to work around this problem. */
  if (can_do_previous)
    *can_do_previous = TRUE;
  if (can_do_next)
    *can_do_next = TRUE;
}

void
empathy_theme_adium_search (EmpathyThemeAdium *self,
    const gchar *text,
    gboolean match_case)
{
  WebKitFindController *find_controller;
  WebKitFindOptions options = WEBKIT_FIND_OPTIONS_NONE;

  find_controller = webkit_web_view_get_find_controller (WEBKIT_WEB_VIEW (self));
  if (!text || !*text)
    {
      webkit_find_controller_search_finish (find_controller);
      return;
    }

  if (!match_case)
    options |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;

  webkit_find_controller_search (find_controller, text, options, G_MAXUINT);
}

void
empathy_theme_adium_copy_clipboard (EmpathyThemeAdium *self)
{
  webkit_web_view_execute_editing_command (WEBKIT_WEB_VIEW (self),
      WEBKIT_EDITING_COMMAND_COPY);
}

static void
theme_adium_remove_mark_from_message (EmpathyThemeAdium *self,
    guint32 id)
{
#if 0
  /* FIXME: check what this is for and port to WebKit2 */
  WebKitDOMDocument *dom;
  WebKitDOMNodeList *nodes;
  gchar *class;
  GError *error = NULL;

  dom = webkit_web_view_get_dom_document (WEBKIT_WEB_VIEW (self));
  if (dom == NULL)
    return;

  class = g_strdup_printf (".x-empathy-message-id-%u", id);

  /* Get all nodes with focus class */
  nodes = webkit_dom_document_query_selector_all (dom, class, &error);
  g_free (class);

  if (nodes == NULL)
    {
      DEBUG ("Error getting focus nodes: %s",
        error ? error->message : "No error");
      g_clear_error (&error);
      return;
    }

  theme_adium_remove_focus_marks (self, nodes);
#endif
}

static void
theme_adium_remove_acked_message_unread_mark_foreach (gpointer data,
    gpointer user_data)
{
  EmpathyThemeAdium *self = user_data;
  guint32 id = GPOINTER_TO_UINT (data);

  theme_adium_remove_mark_from_message (self, id);
}

void
empathy_theme_adium_focus_toggled (EmpathyThemeAdium *self,
    gboolean has_focus)
{
  self->priv->has_focus = has_focus;
  if (!self->priv->has_focus)
    {
      /* We've lost focus, so let's make sure all the acked
       * messages have lost their unread marker. */
      g_queue_foreach (&self->priv->acked_messages,
          theme_adium_remove_acked_message_unread_mark_foreach, self);
      g_queue_clear (&self->priv->acked_messages);

      self->priv->has_unread_message = FALSE;
    }
}

void
empathy_theme_adium_message_acknowledged (EmpathyThemeAdium *self,
    EmpathyMessage *message)
{
  TpMessage *tp_msg;
  guint32 id;
  gboolean valid;

  tp_msg = empathy_message_get_tp_message (message);

  if (tp_msg == NULL)
    return;

  id = tp_message_get_pending_message_id (tp_msg, &valid);
  if (!valid)
    {
      g_warning ("Acknoledged message doesn't have a pending ID");
      return;
    }

  /* We only want to actually remove the unread marker if the
   * view doesn't have focus. If we did it all the time we would
   * never see the unread markers, ever! So, we'll queue these
   * up, and when we lose focus, we'll remove the markers. */
  if (self->priv->has_focus)
    {
      g_queue_push_tail (&self->priv->acked_messages,
             GUINT_TO_POINTER (id));
      return;
    }

  theme_adium_remove_mark_from_message (self, id);
}

static gboolean
theme_adium_context_menu_cb (EmpathyThemeAdium *self,
    WebKitContextMenu *context_menu,
    GdkEvent *event,
    WebKitHitTestResult *hit_test_result,
    gpointer user_data)
{
  EmpathyWebKitMenuFlags flags = EMPATHY_WEBKIT_MENU_CLEAR;

  if (g_settings_get_boolean (self->priv->gsettings_chat,
        EMPATHY_PREFS_CHAT_WEBKIT_DEVELOPER_TOOLS))
    flags |= EMPATHY_WEBKIT_MENU_INSPECT;

  empathy_webkit_populate_context_menu (WEBKIT_WEB_VIEW (self), context_menu, hit_test_result, flags);
  return FALSE;
}

void
empathy_theme_adium_set_show_avatars (EmpathyThemeAdium *self,
    gboolean show_avatars)
{
  self->priv->show_avatars = show_avatars;
}

static void
theme_adium_load_changed_cb (WebKitWebView *view,
    WebKitLoadEvent event,
    gpointer user_data)
{
  EmpathyThemeAdium *self = EMPATHY_THEME_ADIUM (view);
  GList *l;

  if (event != WEBKIT_LOAD_FINISHED)
    return;

  DEBUG ("Page loaded");
  self->priv->pages_loading--;

  if (self->priv->pages_loading != 0)
    return;

  /* Display queued messages */
  for (l = self->priv->message_queue.head; l != NULL; l = l->next)
    {
      QueuedItem *item = l->data;

      switch (item->type)
        {
          case QUEUED_MESSAGE:
            empathy_theme_adium_append_message (self, item->msg,
              item->should_highlight);
            break;

          case QUEUED_EDIT:
            empathy_theme_adium_edit_message (self, item->msg);
            break;

          case QUEUED_EVENT:
            empathy_theme_adium_append_event (self, item->str);
            break;
        }

      free_queued_item (item);
    }

  g_queue_clear (&self->priv->message_queue);
}

static void
theme_adium_finalize (GObject *object)
{
  EmpathyThemeAdium *self = EMPATHY_THEME_ADIUM (object);

  empathy_adium_data_unref (self->priv->data);

  g_object_unref (self->priv->gsettings_chat);
  g_object_unref (self->priv->gsettings_desktop);

  g_free (self->priv->variant);

  G_OBJECT_CLASS (empathy_theme_adium_parent_class)->finalize (object);
}

static void
theme_adium_dispose (GObject *object)
{
  EmpathyThemeAdium *self = EMPATHY_THEME_ADIUM (object);

  if (self->priv->smiley_manager)
    {
      g_object_unref (self->priv->smiley_manager);
      self->priv->smiley_manager = NULL;
    }

  g_clear_object (&self->priv->first_contact);

  if (self->priv->last_contact)
    {
      g_object_unref (self->priv->last_contact);
      self->priv->last_contact = NULL;
    }

  if (self->priv->inspector_window)
    {
      gtk_widget_destroy (self->priv->inspector_window);
      self->priv->inspector_window = NULL;
    }

  if (self->priv->acked_messages.length > 0)
    {
      g_queue_clear (&self->priv->acked_messages);
    }

  G_OBJECT_CLASS (empathy_theme_adium_parent_class)->dispose (object);
}

static void
theme_adium_constructed (GObject *object)
{
  EmpathyThemeAdium *self = EMPATHY_THEME_ADIUM (object);
  const gchar *font_family = NULL;
  gint font_size = 0;
  WebKitWebView *webkit_view = WEBKIT_WEB_VIEW (object);

  G_OBJECT_CLASS (empathy_theme_adium_parent_class)->constructed (object);

  /* Set default settings */
  font_family = tp_asv_get_string (self->priv->data->info, "DefaultFontFamily");
  font_size = tp_asv_get_int32 (self->priv->data->info, "DefaultFontSize", NULL);

  if (font_family && font_size)
    {
      g_object_set (webkit_web_view_get_settings (webkit_view),
        "default-font-family", font_family,
        "default-font-size", font_size,
        NULL);
    }
  else
    {
      empathy_webkit_bind_font_setting (webkit_view,
        self->priv->gsettings_desktop,
        EMPATHY_PREFS_DESKTOP_INTERFACE_DOCUMENT_FONT_NAME);
    }

    g_object_set (webkit_web_view_get_settings (webkit_view),
        "default-charset", "utf8",
        NULL);

  /* Load template */
  theme_adium_load_template (EMPATHY_THEME_ADIUM (object));

  self->priv->in_construction = FALSE;
}

static void
theme_adium_get_property (GObject *object,
    guint param_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyThemeAdium *self = EMPATHY_THEME_ADIUM (object);

  switch (param_id)
    {
      case PROP_ADIUM_DATA:
        g_value_set_boxed (value, self->priv->data);
        break;
      case PROP_VARIANT:
        g_value_set_string (value, self->priv->variant);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        break;
    };
}

static void
theme_adium_set_property (GObject *object,
    guint param_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyThemeAdium *self = EMPATHY_THEME_ADIUM (object);

  switch (param_id)
    {
      case PROP_ADIUM_DATA:
        g_assert (self->priv->data == NULL);
        self->priv->data = g_value_dup_boxed (value);
        break;
      case PROP_VARIANT:
        empathy_theme_adium_set_variant (self, g_value_get_string (value));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        break;
    };
}

static void
empathy_theme_adium_class_init (EmpathyThemeAdiumClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = theme_adium_finalize;
  object_class->dispose = theme_adium_dispose;
  object_class->constructed = theme_adium_constructed;
  object_class->get_property = theme_adium_get_property;
  object_class->set_property = theme_adium_set_property;

  g_object_class_install_property (object_class, PROP_ADIUM_DATA,
      g_param_spec_boxed ("adium-data",
        "The theme data",
        "Data for the adium theme",
        EMPATHY_TYPE_ADIUM_DATA,
        G_PARAM_CONSTRUCT_ONLY |
        G_PARAM_READWRITE |
        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_VARIANT,
      g_param_spec_string ("variant",
        "The theme variant",
        "Variant name for the theme",
        NULL,
        G_PARAM_CONSTRUCT |
        G_PARAM_READWRITE |
        G_PARAM_STATIC_STRINGS));

  g_type_class_add_private (object_class, sizeof (EmpathyThemeAdiumPriv));
}

static void
empathy_theme_adium_init (EmpathyThemeAdium *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
    EMPATHY_TYPE_THEME_ADIUM, EmpathyThemeAdiumPriv);

  self->priv->in_construction = TRUE;
  g_queue_init (&self->priv->message_queue);
  self->priv->allow_scrolling = TRUE;
  self->priv->smiley_manager = empathy_smiley_manager_dup_singleton ();

  /* Show avatars by default. */
  self->priv->show_avatars = TRUE;
  g_signal_connect (self, "load-changed",
      G_CALLBACK (theme_adium_load_changed_cb), NULL);
  g_signal_connect (self, "decide-policy",
        G_CALLBACK (theme_adium_policy_decision_requested_cb), NULL);
  g_signal_connect (self, "context-menu",
      G_CALLBACK (theme_adium_context_menu_cb), NULL);

  self->priv->gsettings_chat = g_settings_new (EMPATHY_PREFS_CHAT_SCHEMA);
  self->priv->gsettings_desktop = g_settings_new (
    EMPATHY_PREFS_DESKTOP_INTERFACE_SCHEMA);
}

EmpathyThemeAdium *
empathy_theme_adium_new (EmpathyAdiumData *data,
    const gchar *variant)
{
  g_return_val_if_fail (data != NULL, NULL);

  return g_object_new (EMPATHY_TYPE_THEME_ADIUM,
      "web-context", empathy_webkit_get_web_context (),
      "settings", empathy_webkit_get_web_settings (),
      "adium-data", data,
      "variant", variant,
      NULL);
}

void
empathy_theme_adium_set_variant (EmpathyThemeAdium *self,
    const gchar *variant)
{
  gchar *variant_path;
  gchar *script;

  if (!tp_strdiff (self->priv->variant, variant))
    return;

  g_free (self->priv->variant);
  self->priv->variant = g_strdup (variant);

  if (self->priv->in_construction)
    return;

  DEBUG ("Update view with variant: '%s'", variant);
  variant_path = adium_info_dup_path_for_variant (self->priv->data->info,
    self->priv->variant);
  script = g_strdup_printf ("setStylesheet(\"mainStyle\",\"%s\");",
      variant_path);

  webkit_web_view_run_javascript (WEBKIT_WEB_VIEW (self), script, NULL, NULL, NULL);

  g_free (variant_path);
  g_free (script);

  g_object_notify (G_OBJECT (self), "variant");
}

void
empathy_theme_adium_show_inspector (EmpathyThemeAdium *self)
{
  webkit_web_inspector_show (webkit_web_view_get_inspector (WEBKIT_WEB_VIEW (self)));
}

gboolean
empathy_adium_path_is_valid (const gchar *path)
{
  gboolean ret;
  gchar *file;
  gchar **tmp;
  const gchar *dir;

  if (path[0] != '/')
    return FALSE;

  /* The directory has to be *.AdiumMessageStyle per the Adium spec */
  tmp = g_strsplit (path, "/", 0);
  if (tmp == NULL)
    return FALSE;

  dir = tmp[g_strv_length (tmp) - 1];

  if (!g_str_has_suffix (dir, ".AdiumMessageStyle"))
    {
      g_strfreev (tmp);
      return FALSE;
    }

  g_strfreev (tmp);

  /* The theme is not valid if there is no Info.plist */
  file = g_build_filename (path, "Contents", "Info.plist",
         NULL);
  ret = g_file_test (file, G_FILE_TEST_EXISTS);
  g_free (file);

  if (!ret)
    return FALSE;

  /* We ship a default Template.html as fallback if there is any problem
   * with the one inside the theme. The only other required file is
   * Content.html OR Incoming/Content.html*/
  file = g_build_filename (path, "Contents", "Resources", "Content.html",
      NULL);
  ret = g_file_test (file, G_FILE_TEST_EXISTS);
  g_free (file);

  if (!ret)
    {
      file = g_build_filename (path, "Contents", "Resources", "Incoming",
             "Content.html", NULL);
      ret = g_file_test (file, G_FILE_TEST_EXISTS);
      g_free (file);
    }

  return ret;
}

GHashTable *
empathy_adium_info_new (const gchar *path)
{
  gchar *file;
  GValue *value;
  GHashTable *info = NULL;

  g_return_val_if_fail (empathy_adium_path_is_valid (path), NULL);

  file = g_build_filename (path, "Contents", "Info.plist", NULL);
  value = empathy_plist_parse_from_file (file);
  g_free (file);

  if (value == NULL)
    return NULL;

  info = g_value_dup_boxed (value);
  tp_g_value_slice_free (value);

  /* Insert the theme's path into the hash table,
   * keys have to be dupped */
  tp_asv_set_string (info, g_strdup ("path"), path);

  return info;
}

static guint
adium_info_get_version (GHashTable *info)
{
  return tp_asv_get_int32 (info, "MessageViewVersion", NULL);
}

static const gchar *
adium_info_get_no_variant_name (GHashTable *info)
{
  const gchar *name = tp_asv_get_string (info, "DisplayNameForNoVariant");
  return name ? name : _("Normal");
}

static gchar *
adium_info_dup_path_for_variant (GHashTable *info,
    const gchar *variant)
{
  guint version = adium_info_get_version (info);
  const gchar *no_variant = adium_info_get_no_variant_name (info);
  GPtrArray *variants;
  guint i;

  if (version <= 2 && !tp_strdiff (variant, no_variant))
    return g_strdup ("main.css");

  variants = empathy_adium_info_get_available_variants (info);
  if (variants->len == 0)
    return g_strdup ("main.css");

  /* Verify the variant exists, fallback to the first one */
  for (i = 0; i < variants->len; i++)
    {
      if (!tp_strdiff (variant, g_ptr_array_index (variants, i)))
        break;
    }

  if (i == variants->len)
    {
      DEBUG ("Variant %s does not exist", variant);
      variant = g_ptr_array_index (variants, 0);
    }

  return g_strdup_printf ("Variants/%s.css", variant);

}

const gchar *
empathy_adium_info_get_default_variant (GHashTable *info)
{
  if (adium_info_get_version (info) <= 2)
    return adium_info_get_no_variant_name (info);

  return tp_asv_get_string (info, "DefaultVariant");
}

GPtrArray *
empathy_adium_info_get_available_variants (GHashTable *info)
{
  GPtrArray *variants;
  const gchar *path;
  gchar *dirpath;
  GDir *dir;

  variants = tp_asv_get_boxed (info, "AvailableVariants", G_TYPE_PTR_ARRAY);
  if (variants != NULL)
    return variants;

  variants = g_ptr_array_new_with_free_func (g_free);
  tp_asv_take_boxed (info, g_strdup ("AvailableVariants"),
    G_TYPE_PTR_ARRAY, variants);

  path = tp_asv_get_string (info, "path");
  dirpath = g_build_filename (path, "Contents", "Resources", "Variants", NULL);
  dir = g_dir_open (dirpath, 0, NULL);
  if (dir != NULL)
    {
      const gchar *name;

      for (name = g_dir_read_name (dir);
           name != NULL;
           name = g_dir_read_name (dir))
        {
          gchar *display_name;

          if (!g_str_has_suffix (name, ".css"))
            continue;

          display_name = g_strdup (name);
          strstr (display_name, ".css")[0] = '\0';
          g_ptr_array_add (variants, display_name);
        }

      g_dir_close (dir);
    }
  g_free (dirpath);

  if (adium_info_get_version (info) <= 2)
    g_ptr_array_add (variants,
      g_strdup (adium_info_get_no_variant_name (info)));

  return variants;
}

GType
empathy_adium_data_get_type (void)
{
  static GType type_id = 0;

  if (!type_id)
    {
      type_id = g_boxed_type_register_static ("EmpathyAdiumData",
          (GBoxedCopyFunc) empathy_adium_data_ref,
          (GBoxedFreeFunc) empathy_adium_data_unref);
    }

  return type_id;
}

EmpathyAdiumData *
empathy_adium_data_new_with_info (const gchar *path,
    GHashTable *info)
{
  EmpathyAdiumData *data;
  gchar *template_html = NULL;
  gchar *footer_html = NULL;
  gchar *tmp;

  g_return_val_if_fail (empathy_adium_path_is_valid (path), NULL);

  data = g_slice_new0 (EmpathyAdiumData);
  data->ref_count = 1;
  data->path = g_strdup (path);
  data->basedir = g_strconcat (path, G_DIR_SEPARATOR_S "Contents"
    G_DIR_SEPARATOR_S "Resources" G_DIR_SEPARATOR_S, NULL);
  data->info = g_hash_table_ref (info);
  data->version = adium_info_get_version (info);
  data->strings_to_free = g_ptr_array_new_with_free_func (g_free);
  data->date_format_cache = g_hash_table_new_full (g_str_hash,
    g_str_equal, g_free, g_free);

  DEBUG ("Loading theme at %s", path);

#define LOAD(path, var) \
    tmp = g_build_filename (data->basedir, path, NULL); \
    g_file_get_contents (tmp, &var, NULL, NULL); \
    g_free (tmp); \

#define LOAD_CONST(path, var) \
  { \
    gchar *content; \
    LOAD (path, content); \
    if (content != NULL) { \
      g_ptr_array_add (data->strings_to_free, content); \
    } \
    var = content; \
  }

  /* Load html files */
  LOAD_CONST ("Content.html", data->content_html);
  LOAD_CONST ("Incoming/Content.html", data->in_content_html);
  LOAD_CONST ("Incoming/NextContent.html", data->in_nextcontent_html);
  LOAD_CONST ("Incoming/Context.html", data->in_context_html);
  LOAD_CONST ("Incoming/NextContext.html", data->in_nextcontext_html);
  LOAD_CONST ("Outgoing/Content.html", data->out_content_html);
  LOAD_CONST ("Outgoing/NextContent.html", data->out_nextcontent_html);
  LOAD_CONST ("Outgoing/Context.html", data->out_context_html);
  LOAD_CONST ("Outgoing/NextContext.html", data->out_nextcontext_html);
  LOAD_CONST ("Status.html", data->status_html);
  LOAD ("Template.html", template_html);
  LOAD ("Footer.html", footer_html);

#undef LOAD_CONST
#undef LOAD

  /* HTML fallbacks: If we have at least content OR in_content, then
   * everything else gets a fallback */

#define FALLBACK(html, fallback) \
  if (html == NULL) { \
    html = fallback; \
  }

  /* in_nextcontent -> in_content -> content */
  FALLBACK (data->in_content_html,      data->content_html);
  FALLBACK (data->in_nextcontent_html,  data->in_content_html);

  /* context -> content */
  FALLBACK (data->in_context_html,      data->in_content_html);
  FALLBACK (data->in_nextcontext_html,  data->in_nextcontent_html);
  FALLBACK (data->out_context_html,     data->out_content_html);
  FALLBACK (data->out_nextcontext_html, data->out_nextcontent_html);

  /* out -> in */
  FALLBACK (data->out_content_html,     data->in_content_html);
  FALLBACK (data->out_nextcontent_html, data->in_nextcontent_html);
  FALLBACK (data->out_context_html,     data->in_context_html);
  FALLBACK (data->out_nextcontext_html, data->in_nextcontext_html);

  /* status -> in_content */
  FALLBACK (data->status_html,          data->in_content_html);

#undef FALLBACK

  /* template -> empathy's template */
  data->custom_template = (template_html != NULL);
  if (template_html == NULL)
    {
      GError *error = NULL;

      tmp = empathy_file_lookup ("Template.html", "data");

      if (!g_file_get_contents (tmp, &template_html, NULL, &error)) {
        g_warning ("couldn't load Empathy's default theme "
          "template: %s", error->message);
        g_return_val_if_reached (data);
      }

      g_free (tmp);
    }

  /* Default avatar */
  tmp = g_build_filename (data->basedir, "Incoming", "buddy_icon.png", NULL);
  if (g_file_test (tmp, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
    {
      data->default_incoming_avatar_filename = tmp;
    }
  else
    {
      g_free (tmp);
    }

  tmp = g_build_filename (data->basedir, "Outgoing", "buddy_icon.png", NULL);
  if (g_file_test (tmp, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
    {
      data->default_outgoing_avatar_filename = tmp;
    }
  else
    {
      g_free (tmp);
    }

  /* Old custom templates had only 4 parameters.
   * New templates have 5 parameters */
  if (data->version <= 2 && data->custom_template)
    {
      tmp = string_with_format (template_html,
        data->basedir,
        "%@", /* Leave variant unset */
        "", /* The header */
        footer_html ? footer_html : "",
        NULL);
    }
  else
    {
      tmp = string_with_format (template_html,
        data->basedir,
        data->version <= 2 ? "" : "@import url( \"main.css\" );",
        "%@", /* Leave variant unset */
        "", /* The header */
        footer_html ? footer_html : "",
        NULL);
    }
  g_ptr_array_add (data->strings_to_free, tmp);
  data->template_html = tmp;

  g_free (template_html);
  g_free (footer_html);

  return data;
}

EmpathyAdiumData *
empathy_adium_data_new (const gchar *path)
{
  EmpathyAdiumData *data;
  GHashTable *info;

  info = empathy_adium_info_new (path);
  data = empathy_adium_data_new_with_info (path, info);
  g_hash_table_unref (info);

  return data;
}

EmpathyAdiumData *
empathy_adium_data_ref (EmpathyAdiumData *data)
{
  g_return_val_if_fail (data != NULL, NULL);

  g_atomic_int_inc (&data->ref_count);

  return data;
}

void
empathy_adium_data_unref (EmpathyAdiumData *data)
{
  g_return_if_fail (data != NULL);

  if (g_atomic_int_dec_and_test (&data->ref_count)) {
    g_free (data->path);
    g_free (data->basedir);
    g_free (data->default_avatar_filename);
    g_free (data->default_incoming_avatar_filename);
    g_free (data->default_outgoing_avatar_filename);
    g_hash_table_unref (data->info);
    g_ptr_array_unref (data->strings_to_free);
    tp_clear_pointer (&data->date_format_cache, g_hash_table_unref);

    g_slice_free (EmpathyAdiumData, data);
  }
}

GHashTable *
empathy_adium_data_get_info (EmpathyAdiumData *data)
{
  g_return_val_if_fail (data != NULL, NULL);

  return data->info;
}

const gchar *
empathy_adium_data_get_path (EmpathyAdiumData *data)
{
  g_return_val_if_fail (data != NULL, NULL);

  return data->path;
}
