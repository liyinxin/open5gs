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

#include <nghttp2/nghttp2.h>

static void server_init(int num_of_session_pool);
static void server_final(void);

static void server_start(ogs_sbi_server_t *server, int (*cb)(
            ogs_sbi_server_t *server, ogs_sbi_session_t *sbi_sess,
            ogs_sbi_request_t *request));
static void server_stop(ogs_sbi_server_t *server);

static void server_send_response(
        ogs_sbi_session_t *sbi_sess, ogs_sbi_response_t *response);

static ogs_sbi_server_t *server_from_session(void *session);

const ogs_sbi_server_actions_t ogs_nghttp2_server_actions = {
    server_init,
    server_final,

    server_start,
    server_stop,

    server_send_response,
    server_from_session,
};

static void accept_handler(short when, ogs_socket_t fd, void *data);
static void recv_handler(short when, ogs_socket_t fd, void *data);

static void session_timer_expired(void *data);

typedef struct ogs_sbi_session_s {
    ogs_lnode_t             lnode;

    ogs_sock_t              *sock;
    ogs_sockaddr_t          *addr;
    struct {
        ogs_poll_t          *read;
        ogs_poll_t          *write;
    } poll;

    ogs_sbi_request_t       *request;
    ogs_sbi_server_t        *server;

    ogs_timer_t             *timer;

    void *data;

    nghttp2_session         *session;
} ogs_sbi_session_t;

static void initialize_nghttp2_session(ogs_sbi_session_t *sbi_sess);

static OGS_POOL(session_pool, ogs_sbi_session_t);

static void server_init(int num_of_session_pool)
{
    ogs_pool_init(&session_pool, num_of_session_pool);
}

static void server_final(void)
{
    ogs_pool_final(&session_pool);
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

    sbi_sess->poll.read = ogs_pollset_add(ogs_app()->pollset,
        OGS_POLLIN, sock->fd, recv_handler, sbi_sess);
    ogs_assert(sbi_sess->poll.read);

    sbi_sess->timer = ogs_timer_add(
            ogs_app()->timer_mgr, session_timer_expired, sbi_sess);
    ogs_assert(sbi_sess->timer);

    ogs_timer_start(sbi_sess->timer,
            ogs_app()->time.message.sbi.connection_deadline);

    ogs_list_add(&server->suspended_session_list, sbi_sess);

    return sbi_sess;
}

static void session_remove(ogs_sbi_session_t *sbi_sess)
{
    ogs_sbi_server_t *server = NULL;
    ogs_poll_t *poll = NULL;

    ogs_assert(sbi_sess);
    server = sbi_sess->server;
    ogs_assert(server);

    ogs_list_remove(&server->suspended_session_list, sbi_sess);

    /* TODO:
     * delete request */

    ogs_assert(sbi_sess->timer);
    ogs_timer_delete(sbi_sess->timer);

    poll = ogs_pollset_cycle(ogs_app()->pollset, sbi_sess->poll.read);
    ogs_assert(poll);
    ogs_pollset_remove(poll);

    poll = ogs_pollset_cycle(ogs_app()->pollset, sbi_sess->poll.write);
    if (poll)
        ogs_pollset_remove(poll);

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

    ogs_list_for_each_safe(
            &server->suspended_session_list, next_sbi_sess, sbi_sess)
        session_remove(sbi_sess);
}

static void server_start(ogs_sbi_server_t *server, int (*cb)(
            ogs_sbi_server_t *server, ogs_sbi_session_t *sbi_sess,
            ogs_sbi_request_t *request))
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

static void accept_handler(short when, ogs_socket_t fd, void *data)
{
#if 0
    char buf[OGS_ADDRSTRLEN];
#endif

    ogs_sbi_server_t *server = data;
    ogs_sbi_session_t *sbi_sess = NULL;
    ogs_sock_t *sock = NULL;
    ogs_sock_t *new = NULL;

    ogs_assert(data);
    ogs_assert(fd != INVALID_SOCKET);

    sock = server->node.sock;

    new = ogs_sock_accept(sock);
    if (!new) {
        ogs_error("ogs_sock_accept() failed");
        return;
    }

    sbi_sess = session_add(server, new);
    ogs_assert(sbi_sess);

    initialize_nghttp2_session(sbi_sess);

#if 0
    if (send_server_connection_header(session_data) != 0 ||
        session_send(session_data) != 0) {
        session_remove(sbi_sess);
        return;
    }
#endif
}

