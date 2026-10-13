/*
 * SPDX-FileCopyrightText: 2026 Robert Wendt <tffb4szhgc@privaterelay.appleid.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Bounded, opt-in IRCv3 history catch-up and durable timestamp cursors. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/time.h>

#include "../weechat-plugin.h"
#include "irc.h"
#include "irc-batch.h"
#include "irc-channel.h"
#include "irc-color.h"
#include "irc-history.h"
#include "irc-ignore.h"
#include "irc-message.h"
#include "irc-protocol.h"
#include "irc-server.h"
#include "irc-tag.h"

#define IRC_HISTORY_RECENT 256
#define IRC_HISTORY_TARGETS 256
#define IRC_HISTORY_PAGES 10

struct t_irc_history
{
    char *server;
    char *target;
    char timestamp[25];
    char *msgid;
    unsigned char recent[IRC_HISTORY_RECENT][32];
    int recent_count;
    int recent_next;
    int pending;
    int timestamp_only;
    int pages;
    int limit;
    time_t requested;
    struct t_irc_history *next;
};

static struct t_irc_history *irc_history = NULL;
static struct t_config_file *irc_history_config = NULL;
static int irc_history_dirty = 0;
static struct t_hook *irc_history_timer = NULL;
static struct t_hook *irc_history_command = NULL;

/*
 * Accept only canonical millisecond UTC references, never command delimiters.
 */

int
irc_history_timestamp_valid (const char *timestamp)
{
    int i;
    struct timeval tv;
    const char *format = "0000-00-00T00:00:00.000Z";

    if (!timestamp || strlen (timestamp) != 24)
        return 0;
    for (i = 0; i < 24; i++)
    {
        if ((format[i] == '0') ? !isdigit ((unsigned char)timestamp[i]) :
            timestamp[i] != format[i])
            return 0;
    }
    return weechat_util_parse_time (timestamp, &tv);
}

/*
 * Reject whitespace and unreasonably large opaque message references.
 */

int
irc_history_msgid_valid (const char *msgid)
{
    const unsigned char *ptr;

    if (!msgid || !msgid[0] || strlen (msgid) > 256)
        return 0;
    for (ptr = (const unsigned char *)msgid; *ptr; ptr++)
    {
        if (*ptr <= 32 || *ptr == 127)
            return 0;
    }
    return 1;
}

/*
 * Look up a cursor by server configuration name and IRC case-mapped target.
 */

struct t_irc_history *
irc_history_search (struct t_irc_server *server, const char *server_name,
                    const char *target, int create)
{
    struct t_irc_history *ptr;
    int count;

    count = 0;
    for (ptr = irc_history; ptr; ptr = ptr->next)
    {
        count++;
        if (strcmp (ptr->server, server_name) == 0
            && ((server) ? irc_server_strcasecmp (server, ptr->target, target) :
                strcmp (ptr->target, target)) == 0)
            return ptr;
    }
    if (!create || count >= IRC_HISTORY_TARGETS)
        return NULL;
    ptr = calloc (1, sizeof (*ptr));
    if (!ptr)
        return NULL;
    ptr->server = strdup (server_name);
    ptr->target = strdup (target);
    if (!ptr->server || !ptr->target)
    {
        free (ptr->server);
        free (ptr->target);
        free (ptr);
        return NULL;
    }
    ptr->next = irc_history;
    irc_history = ptr;
    return ptr;
}

/*
 * Require all dependencies to have been acknowledged on this connection.
 */

int
irc_history_enabled (struct t_irc_server *server)
{
    return server
        && weechat_hashtable_has_key (server->cap_list, "draft/chathistory")
        && weechat_hashtable_has_key (server->cap_list, "batch")
        && weechat_hashtable_has_key (server->cap_list, "server-time")
        && weechat_hashtable_has_key (server->cap_list, "message-tags");
}

/*
 * ISUPPORT can change during a connection. The raw list retains old tokens,
 * so use the last occurrence, including explicit feature removal.
 */

