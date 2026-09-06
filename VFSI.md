# Vectorized filesystem scans

This branch can use the VFSI C ABI to build a sender's source file list with
one recursive, vectorized directory operation. The ordinary rsync code still
applies filters, constructs the file list, reads symlinks, and transfers file
data. VFSI is optional at build time and off by default at runtime.

## Build

Point the preprocessor at the `vfsi.h` supplied by vnfs and enable the module:

```sh
CPPFLAGS=-I/path/to/vnfs/vfsi-c/include ./configure --enable-vfsi
make
```

A build without `--enable-vfsi` contains no VFSI code or dependency. An
enabled build loads the shared library dynamically, so it also runs normally
when the environment variables below are absent.

## Run on NFS

```sh
VFSI_IMPL=nfs \
VFSI_LIBRARY=/path/to/libvfsi_c.so \
rsync -a /mounted/source/ /destination/
```

On Linux, rsync selects the longest matching NFS mount from
`/proc/self/mounts` and derives the server, export root, and local mountpoint.
They can be overridden with `VFSI_HOST`, `VFSI_EXPORT`, and `VFSI_MOUNT`.
Set `VFSI_VERBOSE=1` to report how many entries and directories were cached.

The local test backend uses:

```sh
VFSI_IMPL=dummy \
VFSI_LIBRARY=/path/to/libvfsi_c.so \
VFSI_ROOT=/real/backend/root \
VFSI_MOUNT=/kernel/visible/root \
rsync -a /kernel/visible/root/ /destination/
```

## Semantics and fallback

The optimization is intentionally limited to ordinary, non-daemon sender
scans. Rsync uses its POSIX traversal when an option needs semantics that VFSI
ABI v2 directory attributes cannot reproduce, including fake-super,
symlink-following modes, `--one-file-system`, `--hard-links`, daemon modules,
and insecure-link mode. Symlink and special-file metadata also falls back to
the existing rsync path one entry at a time.

Each recursive VFSI result is treated as a metadata snapshot. Its directory
and path indexes live for one source file-list traversal, including all
incremental-recursion batches, and are freed at file-list EOF. A VFSI open,
listing, ABI, or attribute-validation failure disables the snapshot and uses
the normal POSIX scan; partial VFSI results are never mixed into that fallback.
As with a normal rsync source walk, applications should avoid mutating the
source tree while its file list is being built.
