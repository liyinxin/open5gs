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

#ifndef OGS_SBI_PRIVATE_H
#define OGS_SBI_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "mhd-server.h"

typedef struct ogs_sbi_server_s {
    ogs_lnode_t     lnode;                  /* A node of list_t */

    ogs_sockaddr_t  *addr;                  /* Listen socket address */

    struct {
        const char  *key;
        const char  *pem;
    } tls;

    int (*cb)(ogs_sbi_server_t *server, ogs_sbi_session_t *session,
            ogs_sbi_request_t *request);
    void *data;

    ogs_list_t      suspended_session_list; /* MHD suspended list */

    void            *mhd;                   /* MHD instance */
    ogs_poll_t      *poll;                  /* MHD server poll */

} ogs_sbi_server_t;

#ifdef __cplusplus
}
#endif

#endif /* OGS_SBI_PRIVATE_H */
