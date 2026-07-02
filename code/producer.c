/*
 * producer.c - Thread 1: the asynchronous, event-driven network producer.
 *
 * Responsibilities
 * ----------------
 *  - Maintain a TLS WebSocket connection to the Bluesky Jetstream firehose.
 *  - For every complete text frame: copy it into the ring buffer and return
 *    to the event loop IMMEDIATELY (no parsing, no I/O here) so the socket is
 *    drained as fast as possible and no packets are lost.
 *  - Survive arbitrary network failures: if the link drops, the connection
 *    times out, the Wi-Fi disappears, or DNS fails, the producer reconnects
 *    automatically with exponential back-off and KEEPS RETRYING FOREVER. The
 *    consumer and logger threads keep running throughout, so logging never
 *    stops - outage seconds simply show zero message counts.
 *
 * Resilience mechanisms
 * ---------------------
 *  1. lws retry/back-off policy (table below) reconnects after clean closes
 *     and connection errors, with jitter to avoid thundering-herd reconnects.
 *  2. secs_since_valid_ping / secs_since_valid_hangup make lws actively PING
 *     and then tear down a *silently dead* link (e.g. cable pulled / Wi-Fi
 *     lost) that TCP itself would not notice for a long time -> fast recovery.
 *  3. conceal_count is set effectively to infinity, so it never gives up.
 */
#include "app.h"
#include "config.h"

#include <libwebsockets.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Per-WebSocket-session scratch space (managed by lws via protocols[].
 * Used to reassemble messages that lws delivers in several fragments. */
struct pss {
    char  *buf;
    size_t len;
    size_t cap;
    int    overflow;   /* set if a frame exceeded WS_RX_BUFFER -> drop it    */
};

/* Single-producer module state (only ever touched by the producer thread). */
struct prod_state {
    app_t                          *app;
    struct lws                     *client_wsi;
    lws_sorted_usec_list_t          sul;        /* reconnection timer        */
    uint16_t                        retry_count;
    struct lws_client_connect_info  ccinfo;
};
static struct prod_state P;

/* Back-off schedule (ms). conceal_count huge => retry forever. */
static const uint32_t backoff_ms[] = { 1000, 2000, 3000, 5000, 8000, 10000 };
static const lws_retry_bo_t retry_policy = {
    .retry_ms_table         = backoff_ms,
    .retry_ms_table_count   = LWS_ARRAY_SIZE(backoff_ms),
    .conceal_count          = 0xffff,                 /* ~never stop trying  */
    .secs_since_valid_ping  = WS_PING_IDLE_SECS,
    .secs_since_valid_hangup= WS_HANGUP_IDLE_SECS,
    .jitter_percent         = 20,
};

static void connect_client(lws_sorted_usec_list_t *sul);

/* (Re)issue the client connection. Runs on the lws event-loop thread. */
static void connect_client(lws_sorted_usec_list_t *sul)
{
    struct prod_state *p = lws_container_of(sul, struct prod_state, sul);
    if (p->app->stop)
        return;

    p->ccinfo.pwsi = &p->client_wsi;
    if (!lws_client_connect_via_info(&p->ccinfo)) {
        /* Could not even start the attempt (e.g. DNS down): back off & retry.
         * Returns non-zero only when conceal_count is exhausted (never here).*/
        lws_retry_sul_schedule(p->ccinfo.context, 0, &p->sul, &retry_policy,
                               connect_client, &p->retry_count);
    }
}

static int callback_jetstream(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len)
{
    struct pss *pss = (struct pss *)user;

    switch (reason) {

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        lwsl_warn("jetstream: connected\n");
        P.retry_count = 0;                 /* reset back-off after success   */
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        /* Lazily (re)allocate the reassembly buffer once per session. */
        if (!pss->buf) {
            pss->buf = malloc(WS_RX_BUFFER);
            pss->cap = WS_RX_BUFFER;
            pss->len = 0;
            pss->overflow = 0;
            if (!pss->buf)
                return -1;                 /* hopeless: close, will reconnect*/
        }

        if (pss->len + len > pss->cap) {
            pss->overflow = 1;             /* too big: keep draining, drop    */
        } else {
            memcpy(pss->buf + pss->len, in, len);
            pss->len += len;
        }

        /* A logical message is complete only on the final fragment with no
         * remaining payload pending for this frame. */
        if (lws_is_final_fragment(wsi) && !lws_remaining_packet_payload(wsi)) {
            if (pss->overflow) {
                /* Force the ring's oversized counter (len==cap => len+1>slot)*/
                rb_push(&P.app->ring, pss->buf, pss->cap);
            } else if (pss->len > 0) {
                rb_push(&P.app->ring, pss->buf, pss->len);
            }
            pss->len = 0;
            pss->overflow = 0;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        lwsl_warn("jetstream: connection error: %s\n",
                  in ? (char *)in : "(null)");
        /* fall through */
    case LWS_CALLBACK_CLIENT_CLOSED:
        P.client_wsi = NULL;
        if (pss && pss->buf) {            /* free reassembly buffer on close */
            free(pss->buf);
            pss->buf = NULL;
            pss->len = pss->cap = 0;
            pss->overflow = 0;
        }
        if (!P.app->stop) {
            lwsl_warn("jetstream: link down, scheduling reconnect\n");
            lws_retry_sul_schedule(P.ccinfo.context, 0, &P.sul, &retry_policy,
                                   connect_client, &P.retry_count);
        }
        break;

    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    { JS_PROTOCOL_NAME, callback_jetstream, sizeof(struct pss),
      WS_RX_BUFFER, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

void *producer_thread(void *arg)
{
    app_t *app = (app_t *)arg;
    P.app = app;
    P.retry_count = 0;

    /* Only show errors/warnings from lws (keeps a 24h run's logs sane). */
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port      = CONTEXT_PORT_NO_LISTEN;   /* client only               */
    info.protocols = protocols;
    info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.fd_limit_per_thread = 1 + 1 + 1;      /* tiny: we hold one socket   */
    info.user      = app;

    struct lws_context *ctx = lws_create_context(&info);
    if (!ctx) {
        fprintf(stderr, "FATAL: lws_create_context failed\n");
        app->stop = 1;
        rb_wake_all(&app->ring);
        return NULL;
    }

    memset(&P.ccinfo, 0, sizeof(P.ccinfo));
    P.ccinfo.context             = ctx;
    P.ccinfo.address             = JS_HOST;
    P.ccinfo.port                = JS_PORT;
    P.ccinfo.path                = JS_PATH;
    P.ccinfo.host                = JS_HOST;
    P.ccinfo.origin              = JS_HOST;
    P.ccinfo.ssl_connection      = LCCSCF_USE_SSL;
    P.ccinfo.protocol            = NULL;             /* request no subproto  */
    P.ccinfo.local_protocol_name = JS_PROTOCOL_NAME; /* bind our callback    */
    P.ccinfo.retry_and_idle_policy = &retry_policy;  /* enables ping/hangup  */
    P.ccinfo.pwsi                = &P.client_wsi;

    /* Kick off the first connection attempt. */
    memset(&P.sul, 0, sizeof(P.sul));
    connect_client(&P.sul);

    /* Event loop. lws_service() returns at least every 100 ms so we notice
     * the stop flag promptly even while the network is idle. */
    int n = 0;
    while (n >= 0 && !app->stop)
        n = lws_service(ctx, 100);

    lws_context_destroy(ctx);
    rb_wake_all(&app->ring);   /* make sure the consumer can leave rb_peek() */
    lwsl_warn("jetstream: producer thread exiting\n");
    return NULL;
}
