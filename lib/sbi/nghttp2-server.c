/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ogs-app.h"
#include "ogs-sbi.h"
#include "yuarel.h"

#include <netinet/tcp.h>
#include <nghttp2/nghttp2.h>

static void server_init(int num_of_stream_pool);
static void server_final(void);

static void server_start(ogs_sbi_server_t *server,
        int (*cb)(ogs_sbi_request_t *request, void *data));
static void server_stop(ogs_sbi_server_t *server);

static void server_send_response(
        ogs_sbi_stream_t *stream, ogs_sbi_response_t *response);

static ogs_sbi_server_t *server_from_stream(ogs_sbi_stream_t *data);

const ogs_sbi_server_actions_t ogs_nghttp2_server_actions = {
    server_init,
    server_final,

    server_start,
    server_stop,

    server_send_response,
    server_from_stream,
};

typedef struct ogs_sbi_session_s {
    ogs_lnode_t             lnode;

    ogs_sock_t              *sock;
    ogs_sockaddr_t          *addr;
    struct {
        ogs_poll_t          *read;
        ogs_poll_t          *write;
    } poll;

    nghttp2_session         *session;
    ogs_list_t              write_queue;

    ogs_sbi_server_t        *server;
    ogs_list_t              stream_list;

    ogs_timer_t             *timer;

    void *data;
} ogs_sbi_session_t;

typedef struct ogs_sbi_stream_s {
    ogs_lnode_t             lnode;

    int32_t                 stream_id;
    ogs_sbi_request_t       *request;

    ogs_sbi_session_t       *session;
} ogs_sbi_stream_t;

static void session_remove(ogs_sbi_session_t *sbi_sess);
static void session_remove_all(ogs_sbi_server_t *server);
static void session_timer_expired(void *data);

static void stream_remove(ogs_sbi_stream_t *stream);

static void accept_handler(short when, ogs_socket_t fd, void *data);
static void recv_handler(short when, ogs_socket_t fd, void *data);

static void initialize_nghttp2_session(ogs_sbi_session_t *sbi_sess);
static int submit_server_connection_header(ogs_sbi_session_t *sbi_sess);
static int submit_rst_stream(ogs_sbi_stream_t *stream, uint32_t error_code);

static int session_send(ogs_sbi_session_t *sbi_sess);
static void session_write_to_buffer(
        ogs_sbi_session_t *sbi_sess, ogs_pkbuf_t *pkbuf);

typedef struct ogs_sbi_pseudo_header_s {
    const char *name;
    size_t len;
} ogs_sbi_pseudo_header_t;

static ogs_sbi_pseudo_header_t pseudo_headers[] = {
    { .name = ":method",    .len = 7  },
    { .name = ":scheme",    .len = 7  },
    { .name = ":authority", .len = 10 },
    { .name = ":path",      .len = 5  },
};

static OGS_POOL(session_pool, ogs_sbi_session_t);
static OGS_POOL(stream_pool, ogs_sbi_stream_t);

static void server_init(int num_of_stream_pool)
{
    ogs_pool_init(&session_pool, num_of_stream_pool);
    ogs_pool_init(&stream_pool, num_of_stream_pool);
}

static void server_final(void)
{
    ogs_pool_final(&stream_pool);
    ogs_pool_final(&session_pool);
}

static void server_start(ogs_sbi_server_t *server,
        int (*cb)(ogs_sbi_request_t *request, void *data))
{
    char buf[OGS_ADDRSTRLEN];
    ogs_sock_t *sock = NULL;
    ogs_sockaddr_t *addr = NULL;
    char *hostname = NULL;

    addr = server->node.addr;
    ogs_assert(addr);

    sock = ogs_tcp_server(&server->node);
    if (!sock) {
        ogs_error("Cannot start SBI server");
        return;
    }

    /* Setup callback function */
    server->cb = cb;

    /* Setup poll for server listening socket */
    server->node.poll = ogs_pollset_add(ogs_app()->pollset,
            OGS_POLLIN, sock->fd, accept_handler, server);
    ogs_assert(server->node.poll);

    hostname = ogs_gethostname(addr);
    if (hostname)
        ogs_info("nghttp2_server() [%s]:%d", hostname, OGS_PORT(addr));
    else
        ogs_info("nghttp2_server() [%s]:%d",
                OGS_ADDR(addr, buf), OGS_PORT(addr));
}

