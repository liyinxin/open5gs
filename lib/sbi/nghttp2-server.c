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

typedef struct ogs_nghttp2_session_s {
    ogs_lnode_t             lnode;

#if 0
    struct MHD_Connection   *connection;
#endif

    ogs_sbi_request_t       *request;
    ogs_sbi_server_t        *server;

#if 0
    /*
     * The HTTP server(MHD) should send an HTTP response
     * if an HTTP client(CURL) is requested.
     *
     * If the HTTP client closes the socket without sending an HTTP response,
     * the CPU load of a program using MHD is 100%. This is because
     * POLLIN(POLLRDHUP) is generated. So, the callback function of poll
     * continues to be called.
     *
     * I've created the timer to check whether the user does not use
     * the HTTP response. When the timer expires, an assertion occurs and
     * terminates the program.
     */
    ogs_timer_t             *timer;
#endif

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

static void server_start(ogs_sbi_server_t *server, int (*cb)(
            ogs_sbi_server_t *server, ogs_sbi_session_t *session,
            ogs_sbi_request_t *request))
{
#if 0
    char buf[128];
    ogs_assert(server);

    /* Setup callback function */
    server->cb = cb;

    ogs_fatal("addr = %s", OGS_ADDR(server->addr, buf));
#endif
}

static void server_stop(ogs_sbi_server_t *server)
{
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
