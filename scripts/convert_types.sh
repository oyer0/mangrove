#!/usr/bin/env bash
set -e

find . \( -name "*.c" -o -name "*.h" \) -print0 | while IFS= read -r -d '' file; do
    sed -i \
        -e 's/\buint8_t\b/u8/g' \
        -e 's/\buint16_t\b/u16/g' \
        -e 's/\buint32_t\b/u32/g' \
        -e 's/\buint64_t\b/u64/g' \
        -e 's/\bint8_t\b/i8/g' \
        -e 's/\bint16_t\b/i16/g' \
        -e 's/\bint32_t\b/i32/g' \
        -e 's/\bint64_t\b/i64/g' \
        -e 's/\bsize_t\b/usize/g' \
        "$file"
done

echo "Done."