static void server_stop(ogs_sbi_server_t *server)
{
    ogs_assert(server);

    if (server->node.poll)
        ogs_pollset_remove(server->node.poll);

    if (server->node.sock)
        ogs_sock_destroy(server->node.sock);

    session_remove_all(server);
}

static void add_header(nghttp2_nv *nv, const char *key, const char *value)
{
    nv->name = (uint8_t *)key;
    nv->namelen = strlen (key);
    nv->value = (uint8_t *)value;
    nv->valuelen = strlen(value);
    nv->flags = NGHTTP2_NV_FLAG_NONE;
}

static char status_string[600][4] = {
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "100", "101", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "200", "201", "202", "203", "204", "205", "206", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "300", "301", "302", "303", "304", "305", "306", "307", "308", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "400", "401", "402", "403", "404", "405", "406", "407", "408", "409",
 "410", "411", "412", "413", "414", "415", "416", "417", "", "",
 "", "421", "", "", "", "", "426", "", "428", "429", "", "431", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "451", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "500", "501", "502", "503", "504", "505", "", "", "", "", "", "511", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
 "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""
};

#define DATE_STRLEN 128
static char *get_date_string(char *date)
{
    static const char *const days[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char *const mons[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    ogs_assert(date);

    struct tm tm;
    ogs_gmtime(ogs_time_sec(ogs_time_now()), &tm);

    ogs_snprintf(date, DATE_STRLEN, "%3s, %02u %3s %04u %02u:%02u:%02u GMT",
            days[tm.tm_wday % 7],
            (unsigned int)tm.tm_mday,
            mons[tm.tm_mon % 12],
            (unsigned int)(1900 + tm.tm_year),
            (unsigned int)tm.tm_hour,
            (unsigned int)tm.tm_min,
            (unsigned int)tm.tm_sec);

    return date;
}

static ssize_t response_read_callback(
        nghttp2_session *session, int32_t stream_id,
        uint8_t *buf, size_t length, uint32_t *data_flags,
        nghttp2_data_source *source, void *user_data)
{
    ogs_sbi_response_t *response = NULL;
    ogs_sbi_stream_t *stream = NULL;

    ogs_assert(session);
    stream = nghttp2_session_get_stream_user_data(session, stream_id);
    if (!stream) {
        ogs_error("No stream [%d]", stream_id);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    ogs_assert(source);
    response = source->ptr;
    ogs_assert(response);

    ogs_assert(response->http.content);
    ogs_assert(response->http.content_length);

    *data_flags |= NGHTTP2_DATA_FLAG_NO_COPY;
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;

    if (nghttp2_session_get_stream_remote_close(session, stream_id) == 0) {
        ogs_error("nghttp2_session_get_stream_remote_close() failed");
        nghttp2_submit_rst_stream(
                session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_NO_ERROR);
    }

    return response->http.content_length;
}

static void server_send_response(
        ogs_sbi_stream_t *stream, ogs_sbi_response_t *response)
{
    ogs_sbi_session_t *sbi_sess = NULL;
    ogs_hash_index_t *hi;
    nghttp2_nv *nva;
    size_t nvlen;
    int i, rv;
    char datebuf[DATE_STRLEN];
    char clen[32];

    ogs_assert(stream);
    sbi_sess = stream->session;
    ogs_assert(sbi_sess);
    ogs_assert(response);

    nvlen = 2; /* :status && date */

    for (hi = ogs_hash_first(response->http.headers);
            hi; hi = ogs_hash_next(hi))
        nvlen++;

    if (response->http.content && response->http.content_length)
        nvlen++;

    nva = ogs_calloc(nvlen, sizeof(nghttp2_nv));
    ogs_assert(nva);

    i = 0;

    ogs_assert(response->status < 600);
    ogs_assert(strlen(status_string[response->status]) == 3);
    add_header(&nva[i++], ":status", status_string[response->status]);

    add_header(&nva[i++], "date", get_date_string(datebuf));

    if (response->http.content && response->http.content_length) {
        ogs_snprintf(clen, sizeof(clen),
                "%d", (int)response->http.content_length);
        add_header(&nva[i++], "content-length", clen);
    }

    for (hi = ogs_hash_first(response->http.headers);
            hi; hi = ogs_hash_next(hi)) {
        add_header(&nva[i++], ogs_hash_this_key(hi), ogs_hash_this_val(hi));
    }

    if (response->http.content && response->http.content_length) {
        nghttp2_data_provider data_prd;

        data_prd.source.ptr = response;
        data_prd.read_callback = response_read_callback;

        rv = nghttp2_submit_response(sbi_sess->session,
                stream->stream_id, nva, nvlen, &data_prd);
        if (rv != OGS_OK)
            ogs_error("nghttp2_submit_response() failed (%d:%s)",
                        rv, nghttp2_strerror(rv));
    } else {
        rv = nghttp2_submit_response(sbi_sess->session,
                stream->stream_id, nva, nvlen, NULL);
        if (rv != OGS_OK)
            ogs_error("nghttp2_submit_response() failed (%d:%s)",
                        rv, nghttp2_strerror(rv));
    }

    if (rv == OGS_OK)
        session_send(sbi_sess);

    ogs_sbi_response_free(response);
    ogs_free(nva);
}

static ogs_sbi_server_t *server_from_stream(ogs_sbi_stream_t *stream)
{
    ogs_sbi_session_t *sbi_sess = NULL;

    ogs_assert(stream);
    sbi_sess = stream->session;
    ogs_assert(sbi_sess);
    ogs_assert(sbi_sess->server);

    return sbi_sess->server;
}

static ogs_sbi_stream_t *stream_add(
        ogs_sbi_session_t *sbi_sess, int32_t stream_id)
{
    ogs_sbi_stream_t *stream = NULL;

    ogs_assert(sbi_sess);

    ogs_pool_alloc(&stream_pool, &stream);
    ogs_assert(stream);
    memset(stream, 0, sizeof(ogs_sbi_stream_t));

    stream->request = ogs_sbi_request_new();
    ogs_assert(stream->request);

    stream->stream_id = stream_id;

    stream->session = sbi_sess;

    return stream;
}

static void stream_remove(ogs_sbi_stream_t *stream)
{
    ogs_sbi_session_t *sbi_sess = NULL;

    ogs_assert(stream);
    sbi_sess = stream->session;
    ogs_assert(sbi_sess);

    ogs_list_remove(&sbi_sess->stream_list, stream);

    ogs_assert(stream->request);
    ogs_sbi_request_free(stream->request);

    ogs_pool_free(&stream_pool, stream);
}

static void stream_remove_all(ogs_sbi_session_t *sbi_sess)
{
    ogs_sbi_stream_t *stream = NULL, *next_stream = NULL;

    ogs_assert(sbi_sess);

    ogs_list_for_each_safe(&sbi_sess->stream_list, next_stream, stream)
        stream_remove(stream);
}

static ogs_sbi_session_t *session_add(
        ogs_sbi_server_t *server, ogs_sock_t *sock)
{
    ogs_sbi_session_t *sbi_sess = NULL;

    ogs_assert(server);
    ogs_assert(sock);

    ogs_pool_alloc(&session_pool, &sbi_sess);
    ogs_assert(sbi_sess);
    memset(sbi_sess, 0, sizeof(ogs_sbi_session_t));

    sbi_sess->server = server;
    sbi_sess->sock = sock;

    sbi_sess->addr = ogs_calloc(1, sizeof(ogs_sockaddr_t));
    ogs_assert(sbi_sess->addr);
    memcpy(sbi_sess->addr, &sock->remote_addr, sizeof(ogs_sockaddr_t));

    sbi_sess->timer = ogs_timer_add(
            ogs_app()->timer_mgr, session_timer_expired, sbi_sess);
    ogs_assert(sbi_sess->timer);

    ogs_timer_start(sbi_sess->timer,
            ogs_app()->time.message.sbi.connection_deadline);

    ogs_list_add(&server->session_list, sbi_sess);

    return sbi_sess;
}

static void session_remove(ogs_sbi_session_t *sbi_sess)
{
    ogs_sbi_server_t *server = NULL;
    ogs_poll_t *poll = NULL;
    ogs_pkbuf_t *pkbuf = NULL, *next_pkbuf = NULL;

    ogs_assert(sbi_sess);
    server = sbi_sess->server;
    ogs_assert(server);

    ogs_list_remove(&server->session_list, sbi_sess);

    ogs_assert(sbi_sess->timer);
    ogs_timer_delete(sbi_sess->timer);

    stream_remove_all(sbi_sess);

    poll = ogs_pollset_cycle(ogs_app()->pollset, sbi_sess->poll.read);
    ogs_assert(poll);
    ogs_pollset_remove(poll);

    poll = ogs_pollset_cycle(ogs_app()->pollset, sbi_sess->poll.write);
    if (poll)
        ogs_pollset_remove(poll);

    ogs_list_for_each_safe(&sbi_sess->write_queue, next_pkbuf, pkbuf)
        ogs_pkbuf_free(pkbuf);

    ogs_assert(sbi_sess->addr);
    ogs_free(sbi_sess->addr);

    ogs_assert(sbi_sess->sock);
    ogs_sock_destroy(sbi_sess->sock);

    ogs_pool_free(&session_pool, sbi_sess);
}

static void session_timer_expired(void *data)
{
    ogs_sbi_session_t *sbi_sess = data;

    ogs_assert(sbi_sess);

    ogs_error("An HTTP was requested, but the HTTP response is missing.");

    session_remove(sbi_sess);
}

static void session_remove_all(ogs_sbi_server_t *server)
{
    ogs_sbi_session_t *sbi_sess = NULL, *next_sbi_sess = NULL;

    ogs_assert(server);

    ogs_list_for_each_safe(&server->session_list, next_sbi_sess, sbi_sess)
        session_remove(sbi_sess);
}

static void accept_handler(short when, ogs_socket_t fd, void *data)
{
    ogs_sbi_server_t *server = data;
    ogs_sbi_session_t *sbi_sess = NULL;
    ogs_sock_t *sock = NULL;
    ogs_sock_t *new = NULL;

    int on;

    ogs_assert(data);
    ogs_assert(fd != INVALID_SOCKET);

    sock = server->node.sock;

    new = ogs_sock_accept(sock);
    if (!new) {
        ogs_log_message(OGS_LOG_ERROR, ogs_socket_errno, "accept() failed");
        return;
    }
    ogs_assert(new->fd != INVALID_SOCKET);

    on = 1;
    if (setsockopt(new->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) {
        ogs_log_message(OGS_LOG_ERROR, ogs_socket_errno,
                "setsockopt() for SCTP_NODELAY failed");
        return;
    }

    sbi_sess = session_add(server, new);
    ogs_assert(sbi_sess);

    sbi_sess->poll.read = ogs_pollset_add(ogs_app()->pollset,
        OGS_POLLIN, new->fd, recv_handler, sbi_sess);
    ogs_assert(sbi_sess->poll.read);

    initialize_nghttp2_session(sbi_sess);

    if (submit_server_connection_header(sbi_sess) != OGS_OK) {
        ogs_error("send_server_connection_header() failed");
        session_remove(sbi_sess);
        return;
    }

    session_send(sbi_sess);
}

static void recv_handler(short when, ogs_socket_t fd, void *data)
{
    char buf[OGS_ADDRSTRLEN];
    ogs_sockaddr_t *addr = NULL;

    ogs_sbi_session_t *sbi_sess = data;
    ogs_pkbuf_t *pkbuf = NULL;
    ssize_t readlen;
    int n;

    ogs_assert(sbi_sess);
    ogs_assert(fd != INVALID_SOCKET);
    addr = sbi_sess->addr;
    ogs_assert(addr);

    pkbuf = ogs_pkbuf_alloc(NULL, OGS_MAX_SDU_LEN);
    ogs_assert(pkbuf);

    n = ogs_recv(fd, pkbuf->data, OGS_MAX_SDU_LEN, 0);
    if (n > 0) {
        ogs_pkbuf_put(pkbuf, n);

        ogs_assert(sbi_sess->session);
        readlen = nghttp2_session_mem_recv(
                sbi_sess->session, pkbuf->data, pkbuf->len);
        if (readlen < 0) {
            ogs_error("nghttp2_session_mem_recv() failed (%d:%s)",
                        (int)readlen, nghttp2_strerror((int)readlen));
            session_remove(sbi_sess);
        }
    } else {
        ogs_sockaddr_t *addr = sbi_sess->addr;

        ogs_assert(addr);
        if (n < 0)
            ogs_log_message(OGS_LOG_ERROR, ogs_socket_errno,
                "lost connection [%s]:%d", OGS_ADDR(addr, buf), OGS_PORT(addr));
        else if (n == 0)
            ogs_error("connection closed [%s]:%d",
                        OGS_ADDR(addr, buf), OGS_PORT(addr));

        session_remove(sbi_sess);
    }

    ogs_pkbuf_free(pkbuf);
}

static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data);

static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                                    uint32_t error_code, void *user_data);

static int on_header_callback2(nghttp2_session *session,
                               const nghttp2_frame *frame,
                               nghttp2_rcbuf *name, nghttp2_rcbuf *value,
                               uint8_t flags, void *user_data);

static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,
                                       int32_t stream_id, const uint8_t *data,
                                       size_t len, void *user_data);

static int on_begin_headers_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data);

