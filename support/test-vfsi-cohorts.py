#!/usr/bin/env python3
"""Sender frontier identity, batching, pruning, failure and SDK compatibility."""
import os
from pathlib import Path
import re
import shlex
import sys
import subprocess
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("RSYNC_TEST_BINARY", Path(os.environ.get("TOOLDIR", REPO)) / "rsync"))
if "RSYNC" in os.environ and "SCRATCHDIR" in os.environ:
    sys.path.insert(0, str(REPO / "testsuite"))
    from rsyncfns import rsync_argv
    COMMAND = rsync_argv()
else:
    COMMAND = [str(BINARY)]


def sdk_include():
    if os.environ.get("VFSI_INCLUDE"):
        return Path(os.environ["VFSI_INCLUDE"])
    makefile = BINARY.parent / "Makefile"
    if not makefile.exists():
        return None
    match = re.search(r"^CPPFLAGS=(.*)$", makefile.read_text(), re.M)
    for token in shlex.split(match.group(1) if match else ""):
        if token.startswith("-I") and (Path(token[2:]) / "vfsi.h").exists():
            return Path(token[2:])
    return None


class Cohorts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        include = sdk_include()
        config = BINARY.parent / "config.h"
        if not config.exists() or not re.search(r"^#define SUPPORT_VFSI 1$", config.read_text(), re.M):
            raise unittest.SkipTest("this rsync build does not enable VFSI")
        if not include or "VFSI_DIRECTORY_CURSORS_VERSION" not in (include / "vfsi.h").read_text():
            raise unittest.SkipTest("build with the optional retained-cursor SDK to exercise cohort traversal")
        cls.build = tempfile.TemporaryDirectory(prefix="rsync-cohort-mock-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.library = Path(cls.build.name) / "cohort.so"
        cls.legacy = Path(cls.build.name) / "legacy.so"
        cls.cursor_only = Path(cls.build.name) / "cursor-only.so"
        cls.incomplete = []
        cls.incomplete_no_legacy = []
        variants = [(cls.library, []), (cls.legacy, ["-DVFSI_TEST_OLD_LIBRARY"]),
                    (cls.cursor_only, ["-DVFSI_TEST_NO_LEGACY"])]
        for missing in ("CURSOR_FREE", "CURSOR_LISTING"):
            partial = Path(cls.build.name) / (missing + ".so")
            no_legacy = Path(cls.build.name) / (missing + "-no-legacy.so")
            cls.incomplete.append(partial)
            cls.incomplete_no_legacy.append(no_legacy)
            variants.extend([(partial, ["-DVFSI_TEST_NO_" + missing]),
                             (no_legacy, ["-DVFSI_TEST_NO_" + missing, "-DVFSI_TEST_NO_LEGACY"])])
        for output, flags in variants:
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(include), *flags, str(REPO / "support/vfsi-cohort-mock.c"), "-o", str(output)], check=True)

    def fixture(self, root):
        source = root / "source with spaces"
        source.mkdir()
        (source / "excluded").mkdir()
        (source / "empty").mkdir()
        (source / "excluded/hidden").write_text("excluded")
        (source / ".rsync-filter").write_text("- blocked/\n")
        for index in range(52):
            directory = source / f"d-{index:02}"
            (directory / "sub").mkdir(parents=True)
            (directory / "leaf").write_text(f"data {index}\n")
            (directory / "sub/deep").write_text("deep\n")
            (directory / "blocked").mkdir()
            (directory / "blocked/hidden").write_text("excluded by merge filter")
        deep = source / "d-00"
        for _ in range(20):
            deep /= "level"
            deep.mkdir()
        (deep / "end").write_text("end\n")
        (source / "link").symlink_to("d-00/leaf")
        return source

    def snapshot(self, root):
        out = []
        for path in sorted(root.rglob("*")):
            relative = path.relative_to(root).as_posix()
            if path.is_symlink():
                out.append((relative, "link", os.readlink(path)))
            elif path.is_dir():
                out.append((relative, "dir", path.stat().st_mode & 0o7777))
            else:
                out.append((relative, path.read_bytes(), path.stat().st_mode & 0o7777))
        return out

    def run_scan(self, root, source, destination, incremental, library=None, fault=None, extra=(), dry=False):
        destination.mkdir(exist_ok=True)
        if dry:
            # Directory update lines must compare identical initial metadata,
            # even if the two scans straddle a wall-clock second.
            os.utime(destination, (1_600_000_000, 1_600_000_000))
        env = os.environ.copy()
        env.update(VFSI_IMPL="dummy" if library else "off", VFSI_ROOT=str(root), VFSI_MOUNT=str(root),
                   VFSI_TRACE="1", VFSI_TEST_LOG=str(root / "calls"))
        env.pop("VFSI_TEST_FAULT", None)
        if library:
            env["VFSI_LIBRARY"] = str(library)
        if fault:
            env["VFSI_TEST_FAULT"] = fault
        args = [*COMMAND, "-a", "--delete", "--exclude=excluded/", "--filter=dir-merge .rsync-filter"]
        if not incremental:
            args.append("--no-inc-recursive")
        if dry:
            args.extend(["--dry-run", "--out-format=%n"])
        result = subprocess.run([*args, *extra, str(source) + "/", str(destination) + "/"],
                                env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def check_case(self, incremental, library, fault=None, extra=()):
        with tempfile.TemporaryDirectory(prefix="rsync-frontier-") as temp:
            root = Path(temp)
            source = self.fixture(root)
            reference, vector = root / "reference", root / "vector"
            for directory in [reference, vector]:
                directory.mkdir()
                (directory / "stale").write_text("must be deleted")
            self.run_scan(root, source, reference, incremental, extra=extra)
            result = self.run_scan(root, source, vector, incremental, library, fault, extra)
            self.assertEqual(self.snapshot(reference), self.snapshot(vector))
            self.assertFalse((vector / "excluded").exists())
            self.assertFalse((vector / "d-01/blocked").exists())
            self.assertFalse((vector / "stale").exists())
            events = (root / "calls").read_text().splitlines() if (root / "calls").exists() else []
            for event in events:
                if event.startswith("FREE\t"):
                    self.assertEqual(event.split("\t")[1], "0", "cursor ownership leaked")
            return result, events

    def test_frontier_cohorts_retain_siblings_prune_and_release_cursors(self):
        for incremental in [True, False]:
            with self.subTest(incremental=incremental):
                result, events = self.check_case(incremental, self.library)
                batches = [tuple(map(int, line.split("\t")[1:3])) for line in events if line.startswith("BATCH\t")]
                self.assertTrue(any(count > 1 and count == anchored for count, anchored in batches), batches)
                dirs = [line.split("\t", 3)[3] for line in events if line.startswith("DIR\t")]
                self.assertEqual(len(dirs), len(set(dirs)), "unconsumed sibling cohorts were discarded and fetched twice")
                self.assertFalse(any(path.endswith(("/excluded", "/blocked")) for path in dirs), dirs)
                self.assertTrue(any(line.startswith("FREE\t") for line in events))
                self.assertIn("mode=cohort", result.stderr)

    def test_failed_or_invalid_cohorts_discard_partial_cache_before_fallback(self):
        for fault in ["partial", "invalid"]:
            for incremental in [True, False]:
                with self.subTest(fault=fault, incremental=incremental):
                    result, events = self.check_case(incremental, self.library, fault)
                    self.assertIn("using POSIX scan", result.stderr)
                    self.assertTrue(any(line.startswith("FREE\t0\t") for line in events))

    def test_missing_optional_symbols_preserve_legacy_v3_listing(self):
        for incremental in [True, False]:
            with self.subTest(incremental=incremental):
                result, events = self.check_case(incremental, self.legacy)
                self.assertIn("mode=paths", result.stderr)
                self.assertNotIn("mode=cohort", result.stderr)
                self.assertTrue(any(line.startswith("LEGACY\t") for line in events))

    def test_incomplete_optional_symbol_pair_keeps_legacy_listing(self):
        for library in self.incomplete:
            for incremental in (True, False):
                with self.subTest(library=library.name, incremental=incremental):
                    result, events = self.check_case(incremental, library)
                    self.assertIn("mode=paths", result.stderr)
                    self.assertNotIn("mode=cohort", result.stderr)
                    self.assertTrue(any(line.startswith("LEGACY\t") for line in events))
                    self.assertFalse(any(line.startswith("BATCH\t") for line in events))

    def test_incomplete_optional_symbol_pair_without_legacy_uses_posix(self):
        for library in self.incomplete_no_legacy:
            for incremental in (True, False):
                with self.subTest(library=library.name, incremental=incremental):
                    result, events = self.check_case(incremental, library)
                    self.assertNotIn("vfsi: listdirs completed", result.stderr)
                    self.assertFalse(events, "an unusable SDK must not open or enumerate directories")

    def test_cursor_only_library_does_not_require_the_legacy_listing_symbol(self):
        for incremental in [True, False]:
            with self.subTest(incremental=incremental):
                result, events = self.check_case(incremental, self.cursor_only)
                self.assertIn("mode=cohort", result.stderr)
                self.assertNotIn("using POSIX scan", result.stderr)
                self.assertTrue(any(line.startswith("BATCH\t") for line in events))

    def test_unsupported_hardlink_mode_stays_on_posix(self):
        result, events = self.check_case(True, self.library, extra=("--hard-links",))
        self.assertNotIn("vfsi: listdirs completed", result.stderr)
        self.assertFalse(events)

    def test_relative_duplicate_operands_keep_frontier_associations(self):
        for incremental in [True, False]:
            with tempfile.TemporaryDirectory(prefix="rsync-frontier-relative-") as temp:
                root = Path(temp)
                source = self.fixture(root)
                reference, vector = root / "ref", root / "dst"
                reference.mkdir()
                vector.mkdir()
                operands = [str(source) + "/./d-00/", str(source) + "/./d-01/", str(source) + "/./d-00/"]
                args = [*COMMAND, "-aR", "--delete"]
                if not incremental:
                    args.append("--no-inc-recursive")
                env = os.environ.copy()
                env.pop("VFSI_TEST_FAULT", None)
                env.update(VFSI_IMPL="off")
                ordinary = subprocess.run([*args, *operands, str(reference) + "/"], env=env, capture_output=True, text=True, timeout=120)
                self.assertEqual(ordinary.returncode, 0, ordinary.stderr)
                env.update(VFSI_IMPL="dummy", VFSI_ROOT=str(root), VFSI_MOUNT=str(root),
                           VFSI_LIBRARY=str(self.library), VFSI_TRACE="1", VFSI_TEST_LOG=str(root / "calls"))
                result = subprocess.run([*args, *operands, str(vector) + "/"], env=env, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.snapshot(reference), self.snapshot(vector))
                self.assertIn("mode=cohort", result.stderr)
                self.assertNotIn("using POSIX scan", result.stderr)
                self.assertTrue(all(line.split("\t")[1] == "0" for line in (root / "calls").read_text().splitlines() if line.startswith("FREE\t")))

    @unittest.skipUnless(sys.platform.startswith("linux"), "requires ELF memset interposition")
    def test_wide_frontier_does_not_repeatedly_clear_a_large_path_index(self):
        if b"__asan_init" in BINARY.read_bytes():
            self.skipTest("run memset accounting on a normal build; ASan interposes memory functions")
        with tempfile.TemporaryDirectory(prefix="rsync-cohort-wide-") as temp:
            root = Path(temp)
            probe = root / "clears.so"
            # Count actual bulk clearing in the sender without production hooks
            # or timing thresholds. Child processes append their own totals.
            code = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
static void *(*next_memset)(void *, int, size_t);
static unsigned long long cleared;
__attribute__((constructor)) static void initialize(void) {
    next_memset = dlsym(RTLD_NEXT, "memset");
}
void *memset(void *p, int value, size_t length) {
    if (value == 0 && length >= 65536)
        __atomic_fetch_add(&cleared, length, __ATOMIC_RELAXED);
    if (next_memset)
        return next_memset(p, value, length);
    volatile unsigned char *bytes = p;
    for (size_t i = 0; i < length; i++)
        bytes[i] = (unsigned char)value;
    return p;
}
__attribute__((destructor)) static void finish(void) {
    const char *path = getenv("VFSI_TEST_CLEAR_LOG");
    FILE *out = path ? fopen(path, "a") : NULL;
    if (out) { fprintf(out, "%llu\n", cleared); fclose(out); }
}
"""
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-shared", "-fPIC", "-O2",
                            "-fno-builtin", "-Wall", "-Wextra", "-Werror",
                            "-x", "c", "-", "-o", str(probe), "-ldl"], input=code, text=True, check=True)
            source = root / "source"
            source.mkdir()
            width = 2048
            for index in range(width):
                child = source / f"d-{index:04}"
                child.mkdir()
                if index % 2:
                    (child / "leaf").write_bytes(b"x")
            clear_log = root / "cleared"
            preload = os.pathsep.join(filter(None, [os.environ.get("LD_PRELOAD"), str(probe)]))
            for incremental in [True, False]:
                with self.subTest(incremental=incremental):
                    ordinary = self.run_scan(root, source, root / f"ref-{incremental}", incremental, dry=True)
                    clear_log.unlink(missing_ok=True)
                    with mock.patch.dict(os.environ, LD_PRELOAD=preload, VFSI_TEST_CLEAR_LOG=str(clear_log)):
                        vector = self.run_scan(root, source, root / f"dst-{incremental}",
                                               incremental, self.library, dry=True)
                    self.assertEqual(vector.stdout, ordinary.stdout)
                    self.assertIn("mode=cohort", vector.stderr)
                    self.assertNotIn("using POSIX scan", vector.stderr)
                    counts = clear_log.read_text().splitlines()
                    self.assertTrue(counts, "memset probe did not run")
                    self.assertLess(sum(map(int, counts)), width * 1024,
                                    "path-index clearing must scale with entries, not directories times peak width")

    def test_dry_run_output_matches_ordinary_sender(self):
        for incremental in [True, False]:
            with tempfile.TemporaryDirectory(prefix="rsync-frontier-dry-") as temp:
                root = Path(temp)
                source = self.fixture(root)
                ordinary = self.run_scan(root, source, root / "ref", incremental, dry=True)
                vector = self.run_scan(root, source, root / "dst", incremental, self.library, dry=True)
                self.assertEqual(vector.stdout, ordinary.stdout)
                self.assertIn("mode=cohort", vector.stderr)


class StatProjection(unittest.TestCase):
    def test_stat_timestamp_projection_preserves_seconds_and_nanoseconds(self):
        include = sdk_include()
        if not include or not (include / "vfsi.h").exists():
            self.skipTest("set VFSI_INCLUDE to an ABI-v3 SDK header directory")
        with tempfile.TemporaryDirectory(prefix="rsync-cohort-times-") as temp:
            root = Path(temp)
            helper = root / "attributes"
            path = root / "input"
            path.write_bytes(b"x")
            os.utime(path, ns=(1_600_000_000_123_456_789, 1_600_000_001_987_654_321))
            st = path.stat()
            expected = [value for ns in [st.st_atime_ns, st.st_mtime_ns, st.st_ctime_ns]
                        for value in divmod(ns, 1_000_000_000)]
            for features in [[], ["-D_POSIX_C_SOURCE=200809L"]]:
                with self.subTest(features=features):
                    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-I" + str(include), *features, "-DVFSI_TEST_ATTRIBUTES",
                                    str(REPO / "support/vfsi-cohort-mock.c"), "-o", str(helper)], check=True)
                    result = subprocess.run([str(helper), str(path)], capture_output=True, text=True, check=True)
                    self.assertEqual(list(map(int, result.stdout.split())), expected)

if __name__ == "__main__":
    unittest.main()
