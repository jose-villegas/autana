"""Shared lexer edge cases and deterministic self-test markdown."""
from datetime import datetime, timezone
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts/gates'))
sys.path.insert(0, str(ROOT / 'scripts/device'))
import c_comments
import device_report


class SharedContractsTests(unittest.TestCase):
    def test_lexer_literals_unterminated_and_offsets(self):
        source = '"escaped\\" // literal"; \'/\'; /* block\ntext */ x; // tail\n'
        comments = c_comments.scan('scratch.c', source)
        self.assertEqual([c.text for c in comments], ['block text', 'tail'])
        for mode in ('comments', 'code', 'spelled'):
            blanked = c_comments.blank_comments(source, mode=mode)
            self.assertEqual(len(blanked), len(source))
            self.assertEqual([i for i, ch in enumerate(blanked) if ch == '\n'],
                             [i for i, ch in enumerate(source) if ch == '\n'])
        self.assertIn('// literal', c_comments.blank_comments(source))
        self.assertNotIn('literal', c_comments.blank_comments(source, mode='code'))
        for source, expected in [('x /* unterminated', [('block', 2, 17)]),
                                 ('x // eof', [('line', 2, 8)]),
                                 ('"unterminated // string', [('literal', 0, 23)])]:
            self.assertEqual(list(c_comments.tokens(source)), expected)
        comments = c_comments.scan('scratch.c', '// one\n// two\nx; // three\n/* four */\n/* five */')
        self.assertEqual([c.text for c in comments], ['one two', 'three', 'four five'])
        self.assertEqual(c_comments.file_header('scratch.h', '#pragma once\n/* header */\nx;').text, 'header')

    def test_selftest_markdown_is_pinned(self):
        text = ':1:good:PASS\nf.c:2:bad:FAIL: broken\n:3:skip:IGNORE\nSELFTEST_COMPLETE failures=1 elapsed_ms=42\n'
        parsed = device_report.results(text)
        markdown, passed, failed = device_report.selftest_markdown(parsed, text, 'capture.log',
            datetime(2026, 1, 2, 3, 4, 5, tzinfo=timezone.utc))
        self.assertEqual((passed, failed), (1, 1))
        self.assertEqual(markdown, '''# Device Self-Test Results

Captured: 2026-01-02 03:04:05 UTC
Source: `capture.log`

**2 tests, 1 passed, 1 failed**, 42 ms total

## Failures

- **`bad`** (`f.c:2`)
  broken

<details>
<summary>All results (2 tests)</summary>

| Test | Result |
|---|---|
| `good` | PASS |
| `bad` | **FAIL** - broken |

</details>
''')
        for sentinel, warning in [('', 'No `SELFTEST_COMPLETE` line found'),
                                  ('SELFTEST_COMPLETE failures=2 elapsed_ms=42', 'parsing mismatch')]:
            markdown, _, _ = device_report.selftest_markdown(parsed, sentinel, 'capture.log',
                datetime(2026, 1, 2, tzinfo=timezone.utc))
            self.assertIn(warning, markdown)