static int on_send_data_callback(nghttp2_session *session, nghttp2_frame *frame,
                                 const uint8_t *framehd, size_t length,
                                 nghttp2_data_source *source, void *user_data);

static void initialize_nghttp2_session(ogs_sbi_session_t *sbi_sess)
{
    nghttp2_session_callbacks *callbacks = NULL;

    ogs_assert(sbi_sess);

    nghttp2_session_callbacks_new(&callbacks);

    nghttp2_session_callbacks_set_on_frame_recv_callback(
            callbacks, on_frame_recv_callback);

    nghttp2_session_callbacks_set_on_stream_close_callback(
            callbacks, on_stream_close_callback);

    nghttp2_session_callbacks_set_on_header_callback2(
            callbacks, on_header_callback2);

    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
            callbacks, on_data_chunk_recv_callback);

    nghttp2_session_callbacks_set_on_begin_headers_callback(
            callbacks, on_begin_headers_callback);

    nghttp2_session_callbacks_set_send_data_callback(
            callbacks, on_send_data_callback);

    nghttp2_session_server_new(&sbi_sess->session, callbacks, sbi_sess);

    nghttp2_session_callbacks_del(callbacks);
}

static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data)
{
    ogs_sbi_session_t *sbi_sess = user_data;

    ogs_sbi_server_t *server = NULL;
    ogs_sbi_stream_t *stream = NULL;
    ogs_sbi_request_t *request = NULL;

    ogs_assert(sbi_sess);
    server = sbi_sess->server;
    ogs_assert(server);

    switch (frame->hd.type) {
    case NGHTTP2_DATA:
    case NGHTTP2_HEADERS:
        /* Check that the client request has finished */
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            stream = nghttp2_session_get_stream_user_data(
                            session, frame->hd.stream_id);

            /* For DATA and HEADERS frame, this callback may be called after
            on_stream_close_callback. Check that stream still alive. */
            if (!stream) {
                return 0;
            }

            request = stream->request;
            ogs_assert(request);

            if (server->cb) {
                if (server->cb(request, stream) != OGS_OK) {
                    ogs_warn("server callback error");
                    ogs_sbi_server_send_error(stream,
                            OGS_SBI_HTTP_STATUS_INTERNAL_SERVER_ERROR, NULL,
                            "server callback error", NULL);

                    return 0;
                }
            } else {
                ogs_fatal("server callback is not registered");
                ogs_assert_if_reached();
            }

            break;
        }
    default:
        break;
    }

    return 0;
}

