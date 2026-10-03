#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
library=${VFSI_LIBRARY:?set VFSI_LIBRARY to the development adapter}
fixture=$(mktemp -d /tmp/vfsi-rsync-port.XXXXXX)
trap 'rm -rf "${fixture:?}"' EXIT HUP INT TERM
mkdir -p "$fixture/source/sub/deep" "$fixture/source/excluded" "$fixture/reference" "$fixture/vector"
printf 'a\n' > "$fixture/source/a"
printf 'b\n' > "$fixture/source/sub/b"
printf 'c\n' > "$fixture/source/sub/deep/c"
printf 'do not traverse\n' > "$fixture/source/excluded/hidden"
ln -s sub/b "$fixture/source/link"
"$repo/rsync" -a --exclude=excluded/ "$fixture/source/" "$fixture/reference/"
VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$repo/rsync" -a --exclude=excluded/ "$fixture/source/" "$fixture/vector/"
diff -r "$fixture/reference" "$fixture/vector"
test ! -e "$fixture/vector/excluded"
# Non-incremental recursion must also finish copying parent names before
# advancing the frontier; exercising it catches dangling cache pointers.
VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$repo/rsync" -a --no-inc-recursive --exclude=excluded/ "$fixture/source/" "$fixture/vector/"
diff -r "$fixture/reference" "$fixture/vector"
printf '%s\n' 'Rsync port parity passed (incremental and non-incremental recursion).'
