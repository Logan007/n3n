/*
 * Copyright (C) 2023-24 Hamish Coleman
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Common routines shared between the management interfaces
 *
 */


#include <connslot/connslot.h>  // for conn_t
#include <connslot/jsonrpc.h>   // for jsonrpc_t, jsonrpc_parse
#include <n3n/ethernet.h>       // for is_null_mac
#include <n3n/logging.h> // for traceEvent
#include <n3n/strings.h> // for ip_subnet_to_str, sock_to_cstr
#include <n3n/supernode.h>      // for load_allowed_sn_community
#include <pearson.h>     // for pearson_hash_64
#include <sn_selection.h> // for sn_selection_criterion_str
#include <stdbool.h>
#include <stdio.h>       // for snprintf, NULL, size_t
#include <stdlib.h>      // for strtoul
#include <string.h>      // for strtok, strlen, strncpy
#include "management.h"
#include "peer_info.h"   // for peer_info

#ifdef _WIN32
#include "win32/defs.h"
#else
#include <netdb.h>       // for getnameinfo, NI_NUMERICHOST, NI_NUMERICSERV
#include <sys/socket.h>  // for sendto, sockaddr
#endif

#if 0
/*
 * Check if the user is authorised for this command.
 * - this should be more configurable!
 * - for the moment we use some simple heuristics:
 *   Reads are not dangerous, so they are simply allowed
 *   Writes are possibly dangerous, so they need a fake password
 */
int mgmt_auth (mgmt_req_t *req, char *auth) {

    if(auth) {
        /* If we have an auth key, it must match */
        if(!strcmp(req->mgmt_password, auth)) {
            return 1;
        }
        return 0;
    }
    /* if we dont have an auth key, we can still read */
    if(req->type == N2N_MGMT_READ) {
        return 1;
    }

    return 0;
}
#endif

static void event_debug (strbuf_t *buf, enum n3n_event_topic topic, int data0, const void *data1) {
    traceEvent(TRACE_DEBUG, "Unexpected call to event_debug");
    return;
}

static void event_test (strbuf_t *buf, enum n3n_event_topic topic, int data0, const void *data1) {
    sb_printf(
        buf,
        "\x1e{"
        "\"event\":\"test\","
        "\"params\":%s}\n",
        (char *)data1);
}

static void event_peer (strbuf_t *buf, enum n3n_event_topic topic, int data0, const void *data1) {
    int action = data0;
    struct peer_info *peer = (struct peer_info *)data1;

    macstr_t mac_buf;
    n2n_sock_str_t sockbuf;

    /*
     * Just the peer_info bits that are needed for lookup (maccaddr) or
     * firewall and routing (sockaddr)
     * If needed, other details can be fetched via the edges method call.
     */
    sb_printf(
        buf,
        "\x1e{"
        "\"event\":\"peer\","
        "\"action\":%i,"
        "\"macaddr\":\"%s\","
        "\"sockaddr\":\"%s\"}\n",
        action,
        (is_null_mac(peer->mac_addr)) ? "" : macaddr_str(mac_buf, peer->mac_addr),
        sock_to_cstr(sockbuf, &(peer->sock))
        );

    // TODO: a generic truncation watcher for these buffers
}

/* Current subscriber for each event topic */
static SOCKET mgmt_event_subscribers[] = {
    [N3N_EVENT_DEBUG] = -1,
    [N3N_EVENT_TEST] = -1,
    [N3N_EVENT_PEER] = -1,
};

struct mgmt_event {
    char *topic;
    char *desc;
    void (*func)(strbuf_t *buf, enum n3n_event_topic topic, int data0, const void *data1);
};

static const struct mgmt_event mgmt_events[] = {
    [N3N_EVENT_DEBUG] = {
        .topic = "debug",
        .desc = "All events - for event debugging",
        .func = event_debug,
    },
    [N3N_EVENT_TEST] = {
        .topic = "test",
        .desc = "Used only by post.test",
        .func = event_test,
    },
    [N3N_EVENT_PEER] = {
        .topic = "peer",
        .desc = "Changes to peer list",
        .func = event_peer,
    },
};

