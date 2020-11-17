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

static void server_init(int num_of_session_pool);
static void server_final(void);

static void server_start(ogs_sbi_server_t *server, int (*cb)(
            ogs_sbi_server_t *server, ogs_sbi_session_t *session,
            ogs_sbi_request_t *request));
static void server_stop(ogs_sbi_server_t *server);

static void server_send_response(
        ogs_sbi_session_t *session, ogs_sbi_response_t *response);

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

typedef struct ogs_nghttp2_session_s {
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
} ogs_nghttp2_session_t;

static OGS_POOL(session_pool, ogs_nghttp2_session_t);

static void server_init(int num_of_session_pool)
{
    ogs_pool_init(&session_pool, num_of_session_pool);
}

static void server_final(void)
{
    ogs_pool_final(&session_pool);
}

static ogs_nghttp2_session_t *session_add(ogs_sbi_server_t *server,
        ogs_sbi_request_t *request, ogs_sock_t *sock)
{
    ogs_nghttp2_session_t *nghttp2_sess = NULL;

    ogs_assert(server);
    ogs_assert(request);
    ogs_assert(sock);

    ogs_pool_alloc(&session_pool, &nghttp2_sess);
    ogs_assert(nghttp2_sess);
    memset(nghttp2_sess, 0, sizeof(ogs_nghttp2_session_t));

    nghttp2_sess->server = server;
    nghttp2_sess->request = request;
    nghttp2_sess->sock = sock;

    nghttp2_sess->addr = ogs_calloc(1, sizeof(ogs_sockaddr_t));
    ogs_assert(nghttp2_sess->addr);
    memcpy(nghttp2_sess->addr, &sock->remote_addr, sizeof(ogs_sockaddr_t));

    nghttp2_sess->poll.read = ogs_pollset_add(ogs_app()->pollset,
        OGS_POLLIN, sock->fd, recv_handler, sock);
    ogs_assert(nghttp2_sess->poll.read);

    nghttp2_sess->timer = ogs_timer_add(
            ogs_app()->timer_mgr, session_timer_expired, nghttp2_sess);
    ogs_assert(nghttp2_sess->timer);

    ogs_timer_start(nghttp2_sess->timer,
            ogs_app()->time.message.sbi.connection_deadline);

    ogs_list_add(&server->suspended_session_list, nghttp2_sess);

    return nghttp2_sess;
}

static void session_remove(ogs_nghttp2_session_t *nghttp2_sess)
{
    ogs_sbi_server_t *server = NULL;
    ogs_poll_t *poll = NULL;

    ogs_assert(nghttp2_sess);
    server = nghttp2_sess->server;
    ogs_assert(server);

    ogs_list_remove(&server->suspended_session_list, nghttp2_sess);

    ogs_assert(nghttp2_sess->timer);
    ogs_timer_delete(nghttp2_sess->timer);

    poll = ogs_pollset_cycle(ogs_app()->pollset, nghttp2_sess->poll.read);
    ogs_assert(poll);
    ogs_pollset_remove(poll);

    poll = ogs_pollset_cycle(ogs_app()->pollset, nghttp2_sess->poll.write);
    if (poll)
        ogs_pollset_remove(poll);

    ogs_assert(nghttp2_sess->addr);
    ogs_free(nghttp2_sess->addr);

    ogs_assert(nghttp2_sess->sock);
    ogs_sock_destroy(nghttp2_sess->sock);

    ogs_pool_free(&session_pool, nghttp2_sess);
}

static void session_timer_expired(void *data)
{
    ogs_nghttp2_session_t *nghttp2_sess = NULL;

    nghttp2_sess = data;
    ogs_assert(nghttp2_sess);

    ogs_error("An HTTP was requested, but the HTTP response is missing.");

    session_remove(nghttp2_sess);
}

static void session_remove_all(ogs_sbi_server_t *server)
{
    ogs_nghttp2_session_t *nghttp2_sess = NULL, *next_nghttp2_sess = NULL;

    ogs_assert(server);

    ogs_list_for_each_safe(
            &server->suspended_session_list, next_nghttp2_sess, nghttp2_sess)
        session_remove(nghttp2_sess);
}

static void server_start(ogs_sbi_server_t *server, int (*cb)(
            ogs_sbi_server_t *server, ogs_sbi_session_t *session,
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
#if 0
    ogs_sbi_request_t *request = NULL;
#else
    ogs_sbi_request_t *request = (ogs_sbi_request_t *)1;
#endif
    ogs_sbi_session_t *session = NULL;

    ogs_nghttp2_session_t *nghttp2_sess = NULL;
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

    nghttp2_sess = session_add(server, request, new);
    ogs_assert(nghttp2_sess);
    session = (ogs_sbi_session_t *)nghttp2_sess;
    ogs_assert(session);
}

static void recv_handler(short when, ogs_socket_t fd, void *data)
{
    ogs_sock_t *sock = data;

    ogs_assert(sock);
    ogs_assert(fd != INVALID_SOCKET);

    if (when == OGS_POLLIN) {
    } else if (when == OGS_POLLOUT) {
    } else
        ogs_assert_if_reached();
}

static void server_send_response(
        ogs_sbi_session_t *session, ogs_sbi_response_t *response)
{
}

static ogs_sbi_server_t *server_from_session(void *session)
{
    ogs_nghttp2_session_t *nghttp2_sess = NULL;

    nghttp2_sess = session;
    ogs_assert(nghttp2_sess);
    ogs_assert(nghttp2_sess->server);

    return nghttp2_sess->server;
}
