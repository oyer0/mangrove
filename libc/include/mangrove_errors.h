#pragma once

/* Stable Mangrove-native result values.  Zero and positive values are
 * successful results; all defined failures are negative. */
#define MG_OK                    0
#define MG_ERR_NOT_FOUND        (-2)
#define MG_ERR_INVALID_HANDLE   (-3)
#define MG_ERR_BAD_ARGUMENT     (-4)
#define MG_ERR_NOT_DIRECTORY    (-5)
#define MG_ERR_NO_MEMORY        (-6)
#define MG_ERR_UNSUPPORTED      (-7)
#define MG_ERR_ACCESS_DENIED    (-8)
#define MG_ERR_ALREADY_EXISTS   (-9)
#define MG_ERR_BUFFER_TOO_SMALL (-10)
#define MG_ERR_END_OF_FILE      (-11)
#define MG_ERR_IO               (-12)
#define MG_ERR_NOT_CHILD        (-13)
#define MG_ERR_BUSY             (-14)
#define MG_ERR_INVALID_EXEC     (-15)
#define MG_ERR_NOT_EMPTY        (-16)
#define MG_ERR_NETWORK_UNAVAILABLE (-17)
#define MG_ERR_TIMEOUT          (-18)
#define MG_ERR_CONNECTION_RESET (-19)
#define MG_ERR_CONNECTION_CLOSED (-20)
#define MG_ERR_WOULD_BLOCK      (-21)
#define MG_ERR_ADDRESS_IN_USE   (-22)