const char *
irc_history_isupport (struct t_irc_server *server, const char *feature)
{
    static char value[256];
    const char *ptr, *end;
    size_t length, size;
    int found;

    found = 0;
    length = strlen (feature);
    ptr = server->isupport;
    while (ptr && ptr[0])
    {
        while (*ptr == ' ')
            ptr++;
        end = strchr (ptr, ' ');
        size = end ? (size_t)(end - ptr) : strlen (ptr);
        if (size > length && strncmp (ptr, feature, length) == 0
            && ptr[length] == '=')
        {
            found = 1;
            size -= length + 1;
            if (size >= sizeof (value))
                size = sizeof (value) - 1;
            memcpy (value, ptr + length + 1, size);
            value[size] = '\0';
        }
        else if (size == length && strncmp (ptr, feature, length) == 0)
        {
            found = 1;
            value[0] = '\0';
        }
        else if (size == length + 1 && ptr[0] == '-'
                 && strncmp (ptr + 1, feature, length) == 0)
            found = 0;
        ptr = end;
    }
    return found ? value : NULL;
}

/*
 * Use a local ceiling even when the server advertises an unlimited result.
 * A missing or invalid mandatory CHATHISTORY token disables requests.
 */

int
irc_history_limit (struct t_irc_server *server)
{
    const char *value;
    char *end;
    unsigned long limit;

    value = irc_history_isupport (server, "CHATHISTORY");
    if (!value || !value[0] || strspn (value, "0123456789") != strlen (value))
        return 0;
    errno = 0;
    limit = strtoul (value, &end, 10);
    if (errno || *end)
        return 0;
    return (!limit || limit > 100) ? 100 : (int)limit;
}

/*
 * Select the first supported reference in the server's preference order.
 */

const char *
irc_history_reference (struct t_irc_server *server, struct t_irc_history *history)
{
    const char *types;
    char **list;
    int i;
    const char *result;

    types = irc_history_isupport (server, "MSGREFTYPES");
    list = weechat_string_split ((types) ? types : "msgid,timestamp",
                                 ",", NULL, 0, 0, NULL);
    result = NULL;
    if (list)
    {
        for (i = 0; list[i]; i++)
        {
            if (strcmp (list[i], "msgid") == 0 && history->msgid
                && !history->timestamp_only)
                result = "msgid";
            if (strcmp (list[i], "timestamp") == 0 && history->timestamp[0])
                result = "timestamp";
            if (result)
                break;
        }
        weechat_string_free_split (list);
    }
    return result;
}

/*
 * Request a bounded page; an existing cursor always uses AFTER, never LATEST.
 */

void
irc_history_request (struct t_irc_server *server, const char *target)
{
    struct t_irc_history *history;
    const char *reference;
    char timestamp[32];
    struct timeval tv;
    int limit;

    if (!irc_history_enabled (server) || !target || !target[0]
        || strpbrk (target, " ,\r\n") || strlen (target) > 255)
        return;
    limit = irc_history_limit (server);
    if (!limit)
        return;
    history = irc_history_search (server, server->name, target, 1);
    if (!history || history->pending || history->pages >= IRC_HISTORY_PAGES)
        return;
    reference = irc_history_reference (server, history);
    if (history->timestamp[0] && !reference)
        return;
    history->pending = 1;
    history->requested = time (NULL);
    history->limit = limit;
    history->pages++;
    if (reference)
    {
        /* Include the boundary millisecond; deduplication removes overlap. */
        weechat_util_parse_time (history->timestamp, &tv);
        if (tv.tv_usec < 1000)
        {
            tv.tv_sec--;
            tv.tv_usec += 1000000;
        }
        tv.tv_usec -= 1000;
        weechat_util_strftimeval (timestamp, sizeof (timestamp),
                                  "%@%Y-%m-%dT%H:%M:%S.%.3Z", &tv);
        irc_server_sendf (server, 0, NULL, "CHATHISTORY AFTER %s %s=%s %d",
                          target, reference,
                          (strcmp (reference, "msgid") == 0) ?
                          history->msgid : timestamp, limit);
    }
    else
        irc_server_sendf (server, 0, NULL, "CHATHISTORY LATEST %s * %d",
                          target, limit);
}

/*
 * Advance a cursor monotonically. Equal timestamps can still advance msgids.
 */