static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                                    uint32_t error_code, void *user_data)
{
    ogs_sbi_stream_t *stream = NULL;

    ogs_assert(session);
    stream = nghttp2_session_get_stream_user_data(session, stream_id);

    if (!stream) {
        return 0;
    }

    if (error_code) {
        ogs_error("on_stream_close_callback() failed (%d:%s)",
                    error_code, nghttp2_strerror(error_code));
        nghttp2_submit_rst_stream(
                session, NGHTTP2_FLAG_NONE, stream_id, error_code);
    }

    stream_remove(stream);
    return 0;
}

static int on_header_callback2(nghttp2_session *session,
                               const nghttp2_frame *frame,
                               nghttp2_rcbuf *name, nghttp2_rcbuf *value,
                               uint8_t flags, void *user_data)
{
    ogs_sbi_session_t *sbi_sess = user_data;
    ogs_sbi_stream_t *stream = NULL;
    ogs_sbi_request_t *request = NULL;

    const char PATH[] = ":path";
    const char METHOD[] = ":method";
    const char ACCEPT[] = "accept";
    const char ACCEPT_ENCODING[] = "accept-encoding";
    const char CONTENT_TYPE[] = "content-type";
    const char LOCATION[] = "location";

    nghttp2_vec namebuf, valuebuf;
    char *namestr = NULL, *valuestr = NULL;

    ogs_assert(sbi_sess);

    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    stream = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (!stream) {
        return 0;
    }

    request = stream->request;
    ogs_assert(request);

    ogs_assert(name);
    namebuf = nghttp2_rcbuf_get_buf(name);
    ogs_assert(namebuf.base);
    ogs_assert(namebuf.len);

    ogs_assert(value);
    valuebuf = nghttp2_rcbuf_get_buf(value);
    ogs_assert(valuebuf.base);
    ogs_assert(valuebuf.len);

    namestr = ogs_strndup((const char *)namebuf.base, namebuf.len);
    if (!namestr) {
        ogs_error("ogs_strndup() failed");
        goto cleanup;
    }

    valuestr = ogs_strndup((const char *)valuebuf.base, valuebuf.len);
    if (!valuestr) {
        ogs_error("ogs_strndup() failed");
        goto cleanup;
    }

    if (namebuf.len == sizeof(PATH) - 1 &&
            memcmp(PATH, namebuf.base, namebuf.len) == 0) {
        char *saveptr = NULL, *query;
#define MAX_NUM_OF_PARAM_IN_QUERY 16
        struct yuarel_param params[MAX_NUM_OF_PARAM_IN_QUERY+2];
        int j;

        request->h.uri = ogs_sbi_parse_uri(valuestr, "?", &saveptr);
        if (!request->h.uri) {
            ogs_error("ogs_sbi_parse_uri() failed");
            goto cleanup;
        }

        memset(params, 0, sizeof(params));

        query = ogs_sbi_parse_uri(NULL, "?", &saveptr);
        if (query && *query && strlen(query))
            yuarel_parse_query(query, '&', params, MAX_NUM_OF_PARAM_IN_QUERY+1);

        j = 0;
        while(params[j].key && params[j].val) {
            ogs_sbi_header_set(request->http.params,
                    params[j].key, params[j].val);
            j++;
        }

        if (j >= MAX_NUM_OF_PARAM_IN_QUERY+1) {
            ogs_fatal("Maximum number(%d) of query params reached",
                    MAX_NUM_OF_PARAM_IN_QUERY);
            ogs_assert_if_reached();
        }

        ogs_free(query);

    } else if (namebuf.len == sizeof(METHOD) - 1 &&
            memcmp(METHOD, namebuf.base, namebuf.len) == 0) {

        request->h.method = valuestr;

    } else if (namebuf.len == sizeof(ACCEPT) - 1 &&
            memcmp(ACCEPT, namebuf.base, namebuf.len) == 0) {

        ogs_sbi_header_set(request->http.headers, OGS_SBI_ACCEPT, valuestr);

    } else if (namebuf.len == sizeof(ACCEPT_ENCODING) - 1 &&
            memcmp(ACCEPT_ENCODING, namebuf.base, namebuf.len) == 0) {

        ogs_sbi_header_set(request->http.headers,
                OGS_SBI_ACCEPT_ENCODING, valuestr);

    } else if (namebuf.len == sizeof(CONTENT_TYPE) - 1 &&
            memcmp(CONTENT_TYPE, namebuf.base, namebuf.len) == 0) {

        ogs_sbi_header_set(request->http.headers,
                OGS_SBI_CONTENT_TYPE, valuestr);

    } else if (namebuf.len == sizeof(LOCATION) - 1 &&
            memcmp(LOCATION, namebuf.base, namebuf.len) == 0) {

        ogs_sbi_header_set(request->http.headers, OGS_SBI_LOCATION, valuestr);

    } else {

        ogs_sbi_header_set(request->http.headers, namestr, valuestr);
    }

    if (namestr) ogs_free(namestr);
    if (valuestr) ogs_free(valuestr);

    return 0;

cleanup:
    if (submit_rst_stream(stream, NGHTTP2_INTERNAL_ERROR) != OGS_OK) {
        ogs_error("submit_rst_stream() failed");
        session_remove(sbi_sess);
        return 0;
    }

    session_send(sbi_sess);

    if (namestr) ogs_free(namestr);
    if (valuestr) ogs_free(valuestr);

    return 0;
}

