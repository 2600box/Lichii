#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class SwitchDiscovery(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self.bin = self.root / "mount/usr/bin"
        self.bin.mkdir(parents=True)
        self.config = self.root / "config/eden"
        self.config.mkdir(parents=True)
        source = self.root / "probe.cpp"
        source.write_text('''#include "yuzu/nx2_switches.h"
#include <iostream>
namespace Common::FS {
std::filesystem::path GetDataDirectory(const std::string&) {
    return std::getenv("XDG_CONFIG_HOME");
}
}
int main(int, char** argv) {
    LoadNx2Switches(argv[0]);
    const char* value = std::getenv("EDEN_NX2_PRELOAD_ALL_PIPELINES");
    std::cout << (value ? value : "unset");
}
''')
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"), str(source), "-o", str(self.bin / "lichii")], check=True)

    def tearDown(self):
        self.tmp.cleanup()

    def run_probe(self, executable, **env):
        clean = os.environ.copy()
        clean.pop("EDEN_NX2_PRELOAD_ALL_PIPELINES", None)
        clean.pop("APPIMAGE", None)
        clean.update(XDG_CONFIG_HOME=str(self.config.parent), **env)
        return subprocess.check_output([str(executable)], env=clean, cwd=self.root, text=True)

    def test_elf_path_symlink_and_precedence(self):
        (self.root / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=99\n")
        self.assertEqual(self.run_probe(self.bin / "lichii"), "unset")
        (self.bin / "nx2_switches.txt").write_text('EDEN_NX2_PRELOAD_ALL_PIPELINES="1"\n')
        linkdir = self.root / "links"
        linkdir.mkdir()
        (linkdir / "lichii").symlink_to(self.bin / "lichii")
        self.assertEqual(self.run_probe("lichii", PATH=str(linkdir)), "1")
        (self.config / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=2\n")
        self.assertEqual(self.run_probe(linkdir / "lichii"), "2")
        self.assertEqual(self.run_probe(linkdir / "lichii", EDEN_NX2_PRELOAD_ALL_PIPELINES="3"), "3")

    def test_appimage_external_overrides_internal(self):
        (self.bin / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=1\n")
        outer = self.root / "app"
        outer.mkdir()
        image = outer / "Lichii.AppImage"
        image.touch()
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(image)), "1")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(outer / "missing.AppImage")), "1")
        (outer / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=2\n")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(image)), "2")
        link = self.root / "link.AppImage"
        link.symlink_to(image)
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(link)), "2")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE="app/Lichii.AppImage"), "1")
        (self.config / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=3\n")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(image)), "3")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(image), EDEN_NX2_PRELOAD_ALL_PIPELINES="4"), "4")


if __name__ == "__main__":
    unittest.main()
