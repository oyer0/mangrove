#include <service.h>

#include <pass.h>
#include <mangrove_errors.h>
#include <mg/service.h>
#include <string.h>

/* The first service table is deliberately kernel-owned.  Sprout keeps the
 * matching lifecycle/restart policy table, but it cannot choose an
 * executable or grant itself a capability through the syscall ABI. */
static const kernel_service_definition_t service_definitions[] = {
    {MG_SERVICE_SPROUT, "sprout", "/core/sprout",
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_SERVICES), "sprout"},
    {MG_SERVICE_SESSIOND, "sessiond", "/core/sessiond",
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_SESSIONS), "session"},
    {MG_SERVICE_LOGIND, "logind", "/core/logind",
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_SESSIONS), NULL},
    {MG_SERVICE_NETWORKD, "networkd", "/core/networkd",
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_NETWORK) |
         IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_CONFIGURATION),
     "network"},
    {MG_SERVICE_DEVICED, "deviced", "/core/deviced",
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_DEVICES), "device"},
    {MG_SERVICE_VOLUMED, "volumed", "/core/volumed",
     IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_DEVICES) |
         IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_STORAGE), "volume"},
    {MG_SERVICE_LOGD, "logd", "/core/logd", 0, "log"},
};

bool service_definition_lookup(u32 id,
                               const kernel_service_definition_t **definition)
{
    if (!definition) return false;
    for (usize index = 0;
         index < sizeof(service_definitions) / sizeof(service_definitions[0]);
         index++) {
        if (service_definitions[index].id == id) {
            *definition = &service_definitions[index];
            return true;
        }
    }
    return false;
}

int service_authorize_control(process_t *sprout,
                              process_handle_t request_handle,
                              u32 operation, u32 service_id)
{
    const kernel_service_definition_t *definition;
    const char *suffix;
    const char *prefix;
    char description[128];
    usize prefix_length;
    usize suffix_length;
    usize name_length;

    if (!sprout || sprout != process_current() ||
        !sprout->system_service || sprout->service_id != MG_SERVICE_SPROUT ||
        operation == MG_SERVICE_OP_STATUS ||
        !service_definition_lookup(service_id, &definition) ||
        service_id == MG_SERVICE_SPROUT || !definition->name)
        return MG_ERR_PRIVILEGE_REQUIRED;

    switch (operation) {
        case MG_SERVICE_OP_START:
            prefix = "Start ";
            suffix = " service.";
            break;
        case MG_SERVICE_OP_STOP:
            prefix = "Stop ";
            suffix = " service.";
            break;
        case MG_SERVICE_OP_RESTART:
            prefix = "Restart ";
            suffix = " service.";
            break;
        case MG_SERVICE_OP_RELOAD:
            prefix = "Reload ";
            suffix = " service configuration.";
            break;
        default:
            return MG_ERR_BAD_ARGUMENT;
    }

    prefix_length = strlen(prefix);
    suffix_length = strlen(suffix);
    name_length = strlen(definition->name);
    if (prefix_length + name_length + suffix_length >= sizeof(description))
        return MG_ERR_BAD_ARGUMENT;
    memcpy(description, prefix, prefix_length);
    memcpy(description + prefix_length, definition->name, name_length);
    memcpy(description + prefix_length + name_length, suffix, suffix_length);
    description[prefix_length + name_length + suffix_length] = '\0';
    return pass_authorize_request(
        sprout, request_handle, IDENTITY_PRIVILEGE_MANAGE_SERVICES,
        description);
}
