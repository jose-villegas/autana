"""The mixer's frame-cost stage name comes from its define, and a missing define fails."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sound_mix_capture import mix_stage_names


class MixerNamesTests(unittest.TestCase):
    def test_define_is_owner_and_missing_define_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            main = tree / "launcher/main"
            (main / "sound").mkdir(parents=True)
            (main / "shell").mkdir()
            (main / "shell/shell_apps.c").write_text('FRAME_COST_END(began, "present");')
            header = main / "sound/sound_engine.h"
            header.write_text('#define SOUND_MIXER_FRAME_COST_NAME "renamed_mix"\n')
            self.assertEqual(mix_stage_names(tree), ["renamed_mix", "present"])
            header.write_text('#define RENAMED_FRAME_COST_NAME "renamed_mix"\n')
            with self.assertRaisesRegex(RuntimeError, "SOUND_MIXER_FRAME_COST_NAME"):
                mix_stage_names(tree)