static void recv_handler(short when, ogs_socket_t fd, void *data)
{
    ogs_sbi_session_t *sbi_sess = data;
    ogs_pkbuf_t *pkbuf = NULL;
    int n;

    ogs_assert(sbi_sess);

    ogs_assert(fd != INVALID_SOCKET);
    ogs_assert(when == OGS_POLLIN);

    pkbuf = ogs_pkbuf_alloc(NULL, OGS_MAX_SDU_LEN);
    ogs_assert(pkbuf);

    n = ogs_recv(fd, pkbuf->data, OGS_MAX_SDU_LEN, 0);
    if (n > 0) {
        ogs_pkbuf_put(pkbuf, n);
    } else {
        if (n < 0)
            ogs_log_message(OGS_LOG_ERROR, ogs_socket_errno, "lost connection");
        else if (n == 0)
            ogs_error("connection closed");

        session_remove(sbi_sess);
    }

    ogs_pkbuf_free(pkbuf);
}

static void server_send_response(
        ogs_sbi_session_t *sbi_sess, ogs_sbi_response_t *response)
{
}

static ogs_sbi_server_t *server_from_session(void *session)
{
    ogs_sbi_session_t *sbi_sess = session;

    ogs_assert(sbi_sess);
    ogs_assert(sbi_sess->server);

    return sbi_sess->server;
}

static ssize_t send_callback(nghttp2_session *session, const uint8_t *data,
                             size_t length, int flags, void *user_data);
static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data);
static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                                    uint32_t error_code, void *user_data);
static int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, const uint8_t *name,
                              size_t namelen, const uint8_t *value,
                              size_t valuelen, uint8_t flags, void *user_data);
static int on_begin_headers_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data);

static void initialize_nghttp2_session(ogs_sbi_session_t *nghttp2_sess)
{
    nghttp2_session_callbacks *callbacks = NULL;

    ogs_assert(nghttp2_sess);

    nghttp2_session_callbacks_new(&callbacks);

    nghttp2_session_callbacks_set_send_callback(
            callbacks, send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(
            callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(
            callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_header_callback(
            callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(
            callbacks, on_begin_headers_callback);

    nghttp2_session_server_new(&nghttp2_sess->session, callbacks, nghttp2_sess);

    nghttp2_session_callbacks_del(callbacks);
}

static ssize_t send_callback(nghttp2_session *session, const uint8_t *data,
                             size_t length, int flags, void *user_data)
{
#if 0
  http2_session_data *session_data = (http2_session_data *)user_data;
  struct bufferevent *bev = session_data->bev;
  (void)session;
  (void)flags;

  /* Avoid excessive buffering in server side. */
  if (evbuffer_get_length(bufferevent_get_output(session_data->bev)) >=
      OUTPUT_WOULDBLOCK_THRESHOLD) {
    return NGHTTP2_ERR_WOULDBLOCK;
  }
  bufferevent_write(bev, data, length);
  return (ssize_t)length;
#else
  return 0;
#endif
}

static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data) {
#if 0
  http2_session_data *session_data = (http2_session_data *)user_data;
  http2_stream_data *stream_data;
  switch (frame->hd.type) {
  case NGHTTP2_DATA:
  case NGHTTP2_HEADERS:
    /* Check that the client request has finished */
    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
      stream_data =
          nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
      /* For DATA and HEADERS frame, this callback may be called after
         on_stream_close_callback. Check that stream still alive. */
      if (!stream_data) {
        return 0;
      }
      return on_request_recv(session, session_data, stream_data);
    }
    break;
  default:
    break;
  }
#endif
  return 0;
}

static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                                    uint32_t error_code, void *user_data) {
#if 0
  http2_session_data *session_data = (http2_session_data *)user_data;
  http2_stream_data *stream_data;
  (void)error_code;

  stream_data = nghttp2_session_get_stream_user_data(session, stream_id);
  if (!stream_data) {
    return 0;
  }
  remove_stream(session_data, stream_data);
  delete_http2_stream_data(stream_data);
#endif
  return 0;
}

/* nghttp2_on_header_callback: Called when nghttp2 library emits
   single header name/value pair. */
static int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, const uint8_t *name,
                              size_t namelen, const uint8_t *value,
                              size_t valuelen, uint8_t flags, void *user_data)
{
#if 0
  http2_stream_data *stream_data;
  const char PATH[] = ":path";
  (void)flags;
  (void)user_data;

  switch (frame->hd.type) {
  case NGHTTP2_HEADERS:
    if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
      break;
    }
    stream_data =
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (!stream_data || stream_data->request_path) {
      break;
    }
    if (namelen == sizeof(PATH) - 1 && memcmp(PATH, name, namelen) == 0) {
      size_t j;
      for (j = 0; j < valuelen && value[j] != '?'; ++j)
        ;
      stream_data->request_path = percent_decode(value, j);
    }
    break;
  }
#endif
  return 0;
}

static int on_begin_headers_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data)
{
#if 0
  http2_session_data *session_data = (http2_session_data *)user_data;
  http2_stream_data *stream_data;

  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }
  stream_data = create_http2_stream_data(session_data, frame->hd.stream_id);
  nghttp2_session_set_stream_user_data(session, frame->hd.stream_id,
                                       stream_data);
#endif
  return 0;
}