static void event_subscribe (struct n3n_runtime_data *eee, conn_t *conn) {
    // TODO: look at url tail for event name
    enum n3n_event_topic topic = N3N_EVENT_DEBUG;
    bool replacing = false;

    if(mgmt_event_subscribers[topic] != -1) {
        // TODO: send a goodbuy message to old subscriber
        close(mgmt_event_subscribers[topic]);

        replacing = true;
    }

    // Take the filehandle away from the connslots.
    mgmt_event_subscribers[topic] = conn->fd;
    conn_zero(conn);

    // TODO: shutdown(fd, SHUT_RD) - but that does nothing for unix domain

    char *msg1 = "HTTP/1.1 200 event\r\nContent-Type: application/json\r\n\r\n";
    write(mgmt_event_subscribers[topic], msg1, strlen(msg1));
    // Ignore the result
    // (the message is leaving here fine, the problem must be at your end)

    if(replacing) {
        char *msg2 = "\x1e\"replacing\"\n";
        write(mgmt_event_subscribers[topic], msg2, strlen(msg2));
    }
}

void mgmt_event_post (const enum n3n_event_topic topic, int data0, const void *data1) {
    traceEvent(TRACE_DEBUG, "post topic=%i data0=%i", topic, data0);

    SOCKET debug = mgmt_event_subscribers[N3N_EVENT_DEBUG];
    SOCKET sub = mgmt_event_subscribers[topic];

    if( sub == -1 && debug == -1) {
        // If neither of this topic or the debug topic have a subscriber
        // then we dont need to do any work
        return;
    }

    char buf_space[200];
    strbuf_t *buf;
    STRBUF_INIT(buf, buf_space);

    mgmt_events[topic].func(buf, topic, data0, data1);

    if( sub != -1 ) {
        sb_write(sub, buf, 0, -1);
    }
    if( debug != -1 ) {
        sb_write(debug, buf, 0, -1);
    }
    // TODO:
    // - ideally, we would detect that the far end has gone away and
    //   set the subscriber socket back to -1
    // - this all assumes that the socket is set to non blocking
    // - if the write returns EWOULDBLOCK, increment a metric and return
}

static void generate_http_headers (conn_t *conn, const char *type, int code) {
    strbuf_t **pp = &conn->reply_header;
    sb_reprintf(pp, "HTTP/1.1 %i result\r\n", code);
    // TODO:
    // - caching
    int len = sb_len(conn->reply);
    sb_reprintf(pp, "Content-Type: %s\r\n", type);
    sb_reprintf(pp, "Content-Length: %i\r\n\r\n", len);
}

static void jsonrpc_error (char *id, conn_t *conn, int code, char *message) {
    // Reuse the request buffer
    sb_zero(conn->request);

    sb_reprintf(
        &conn->request,
        "{"
        "\"jsonrpc\":\"2.0\","
        "\"id\":\"%s\","
        "\"error\":{"
        " \"code\":%i,"
        " \"message\":\"%s\""
        "}}",
        id,
        code,
        message
        );

    // Update the reply buffer after last potential realloc
    conn->reply = conn->request;
    generate_http_headers(conn, "application/json", code);
}

static void jsonrpc_result_head (char *id, conn_t *conn) {
    // Reuse the request buffer
    sb_zero(conn->request);

    sb_reprintf(
        &conn->request,
        "{"
        "\"jsonrpc\":\"2.0\","
        "\"id\":\"%s\","
        "\"result\":",
        id
        );
}

static void jsonrpc_result_tail (conn_t *conn, int code) {
    sb_reprintf(&conn->request, "}");

    // Update the reply buffer after last potential realloc
    conn->reply = conn->request;

    generate_http_headers(conn, "application/json", code);
}