void
irc_history_advance (struct t_irc_history *history, const char *timestamp,
                     const char *msgid)
{
    if (!irc_history_timestamp_valid (timestamp)
        || strcmp (timestamp, history->timestamp) < 0)
        return;
    memcpy (history->timestamp, timestamp, 25);
    free (history->msgid);
    history->msgid = irc_history_msgid_valid (msgid) ? strdup (msgid) : NULL;
    irc_history_dirty = 1;
}

/*
 * Remember bounded message identities. Without msgid, use time plus full wire
 * message content (excluding batch tags), never just the timestamp.
 */

int
irc_history_seen (struct t_irc_history *history, const char *timestamp,
                  const char *msgid, const char *message)
{
    char *key;
    unsigned char digest[32];
    int i, hash_size;
    size_t size;

    size = strlen (timestamp) + strlen (message) + 260;
    key = malloc (size);
    if (!key)
        return 0;
    if (irc_history_msgid_valid (msgid))
        snprintf (key, size, "id:%s", msgid);
    else
        snprintf (key, size, "%s:%s", timestamp, message);
    if (!weechat_crypto_hash (key, strlen (key), "sha256", digest, &hash_size))
    {
        free (key);
        return 0;
    }
    free (key);
    for (i = 0; i < history->recent_count; i++)
    {
        if (memcmp (history->recent[i], digest, sizeof (digest)) == 0)
            return 1;
    }
    memcpy (history->recent[history->recent_next], digest, sizeof (digest));
    if (history->recent_count < IRC_HISTORY_RECENT)
        history->recent_count++;
    history->recent_next = (history->recent_next + 1) % IRC_HISTORY_RECENT;
    return 0;
}

/*
 * Track delivered live chat, but freeze the durable cursor during catch-up.
 */

int
irc_history_live (struct t_irc_protocol_ctxt *ctxt)
{
    struct t_irc_history *history;
    const char *target, *timestamp, *msgid;

    if (!irc_history_enabled (ctxt->server) || ctxt->num_params < 2
        || !ctxt->nick || ctxt->ignore_remove
        || (strcmp (ctxt->command, "privmsg") != 0
            && strcmp (ctxt->command, "notice") != 0))
        return 0;
    target = ctxt->params[0];
    if (!irc_channel_is_channel (ctxt->server, target) && !ctxt->nick_is_me)
        target = ctxt->nick;
    timestamp = weechat_hashtable_get (ctxt->tags, "time");
    if (!irc_history_timestamp_valid (timestamp))
        return 0;
    history = irc_history_search (ctxt->server, ctxt->server->name, target, 1);
    if (!history)
        return 0;
    msgid = weechat_hashtable_get (ctxt->tags, "msgid");
    if (irc_history_seen (history, timestamp, msgid, ctxt->irc_message))
        return 1;
    if (!history->requested && history->timestamp[0])
    {
        irc_history_request (ctxt->server, target);
        if (!history->pending)
            return 0;
    }
    if (!history->pending && !history->pages)
        irc_history_advance (history, timestamp, msgid);
    return 0;
}

/*
 * Stable-sort a bounded history page by server-time, retaining tie order.
 */

void
irc_history_sort (char **lines, int count)
{
    struct t_hashtable *tags;
    char **times, *raw_tags, *line, *timestamp;
    int i, j;

    times = calloc (count + 1, sizeof (*times));
    tags = weechat_hashtable_new (32, WEECHAT_HASHTABLE_STRING,
                                   WEECHAT_HASHTABLE_STRING, NULL, NULL);
    if (!times || !tags)
        goto end;
    for (i = 0; i < count; i++)
    {
        irc_message_parse (NULL, lines[i], &raw_tags, NULL, NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL);
        weechat_hashtable_remove_all (tags);
        irc_tag_parse (raw_tags, tags, NULL);
        timestamp = weechat_hashtable_get (tags, "time");
        times[i] = strdup (timestamp ? timestamp : "");
        free (raw_tags);
        if (!times[i])
            goto end;
    }
    for (i = 1; i < count; i++)
    {
        line = lines[i];
        timestamp = times[i];
        for (j = i; j > 0 && strcmp (times[j - 1], timestamp) > 0; j--)
        {
            lines[j] = lines[j - 1];
            times[j] = times[j - 1];
        }
        lines[j] = line;
        times[j] = timestamp;
    }
end:
    if (times)
    {
        for (i = 0; i < count; i++)
            free (times[i]);
        free (times);
    }
    weechat_hashtable_free (tags);
}

