/*
 * SPDX-FileCopyrightText: 2026 Robert Wendt <tffb4szhgc@privaterelay.appleid.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WEECHAT_PLUGIN_IRC_HISTORY_H
#define WEECHAT_PLUGIN_IRC_HISTORY_H

struct t_irc_server;
struct t_irc_batch;
struct t_irc_protocol_ctxt;

extern int irc_history_enabled (struct t_irc_server *server);
extern int irc_history_limit (struct t_irc_server *server);
extern void irc_history_request (struct t_irc_server *server, const char *target);
extern int irc_history_live (struct t_irc_protocol_ctxt *ctxt);
extern void irc_history_batch (struct t_irc_server *server,
                               struct t_irc_batch *batch);
extern void irc_history_fail (struct t_irc_protocol_ctxt *ctxt);
extern void irc_history_forget (struct t_irc_server *server);
extern void irc_history_disconnect (struct t_irc_server *server);
extern void irc_history_init (void);
extern void irc_history_end (void);

#endif /* WEECHAT_PLUGIN_IRC_HISTORY_H */