static void jsonrpc_1uint (char *id, conn_t *conn, uint32_t result) {
    jsonrpc_result_head(id, conn);
    sb_reprintf(&conn->request, "%u", result);
    jsonrpc_result_tail(conn, 200);
}

static void jsonrpc_get_verbose (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    jsonrpc_1uint(id, conn, getTraceLevel());
}

static void jsonrpc_set_verbose (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params_in) {
    // FIXME: add auth check

    if(!params_in) {
        jsonrpc_error(id, conn, 400, "missing param");
        return;
    }

    if(*params_in != '[') {
        jsonrpc_error(id, conn, 400, "expecting array");
        return;
    }

    // Avoid discarding the const attribute
    char *params = strdup(params_in+1);

    char *arg1 = json_extract_val(params);

    if(*arg1 == '"') {
        arg1++;
    }

    setTraceLevel(strtoul(arg1, NULL, 0));
    jsonrpc_get_verbose(id, eee, conn, params);
    free(params);
}

static void jsonrpc_stop (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    // FIXME: add auth check

    *eee->keep_running = false;

    jsonrpc_1uint(id, conn, *eee->keep_running);
}

static void jsonrpc_get_communities (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    if(!eee->communities) {
        // This is an edge
        if(eee->conf.header_encryption != HEADER_ENCRYPTION_NONE) {
            jsonrpc_error(id, conn, 403, "Forbidden");
            return;
        }

        jsonrpc_result_head(id, conn);
        sb_reprintf(
            &conn->request,
            "[{\"community\":\"%s\"}]",
            eee->conf.community_name
            );
        jsonrpc_result_tail(conn, 200);
        return;
    }

    // Otherwise send the supernode's view
    struct sn_community *community, *tmp;
    dec_ip_bit_str_t ip_bit_str = {'\0'};

    jsonrpc_result_head(id, conn);
    sb_reprintf(&conn->request, "[");

    HASH_ITER(hh, eee->communities, community, tmp) {

        sb_reprintf(&conn->request,
                    "{"
                    "\"community\":\"%s\","
                    "\"purgeable\":%i,"
                    "\"is_federation\":%i,"
                    "\"ip4addr\":\"%s\"},",
                    (community->is_federation) ? "-/-" : community->community,
                    community->purgeable,
                    community->is_federation,
                    (community->auto_ip_net.net_addr == 0) ? "" : ip_subnet_to_str(ip_bit_str, &community->auto_ip_net));
    }

    // HACK: back up over the final ','
    if(conn->request->str[conn->request->wr_pos-1] == ',') {
        conn->request->wr_pos--;
    }

    sb_reprintf(&conn->request, "]");
    jsonrpc_result_tail(conn, 200);
}

static void jsonrpc_get_edges_row (strbuf_t **reply, struct peer_info *peer, const char *mode, const char *community) {
    macstr_t mac_buf;
    n2n_sock_str_t sockbuf;
    dec_ip_bit_str_t ip_bit_str = {'\0'};

    sb_reprintf(reply,
                "{"
                "\"mode\":\"%s\","
                "\"community\":\"%s\","
                "\"ip4addr\":\"%s\","
                "\"purgeable\":%i,"
                "\"local\":%i,"
                "\"macaddr\":\"%s\","
                "\"sockaddr\":\"%s\","
                "\"desc\":\"%s\","
                "\"last_p2p\":%li,"
                "\"last_sent_query\":%li,"
                "\"last_seen\":%li},",
                mode,
                community,
                (peer->dev_addr.net_addr == 0) ? "" : ip_subnet_to_str(ip_bit_str, &peer->dev_addr),
                peer->purgeable,
                peer->local,
                (is_null_mac(peer->mac_addr)) ? "" : macaddr_str(mac_buf, peer->mac_addr),
                sock_to_cstr(sockbuf, &(peer->sock)),
                peer->dev_desc,
                peer->last_p2p,
                peer->last_sent_query,
                peer->last_seen
                );

    // TODO: add a proto: TCP|UDP item to the output
}

