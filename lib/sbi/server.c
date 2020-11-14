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

static OGS_POOL(server_pool, ogs_sbi_server_t);

void ogs_sbi_server_init(int num_of_session_pool)
{
    ogs_mhd_server_init(num_of_session_pool);

    ogs_list_init(&ogs_sbi_self()->server_list);
    ogs_pool_init(&server_pool, ogs_app()->pool.nf);
}

void ogs_sbi_server_final(void)
{
    ogs_sbi_server_remove_all();

    ogs_pool_final(&server_pool);

    ogs_mhd_server_final();
}

ogs_sbi_server_t *ogs_sbi_server_add(ogs_sockaddr_t *addr)
{
    ogs_sbi_server_t *server = NULL;

    ogs_assert(addr);

    ogs_pool_alloc(&server_pool, &server);
    ogs_assert(server);
    memset(server, 0, sizeof(ogs_sbi_server_t));

    ogs_list_init(&server->suspended_session_list);
    ogs_copyaddrinfo(&server->addr, addr);

    ogs_list_add(&ogs_sbi_self()->server_list, server);

    return server;
}

void ogs_sbi_server_remove(ogs_sbi_server_t *server)
{
    ogs_assert(server);

    ogs_list_remove(&ogs_sbi_self()->server_list, server);

    ogs_sbi_server_stop(server);

    ogs_assert(server->addr);
    ogs_freeaddrinfo(server->addr);

    ogs_pool_free(&server_pool, server);
}

void ogs_sbi_server_remove_all(void)
{
    ogs_sbi_server_t *server = NULL, *next_server = NULL;

    ogs_list_for_each_safe(&ogs_sbi_self()->server_list, next_server, server)
        ogs_sbi_server_remove(server);
}
