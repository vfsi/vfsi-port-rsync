# Vectorized filesystem scans

This branch can use the VFSI C ABI to build a sender's source file list with
bounded directory-frontier listings. Cursor-aware SDKs carry native directory cursors across rsync's admitted
sender frontier and batch sibling listings. NFS cursors retain the observed
parent filehandle; path-only backends keep strict no-follow descent.
The ordinary rsync code still
applies filters, constructs the file list, reads symlinks, and transfers file
data. VFSI is optional at build time and off by default at runtime.

## Build

This port targets VNFS 0.0.28 through the public `vfsi-c` 0.3.12 package and
VFSI C ABI v3. Fetch and build that release set from crates.io with:

```sh
support/build-vfsi-c
CPPFLAGS=-I"$PWD/build-vfsi-c/vfsi-c-0.3.12-vnfs-0.0.28/source/include" \
    ./configure --enable-vfsi
make
```

The helper verifies the published `vfsi-c` archive checksum, selects the
public VNFS 0.0.28 crate, rejects Git-sourced Cargo dependencies, and builds
the shared library below. It requires Cargo, curl, Python 3, tar, and
the native prerequisites listed by `vfsi-c` and its dependencies.

Use `support/build-vfsi-c --headers-only` to verify and extract the same
published headers without Cargo. The macOS CI job uses them to check native
stat seconds and nanoseconds independently of optional cursor symbols.

A build without `--enable-vfsi` contains no VFSI code or dependency. An
enabled build loads the shared library dynamically, so it also runs normally
when the environment variables below are absent.

For the optional cursor extension in a merged development checkout, build a
local SDK explicitly (the published baseline above remains supported):

```sh
VFSI_SDK_SOURCE="$HOME/vnfs02/bindings/c" VFSI_SDK_PROFILE=dev \
    support/build-vfsi-c
CPPFLAGS=-I"$PWD/build-vfsi-c/vfsi-c-0.3.12-vnfs-0.0.28/sdk-dev/include" \
    ./configure --enable-vfsi
make
```

Use the library path printed by the SDK builder. Both cursor symbols are
optional: a cursor-aware build also works with older ABI-v3 libraries through
ordinary bounded path listings.

The listing and cursor-free symbols are enabled only as a complete pair.
A library exposing just one uses legacy listings when available, otherwise
the adapter falls back to POSIX before opening a filesystem. Verify cursor
admission, filter pruning, cleanup and both partial-symbol cases with:

```sh
python3 support/test-vfsi-cohorts.py
VFSI_LIBRARY=/path/to/libvfsi_c.so sh support/test-vfsi-port.sh
```

## Run on NFS

```sh
VFSI_IMPL=nfs \
VFSI_LIBRARY="$PWD/build-vfsi-c/vfsi-c-0.3.12-vnfs-0.0.28/target/release/libvfsi_c.so" \
rsync -a /mounted/source/ /destination/
```

On Linux, the adapter discovers the source's NFS mount and retains its export,
protocol, port, and supported authentication settings. Explicit connection
overrides use `VFSI_HOST`, `VFSI_EXPORT`, and `VFSI_MOUNT`.
Set `VFSI_VERBOSE=1` to report how many entries and directories were cached.

The local test backend uses:

```sh
VFSI_IMPL=dummy \
VFSI_LIBRARY="$PWD/build-vfsi-c/vfsi-c-0.3.12-vnfs-0.0.28/target/release/libvfsi_c.so" \
VFSI_ROOT=/real/backend/root \
VFSI_MOUNT=/kernel/visible/root \
rsync -a /kernel/visible/root/ /destination/
```

## Semantics and fallback

The optimization is intentionally limited to ordinary, non-daemon sender
scans. Rsync uses its POSIX traversal when an option needs semantics that this
integration does not consume safely, including fake-super, symlink-following
modes, `--one-file-system`, `--hard-links`, daemon modules, and insecure-link
mode. Symlink and special-file metadata also falls back to the existing rsync
path one entry at a time.

Each bounded shallow listing is treated as a metadata snapshot. After rsync
admits a child directory through its filters, the adapter carries that child's
opaque cursor with the sender's directory record. Up to 24 admitted siblings
are listed together. Their snapshots survive descent into another subtree until
rsync consumes them; the active name/stat index is released after its names
have been copied into rsync's file list. Unused child cursors and completed
frontier records are freed, with the remaining frontier released at file-list
EOF. Cached sibling snapshots share aggregate entry/path limits across depths.

Rsync applies filters before admitting children, so excluded subtrees are not
prefetched. Older libraries keep one-directory path listings. Adapters without
complete bounded listings use POSIX enumeration, preserving `--delete` safety.
A VFSI open, listing, ABI, or attribute-validation failure disables acceleration
and uses the normal POSIX scan; partial rows from a failed cohort are discarded
before that fallback.
As with a normal rsync source walk, applications should avoid mutating the
source tree while its file list is being built.

## Frozen sender-scan benchmark

The [November 2 sender-scan baseline](https://github.com/vmingchen/vnfs/blob/main/docs/rsync-benchmark.md)
uses this published vnfs 0.0.28/vfsi-c 0.3.12 release set on the same NFS tree
for ordinary and VFSI scans. Output parity and fallback checks pass, but VFSI
sends more RPCs and is slower on that workload. The report includes native and
controlled-delay timings, wire captures and reproducible scripts. Those frozen
numbers describe the published baseline, not the cursor-aware development path.
