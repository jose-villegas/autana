"""Shared assertions for generated text reports."""


def assert_contains(test, text, fragments):
    for fragment in fragments:
        with test.subTest(fragment=fragment):
            test.assertIn(fragment, text)