static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,
                                       int32_t stream_id, const uint8_t *data,
                                       size_t len, void *user_data)
{
    ogs_sbi_session_t *sbi_sess = user_data;
    ogs_sbi_stream_t *stream = NULL;
    ogs_sbi_request_t *request = NULL;

    ogs_assert(sbi_sess);

    stream = nghttp2_session_get_stream_user_data(session, stream_id);
    if (!stream) {
        return 0;
    }

    request = stream->request;
    ogs_assert(request);

    ogs_assert(data);
    ogs_assert(len);

    request->http.content_length = len;
    request->http.content = (char*)ogs_malloc(request->http.content_length + 1);
    ogs_assert(request->http.content);
    memcpy(request->http.content, data, len);
    request->http.content[request->http.content_length] = '\0';

    return 0;
}

static int on_begin_headers_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data)
{
    ogs_sbi_session_t *sbi_sess = user_data;
    ogs_sbi_stream_t *stream = NULL;

    ogs_assert(sbi_sess);
    ogs_assert(session);
    ogs_assert(frame);

    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    stream = stream_add(sbi_sess, frame->hd.stream_id);
    ogs_assert(stream);

    nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, stream);

    return 0;
}

static int on_send_data_callback(nghttp2_session *session, nghttp2_frame *frame,
                                 const uint8_t *framehd, size_t length,
                                 nghttp2_data_source *source, void *user_data)
{
    ogs_sbi_session_t *sbi_sess = user_data;
    ogs_sbi_response_t *response = NULL;
    ogs_pkbuf_t *pkbuf = NULL;
    size_t padlen = 0;

    ogs_assert(sbi_sess);
    ogs_assert(frame);
    ogs_assert(framehd);
    ogs_assert(length);

    ogs_assert(source);
    response = source->ptr;
    ogs_assert(response);

    ogs_assert(response->http.content);
    ogs_assert(response->http.content_length);

    pkbuf = ogs_pkbuf_alloc(NULL, OGS_MAX_SDU_LEN);
    ogs_assert(pkbuf);
    ogs_pkbuf_put_data(pkbuf, framehd, 9);

    padlen = frame->data.padlen;

    if (padlen > 0) {
        ogs_pkbuf_put_u8(pkbuf, padlen-1);
    }

    ogs_pkbuf_put_data(pkbuf,
            response->http.content, response->http.content_length);

    if (padlen > 0) {
        memset(pkbuf->tail, 0, padlen-1);
        ogs_pkbuf_put(pkbuf, padlen-1);
    }

    session_write_to_buffer(sbi_sess, pkbuf);

    return 0;
}

