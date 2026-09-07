#pragma once

#include <identity.h>
#include <mg/event.h>
#include <mg/ipc.h>
#include <mg/service.h>
#include <process.h>
#include <types.h>

#define IPC_MAX_ENDPOINTS       8U
#define IPC_MAX_REQUESTS        32U
#define IPC_MAX_QUEUE_DEPTH     8U

bool ipc_init(void);

int ipc_service_register(process_t *process, const char *name,
                         process_handle_t *out_handle);
int ipc_service_lookup(process_t *process, const char *name,
                       process_handle_t *out_handle);
int ipc_kernel_request(process_t *process, process_handle_t endpoint_handle,
                       const mg_ipc_message_t *message,
                       mg_ipc_message_t *reply);
int ipc_kernel_receive(process_t *process, process_handle_t endpoint_handle,
                       mg_ipc_received_t *received);
int ipc_kernel_receive_timed(process_t *process, process_handle_t endpoint_handle,
                             mg_ipc_received_t *received, u32 timeout_ms);
int ipc_kernel_try_receive(process_t *process, process_handle_t endpoint_handle,
                           mg_ipc_received_t *received);
int ipc_kernel_reply(process_t *process, process_handle_t request_handle,
                     const mg_ipc_message_t *message);
int ipc_kernel_event_subscribe(process_t *process,
                               process_handle_t endpoint_handle,
                               u32 event_classes);
int ipc_kernel_event_unsubscribe(process_t *process,
                                 process_handle_t endpoint_handle);

/* Called only by kernel hardware/subsystem code. */
void ipc_publish_event(u32 event_class, u16 type, u64 resource_id,
                       const char *name);

/* Called before a process changes state or releases its handle table. */
void ipc_process_exit(process_t *process);

/* Kernel-only bridge for a protected operation reached through IPC.  The
 * context is consumed exactly once and returns the request's authenticated
 * identity; it never trusts requester fields supplied in the payload. */
bool ipc_request_context_claim(process_t *service,
                               process_handle_t request_handle,
                               process_credentials_t *credentials,
                               char *requester_name,
                               usize requester_name_size);
/* Validate the kernel-authenticated service that originated a delivered
 * request without trusting any payload identity fields. */
bool ipc_request_context_origin(process_t *service,
                                process_handle_t request_handle,
                                u32 expected_service_id,
                                u64 *origin_pid);
