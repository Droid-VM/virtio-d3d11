import tempfile
from pathlib import Path
import unittest

from gen_dxvk_version import generate, version


class VersionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "dxvk"
        self.root.mkdir()
        (self.root / "version.h.in").write_text('#define DXVK_VERSION "@VCS_TAG@"\n')
        self.source = self.root / "subprojects/dxbc-spirv/dxbc/dxbc_signature.h"
        self.source.parent.mkdir(parents=True)
        self.source.write_text("original\n")
        self.output = Path(self.temporary.name) / "version.h"

    def generate(self):
        return generate(self.root, self.output, "arm64", "clang", "-O2")

    def test_unchanged_build_keeps_timestamp(self):
        self.assertTrue(self.generate())
        timestamp = self.output.stat().st_mtime_ns
        self.assertFalse(self.generate())
        self.assertEqual(timestamp, self.output.stat().st_mtime_ns)

    def test_nested_dirty_edits_each_invalidate(self):
        self.generate()
        previous = self.output.read_bytes()
        for text in ("first fix\n", "second fix\n"):
            self.source.write_text(text)
            self.assertTrue(self.generate())
            current = self.output.read_bytes()
            self.assertNotEqual(previous, current)
            previous = current

    def test_new_and_removed_sources_invalidate(self):
        original = version(self.root, "arm64", "clang", "-O2")
        added = self.source.with_name("new.cpp")
        added.write_text("new\n")
        self.assertNotEqual(original, version(self.root, "arm64", "clang", "-O2"))
        added.unlink()
        self.assertEqual(original, version(self.root, "arm64", "clang", "-O2"))

    def test_target_compiler_and_flags_invalidate(self):
        original = version(self.root, "arm64", "clang", "-O2")
        for args in (("x86", "clang", "-O2"), ("arm64", "clang-new", "-O2"),
                     ("arm64", "clang", "-O0")):
            self.assertNotEqual(original, version(self.root, *args))

    def test_outputs_and_documentation_do_not_invalidate(self):
        original = version(self.root, "arm64", "clang", "-O2")
        (self.root / "README.md").write_text("documentation\n")
        (self.root / "src").mkdir()
        (self.root / "src/temporary.o").write_bytes(b"object")
        self.assertEqual(original, version(self.root, "arm64", "clang", "-O2"))


if __name__ == "__main__":
    unittest.main()
