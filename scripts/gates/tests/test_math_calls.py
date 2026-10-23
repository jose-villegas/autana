"""Call counting must retain the original TU through inline package wrappers."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class MathCallsTests(unittest.TestCase):
    def test_two_translation_units_and_disabled_build(self):
        cc = shutil.which("gcc") or shutil.which("clang")
        self.assertIsNotNone(cc, "host C compiler required")
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            for name, count in (("first", 6), ("second", 10)):
                (work / f"{name}.c").write_text(
                    '#include "math/linear/vec3f.h"\n'
                    f'float {name}(void) {{ float sum = 0; for (int i = 0; i < {count}; i++) '
                    '{ sum += vec3f_normalize((vec3f_t){1, 2, 3}).x; } return sum; }\n')
            (work / "main.c").write_text(
                '#include "math/scalar/math_calls.h"\n'
                'float first(void); float second(void);\n'
                'int main(void) { (void)first(); math_calls_begin(); '
                '(void)first(); (void)second(); math_calls_report(2); '
                'math_calls_begin(); (void)first(); math_calls_report(3); return 0; }\n')
            for enabled in (0, 1):
                with self.subTest(enabled=enabled):
                    binary = work / f"counter-{enabled}.exe"
                    command = [cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                               f"-DCONFIG_MATH_COUNT_CALLS={enabled}", "-I",
                               str(ROOT / "launcher/packages/math/include"),
                               *(str(work / f"{name}.c") for name in ("main", "first", "second")),
                               str(ROOT / "launcher/packages/math/src/math_calls.c"), "-lm", "-o", str(binary)]
                    built = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(built.returncode, 0, built.stderr)
                    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
                    if not enabled:
                        self.assertEqual(result, "")
                        self.assertNotIn(b"MATHCALLS", binary.read_bytes())
                    else:
                        windows = result.split("MATHCALLS begin\n")[1:]
                        self.assertEqual(len(windows), 2)
                        self.assertRegex(windows[0], r"fn=mathf_rsqrt caller=\S*first.c per_frame=3\n")
                        self.assertRegex(windows[0], r"fn=mathf_rsqrt caller=\S*second.c per_frame=5\n")
                        self.assertRegex(windows[1], r"fn=mathf_rsqrt caller=\S*first.c per_frame=2\n")
                        self.assertRegex(windows[1], r"fn=mathf_rsqrt caller=\S*second.c per_frame=0\n")


if __name__ == "__main__":
    unittest.main()
