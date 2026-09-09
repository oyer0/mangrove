/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <session.h>

#include <identity.h>
#include <ipc.h>
#include <mangrove_errors.h>
#include <mg/service.h>
#include <mg/session_service.h>
#include <string.h>
#include <spinlock.h>

#define SESSION_MAX_ACTIVE 8U

typedef struct {
    mg_session_id_t id;
    u64 shell_pid;
    u64 owner_pid;
    user_identity_t identity;
    mg_session_state_t state;
    mg_session_activity_t activity;
    bool active;
} session_record_t;

static session_record_t session_records[SESSION_MAX_ACTIVE];
static mg_session_id_t next_session_id;
static bool autologin_attempted;
static spinlock_t session_lock;

bool session_init(void)
{
    spinlock_init(&session_lock);
    memset(session_records, 0, sizeof(session_records));
    next_session_id = 1;
    autologin_attempted = false;
    return true;
}

static bool session_service_has_privilege(process_t *requester,
                                          mg_service_id_t service_id)
{
    process_credentials_t credentials;

    return requester && requester->system_service &&
        requester->service_id == service_id &&
        process_get_credentials(requester, &credentials) &&
        identity_credentials_has_privilege(
            &credentials, IDENTITY_PRIVILEGE_MANAGE_SESSIONS);
}

static bool session_backend_authorized(process_t *requester)
{
    return session_service_has_privilege(requester, MG_SERVICE_SESSIOND);
}

static bool session_frontend_authorized(process_t *requester)
{
    return session_service_has_privilege(requester, MG_SERVICE_LOGIND);
}

static session_record_t *session_find(mg_session_id_t id)
{
    /* Caller holds session_lock. */
    for (u32 index = 0; index < SESSION_MAX_ACTIVE; index++) {
        if (session_records[index].active && session_records[index].id == id)
            return &session_records[index];
    }
    return NULL;
}

