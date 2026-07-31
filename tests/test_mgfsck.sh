#!/bin/sh
set -eu

img=$(mktemp /tmp/mgfsck-test.XXXXXX)
trap 'rm -f "$img" "$img.bad" "$img.base" "$img.populated" "$img.out"' EXIT

rm -f "$img"
build/mkmgfs --blocks 4096 \
  --uuid 00000000-0000-0000-0000-000000000031 \
  --format-time-ns 0 "$img"
build/mgfsck "$img"

cp "$img" "$img.bad"
printf 'X' | dd of="$img.bad" bs=1 seek=0 conv=notrunc status=none
if build/mgfsck "$img.bad" >/tmp/mgfsck-bad.out 2>&1; then
    echo "mgfsck accepted a bad magic" >&2
    exit 1
fi
grep -q "error:" /tmp/mgfsck-bad.out
rm -f /tmp/mgfsck-bad.out

build/mkmgfs --blocks 16384 \
  --uuid 00000000-0000-0000-0000-000000000032 \
  --format-time-ns 0 "$img.base"
python3 tests/mgfs_fixture.py "$img.base" "$img.populated"
cp "$img.populated" "$img"

expect_bad() {
    mode=$1
    python3 tests/mgfsck_name_fixture.py "$img" "$mode"
    if build/mgfsck "$img" >"$img.out" 2>&1; then
        echo "mgfsck accepted $mode" >&2
        cat "$img.out" >&2
        exit 1
    fi
    cat "$img.out"
    cp "$img.populated" "$img"
}

expect_bad duplicate
grep -q 'duplicate live name' "$img.out"
grep -q 'offsets 0 and 48' "$img.out"
grep -q 'Records 3 and 4' "$img.out"

python3 tests/mgfsck_name_fixture.py "$img" live-tombstone
build/mgfsck "$img" >"$img.out" 2>&1
if grep -q 'duplicate live name' "$img.out"; then
    echo "mgfsck reported a tombstoned duplicate as live" >&2
    exit 1
fi
cp "$img.populated" "$img"

python3 tests/mgfsck_name_fixture.py "$img" tombstone-tombstone
build/mgfsck "$img" >"$img.out" 2>&1
if grep -q 'duplicate live name' "$img.out"; then
    echo "mgfsck reported tombstoned duplicates as live" >&2
    exit 1
fi
cp "$img.populated" "$img"

expect_bad bad-length
grep -q 'directory Record 1 has malformed entry at offset 48' "$img.out"
expect_bad truncated
grep -q 'directory Record 1 has malformed entry at offset 48' "$img.out"

for stream_mode in multi-direct extent-list boundary; do
    cp "$img.populated" "$img"
    python3 tests/mgfsck_stream_fixture.py "$img" "$stream_mode"
    if ! build/mgfsck "$img" >"$img.out" 2>&1; then
        echo "mgfsck rejected valid $stream_mode directory" >&2
        cat "$img.out" >&2
        exit 1
    fi
done

expect_stream_bad() {
    mode=$1
    cp "$img.populated" "$img"
    python3 tests/mgfsck_stream_fixture.py "$img" "$mode"
    if build/mgfsck "$img" >"$img.out" 2>&1; then
        echo "mgfsck accepted malformed $mode directory" >&2
        exit 1
    fi
    cat "$img.out"
}

expect_stream_bad bad-size
expect_stream_bad truncated-later
expect_stream_bad bad-list
expect_stream_bad duplicate-later

expect_graph_bad() {
    mode=$1
    cp "$img.populated" "$img"
    python3 tests/mgfsck_graph_fixture.py "$img" "$mode"
    if build/mgfsck "$img" >"$img.out" 2>&1; then
        echo "mgfsck accepted graph corruption $mode" >&2
        exit 1
    fi
    cat "$img.out"
}

for graph_mode in self two-cycle long-cycle root-cycle multi-parent same-parent regular-twice unreachable-cycle; do
    expect_graph_bad "$graph_mode"
done

cp "$img.populated" "$img"
python3 tests/mgfsck_graph_fixture.py "$img" deep
build/mgfsck "$img" >"$img.out" 2>&1
