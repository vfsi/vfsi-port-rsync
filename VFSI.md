# Vectorized filesystem scans

This branch can use the VFSI C ABI to build a sender's source file list with
one recursive, vectorized directory operation. The ordinary rsync code still
applies filters, constructs the file list, reads symlinks, and transfers file
data. VFSI is optional at build time and off by default at runtime.

## Build

This port targets VNFS 0.0.11 through the public `vfsi-c` 0.3.0 package and
VFSI C ABI v3. Fetch and build that release set from crates.io with:

```sh
support/build-vfsi-c
CPPFLAGS=-I"$PWD/build-vfsi-c/vfsi-c-0.3.0-vnfs-0.0.11/source/include" \
    ./configure --enable-vfsi
make
```

The helper verifies the published `vfsi-c` archive checksum, selects the
public VNFS 0.0.11 crate, rejects Git-sourced Cargo dependencies, and builds
the shared library below. It requires Cargo, curl, patch, Python 3, tar, and
the native prerequisites listed by `vfsi-c` and its dependencies.

A build without `--enable-vfsi` contains no VFSI code or dependency. An
enabled build loads the shared library dynamically, so it also runs normally
when the environment variables below are absent.

## Run on NFS

```sh
VFSI_IMPL=nfs \
VFSI_LIBRARY="$PWD/build-vfsi-c/vfsi-c-0.3.0-vnfs-0.0.11/target/release/libvfsi_c.so" \
rsync -a /mounted/source/ /destination/
```

On Linux, rsync selects the longest matching NFS mount from
`/proc/self/mounts` and derives the server, export root, and local mountpoint.
They can be overridden with `VFSI_HOST`, `VFSI_EXPORT`, and `VFSI_MOUNT`.
Set `VFSI_VERBOSE=1` to report how many entries and directories were cached.

The local test backend uses:

```sh
VFSI_IMPL=dummy \
VFSI_LIBRARY="$PWD/build-vfsi-c/vfsi-c-0.3.0-vnfs-0.0.11/target/release/libvfsi_c.so" \
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

Each recursive VFSI result is treated as a metadata snapshot. Its directory
and path indexes live for one source file-list traversal, including all
incremental-recursion batches, and are freed at file-list EOF. A VFSI open,
listing, ABI, or attribute-validation failure disables the snapshot and uses
the normal POSIX scan; partial VFSI results are never mixed into that fallback.
As with a normal rsync source walk, applications should avoid mutating the
source tree while its file list is being built.
