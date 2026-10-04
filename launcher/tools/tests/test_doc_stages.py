import importlib.util
from pathlib import Path

import pytest


PATH = Path(__file__).resolve().parents[1] / "render/doc_stages.py"
spec = importlib.util.spec_from_file_location("doc_stages", PATH)
stages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stages)


def test_capture_requires_matching_commit(tmp_path):
    capture = tmp_path / "capture.txt"
    capture.write_text("other-diag\nlite both cores: mean 42000us\n")
    with pytest.raises(ValueError, match="build"):
        stages.capture_rows(capture, "abcdef123456")


def test_capture_uses_existing_perf_parser(tmp_path):
    capture = tmp_path / "capture.txt"
    capture.write_text("abcdef123456-diag\nlite both cores: mean 42000us\n")
    assert stages.capture_rows(capture, "abcdef123456") == {"lite": 42.0}


def test_source_stamp_ignores_doc_refresh_but_not_recipe(tmp_path):
    (tmp_path / "launcher").mkdir()
    source = tmp_path / "launcher/recipe.toml"
    source.write_text("steps = 8")
    stamp = stages.source_stamp(tmp_path, [source])
    (tmp_path / "README.md").write_text("refreshed docs")
    assert stages.source_stamp(tmp_path, [source]) == stamp
    source.write_text("steps = 9")
    assert stages.source_stamp(tmp_path, [source]) != stamp


def test_smoke_cannot_publish_or_check():
    with pytest.raises(ValueError, match="smoke"):
        stages.validate_mode(smoke=True, check=True)


def test_board_rejects_missing_variants(tmp_path):
    capture = tmp_path / "capture.txt"
    capture.write_text("abcdef123456-diag\nlite both cores: mean 42000us\n")
    with pytest.raises(ValueError, match="missing"):
        stages.board_means([capture], "abcdef123456")


def test_board_pose_rows_require_complete_unique_samples(tmp_path):
    capture = tmp_path / "capture.txt"
    capture.write_text("atrium t=    0s clusters= 4 tris=5 | both cores: frame 51000us\n"
                       "atrium_lite t=    0s clusters= 4 tris=5 | both cores: frame 42000us\n")
    expected = [("sponza", 0), ("lite", 0)]
    assert stages.board_frames([capture], expected) == {("sponza", 0): 51.0, ("lite", 0): 42.0}
    with pytest.raises(ValueError, match="exactly once"):
        stages.board_frames([capture], [*expected, ("sponza", 5)])
    capture.write_text(capture.read_text() + capture.read_text().splitlines()[0] + "\n")
    with pytest.raises(ValueError, match="exactly once"):
        stages.board_frames([capture], expected)


def test_gpu_check_requires_saved_full_run(tmp_path):
    with pytest.raises(ValueError, match="no full run"):
        stages.check_gpu(tmp_path)


def test_board_rejects_a_different_render_size(tmp_path):
    capture = tmp_path / "capture.txt"
    capture.write_text("=== atrium FRAME COST (10 tris, 20 verts, 1 clusters, rendered 368x448) ===\n")
    with pytest.raises(ValueError, match="render size"):
        stages.capture_viewport(capture, (184, 224))


def test_gpu_check_rejects_missing_saved_artifact(tmp_path, monkeypatch):
    import json
    monkeypatch.setattr(stages, "current_stamp", lambda: "stamp")
    (tmp_path / "source.json").write_text(json.dumps({"source": "stamp", "files": {"tables/score.md": "0" * 64}}))
    with pytest.raises(ValueError, match="missing or altered"):
        stages.check_gpu(tmp_path)


def test_memory_guard_checks_limiting_windows_host(monkeypatch):
    read_text = Path.read_text
    exists = Path.exists
    monkeypatch.setattr(Path, "read_text", lambda self, *args, **kwargs:
                        "MemAvailable: 16777216 kB\n" if self == Path("/proc/meminfo")
                        else read_text(self, *args, **kwargs))
    monkeypatch.setattr(Path, "exists", lambda self:
                        True if self == Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")
                        else exists(self))
    monkeypatch.setattr(stages.subprocess, "check_output", lambda *args, **kwargs: b"3221225472\r\n")
    with pytest.raises(ValueError, match="Windows host is short by 3.00 GiB"):
        stages.memory_guard()


def test_memory_guard_checks_limiting_wsl(monkeypatch):
    read_text = Path.read_text
    exists = Path.exists
    monkeypatch.setattr(Path, "read_text", lambda self, *args, **kwargs:
                        "MemAvailable: 4194304 kB\n" if self == Path("/proc/meminfo")
                        else read_text(self, *args, **kwargs))
    monkeypatch.setattr(Path, "exists", lambda self:
                        True if self == Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")
                        else exists(self))
    monkeypatch.setattr(stages.subprocess, "check_output", lambda *args, **kwargs: b"17179869184\r\n")
    with pytest.raises(ValueError, match="WSL is short by 2.00 GiB"):
        stages.memory_guard()


def test_smoke_preserves_full_gpu_results(tmp_path, monkeypatch):
    result = tmp_path / "out/gpu/measurements.json"
    result.parent.mkdir(parents=True)
    result.write_text("full run")
    monkeypatch.setattr(stages, "RESULTS", tmp_path)
    monkeypatch.setattr(stages, "gpu", lambda *args: 0)
    assert stages.main(["--stage", "gpu", "--smoke"]) == 0
    assert result.read_text() == "full run"