static void jsonrpc_get_edges (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    struct peer_info *peer, *tmpPeer;

    jsonrpc_result_head(id, conn);
    sb_reprintf(&conn->request, "[");

    // dump nodes with forwarding through supernodes
    HASH_ITER(hh, eee->pending_peers, peer, tmpPeer) {
        jsonrpc_get_edges_row(
            &conn->request,
            peer,
            "pSp",
            eee->conf.community_name
            );
    }

    // dump peer-to-peer nodes
    HASH_ITER(hh, eee->known_peers, peer, tmpPeer) {
        jsonrpc_get_edges_row(
            &conn->request,
            peer,
            "p2p",
            eee->conf.community_name
            );
    }

    struct sn_community *community, *tmp;
    HASH_ITER(hh, eee->communities, community, tmp) {
        HASH_ITER(hh, community->edges, peer, tmpPeer) {
            jsonrpc_get_edges_row(
                &conn->request,
                peer,
                "sn",
                (community->is_federation) ? "-/-" : community->community
                );
        }
    }


    // HACK: back up over the final ','
    if(conn->request->str[conn->request->wr_pos-1] == ',') {
        conn->request->wr_pos--;
    }

    sb_reprintf(&conn->request, "]");
    jsonrpc_result_tail(conn, 200);
}

static void jsonrpc_get_info (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    macstr_t mac_buf;
    n2n_sock_str_t sockbuf;

    struct in_addr ip_addr;
    ipstr_t ip_address;

    ip_addr.s_addr = eee->device.ip_addr;
    inaddrtoa(ip_address, ip_addr);

    jsonrpc_result_head(id, conn);

    sb_reprintf(&conn->request,
                "{"
                "\"version\":\"%s\","
                "\"builddate\":\"%s\","
                "\"is_edge\":%i,"
                "\"is_supernode\":%i,"
                "\"macaddr\":\"%s\","
                "\"ip4addr\":\"%s\","
                "\"sockaddr\":\"%s\"}",
                VERSION,
                BUILDDATE,
                eee->conf.is_edge,
                eee->conf.is_supernode,
                is_null_mac(eee->device.mac_addr) ? "" : macaddr_str(mac_buf, eee->device.mac_addr),
                ip_address,
                sock_to_cstr(sockbuf, &eee->conf.preferred_sock)
                );

    jsonrpc_result_tail(conn, 200);
}

static void jsonrpc_get_supernodes (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    struct peer_info *peer, *tmpPeer;
    macstr_t mac_buf;
    n2n_sock_str_t sockbuf;
    selection_criterion_str_t sel_buf;

    jsonrpc_result_head(id, conn);
    sb_reprintf(&conn->request, "[");

    HASH_ITER(hh, eee->conf.supernodes, peer, tmpPeer) {

        /*
         * TODO:
         * The version string provided by the remote supernode could contain
         * chars that make our JSON invalid.
         * - do we care?
         */

        sb_reprintf(&conn->request,
                    "{"
                    "\"version\":\"%s\","
                    "\"purgeable\":%i,"
                    "\"current\":%i,"
                    "\"macaddr\":\"%s\","
                    "\"sockaddr\":\"%s\","
                    "\"selection\":\"%s\","
                    "\"last_seen\":%li,"
                    "\"uptime\":%li},",
                    peer->version,
                    peer->purgeable,
                    (peer == eee->curr_sn) ? (eee->sn_wait ? 2 : 1 ) : 0,
                    is_null_mac(peer->mac_addr) ? "" : macaddr_str(mac_buf, peer->mac_addr),
                    sock_to_cstr(sockbuf, &(peer->sock)),
                    sn_selection_criterion_str(eee, sel_buf, peer),
                    peer->last_seen,
                    peer->uptime);
    }

    // HACK: back up over the final ','
    if(conn->request->str[conn->request->wr_pos-1] == ',') {
        conn->request->wr_pos--;
    }

    sb_reprintf(&conn->request, "]");
    jsonrpc_result_tail(conn, 200);
}