/* Send HTTP/2 client connection header, which includes 24 bytes
   magic octets and SETTINGS frame */
static int submit_server_connection_header(ogs_sbi_session_t *sbi_sess)
{
    int rv;
    nghttp2_settings_entry iv[1] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 }
    };

    ogs_assert(sbi_sess);
    ogs_assert(sbi_sess->session);

    rv = nghttp2_submit_settings(
            sbi_sess->session, NGHTTP2_FLAG_NONE, iv, OGS_ARRAY_SIZE(iv));
    if (rv != 0) {
        ogs_error("nghttp2_submit_settings() failed (%d:%s)",
                    rv, nghttp2_strerror(rv));
        return OGS_ERROR;
    }

    return OGS_OK;
}

static int submit_rst_stream(ogs_sbi_stream_t *stream, uint32_t error_code)
{
    ogs_sbi_session_t *sbi_sess = NULL;

    ogs_assert(stream);
    sbi_sess = stream->session;
    ogs_assert(sbi_sess);

    return nghttp2_submit_rst_stream(sbi_sess->session, NGHTTP2_FLAG_NONE,
            stream->stream_id, error_code);
}

static int session_send(ogs_sbi_session_t *sbi_sess)
{
    ogs_pkbuf_t *pkbuf = NULL;
    ssize_t total_bytes = 0;

    ogs_assert(sbi_sess);
    ogs_assert(sbi_sess->session);

    for (;;) {
        const uint8_t *data = NULL;
        ssize_t data_len;

        data_len = nghttp2_session_mem_send(sbi_sess->session, &data);
        if (data_len < 0) {
            ogs_log_message(OGS_LOG_ERROR, ogs_socket_errno,
                    "nghttp2_session_mem_send() failed [%d]", (int)data_len);
            return OGS_ERROR;
        }

        if (data_len == 0) {
            break;
        }

        pkbuf = ogs_pkbuf_alloc(NULL, data_len);
        ogs_assert(pkbuf);
        ogs_pkbuf_put_data(pkbuf, data, data_len);

        session_write_to_buffer(sbi_sess, pkbuf);

        total_bytes += data_len;
    }

    return total_bytes;
}

