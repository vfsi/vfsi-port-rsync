#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
library=${VFSI_LIBRARY:?set VFSI_LIBRARY to the development adapter}
fixture=$(mktemp -d /tmp/vfsi-rsync-port.XXXXXX)
trap 'rm -rf "${fixture:?}"' EXIT HUP INT TERM

check_listing() {
    grep -q 'vfsi: listdirs completed' "$fixture/trace"
    grep -q 'phase=collect cohort_application_entries=0' "$fixture/trace"
    if test "${VFSI_REQUIRE_CURSOR_API:-0}" = 1 &&
        ! grep -Eq 'vfsi: listdirs completed mode=cohort dirs=[0-9]+ cursors=[1-9][0-9]* ' "$fixture/trace"; then
        cat "$fixture/trace" >&2
        printf '%s\n' 'The published VFSI SDK must list descendants with retained cursors.' >&2
        exit 1
    fi
}

mkdir -p "$fixture/source/sub/deep" "$fixture/source/excluded" "$fixture/reference" "$fixture/vector"
printf 'a\n' > "$fixture/source/a"
printf 'b\n' > "$fixture/source/sub/b"
printf 'c\n' > "$fixture/source/sub/deep/c"
printf 'do not traverse\n' > "$fixture/source/excluded/hidden"
ln -s sub/b "$fixture/source/link"
VFSI_IMPL=off "$repo/rsync" -a --exclude=excluded/ "$fixture/source/" "$fixture/reference/"
VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$repo/rsync" -a --exclude=excluded/ "$fixture/source/" "$fixture/vector/" 2>"$fixture/trace"
check_listing
diff -r "$fixture/reference" "$fixture/vector"
test ! -e "$fixture/vector/excluded"
# Non-incremental recursion must also finish copying parent names before
# advancing the frontier; exercising it catches dangling cache pointers.
VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$repo/rsync" -a --no-inc-recursive --exclude=excluded/ "$fixture/source/" "$fixture/vector-no-inc/" 2>"$fixture/trace"
check_listing
diff -r "$fixture/reference" "$fixture/vector-no-inc"
printf '%s\n' 'Rsync port parity passed (incremental and non-incremental recursion).'

# Explicit lists retain rsync's kernel ownership walk.
printf 'sub/\n' >"$fixture/list"
VFSI_IMPL=off "$repo/rsync" -ar --files-from="$fixture/list" \
    "$fixture/source/" "$fixture/reference-list/"
VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$repo/rsync" -ar --files-from="$fixture/list" \
    "$fixture/source/" "$fixture/vector-list/" 2>"$fixture/trace"
diff -r "$fixture/reference-list" "$fixture/vector-list"
test -f "$fixture/vector-list/sub/deep/c"
if grep -q 'vfsi: listdirs completed' "$fixture/trace"; then exit 1; fi

# Runtime absence must retain the normal scan and must not claim acceleration.
VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/missing.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$repo/rsync" -a --exclude=excluded/ "$fixture/source/" "$fixture/vector-missing/" 2>"$fixture/trace"
diff -r "$fixture/reference" "$fixture/vector-missing"
grep -q 'vfsi: cannot load ' "$fixture/trace"
if grep -q 'vfsi: listdirs completed' "$fixture/trace"; then exit 1; fi

# Cursor-aware builds additionally exercise the sender frontier and injected failures.
python3 "$repo/support/test-vfsi-cohorts.py"
