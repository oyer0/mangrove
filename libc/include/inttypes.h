/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIi8 "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "li"
#define PRIo8 "hho"
#define PRIo16 "ho"
#define PRIo32 "o"
#define PRIo64 "lo"
#define PRIu8 "hhu"
#define PRIu16 "hu"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIx8 "hhx"
#define PRIx16 "hx"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIX8 "hhX"
#define PRIX16 "hX"
#define PRIX32 "X"
#define PRIX64 "lX"

#define __MG_PRI_LEAST(bits, conversion) PRI##conversion##bits
#define PRIdLEAST8 PRId8
#define PRIdLEAST16 PRId16
#define PRIdLEAST32 PRId32
#define PRIdLEAST64 PRId64
#define PRIiLEAST8 PRIi8
#define PRIiLEAST16 PRIi16
#define PRIiLEAST32 PRIi32
#define PRIiLEAST64 PRIi64
#define PRIoLEAST8 PRIo8
#define PRIoLEAST16 PRIo16
#define PRIoLEAST32 PRIo32
#define PRIoLEAST64 PRIo64
#define PRIuLEAST8 PRIu8
#define PRIuLEAST16 PRIu16
#define PRIuLEAST32 PRIu32
#define PRIuLEAST64 PRIu64
#define PRIxLEAST8 PRIx8
#define PRIxLEAST16 PRIx16
#define PRIxLEAST32 PRIx32
#define PRIxLEAST64 PRIx64
#define PRIXLEAST8 PRIX8
#define PRIXLEAST16 PRIX16
#define PRIXLEAST32 PRIX32
#define PRIXLEAST64 PRIX64

/* These format modifiers match GCC's actual fast typedefs on this target. */
#define PRIdFAST8 "hhd"
#define PRIiFAST8 "hhi"
#define PRIoFAST8 "hho"
#define PRIuFAST8 "hhu"
#define PRIxFAST8 "hhx"
#define PRIXFAST8 "hhX"
#define PRIdFAST16 "ld"
#define PRIiFAST16 "li"
#define PRIoFAST16 "lo"
#define PRIuFAST16 "lu"
#define PRIxFAST16 "lx"
#define PRIXFAST16 "lX"
#define PRIdFAST32 PRIdFAST16
#define PRIiFAST32 PRIiFAST16
#define PRIoFAST32 PRIoFAST16
#define PRIuFAST32 PRIuFAST16
#define PRIxFAST32 PRIxFAST16
#define PRIXFAST32 PRIXFAST16
#define PRIdFAST64 PRIdFAST16
#define PRIiFAST64 PRIiFAST16
#define PRIoFAST64 PRIoFAST16
#define PRIuFAST64 PRIuFAST16
#define PRIxFAST64 PRIxFAST16
#define PRIXFAST64 PRIXFAST16
#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIoMAX "lo"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define PRIXMAX "lX"
#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIoPTR "lo"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIXPTR "lX"

#define SCNd8 "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 "ld"
#define SCNi8 "hhi"
#define SCNi16 "hi"
#define SCNi32 "i"
#define SCNi64 "li"
#define SCNo8 "hho"
#define SCNo16 "ho"
#define SCNo32 "o"
#define SCNo64 "lo"
#define SCNu8 "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 "lu"
#define SCNx8 "hhx"
#define SCNx16 "hx"
#define SCNx32 "x"
#define SCNx64 "lx"
#define SCNdLEAST8 SCNd8
#define SCNdLEAST16 SCNd16
#define SCNdLEAST32 SCNd32
#define SCNdLEAST64 SCNd64
#define SCNiLEAST8 SCNi8
#define SCNiLEAST16 SCNi16
#define SCNiLEAST32 SCNi32
#define SCNiLEAST64 SCNi64
#define SCNoLEAST8 SCNo8
#define SCNoLEAST16 SCNo16
#define SCNoLEAST32 SCNo32
#define SCNoLEAST64 SCNo64
#define SCNuLEAST8 SCNu8
#define SCNuLEAST16 SCNu16
#define SCNuLEAST32 SCNu32
#define SCNuLEAST64 SCNu64
#define SCNxLEAST8 SCNx8
#define SCNxLEAST16 SCNx16
#define SCNxLEAST32 SCNx32
#define SCNxLEAST64 SCNx64
#define SCNdFAST8 "hhd"
#define SCNiFAST8 "hhi"
#define SCNoFAST8 "hho"
#define SCNuFAST8 "hhu"
#define SCNxFAST8 "hhx"
#define SCNdFAST16 "ld"
#define SCNiFAST16 "li"
#define SCNoFAST16 "lo"
#define SCNuFAST16 "lu"
#define SCNxFAST16 "lx"
#define SCNdFAST32 SCNdFAST16
#define SCNiFAST32 SCNiFAST16
#define SCNoFAST32 SCNoFAST16
#define SCNuFAST32 SCNuFAST16
#define SCNxFAST32 SCNxFAST16
#define SCNdFAST64 SCNdFAST16
#define SCNiFAST64 SCNiFAST16
#define SCNoFAST64 SCNoFAST16
#define SCNuFAST64 SCNuFAST16
#define SCNxFAST64 SCNxFAST16
#define SCNdMAX "ld"
#define SCNiMAX "li"
#define SCNoMAX "lo"
#define SCNuMAX "lu"
#define SCNxMAX "lx"
#define SCNdPTR "ld"
#define SCNiPTR "li"
#define SCNoPTR "lo"
#define SCNuPTR "lu"
#define SCNxPTR "lx"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct { intmax_t quot, rem; } imaxdiv_t;
intmax_t imaxabs(intmax_t value);
imaxdiv_t imaxdiv(intmax_t numerator, intmax_t denominator);
intmax_t strtoimax(const char *restrict text, char **restrict end, int base);
uintmax_t strtoumax(const char *restrict text, char **restrict end, int base);
#ifdef __cplusplus
}
#endif
