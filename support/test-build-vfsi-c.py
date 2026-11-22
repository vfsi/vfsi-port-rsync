#!/usr/bin/env python3
"""Headers-only SDK preparation must verify the release without invoking Cargo."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest

RECIPE = Path(__file__).with_name("build-vfsi-c")


class HeaderPreparation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rsync-sdk-headers-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        script = RECIPE.read_text()
        version = re.search(r"^VFSI_C_VERSION=(.+)$", script, re.M).group(1)
        self.release = self.root / "cache" / f"vfsi-c-{version}"
        self.release.mkdir(parents=True)
        self.archive = self.release / f"vfsi-c-{version}.crate"
        self.manifest = f'[package]\nname="vfsi-c"\nversion="{version}"\n'.encode()
        with tarfile.open(self.archive, "w:gz") as archive:
            for name, data in [("include/vfsi.h", b"#define VFSI_ABI_VERSION 3\n"),
                               ("include/vfsi_dynamic.h", b"/* dynamic helper */\n"),
                               ("include/vfsi_posix.h", b"/* POSIX helper */\n"),
                               ("Cargo.lock", b"version = 4\n"),
                               ("build-sdk.py", b"import os, sys\nfrom pathlib import Path\nPath(os.environ['SDK_ARGS']).write_text(' '.join(sys.argv[1:]))\nos.execvp('cargo', ['cargo', '--version'])\n"),
                               ("Cargo.toml", self.manifest)]:
                entry = tarfile.TarInfo(f"vfsi-c-{version}/{name}")
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
        digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        script = re.sub(r"^VFSI_C_SHA256=.+$", "VFSI_C_SHA256=" + digest, script, flags=re.M)
        self.recipe = self.root / "support" / "build-vfsi-c"
        self.recipe.parent.mkdir()
        self.recipe.write_text(script)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        cargo = self.bin / "cargo"
        cargo.write_text('#!/bin/sh\nprintf called >"$CARGO_LOG"\nexit 79\n')
        cargo.chmod(0o755)
        self.log = self.root / "cargo-called"
        self.output = self.root / "outputs"
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        VFSI_BUILD_DIR=str(self.root / "cache"), GITHUB_OUTPUT=str(self.output),
                        CARGO_LOG=str(self.log), SDK_ARGS=str(self.root / "sdk-args"))
        self.env.pop("VFSI_SDK_SOURCE", None)

    def run_recipe(self, *args, **env):
        return subprocess.run(["sh", str(self.recipe), *args], env=dict(self.env, **env),
                              capture_output=True, text=True, timeout=15)

    def test_verified_headers_do_not_build_or_edit_the_manifest(self):
        for _ in range(2):
            result = self.run_recipe("--headers-only")
            self.assertEqual(result.returncode, 0, result.stderr)
        source = self.release / "source"
        self.assertEqual((source / "Cargo.toml").read_bytes(), self.manifest)
        self.assertTrue((source / "include/vfsi.h").exists())
        self.assertEqual(self.output.read_text().splitlines(),
                         ["include=" + str(source / "include")] * 2)
        self.assertFalse(self.log.exists())
        # The default route still builds rather than silently becoming header-only.
        result = self.run_recipe()
        self.assertEqual(result.returncode, 79, result.stderr)
        self.assertTrue(self.log.exists())
        args = (self.root / "sdk-args").read_text().split()
        self.assertIn("--registry-only", args)
        self.assertEqual(args[args.index("--prefix") + 1], str(self.release / "sdk"))
        self.assertEqual((source / "Cargo.toml").read_bytes(), self.manifest)

    def test_shared_cold_cache_prepares_headers_once(self):
        tar = self.bin / "tar"
        tar.write_text('#!/bin/sh\nprintf "tar\\n" >>"$TAR_LOG"\nsleep 1\nexec "$REAL_TAR" "$@"\n')
        tar.chmod(0o755)
        tar_log = self.root / "tar-called"
        env = {"REAL_TAR": shutil.which("tar"), "TAR_LOG": str(tar_log)}
        with ThreadPoolExecutor(max_workers=8) as pool:
            pending = [pool.submit(self.run_recipe, "--headers-only", **env)
                       for _ in range(8)]
            for task in pending:
                result = task.result()
                self.assertEqual(result.returncode, 0, result.stderr)
        source = self.release / "source"
        self.assertEqual(tar_log.read_text().splitlines(), ["tar"])
        self.assertEqual((source / "Cargo.toml").read_bytes(), self.manifest)
        self.assertEqual(self.output.read_text().splitlines(),
                         ["include=" + str(source / "include")] * len(pending))
        self.assertFalse(self.log.exists())

    def test_invalid_source_cache_is_replaced(self):
        source = self.release / "source"
        populated = self.root / "populated"
        result = self.run_recipe("--headers-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        source.rename(populated)

        for kind in ("regular file", "symlink", "dangling symlink"):
            with self.subTest(kind=kind):
                if kind == "regular file":
                    source.write_bytes(b"invalid cache")
                else:
                    target = populated if kind == "symlink" else self.root / "absent"
                    source.symlink_to(target, target_is_directory=True)
                result = self.run_recipe("--headers-only")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(source.is_dir())
                self.assertFalse(source.is_symlink())
                self.assertEqual((source / "Cargo.toml").read_bytes(), self.manifest)
                self.assertEqual((populated / "Cargo.toml").read_bytes(), self.manifest)
                shutil.rmtree(source)
        self.assertFalse(self.log.exists())

    def test_incomplete_published_cache_is_repaired(self):
        result = self.run_recipe("--headers-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        source = self.release / "source"
        for name in ("include/vfsi_dynamic.h", "include/vfsi_posix.h", "Cargo.lock"):
            with self.subTest(name=name):
                path = source / name
                original = path.read_bytes()
                path.unlink()
                result = self.run_recipe("--headers-only")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(path.read_bytes(), original)
        self.assertFalse(self.log.exists())

    def test_development_install_serializes_shared_prefix(self):
        source = self.root / "dev-sdk"
        source.mkdir()
        builder = source / "build-sdk.py"
        builder.write_text(
            "import sys, time\n"
            "from pathlib import Path\n"
            "prefix = Path(sys.argv[sys.argv.index('--prefix') + 1])\n"
            "prefix.mkdir(parents=True, exist_ok=True)\n"
            "running = prefix / 'install-running'\n"
            "running.touch(exist_ok=False)\n"
            "try:\n"
            "    time.sleep(0.3)\n"
            "finally:\n"
            "    running.unlink()\n"
        )
        with ThreadPoolExecutor(max_workers=4) as pool:
            tasks = [pool.submit(self.run_recipe, VFSI_SDK_SOURCE=str(source))
                     for _ in range(4)]
            for task in tasks:
                result = task.result()
                self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.release / "sdk-dev" / "install-running").exists())
        self.assertFalse((self.release / "source").exists())

    def test_bad_checksum_cannot_export_or_extract_headers(self):
        with self.archive.open("ab") as archive:
            archive.write(b"corrupt")
        result = self.run_recipe("--headers-only")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum mismatch", result.stderr)
        self.assertFalse((self.release / "source").exists())
        self.assertFalse(self.output.exists())
        self.assertFalse(self.log.exists())

    def test_local_headers_do_not_invoke_the_sdk_builder(self):
        source = self.root / "local-sdk"
        (source / "include").mkdir(parents=True)
        (source / "include/vfsi.h").write_text("#define VFSI_ABI_VERSION 3\n")
        for header in ("vfsi_dynamic.h", "vfsi_posix.h"):
            (source / "include" / header).touch()
        (source / "build-sdk.py").write_text("raise SystemExit(88)\n")
        result = self.run_recipe("--headers-only", VFSI_SDK_SOURCE=str(source))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_text(), "include=" + str(source / "include") + "\n")
        self.assertFalse(self.log.exists())

    def test_local_headers_reject_missing_helpers(self):
        source = self.root / "local-sdk"
        include = source / "include"
        include.mkdir(parents=True)
        for header in ("vfsi.h", "vfsi_dynamic.h", "vfsi_posix.h"):
            (include / header).touch()
        for header in ("vfsi_dynamic.h", "vfsi_posix.h"):
            with self.subTest(header=header):
                (include / header).unlink()
                result = self.run_recipe("--headers-only", VFSI_SDK_SOURCE=str(source))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"missing {header}", result.stderr)
                (include / header).touch()
        self.assertFalse(self.output.exists())
        self.assertFalse(self.log.exists())

    def test_unknown_options_fail_before_preparation(self):
        result = self.run_recipe("--unknown")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("usage:", result.stderr)
        self.assertFalse((self.release / "source").exists())
        self.assertFalse(self.output.exists())
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
