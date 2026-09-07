#pragma once

#include <mg/session.h>
#include <process.h>

bool session_init(void);
int session_autologin_identity_process(process_t *requester,
                                       mg_identity_t *identity);
int session_create_process(process_t *requester,
                           process_handle_t request_handle,
                           const char *username,
                           mg_session_info_t *session);
int session_launch_process(process_t *requester,
                           mg_session_id_t session_id,
                           process_handle_t *out_shell);
int session_end_process(process_t *requester, mg_session_id_t session_id);
int session_query_process(process_t *requester, mg_session_id_t session_id,
                          mg_session_status_t *status);
int session_list_process(process_t *requester, u32 offset,
                         mg_session_status_t *status, u32 capacity,
                         u32 *out_count, u32 *out_total);
void session_process_exited(process_t *process);
