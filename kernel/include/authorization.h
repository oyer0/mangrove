#pragma once

#include <identity.h>

/* A confirmation belongs to the protected operation, not to the caller's
 * presentation code.  The kernel derives the requester name from the process
 * object and consumes the answer before returning to the caller. */
#define AUTHORIZATION_MESSAGE_MAX 192U

int authorization_confirm_current(identity_privilege_t privilege,
                                  const char *description);
