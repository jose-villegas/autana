"""The CI step summary, the page GitHub shows under a step: each entry a paragraph of its own."""
import os


def summarize(text):
    """Adds `text` to $GITHUB_STEP_SUMMARY as a paragraph; outside a CI step there is none, and nothing is written."""
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
            summary.write(text + "\n\n")
