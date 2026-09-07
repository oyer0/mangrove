#pragma once

#include <mg/error.h>
#include <mg/identity.h>
#include <mg/types.h>

typedef u64 mg_session_id_t;
typedef struct mg_session_status mg_session_status_t;

typedef struct {
    mg_session_id_t id;
    mg_handle_t shell;
} mg_session_info_t;

typedef struct PACKED {
    u32 offset;
    u32 capacity;
    mg_session_status_t *result;
    u32 *out_count;
    u32 *out_total;
} mg_session_list_request_t;

/* Authentication is a logind-only PASS operation.  Session allocation and
 * shell launch are separate so sessiond remains the authoritative owner of
 * session IDs and membership. */
mg_result_t session_autologin_identity(mg_identity_t *identity);
mg_result_t session_create_from_request(mg_handle_t request,
                                        const char *username,
                                        mg_session_info_t *session);
mg_result_t session_launch_shell(mg_session_id_t session_id,
                                  mg_handle_t *out_shell);
mg_result_t session_query(mg_session_id_t session_id,
                           mg_session_status_t *status);
mg_result_t session_list(u32 offset, mg_session_status_t *status, u32 capacity,
                         u32 *out_count, u32 *out_total);
mg_result_t session_end(mg_session_id_t session_id);