static void jsonrpc_get_timestamps (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    jsonrpc_result_head(id, conn);
    sb_reprintf(&conn->request,
                "{"
                "\"last_register_req\":%lu,"
                "\"last_rx_p2p\":%ld,"
                "\"last_rx_super\":%ld,"
                "\"last_sweep\":%ld,"
                "\"last_sn_fwd\":%ld,"
                "\"last_sn_reg\":%ld,"
                "\"start_time\":%lu}",
                eee->last_register_req,
                eee->last_p2p,
                eee->last_sup,
                eee->last_sweep,
                eee->last_sn_fwd,
                eee->last_sn_reg,
                eee->start_time
                );

    jsonrpc_result_tail(conn, 200);
}

static void jsonrpc_get_packetstats (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    jsonrpc_result_head(id, conn);
    sb_reprintf(&conn->request, "[");

    sb_reprintf(&conn->request,
                "{"
                "\"type\":\"transop\","
                "\"tx_pkt\":%lu,"
                "\"rx_pkt\":%lu},",
                eee->transop.tx_cnt,
                eee->transop.rx_cnt);

    sb_reprintf(&conn->request,
                "{"
                "\"type\":\"p2p\","
                "\"tx_pkt\":%u,"
                "\"rx_pkt\":%u},",
                eee->stats.tx_p2p,
                eee->stats.rx_p2p);

    sb_reprintf(&conn->request,
                "{"
                "\"type\":\"super\","
                "\"tx_pkt\":%u,"
                "\"rx_pkt\":%u},",
                eee->stats.tx_sup,
                eee->stats.rx_sup);

    sb_reprintf(&conn->request,
                "{"
                "\"type\":\"super_broadcast\","
                "\"tx_pkt\":%u,"
                "\"rx_pkt\":%u},",
                eee->stats.tx_sup_broadcast,
                eee->stats.rx_sup_broadcast);

    sb_reprintf(&conn->request,
                "{"
                "\"type\":\"tuntap_error\","
                "\"tx_pkt\":%u,"
                "\"rx_pkt\":%u},",
                eee->stats.tx_tuntap_error,
                eee->stats.rx_tuntap_error);

    sb_reprintf(&conn->request,
                "{"
                "\"type\":\"multicast_drop\","
                "\"tx_pkt\":%u,"
                "\"rx_pkt\":%u},",
                eee->stats.tx_multicast_drop,
                eee->stats.rx_multicast_drop);

    sb_reprintf(&conn->request,
                "{"
                "\"type\":\"sn_fwd\","
                "\"tx_pkt\":%u},",
                eee->stats.sn_fwd);

    sb_reprintf(&conn->request,
                "{"
                "\"type\":\"sn_broadcast\","
                "\"tx_pkt\":%u},",
                eee->stats.sn_broadcast);

    sb_reprintf(&conn->request,
                "{"
                "\"type\":\"sn_reg\","
                "\"tx_pkt\":%u,"
                "\"nak\":%u},",
                eee->stats.sn_reg,
                eee->stats.sn_reg_nak);

    /* Note: sn_reg_nak is not currently incremented anywhere */

    /* Generic errors when trying to sendto() */
    sb_reprintf(&conn->request,
                "{"
                "\"type\":\"sn_errors\","
                "\"tx_pkt\":%u},",
                eee->stats.sn_errors);

    // HACK: back up over the final ','
    if(conn->request->str[conn->request->wr_pos-1] == ',') {
        conn->request->wr_pos--;
    }

    sb_reprintf(&conn->request, "]");
    jsonrpc_result_tail(conn, 200);
}

#if 0
static void jsonrpc_todo (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    jsonrpc_error(id, conn, 501, "TODO");
}
#endif

