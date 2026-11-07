#!/usr/bin/env python3
"""Headers-only SDK preparation must verify the release without invoking Cargo."""
import hashlib
import io
import os
from pathlib import Path
import re
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
                              capture_output=True, text=True)

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
        (source / "build-sdk.py").write_text("raise SystemExit(88)\n")
        result = self.run_recipe("--headers-only", VFSI_SDK_SOURCE=str(source))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_text(), "include=" + str(source / "include") + "\n")
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