/*
 * Decode and display only chat from completed history batches. Never pass
 * replay through live handlers (CTCP replies, nick state, typing, or signals).
 * Keep the cursor unchanged on malformed/oversized/partial responses.
 */

void
irc_history_batch (struct t_irc_server *server, struct t_irc_batch *batch)
{
    struct t_irc_history *history;
    struct t_irc_channel *channel;
    struct t_hashtable *tags;
    char **lines, *raw_tags, *nick, *host, *command, *target, *text, *wire;
    char *display;
    const char *timestamp, *msgid;
    char old_timestamp[25], *old_msgid, *server_name, *target_name;
    struct timeval tv;
    int i, count, valid, more, initial, history_end;

    if (!irc_history_enabled (server) || batch->history_invalid
        || !batch->parameters)
        return;
    history = irc_history_search (server, server->name, batch->parameters, 0);
    if (!history || !history->pending)
        return;
    channel = irc_channel_search (server, history->target);
    if (!channel)
        return;
    server_name = strdup (server->name);
    target_name = strdup (history->target);
    if (!server_name || !target_name)
    {
        free (server_name);
        free (target_name);
        return;
    }
    history_end = weechat_hashtable_has_key (batch->tags, "draft/chathistory-end");
    memcpy (old_timestamp, history->timestamp, sizeof (old_timestamp));
    old_msgid = history->msgid ? strdup (history->msgid) : NULL;
    initial = !old_timestamp[0];
    tags = weechat_hashtable_new (32, WEECHAT_HASHTABLE_STRING,
                                   WEECHAT_HASHTABLE_STRING, NULL, NULL);
    lines = weechat_string_split (batch->messages ? *batch->messages : "",
                                  "\n", NULL, 0, 0, &count);
    if (lines)
        irc_history_sort (lines, count);
    valid = (tags != NULL && (!batch->messages || lines));
    /* Validate the whole page before advancing any durable state. */
    for (i = 0; valid && lines && i < count; i++)
    {
        irc_message_parse (server, lines[i], &raw_tags, &wire, &nick,
                           NULL, &host, &command, &target, NULL, &text,
                           NULL, NULL, NULL, NULL, NULL, NULL);
        weechat_hashtable_remove_all (tags);
        irc_tag_parse (raw_tags, tags, NULL);
        if (weechat_hashtable_has_key (tags, "draft/chathistory-context"))
            goto next_validation;
        timestamp = weechat_hashtable_get (tags, "time");
        if (!nick || !command || !target || !text
            || (weechat_strcasecmp (command, "PRIVMSG") != 0
                && weechat_strcasecmp (command, "NOTICE") != 0)
            || !irc_history_timestamp_valid (timestamp)
            || (irc_channel_is_channel (server, history->target)
                ? irc_server_strcasecmp (server, target, history->target) != 0
                : ((irc_server_strcasecmp (server, nick, history->target) != 0
                    || irc_server_strcasecmp (server, target, server->nick) != 0)
                   && (irc_server_strcasecmp (server, nick, server->nick) != 0
                       || irc_server_strcasecmp (server, target, history->target) != 0))))
            valid = 0;
next_validation:
        free (raw_tags);
        free (wire);
        free (nick);
        free (host);
        free (command);
        free (target);
        free (text);
    }
    for (i = 0; valid && lines && i < count; i++)
    {
        irc_message_parse (server, lines[i], &raw_tags, &wire, &nick,
                           NULL, &host, &command, &target, NULL, &text,
                           NULL, NULL, NULL, NULL, NULL, NULL);
        weechat_hashtable_remove_all (tags);
        irc_tag_parse (raw_tags, tags, NULL);
        if (weechat_hashtable_has_key (tags, "draft/chathistory-context"))
            goto next_message;
        timestamp = weechat_hashtable_get (tags, "time");
        msgid = weechat_hashtable_get (tags, "msgid");
        weechat_util_parse_time (timestamp, &tv);
        irc_history_advance (history, timestamp, msgid);
        if (!irc_history_seen (history, timestamp, msgid, wire)
            && !irc_ignore_check (server, history->target, nick, host))
        {
            /* CTCP is inert; replay must never trigger replies or transfers. */
            display = irc_color_decode (text, 0);
            if (display)
            {
                weechat_printf_datetime_tags (
                    channel->buffer, tv.tv_sec, tv.tv_usec,
                    "irc_chathistory,irc_batch_type_chathistory,notify_none,no_highlight,no_log",
                    "%s\t%s", nick, display);
                free (display);
            }
        }
next_message:
        free (raw_tags);
        free (wire);
        free (nick);
        free (host);
        free (command);
        free (target);
        free (text);
        /* Print hooks may close buffers or disconnect/delete the server. */
        if (irc_server_search (server_name) != server
            || irc_history_search (server, server_name, target_name, 0) != history
            || irc_channel_search (server, target_name) != channel
            || !history->pending)
            goto end;
    }
    more = valid && !initial && count > 0
        && !history_end
        && (strcmp (old_timestamp, history->timestamp) != 0
            || (history->msgid
                && (!old_msgid || strcmp (old_msgid, history->msgid) != 0)));
    history->pending = 0;
    if (valid && (!count || history_end))
        history->pages = 0;
    if (irc_history_dirty && irc_history_config)
    {
        if (weechat_config_write (irc_history_config) == WEECHAT_CONFIG_WRITE_OK)
            irc_history_dirty = 0;
    }
    if (!more && valid && (initial || !count
                      || history_end))
        history->pages = 0;
    else if (!more)
        history->pages = IRC_HISTORY_PAGES;
    if (history->pages >= IRC_HISTORY_PAGES)
        weechat_printf (channel->buffer,
                        _("%sirc: history catch-up paused; use /chathistory to continue"),
                        weechat_prefix ("error"));
    else if (more)
        irc_history_request (server, history->target);
end:
    free (server_name);
    free (target_name);
    free (old_msgid);
    weechat_string_free_split (lines);
    weechat_hashtable_free (tags);
}

