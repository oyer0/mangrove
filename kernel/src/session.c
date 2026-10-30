/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <session.h>

#include <identity.h>
#include <process_cleanup.h>
#include <panic.h>
#include <scheduler.h>
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
    bool retirement_in_progress;
    bool retirement_dirty;
    enum { SESSION_LAUNCH_NONE, SESSION_LAUNCH_PREPARING,
           SESSION_LAUNCH_COMMITTED } launch_phase;
    u64 launch_attempt, launch_thread, launch_pid;
} session_record_t;

static session_record_t session_records[SESSION_MAX_ACTIVE];
static mg_session_id_t next_session_id;
static u64 next_launch_attempt;
static bool autologin_attempted;
static spinlock_t session_lock;

bool session_init(void)
{
    spinlock_init(&session_lock);
    memset(session_records, 0, sizeof(session_records));
    next_session_id = 1;
    next_launch_attempt = 1;
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

/* Called only by the process module while the registry protects frontend
 * liveness. Lock order: process registry -> session. No slot pointer escapes. */
int session_publish_active(u64 frontend_pid, const user_identity_t *identity,
                           mg_session_id_t *out_id)
{
    u64 flags = spin_lock_irqsave(&session_lock);
    if (!next_session_id) {
        spin_unlock_irqrestore(&session_lock, flags);
        return MG_ERR_BUSY;
    }
    for (u32 i = 0; i < SESSION_MAX_ACTIVE; i++) {
        session_record_t *record = &session_records[i];
        if (record->active) continue;
        memset(record, 0, sizeof(*record));
        record->id = next_session_id++;
        record->owner_pid = frontend_pid;
        record->identity = *identity;
        record->state = MG_SESSION_STATE_ACTIVE;
        record->activity = MG_SESSION_ACTIVITY_ACTIVE;
        record->active = true; /* Full scalar identity is initialized first. */
        *out_id = record->id;
        spin_unlock_irqrestore(&session_lock, flags);
        return MG_OK;
    }
    spin_unlock_irqrestore(&session_lock, flags);
    return MG_ERR_BUSY;
}

/* Caller owns registry, so parent exit cannot cross this authorization point.
 * The exact token is retained until enqueue/publication or complete rollback. */
bool session_commit_launch(mg_session_id_t id, u64 attempt, u64 owner_pid,
                            u64 owner_thread, u64 child_pid)
{
    u64 flags = spin_lock_irqsave(&session_lock);
    session_record_t *record = session_find(id);
    bool allowed = record && record->state == MG_SESSION_STATE_ACTIVE &&
        record->launch_phase == SESSION_LAUNCH_PREPARING &&
        record->launch_attempt == attempt && record->owner_pid == owner_pid &&
        record->launch_thread == owner_thread && child_pid;
    if (allowed) {
        record->launch_pid = child_pid;
        record->launch_phase = SESSION_LAUNCH_COMMITTED;
    }
    spin_unlock_irqrestore(&session_lock, flags);
    return allowed;
}

static bool session_mark_ending_locked(session_record_t *record)
{
    if (!record || record->state == MG_SESSION_STATE_ENDING) return false;
    record->state = MG_SESSION_STATE_ENDING;
    record->retirement_dirty = true;
    return true;
}

static void session_try_retire(mg_session_id_t id)
{
    u64 flags = spin_lock_irqsave(&session_lock);
    session_record_t *record = session_find(id);
    if (!record || record->state != MG_SESSION_STATE_ENDING ||
        !record->retirement_dirty || record->retirement_in_progress ||
        record->launch_phase != SESSION_LAUNCH_NONE) {
        spin_unlock_irqrestore(&session_lock, flags);
        return;
    }
    record->retirement_dirty = false;
    record->retirement_in_progress = true;
    spin_unlock_irqrestore(&session_lock, flags);

    /* This cooperative operation never discards a live kernel continuation.
     * A blocked dependency owns its next real progress notification. */
    bool complete = process_retire_session_members(id);
    process_reap_session_members(id);
    flags = spin_lock_irqsave(&session_lock);
    record = session_find(id);
    if (!record || !record->retirement_in_progress ||
        record->state != MG_SESSION_STATE_ENDING ||
        record->launch_phase != SESSION_LAUNCH_NONE)
        panic("session: retirement claim lost ownership");
    if (complete) {
        memset(record, 0, sizeof(*record));
    } else {
        record->retirement_in_progress = false;
    }
    spin_unlock_irqrestore(&session_lock, flags);
}

/* Only the ordinary cleanup worker executes retirement. A global semantic
 * event outlives any process whose exit made retirement newly possible. */
void session_cleanup_progress(void)
{
    mg_session_id_t ids[SESSION_MAX_ACTIVE];
    u32 count = 0;
    u64 flags = spin_lock_irqsave(&session_lock);
    for (u32 i = 0; i < SESSION_MAX_ACTIVE; i++) {
        session_record_t *record = &session_records[i];
        if (!record->active || record->state != MG_SESSION_STATE_ENDING)
            continue;
        record->retirement_dirty = true;
        ids[count++] = record->id;
    }
    spin_unlock_irqrestore(&session_lock, flags);
    for (u32 i = 0; i < count; i++) session_try_retire(ids[i]);
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
    mg_session_id_t id;
    int result = process_activate_session(requester, frontend_pid, &identity, &id);
    if (result != MG_OK) return result;
    session->id = id;
    session->shell = 0;
    return MG_OK;
}

int session_launch_process(process_t *requester,
                           mg_session_id_t session_id,
                           process_handle_t *out_shell)
{
    session_record_t snapshot;
    process_credentials_t credentials;
    kernel_thread_t *creator = thread_current();
    if (!out_shell || !session_frontend_authorized(requester))
        return MG_ERR_PRIVILEGE_REQUIRED;
    if (!creator || creator->process != requester) return MG_ERR_BAD_ARGUMENT;

    u64 flags = spin_lock_irqsave(&session_lock);
    session_record_t *record = session_find(session_id);
    if (!record || record->owner_pid != requester->pid ||
        record->state != MG_SESSION_STATE_ACTIVE || record->shell_pid) {
        spin_unlock_irqrestore(&session_lock, flags);
        return MG_ERR_NOT_FOUND;
    }
    if (record->launch_phase != SESSION_LAUNCH_NONE || !next_launch_attempt) {
        spin_unlock_irqrestore(&session_lock, flags);
        return MG_ERR_BUSY;
    }
    record->launch_phase = SESSION_LAUNCH_PREPARING;
    record->launch_attempt = next_launch_attempt++;
    record->launch_thread = creator->id;
    snapshot = *record;
    spin_unlock_irqrestore(&session_lock, flags);

    process_session_launch_t launch = {.attempt = snapshot.launch_attempt};
    int result = MG_ERR_ACCESS_DENIED;
    if (identity_credentials_from_user(&snapshot.identity, &credentials)) {
        result = process_spawn_session_shell(requester, &credentials,
            snapshot.id, snapshot.launch_attempt, snapshot.identity.home, &launch);
    }
    flags = spin_lock_irqsave(&session_lock);
    record = session_find(snapshot.id);
    if (!record || record->owner_pid != requester->pid ||
        record->launch_attempt != snapshot.launch_attempt ||
        record->launch_thread != creator->id ||
        (record->launch_phase != SESSION_LAUNCH_PREPARING &&
         record->launch_phase != SESSION_LAUNCH_COMMITTED))
        panic("session: shell launch token lost ownership");
    if (result == MG_OK) {
        if (record->launch_phase != SESSION_LAUNCH_COMMITTED ||
            record->launch_pid != launch.pid || !launch.child)
            panic("session: successful shell lacks commit permit");
        record->shell_pid = launch.pid;
    } else if (launch.child) {
        panic("session: failed launch retained an unpublished child");
    }
    record->launch_phase = SESSION_LAUNCH_NONE;
    record->launch_attempt = record->launch_thread = record->launch_pid = 0;
    if (record->state == MG_SESSION_STATE_ENDING) record->retirement_dirty = true;
    spin_unlock_irqrestore(&session_lock, flags);
    if (launch.child) process_handle_reference_release(&launch.child->object);
    process_cleanup_request(PROCESS_CLEANUP_SESSION_PROGRESS);
    if (result == MG_OK) *out_shell = launch.handle;
    return result;
}

int session_end_process(process_t *requester, mg_session_id_t session_id)
{
    if (!session_backend_authorized(requester))
        return MG_ERR_PRIVILEGE_REQUIRED;
    u64 flags = spin_lock_irqsave(&session_lock);
    session_record_t *record = session_find(session_id);
    if (!record) {
        spin_unlock_irqrestore(&session_lock, flags);
        return MG_ERR_NOT_FOUND;
    }
    bool changed = session_mark_ending_locked(record);
    spin_unlock_irqrestore(&session_lock, flags);
    if (changed) process_cleanup_request(PROCESS_CLEANUP_SESSION_PROGRESS);
    return MG_ERR_BUSY; /* The worker owns eventual completion. */
}

int session_query_process(process_t *requester, mg_session_id_t session_id,
                          mg_session_status_t *status)
{
    session_record_t *record;

    /* Clipboard policy may inspect session liveness, never create, list,
     * launch, or terminate sessions. Service identity is kernel assigned. */
    bool clipboard_reader = requester && requester->system_service &&
                            requester->service_id == MG_SERVICE_CLIPD;
    if (!status || (!session_backend_authorized(requester) && !clipboard_reader))
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
    if (!process) return;
    bool changed = false;
    u64 flags = spin_lock_irqsave(&session_lock);
    for (u32 i = 0; i < SESSION_MAX_ACTIVE; i++) {
        session_record_t *record = &session_records[i];
        bool backend_lost = process->system_service &&
                            process->service_id == MG_SERVICE_SESSIOND;
        if (record->active &&
            (record->owner_pid == process->pid || backend_lost))
            changed |= session_mark_ending_locked(record);
    }
    spin_unlock_irqrestore(&session_lock, flags);
    if (changed) process_cleanup_request(PROCESS_CLEANUP_SESSION_PROGRESS);
}
