import pathlib
import subprocess
import tempfile
import unittest


CHECKER = pathlib.Path(__file__).resolve().parents[1] / "check-pr-ticket.sh"

MESSAGE = (
    "PR description needs a line 'Ticket: <id>' naming its backlog ticket, the bare id "
    "without the autana- prefix (for example 'Ticket: cvpg' or 'Ticket: ems.12')."
)


def run_stdin(body):
    return subprocess.run(["sh", str(CHECKER)], input=body, capture_output=True, text=True)


class PrTicketTests(unittest.TestCase):
    def test_accepted(self):
        accepted = {
            "bare id": "Ticket: cvpg\n",
            "dotted id": "Ticket: ems.12\n",
            "prefixed id": "Ticket: autana-cvpg\n",
            "prefixed dotted id": "Ticket: autana-ems.12",
            "no space after colon": "Ticket:cvpg\n",
            "line among other text": "## Summary\n\nDoes a thing.\n\nTicket: cvpg\n\nMore text.\n",
            "crlf body": "Summary\r\n\r\nTicket: cvpg\r\n",
            "trailing blanks": "Ticket: cvpg  \n",
        }
        for label, body in accepted.items():
            with self.subTest(label=label):
                result = run_stdin(body)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejected(self):
        rejected = {
            "empty body": "",
            "missing line": "## Summary\n\nNothing about a ticket.\n",
            "inside other text": "See Ticket: cvpg for details\n",
            "text before the keyword": "Related Ticket: cvpg\n",
            "trailing text": "Ticket: cvpg and more\n",
            "wrong keyword": "Issue: cvpg\n",
            "lowercase keyword": "ticket: cvpg\n",
            "no id": "Ticket:\n",
            "uppercase id": "Ticket: CVPG\n",
            "leading dot": "Ticket: .12\n",
            "letters after dot": "Ticket: ems.ab\n",
            "hyphenated id": "Ticket: foo-bar\n",
            "indented": "  Ticket: cvpg\n",
            "two ids": "Ticket: cvpg ems\n",
        }
        for label, body in rejected.items():
            with self.subTest(label=label):
                result = run_stdin(body)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stderr.splitlines(), [MESSAGE])

    def test_reads_a_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "body.md"
            path.write_text("Ticket: cvpg\n")
            ok = subprocess.run(["sh", str(CHECKER), str(path)], capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            path.write_text("nothing\n")
            bad = subprocess.run(["sh", str(CHECKER), str(path)], capture_output=True, text=True)
            self.assertEqual(bad.returncode, 1)

    def test_usage_errors(self):
        missing = subprocess.run(["sh", str(CHECKER), "/nonexistent/body"], capture_output=True, text=True)
        self.assertEqual(missing.returncode, 2)
        extra = subprocess.run(["sh", str(CHECKER), "a", "b"], capture_output=True, text=True)
        self.assertEqual(extra.returncode, 2)


if __name__ == "__main__":
    unittest.main()