/*
 * Stop failed requests immediately. An unsupported msgid reference gets one
 * timestamp fallback, only if the server also advertised that reference type.
 */

void
irc_history_fail (struct t_irc_protocol_ctxt *ctxt)
{
    struct t_irc_history *ptr;
    const char *reference;

    if (ctxt->num_params < 2
        || weechat_strcasecmp (ctxt->params[0], "CHATHISTORY") != 0)
        return;
    for (ptr = irc_history; ptr; ptr = ptr->next)
    {
        if (!ptr->pending || strcmp (ptr->server, ctxt->server->name) != 0)
            continue;
        if (ctxt->num_params >= 5
            && irc_server_strcasecmp (ctxt->server, ptr->target,
                                      ctxt->params[3]) != 0)
            continue;
        ptr->pending = 0;
        if (strcmp (ctxt->params[1], "INVALID_MSGREFTYPE") == 0
            && ptr->msgid && !ptr->timestamp_only)
        {
            ptr->timestamp_only = 1;
            reference = irc_history_reference (ctxt->server, ptr);
            if (reference)
            {
                irc_history_request (ctxt->server, ptr->target);
                continue;
            }
        }
        ptr->pages = IRC_HISTORY_PAGES;
    }
}

/*
 * Clear connection-local requests, retaining durable cursors and recent IDs.
 */

void
irc_history_disconnect (struct t_irc_server *server)
{
    struct t_irc_history *ptr;

    for (ptr = irc_history; ptr; ptr = ptr->next)
    {
        if (strcmp (ptr->server, server->name) == 0)
        {
            ptr->pending = 0;
            ptr->pages = 0;
            ptr->requested = 0;
            ptr->timestamp_only = 0;
        }
    }
}

/*
 * Remove cursors when the user deletes a server configuration.
 */

void
irc_history_forget (struct t_irc_server *server)
{
    struct t_irc_history **link, *ptr;

    link = &irc_history;
    while (*link)
    {
        ptr = *link;
        if (strcmp (ptr->server, server->name) == 0)
        {
            *link = ptr->next;
            free (ptr->server);
            free (ptr->target);
            free (ptr->msgid);
            free (ptr);
            irc_history_dirty = 1;
        }
        else
            link = &ptr->next;
    }
}

/*
 * Encode arbitrary names/references as hex for unambiguous config storage.
 */