static session_record_t *session_allocate(void)
{
    u64 saved_flags = spin_lock_irqsave(&session_lock);
    session_record_t *record = NULL;

    if (next_session_id != 0) {
        for (u32 index = 0; index < SESSION_MAX_ACTIVE; index++) {
            if (!session_records[index].active) {
                record = &session_records[index];
                record->id = next_session_id++;
                record->shell_pid = 0;
                record->owner_pid = 0;
                record->state = MG_SESSION_STATE_CREATING;
                record->activity = MG_SESSION_ACTIVITY_INACTIVE;
                record->active = true;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&session_lock, saved_flags);
    return record;
}

static void session_release_id(mg_session_id_t id)
{
    session_record_t *record;
    u64 saved_flags;

    if (!id) return;
    saved_flags = spin_lock_irqsave(&session_lock);
    record = session_find(id);
    if (!record) {
        spin_unlock_irqrestore(&session_lock, saved_flags);
        return;
    }
    record->active = false;
    record->shell_pid = 0;
    record->owner_pid = 0;
    record->state = MG_SESSION_STATE_ENDED;
    record->activity = MG_SESSION_ACTIVITY_INACTIVE;
    memset(&record->identity, 0, sizeof(record->identity));
    record->id = 0;
    spin_unlock_irqrestore(&session_lock, saved_flags);
}

static void session_status_from_record(const session_record_t *record,
                                       mg_session_status_t *status)
{
    if (!record || !status) return;
    memset(status, 0, sizeof(*status));
    status->id = record->id;
    status->uid = record->identity.uid;
    status->role = record->identity.role;
    status->state = record->state;
    status->activity = record->activity;
    strncpy(status->username, record->identity.username,
            sizeof(status->username) - 1U);
}

int session_autologin_identity_process(process_t *requester,
                                       mg_identity_t *identity)
{
    user_identity_t candidate;
    u64 saved_flags;

    if (!identity || !session_frontend_authorized(requester))
        return MG_ERR_PRIVILEGE_REQUIRED;
    saved_flags = spin_lock_irqsave(&session_lock);
    if (autologin_attempted) {
        spin_unlock_irqrestore(&session_lock, saved_flags);
        return MG_ERR_AUTH_FAILED;
    }
    /* This is a boot-scoped eligibility token, not an account bypass.  It is
     * consumed before reading the configured identity so a service restart
     * cannot silently autologin again after logout or a shell crash. */
    autologin_attempted = true;
    spin_unlock_irqrestore(&session_lock, saved_flags);
    if (!identity_registry_autologin_user(&candidate))
        return MG_ERR_AUTH_FAILED;
    identity->uid = candidate.uid;
    identity->role = candidate.role;
    strncpy(identity->username, candidate.username,
            sizeof(identity->username) - 1U);
    strncpy(identity->home, candidate.home, sizeof(identity->home) - 1U);
    return MG_OK;
}

int session_create_process(process_t *requester,
                           process_handle_t request_handle,
                           const char *username,
                           mg_session_info_t *session)
{
    session_record_t *record;
    user_identity_t identity;
    u64 frontend_pid = 0;

    if (!session || !username || !session_backend_authorized(requester))
        return MG_ERR_PRIVILEGE_REQUIRED;
    /* The username is only a selector.  UID, role, home, and the frontend
     * identity come from kernel-owned state and the authenticated IPC
     * context, never from request payload claims. */
    if (!ipc_request_context_origin(requester, request_handle,
                                    MG_SERVICE_LOGIND, &frontend_pid))
        return MG_ERR_PRIVILEGE_REQUIRED;
    if (!identity_registry_lookup_username(username, &identity))
        return MG_ERR_NOT_FOUND;
    record = session_allocate();
    if (!record) return MG_ERR_BUSY;
    {
        u64 saved_flags = spin_lock_irqsave(&session_lock);
        record->owner_pid = frontend_pid;
        record->identity = identity;
        record->state = MG_SESSION_STATE_ACTIVE;
        record->activity = MG_SESSION_ACTIVITY_ACTIVE;
        session->id = record->id;
        spin_unlock_irqrestore(&session_lock, saved_flags);
    }
    session->shell = 0;
    return MG_OK;
}

int session_launch_process(process_t *requester,
                           mg_session_id_t session_id,
                           process_handle_t *out_shell)
{
    session_record_t snapshot;
    session_record_t *record;
    process_credentials_t credentials;
    u64 saved_flags;

    if (!out_shell || !session_frontend_authorized(requester))
        return MG_ERR_PRIVILEGE_REQUIRED;
    saved_flags = spin_lock_irqsave(&session_lock);
    record = session_find(session_id);
    if (!record || record->owner_pid != requester->pid ||
        record->state != MG_SESSION_STATE_ACTIVE || record->shell_pid) {
        spin_unlock_irqrestore(&session_lock, saved_flags);
        return MG_ERR_NOT_FOUND;
    }
    snapshot = *record;
    spin_unlock_irqrestore(&session_lock, saved_flags);
    if (!identity_credentials_from_user(&snapshot.identity, &credentials))
        return MG_ERR_ACCESS_DENIED;
    if (!process_spawn_with_context(requester, "/bin/shoot", &credentials,
                                    snapshot.id, true, snapshot.identity.home,
                                    out_shell))
        return MG_ERR_INVALID_EXEC;
    saved_flags = spin_lock_irqsave(&session_lock);
    record = session_find(snapshot.id);
    if (record && record->owner_pid == requester->pid &&
        record->state == MG_SESSION_STATE_ACTIVE && !record->shell_pid)
        record->shell_pid = process_handle_pid(requester, *out_shell);
    u64 shell_pid = record ? record->shell_pid : 0;
    spin_unlock_irqrestore(&session_lock, saved_flags);
    if (!shell_pid) {
        (void)process_terminate_session_members(snapshot.id);
        (void)process_handle_close(requester, *out_shell);
        process_reap_session_members(snapshot.id);
        session_release_id(snapshot.id);
        return MG_ERR_IO;
    }
    return MG_OK;
}

int session_end_process(process_t *requester, mg_session_id_t session_id)
{
    session_record_t *record;
    u64 saved_flags;

    if (!session_backend_authorized(requester))
        return MG_ERR_PRIVILEGE_REQUIRED;
    saved_flags = spin_lock_irqsave(&session_lock);
    record = session_find(session_id);
    if (!record) {
        spin_unlock_irqrestore(&session_lock, saved_flags);
        return MG_ERR_NOT_FOUND;
    }
    record->state = MG_SESSION_STATE_ENDING;
    spin_unlock_irqrestore(&session_lock, saved_flags);
    if (!process_terminate_session_members(session_id))
        return MG_ERR_BUSY;
    process_reap_session_members(session_id);
    session_release_id(session_id);
    return MG_OK;
}

int session_query_process(process_t *requester, mg_session_id_t session_id,
                          mg_session_status_t *status)
{
    session_record_t *record;

    if (!status || !session_backend_authorized(requester))
        return MG_ERR_PRIVILEGE_REQUIRED;
    u64 saved_flags = spin_lock_irqsave(&session_lock);
    record = session_find(session_id);
    if (!record) {
        spin_unlock_irqrestore(&session_lock, saved_flags);
        return MG_ERR_NOT_FOUND;
    }
    session_status_from_record(record, status);
    spin_unlock_irqrestore(&session_lock, saved_flags);
    return MG_OK;
}

int session_list_process(process_t *requester, u32 offset,
                         mg_session_status_t *status, u32 capacity,
                         u32 *out_count, u32 *out_total)
{
    u32 total = 0;
    u32 copied = 0;

    if (!status || !capacity || capacity > SESSION_MAX_ACTIVE ||
        !out_count || !out_total || !session_backend_authorized(requester))
        return MG_ERR_PRIVILEGE_REQUIRED;
    u64 saved_flags = spin_lock_irqsave(&session_lock);
    for (u32 index = 0; index < SESSION_MAX_ACTIVE; index++) {
        if (!session_records[index].active) continue;
        if (total >= offset && copied < capacity)
            session_status_from_record(&session_records[index],
                                       &status[copied++]);
        total++;
    }
    *out_count = copied;
    *out_total = total;
    spin_unlock_irqrestore(&session_lock, saved_flags);
    return MG_OK;
}

void session_process_exited(process_t *process)
{
    mg_session_id_t ended[SESSION_MAX_ACTIVE];
    u32 ended_count = 0;
    u64 saved_flags;

    if (!process) return;
    saved_flags = spin_lock_irqsave(&session_lock);
    for (u32 index = 0; index < SESSION_MAX_ACTIVE; index++) {
        session_record_t *record = &session_records[index];
        bool owned_by_frontend = record->active &&
                                  record->owner_pid == process->pid;
        bool backend_lost = process->system_service &&
                            process->service_id == MG_SERVICE_SESSIOND;
        if (owned_by_frontend || (backend_lost && record->active)) {
            if (ended_count < SESSION_MAX_ACTIVE)
                ended[ended_count++] = record->id;
        }
    }
    spin_unlock_irqrestore(&session_lock, saved_flags);
    for (u32 index = 0; index < ended_count; index++) {
        /* A service restart must never strand the old login shell or carry it
         * into a session created by the replacement service. */
        (void)process_terminate_session_members(ended[index]);
        process_reap_session_members(ended[index]);
        session_release_id(ended[index]);
    }
}