static void session_write_callback(short when, ogs_socket_t fd, void *data)
{
    ogs_sbi_session_t *sbi_sess = data;
    ogs_pkbuf_t *pkbuf = NULL;

    ogs_assert(sbi_sess);

    if (ogs_list_empty(&sbi_sess->write_queue) == true) {
        ogs_assert(sbi_sess->poll.write);
        ogs_pollset_remove(sbi_sess->poll.write);
        return;
    }

    pkbuf = ogs_list_first(&sbi_sess->write_queue);
    ogs_assert(pkbuf);
    ogs_list_remove(&sbi_sess->write_queue, pkbuf);

    ogs_send(fd, pkbuf->data, pkbuf->len, 0);
    ogs_pkbuf_free(pkbuf);
}

static void session_write_to_buffer(
        ogs_sbi_session_t *sbi_sess, ogs_pkbuf_t *pkbuf)
{
    ogs_sock_t *sock = NULL;
    ogs_socket_t fd = INVALID_SOCKET;

    ogs_poll_t *poll = NULL;

    ogs_assert(pkbuf);

    ogs_assert(sbi_sess);
    sock = sbi_sess->sock;
    ogs_assert(sock);
    fd = sock->fd;
    ogs_assert(fd != INVALID_SOCKET);

    ogs_list_add(&sbi_sess->write_queue, pkbuf);

    poll = ogs_pollset_cycle(ogs_app()->pollset, sbi_sess->poll.write);
    if (!poll)
        sbi_sess->poll.write = ogs_pollset_add(ogs_app()->pollset,
            OGS_POLLOUT, fd, session_write_callback, sbi_sess);
}