char *
irc_history_hex (const char *text, int decode)
{
    static const char digits[] = "0123456789abcdef";
    char *result;
    size_t i, length;
    unsigned int value;

    length = strlen (text);
    if (decode && (length % 2 || strspn (text, digits) != length))
        return NULL;
    result = malloc ((decode ? length / 2 : length * 2) + 1);
    if (!result)
        return NULL;
    if (decode)
    {
        for (i = 0; i < length; i += 2)
        {
            sscanf (text + i, "%2x", &value);
            if (!value || value < 32 || value == 127)
            {
                free (result);
                return NULL;
            }
            result[i / 2] = value;
        }
        result[length / 2] = '\0';
    }
    else
    {
        for (i = 0; i < length; i++)
        {
            result[i * 2] = digits[(unsigned char)text[i] >> 4];
            result[i * 2 + 1] = digits[(unsigned char)text[i] & 15];
        }
        result[length * 2] = '\0';
    }
    return result;
}

/*
 * Load cursors only, never stored message bodies or authentication data.
 */

int
irc_history_read_cb (const void *pointer, void *data,
                     struct t_config_file *config_file,
                     struct t_config_section *section,
                     const char *option_name, const char *value)
{
    char **names, **values, *server, *target, *msgid;
    int num_names, num_values;
    struct t_irc_history *history;

    (void) pointer;
    (void) data;
    (void) config_file;
    (void) section;
    if (!option_name || !value || strlen (option_name) > 2048
        || strlen (value) > 1024)
        return WEECHAT_CONFIG_OPTION_SET_ERROR;
    names = weechat_string_split (option_name, ".", NULL, 0, 0, &num_names);
    values = weechat_string_split (value, " ", NULL, 0, 0, &num_values);
    if (num_names == 2 && num_values >= 1
        && irc_history_timestamp_valid (values[0]))
    {
        server = irc_history_hex (names[0], 1);
        target = irc_history_hex (names[1], 1);
        msgid = num_values == 2 ? irc_history_hex (values[1], 1) : NULL;
        if (server && target)
        {
            history = irc_history_search (NULL, server, target, 1);
            if (history)
                irc_history_advance (history, values[0], msgid);
        }
        free (server);
        free (target);
        free (msgid);
    }
    weechat_string_free_split (names);
    weechat_string_free_split (values);
    return WEECHAT_CONFIG_OPTION_SET_OK_SAME_VALUE;
}

/*
 * Write escaped cursor records through WeeChat's atomic config writer.
 */

int
irc_history_write_cb (const void *pointer, void *data,
                      struct t_config_file *config_file,
                      const char *section_name)
{
    struct t_irc_history *ptr;
    char *server, *target, *msgid, *key;
    int rc;

    (void) pointer;
    (void) data;
    if (!weechat_config_write_line (config_file, section_name, NULL))
        return WEECHAT_CONFIG_WRITE_ERROR;
    for (ptr = irc_history; ptr; ptr = ptr->next)
    {
        if (!ptr->timestamp[0])
            continue;
        server = irc_history_hex (ptr->server, 0);
        target = irc_history_hex (ptr->target, 0);
        msgid = irc_history_hex (ptr->msgid ? ptr->msgid : "", 0);
        key = malloc (strlen (ptr->server) * 2 + strlen (ptr->target) * 2 + 2);
        rc = 0;
        if (server && target && msgid && key)
        {
            sprintf (key, "%s.%s", server, target);
            rc = weechat_config_write_line (config_file, key, "\"%s %s\"",
                                             ptr->timestamp, msgid);
        }
        free (server);
        free (target);
        free (msgid);
        free (key);
        if (!rc)
            return WEECHAT_CONFIG_WRITE_ERROR;
    }
    return WEECHAT_CONFIG_WRITE_OK;
}

/*
 * Checkpoint live cursors periodically and expire unanswered requests safely.
 */

