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

#if !defined(OGS_SBI_INSIDE) && !defined(OGS_SBI_COMPILATION)
#error "This header cannot be included directly."
#endif

#ifndef OGS_MHD_SERVER_H
#define OGS_MHD_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

void ogs_mhd_server_init(int num_of_connection_pool);
void ogs_mhd_server_final(void);

void ogs_mhd_server_start(ogs_sbi_server_t *server, int (*cb)(
            ogs_sbi_server_t *server, ogs_sbi_session_t *session,
            ogs_sbi_request_t *request));
void ogs_mhd_server_stop(ogs_sbi_server_t *server);

void ogs_mhd_server_send_response(
        ogs_sbi_session_t *session, ogs_sbi_response_t *response);

ogs_sbi_server_t *ogs_mhd_server_from_session(void *session);

#ifdef __cplusplus
}
#endif

#endif /* OGS_MHD_SERVER_H */