static void jsonrpc_post_test (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {

    mgmt_event_post(N3N_EVENT_TEST, -1, params);

    jsonrpc_result_head(id, conn);
    sb_reprintf(&conn->request, "\"sent\"\n");
    jsonrpc_result_tail(conn, 200);
}


static void jsonrpc_reload_communities (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    // FIXME: add auth check

    int ok = load_allowed_sn_community(eee);

    jsonrpc_result_head(id, conn);
    sb_reprintf(&conn->request, "%i", ok);
    jsonrpc_result_tail(conn, 200);
}

static void jsonrpc_help_events (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    int nr_handlers = sizeof(mgmt_events) / sizeof(mgmt_events[0]);

    jsonrpc_result_head(id, conn);
    sb_reprintf(&conn->request, "[");
    for( int topic=0; topic < nr_handlers; topic++ ) {
        int sub = mgmt_event_subscribers[topic];
        char host[40];
        char serv[6];
        host[0] = '?';
        host[1] = 0;
        serv[0] = '?';
        serv[1] = 0;

        if(sub != -1) {
            struct sockaddr_storage sa;
            socklen_t sa_size = sizeof(sa);

            if(getpeername(sub, (struct sockaddr *)&sa, &sa_size) == 0) {
                getnameinfo(
                    (struct sockaddr *)&sa, sa_size,
                    host, sizeof(host),
                    serv, sizeof(serv),
                    NI_NUMERICHOST|NI_NUMERICSERV
                    );
            }
        }

        sb_reprintf(
            &conn->request,
            "{"
            "\"topic\":\"%s\","
            "\"sockaddr\":\"%s:%s\","
            "\"desc\":\"%s\"},",
            mgmt_events[topic].topic,
            host, serv,
            mgmt_events[topic].desc
            );
    }

    // HACK: back up over the final ','
    if(conn->request->str[conn->request->wr_pos-1] == ',') {
        conn->request->wr_pos--;
    }

    sb_reprintf(&conn->request, "]");
    jsonrpc_result_tail(conn, 200);
}

static void jsonrpc_help (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params);

struct mgmt_jsonrpc_method {
    char *method;
    void (*func)(char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params);
    char *desc;
};

static const struct mgmt_jsonrpc_method jsonrpc_methods[] = {
    { "get_communities", jsonrpc_get_communities, "Show current communities" },
    { "get_edges", jsonrpc_get_edges, "List current edges/peers" },
    { "get_info", jsonrpc_get_info, "Provide basic edge information" },
    { "get_packetstats", jsonrpc_get_packetstats, "traffic counters" },
    { "get_supernodes", jsonrpc_get_supernodes, "List current supernodes" },
    { "get_timestamps", jsonrpc_get_timestamps, "Event timestamps" },
    { "get_verbose", jsonrpc_get_verbose, "Logging verbosity" },
    { "help", jsonrpc_help, "Show JsonRPC methods" },
    { "help.events", jsonrpc_help_events, "Show available event topics" },
    { "post.test", jsonrpc_post_test, "Send a test event" },
    { "reload_communities", jsonrpc_reload_communities, "Reloads communities and user's public keys" },
    { "set_verbose", jsonrpc_set_verbose, "Set logging verbosity" },
    { "stop", jsonrpc_stop, "Stop the daemon" },
    // get_last_event?
};

static void jsonrpc_help (char *id, struct n3n_runtime_data *eee, conn_t *conn, const char *params) {
    jsonrpc_result_head(id, conn);
    sb_reprintf(&conn->request, "[");

    int i;
    int nr_handlers = sizeof(jsonrpc_methods) / sizeof(jsonrpc_methods[0]);
    for( i=0; i < nr_handlers; i++ ) {
        sb_reprintf(&conn->request,
                    "{"
                    "\"method\":\"%s\","
                    "\"desc\":\"%s\"},",
                    jsonrpc_methods[i].method,
                    jsonrpc_methods[i].desc
                    );

    }
    // HACK: back up over the final ','
    if(conn->request->str[conn->request->wr_pos-1] == ',') {
        conn->request->wr_pos--;
    }

    sb_reprintf(&conn->request, "]");
    jsonrpc_result_tail(conn, 200);
}

static void render_error (struct n3n_runtime_data *eee, conn_t *conn) {
    sb_zero(conn->request);
    sb_printf(conn->request, "api error\n");

    // Update the reply buffer after last potential realloc
    conn->reply = conn->request;

    generate_http_headers(conn, "text/plain", 404);
}

static void handle_jsonrpc (struct n3n_runtime_data *eee, conn_t *conn) {
    char *body = strstr(conn->request->str, "\r\n\r\n");
    if(!body) {
        // "Error: no body"
        goto error;
    }
    body += 4;

    jsonrpc_t json;

    if(jsonrpc_parse(body, &json) != 0) {
        // "Error: parsing json"
        goto error;
    }

    traceEvent(
        TRACE_DEBUG,
        "jsonrpc id=%s, method=%s, params=%s",
        json.id,
        json.method,
        json.params
        );

    // Since we are going to reuse the request buffer for the reply, copy
    // the id string out of it as every single reply will need it
    char idbuf[10];
    strncpy(idbuf, json.id, sizeof(idbuf)-1);

    int i;
    int nr_handlers = sizeof(jsonrpc_methods) / sizeof(jsonrpc_methods[0]);
    for( i=0; i < nr_handlers; i++ ) {
        if(!strcmp(
               jsonrpc_methods[i].method,
               json.method
               )) {
            break;
        }
    }
    if( i >= nr_handlers ) {
        // "Unknown method
        goto error;
    } else {
        jsonrpc_methods[i].func(idbuf, eee, conn, json.params);
    }
    return;

error:
    render_error(eee, conn);
}

static void render_todo_page (struct n3n_runtime_data *eee, conn_t *conn) {
    sb_zero(conn->request);
    sb_printf(conn->request, "TODO\n");

    // Update the reply buffer after last potential realloc
    conn->reply = conn->request;
    generate_http_headers(conn, "text/plain", 501);
}

#include "management_index.html.h"

// Generate the output for the human user interface
static void render_index_page (struct n3n_runtime_data *eee, conn_t *conn) {
    // TODO:
    // - could allow overriding of built in text with an external file
    // - there is a race condition if multiple users are fetching the
    //   page and have partial writes (same for render_script_page)
    conn->reply = &management_index;
    generate_http_headers(conn, "text/html", 200);
}

#include "management_script.js.h"

// Generate the output for the small set of javascript functions
static void render_script_page (struct n3n_runtime_data *eee, conn_t *conn) {
    conn->reply = &management_script;
    generate_http_headers(conn, "text/javascript", 200);
}

struct mgmt_api_endpoint {
    char *match;    // when the request buffer starts with this
    void (*func)(struct n3n_runtime_data *eee, conn_t *conn);
    char *desc;
};

static const struct mgmt_api_endpoint api_endpoints[] = {
    { "POST /v1 ", handle_jsonrpc, "JsonRPC" },
    { "GET / ", render_index_page, "Human interface" },
    { "GET /help ", render_todo_page, "Describe available endpoints" },
    { "GET /metrics ", render_todo_page, "Fetch metrics data" },
    { "GET /script.js ", render_script_page, "javascript helpers" },
    { "GET /status ", render_todo_page, "Quick health check" },
    { "GET /events/", event_subscribe, "Subscribe to events" },
};

void mgmt_api_handler (struct n3n_runtime_data *eee, conn_t *conn) {
    int i;
    int nr_handlers = sizeof(api_endpoints) / sizeof(api_endpoints[0]);
    for( i=0; i < nr_handlers; i++ ) {
        if(!strncmp(
               api_endpoints[i].match,
               conn->request->str,
               strlen(api_endpoints[i].match))) {
            break;
        }
    }
    if( i >= nr_handlers ) {
        render_error(eee, conn);
    } else {
        api_endpoints[i].func(eee, conn);
    }

    // Try to immediately start sending the reply
    conn_write(conn);
}