int
irc_history_timer_cb (const void *pointer, void *data, int remaining_calls)
{
    struct t_irc_history *ptr;
    struct t_irc_server *server;
    struct t_irc_channel *channel;
    struct t_irc_batch *batch, *next_batch;

    (void) pointer;
    (void) data;
    (void) remaining_calls;
    for (ptr = irc_history; ptr; ptr = ptr->next)
    {
        if (ptr->pending && time (NULL) - ptr->requested >= 30)
        {
            ptr->pending = 0;
            /* Keep live traffic from skipping the unresolved gap. */
            ptr->pages = IRC_HISTORY_PAGES;
        }
    }
    for (server = irc_servers; server; server = server->next_server)
    {
        batch = server->batches;
        while (batch)
        {
            next_batch = batch->next_batch;
            if (batch->history_managed && time (NULL) - batch->start_time >= 30)
                irc_batch_free (server, batch);
            batch = next_batch;
        }
        if (!server->is_connected || !irc_history_enabled (server))
            continue;
        for (channel = server->channels; channel; channel = channel->next_channel)
        {
            if (channel->type != IRC_CHANNEL_TYPE_PRIVATE && !channel->nicks)
                continue;
            ptr = irc_history_search (server, server->name, channel->name, 0);
            if (!ptr || !ptr->requested)
                irc_history_request (server, channel->name);
        }
    }
    if (irc_history_dirty && irc_history_config
        && weechat_config_write (irc_history_config) == WEECHAT_CONFIG_WRITE_OK)
        irc_history_dirty = 0;
    return WEECHAT_RC_OK;
}

/*
 * Resume bounded catch-up for the current channel or private buffer.
 */

int
irc_history_command_cb (const void *pointer, void *data,
                        struct t_gui_buffer *buffer, int argc,
                        char **argv, char **argv_eol)
{
    struct t_irc_server *server;
    struct t_irc_history *history;
    const char *name, *target;

    (void) pointer;
    (void) data;
    (void) argv;
    (void) argv_eol;
    name = weechat_buffer_get_string (buffer, "localvar_server");
    target = weechat_buffer_get_string (buffer, "localvar_channel");
    server = irc_server_search (name);
    if (argc != 1 || !server || !server->is_connected
        || !irc_history_enabled (server) || !target || !target[0]
        || !irc_channel_search (server, target))
    {
        weechat_printf (buffer,
                        _("%sirc: /chathistory requires a chat buffer and negotiated history capabilities"),
                        weechat_prefix ("error"));
        return WEECHAT_RC_ERROR;
    }
    history = irc_history_search (server, name, target, 1);
    if (history && !history->pending)
        history->pages = 0;
    irc_history_request (server, target);
    if (!history || !history->pending)
    {
        weechat_printf (buffer,
                        _("%sirc: no usable CHATHISTORY limit or message reference"),
                        weechat_prefix ("error"));
        return WEECHAT_RC_ERROR;
    }
    return WEECHAT_RC_OK;
}

/*
 * Initialize private cursor storage before connecting any servers.
 */

void
irc_history_init (void)
{
    irc_history_config = weechat_config_new (TO_STR(IRC_PLUGIN_PRIORITY) "|irc_history",
                                              NULL, NULL, NULL);
    if (irc_history_config)
    {
        weechat_config_new_section (
            irc_history_config, "cursor", 0, 0,
            &irc_history_read_cb, NULL, NULL,
            &irc_history_write_cb, NULL, NULL,
            &irc_history_write_cb, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL);
        weechat_config_read (irc_history_config);
    }
    irc_history_command = weechat_hook_command (
        "chathistory", N_("resume server history catch-up for the current buffer"),
        "", "", NULL, &irc_history_command_cb, NULL, NULL);
    irc_history_timer = weechat_hook_timer (5000, 0, 0,
                                            &irc_history_timer_cb, NULL, NULL);
}

/*
 * Flush cursors on plugin unload, including /upgrade, and release all state.
 */

void
irc_history_end (void)
{
    struct t_irc_history *ptr;

    if (irc_history_timer)
        weechat_unhook (irc_history_timer);
    irc_history_timer = NULL;
    if (irc_history_command)
        weechat_unhook (irc_history_command);
    irc_history_command = NULL;
    if (irc_history_config)
    {
        weechat_config_write (irc_history_config);
        weechat_config_free (irc_history_config);
    }
    irc_history_config = NULL;
    while (irc_history)
    {
        ptr = irc_history;
        irc_history = ptr->next;
        free (ptr->server);
        free (ptr->target);
        free (ptr->msgid);

        free (ptr);
    }
}
