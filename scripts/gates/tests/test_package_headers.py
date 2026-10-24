"""Each package README's header table lists its include/ headers with their purposes, and its
named check fails a stale table without writing it."""
import contextlib
import io
import pathlib
import shlex
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "launcher/tools/gen"))
from package_headers import CHECK, package_headers, update
from check_doc_generated import run_check
from gate_tree import write
from generated_blocks import block_owners, check_commands


class PackageHeaders(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)

    def package(self, readme="# p\n<!-- generated: package-p-headers -->\n<!-- /generated: package-p-headers -->\n"):
        write(self.root, "launcher/packages/p/README.md", readme)
        write(self.root, "launcher/packages/p/include/p/a/x.h", "/* The x header. */\nint x(void);\n")
        write(self.root, "launcher/packages/p/include/p/y.h", "/* The y header. */\nint y(void);\n")
        write(self.root, "launcher/packages/p/tests/helper.h", "/* A test helper. */\n")
        write(self.root, "launcher/packages/p/src/z.h", "/* A private header. */\n")
        return self.root / "launcher/packages/p/README.md"

    def test_a_package_readme_lists_every_include_header_and_only_those(self):
        readme = self.package()
        rows = package_headers(self.root)
        self.assertEqual(["launcher/packages/p/README.md"], list(rows))
        self.assertEqual(["include/p/a/x.h", "include/p/y.h"], [row[0] for row in rows["launcher/packages/p/README.md"]])
        self.assertEqual(0, update(self.root))
        text = readme.read_text(encoding="utf-8")
        self.assertIn("[include/p/a/x.h](include/p/a/x.h) | The x header.", text)
        self.assertIn("[include/p/y.h](include/p/y.h) | The y header.", text)
        self.assertNotIn("helper.h", text)
        self.assertNotIn("z.h", text)

    def test_package_header_blocks_have_unique_names(self):
        self.package()
        write(self.root, "launcher/packages/name_hash/README.md",
              "<!-- generated: package-name-hash-headers -->\n"
              "<!-- /generated: package-name-hash-headers -->\n")
        write(self.root, "launcher/packages/name_hash/include/name_hash/name_hash.h",
              "/* Name keys. */\nint name_hash(void);\n")
        self.assertEqual(0, update(self.root))
        owners = block_owners(self.root)
        self.assertEqual("launcher/packages/p/README.md", owners["package-p-headers"])
        self.assertEqual("launcher/packages/name_hash/README.md", owners["package-name-hash-headers"])
        self.assertEqual(0, update(self.root, check=True))

    def test_stale_check_preserves_crlf(self):
        readme = self.package()
        readme.write_bytes(readme.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        self.assertEqual(0, update(self.root))
        before = readme.read_bytes()
        write(self.root, "launcher/packages/p/include/p/y.h", "/* The changed y header. */\nint y(void);\n")
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(1, update(self.root, check=True))
        self.assertIn("launcher/packages/p/README.md", out.getvalue())
        self.assertEqual(before, readme.read_bytes())
        self.assertNotIn(b"\n", before.replace(b"\r\n", b""))

    def test_the_named_check_command_fails_a_stale_table_and_writes_nothing(self):
        # The command the marker names, run as the generated-document gate runs it.
        readme = self.package()
        update(self.root)
        self.assertEqual({"package-p-headers": CHECK}, check_commands(readme.read_text(encoding="utf-8")))
        command = f"{CHECK} --root {shlex.quote(self.root.as_posix())}"
        self.assertIsNone(run_check(REPO, command))
        readme.write_text(readme.read_text(encoding="utf-8").replace("The x header.", "Hand edit."), encoding="utf-8")
        before = readme.read_bytes()
        self.assertIsNotNone(run_check(REPO, command))
        self.assertEqual(before, readme.read_bytes())

    def test_a_package_readme_without_the_block_fails_loudly(self):
        self.package("# p\nno block here\n")
        with self.assertRaises(ValueError):
            update(self.root)


if __name__ == "__main__":
    unittest.main()
