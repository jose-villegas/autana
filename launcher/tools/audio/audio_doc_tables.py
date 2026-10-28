"""Generate audio documentation tables from committed autana JSON replies.

python launcher/tools/audio/audio_doc_tables.py --write
python launcher/tools/audio/audio_doc_tables.py --check
"""
import argparse
import json
import math
from statistics import median
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from doc_tables import markdown
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define
from audio.sound_onset_capture import EVENTS as ONSET_EVENTS
from audio import sound_asset
from generated_blocks import replace_block, replace_blocks, blocks
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
from frame_cost import frame_cost_window, US_PER_MS

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audio_drc_sweep import AUDIO, DB_AMPLITUDE_SCALE, DRC_LEVEL_DB, TUNABLES, VOLUME_MAX

from audio_doc_blocks import caption, details, picture

DRC_CAPTURE = Path("docs/audio/data/audio_drc_capture.txt")
DRC_CLIPPED_DBFS = -0.5
CODEC_VOLUME_MIN_DB = read_define(AUDIO / "audio_out.c", "AUDIO_OUT_CODEC_VOLUME_MIN_DB")
CODEC_VOLUME_MAX_DB = read_define(AUDIO / "audio_out.c", "AUDIO_OUT_CODEC_VOLUME_MAX_DB")
CODEC_PA_SUPPLY_VOLTS = read_define(AUDIO / "audio_codec.c", "AUDIO_CODEC_PA_SUPPLY_VOLTS")
CODEC_DAC_SUPPLY_VOLTS = read_define(AUDIO / "audio_codec.c", "AUDIO_CODEC_DAC_SUPPLY_VOLTS")

CAPTURE = Path("docs/audio/data/audio_capture.txt")
DOCUMENT = Path("docs/audio/Audio-Path.md")
REGENERATE = "python launcher/tools/audio/audio_doc_tables.py --write"
CAPTURE_COMMANDS = (
    ('buildid',),
    ('heap',),
    ('tone', '300', '500'),
    ('tone', '500', '500', '--internal'),
    ('tone', '1000', '500', '--internal'),
    ('tone', '2000', '500', '--internal'),
    ('tone', '4000', '500', '--internal'),
    ('tone', '8000', '500', '--internal'),
    ('tone', '1000', '500'),
    ('tone', '3000', '500'),
    ('heap',),
    ('tone', '1000', '500', '1'),
    ('tone', '1000', '500', '3'),
    ('tone', '1000', '500', '10'),
    ('tone', '1000', '500', '30'),
    ('tone', '1000', '500', '100'),
    ('tone', '1000', '20'),
    ('tone', '1000', '50'),
    ('tone', '1000', '100'),
    ('tone', '1000', '200'),
    ('tone', '500', '500'),
    ('tone', '2000', '500'),
    ('tone', '4000', '500'),
    ('tone', '8000', '500'),
    ('heap',),
    ('buildid',),
)
DRC_DEFAULT = tuple(read_define(AUDIO / "audio_out.c", name) for name in (
    "AUDIO_OUT_CODEC_VOLUME", "AUDIO_OUT_DRC_ON", "AUDIO_OUT_DRC_MAX_LEVEL",
    "AUDIO_OUT_DRC_MIN_LEVEL", "AUDIO_OUT_DRC_WINSIZE", "AUDIO_OUT_DRC_RAMP"))
DRC_REFERENCE_CODEC = 50

DEFAULT_VOLUME = read_define(ROOT / "launcher/main/sound/console/console_tone_parse.h", "CONSOLE_TONE_DEFAULT_VOLUME_PCT")


def read_capture(path):
    lines = path.read_text(encoding="utf-8").splitlines()
    header = re.fullmatch(r"# autana --json replies on ([\w-]+), ([\d-]+),.*", lines[0])
    if not header:
        raise ValueError("missing board and date in capture header")
    records = [(command.split(), json.loads(reply))
               for command, reply in (line.split("\t", 1) for line in lines[1:] if line and not line.startswith("#"))]
    builds = {reply["build_id"] for command, reply in records if command == ["buildid"]}
    if len(builds) != 1:
        raise ValueError("capture must have one consistent build id")
    return header, records, builds


def path_data(path):
    _, records, _ = read_capture(path)
    heaps = [reply for command, reply in records if command == ["heap"]]
    tones = []
    internal = {}
    for command, reply in records:
        if command[:1] != ["tone"]:
            continue
        flags = [arg for arg in command[1:] if arg.startswith("--")]
        if any(flag not in ("--internal", "--json") for flag in flags) or len(flags) != len(set(flags)):
            raise ValueError("invalid tone capture flags")
        args = tuple(int(arg) for arg in command[1:] if not arg.startswith("--"))
        path_name = "internal" if "--internal" in flags else "acoustic"
        if reply.get("path", "acoustic") != path_name:
            raise ValueError("tone command and reply path disagree")
        if path_name == "internal":
            point = args if len(args) == 3 else (*args, DEFAULT_VOLUME)
            if point in internal:
                raise ValueError("duplicate internal tone point")
            internal[point] = reply
        else:
            tones.append((args, reply))
    if len(heaps) < 2 or len(tones) < 2:
        raise ValueError("capture needs idle/closed heaps and repeated tone opens")
    return heaps, tones, internal


def render(path, internal_required=False):
    heaps, tones, internal = path_data(path)
    idle, closed = heaps[0], heaps[-1]
    kept = idle["free"] - closed["free"]
    first = tones[0][1]["open_dma_bytes"]
    later = sorted({reply["open_dma_bytes"] for _, reply in tones[1:]})
    prefix = caption(path, REGENERATE, "replies")
    ram = [
        ("Idle (first heap)", idle["free"], "n/a", "n/a", idle["largest"]),
        ("Open (first tone)", idle["free"] - first, first, "n/a", "not captured"),
        ("Open (later tones)", "not captured", ", ".join(map(str, later)), "n/a", "not captured"),
        ("After close (last heap)", closed["free"], "n/a", kept, closed["largest"]),
    ]
    tables = {"audio-ram": prefix + markdown(
        ["State", "Free DMA heap (B)", "open_dma_bytes (B)", "Kept vs idle (B)", "Largest DMA block (B)"], ram)
        + "\nFirst-open free heap is idle minus `open_dma_bytes`; later opens report only the delta.\n"}
    for name, column, context, samples in sweep_data(tones):
        rows = [(value, f"{reply['tone_db']:.1f}", f"{reply['silence_db']:.1f}",
                 f"{reply['tone_db'] - reply['silence_db']:.1f}", "yes" if reply["heard"] else "no")
                for value, reply in sorted(samples)]
        table = markdown(
            [column, "Tone (dBFS)", "Silence (dBFS)", "Margin (dB)", "Heard"], rows)
        body = details(table) if name != "audio-duration" else table
        if name != "audio-duration":
            body = picture(f"audio/{name}", f"Microphone level vs {column.lower()}") + body
        tables[name] = prefix + context + "\n\n" + body
    if internal or internal_required:
        if not internal:
            raise ValueError("missing internal loopback records")
        settings = {args[1:] for args in internal}
        if len(settings) != 1:
            raise ValueError("paired loopback sweep must hold duration and volume constant")
        acoustic = {}
        for args, reply in tones:
            point = args if len(args) == 3 else (*args, DEFAULT_VOLUME)
            if point[1:] in settings:
                acoustic.setdefault(point, reply)
        if not internal.keys() <= acoustic.keys():
            raise ValueError("internal and acoustic frequency points must be paired")
        rows = []
        for args in sorted(internal):
            outside = acoustic[args]
            inside = internal[args]
            if any("hz_error" not in reply or "measured_hz" not in reply for reply in (outside, inside)):
                raise ValueError("paired loopback records need measured_hz and hz_error")
            rows.append((args[0], f"{outside['tone_db']:.1f}", f"{inside['tone_db']:.1f}",
                         "n/a" if outside['hz_error'] is None else f"{outside['hz_error']:+.2f}",
                         "n/a" if inside['hz_error'] is None else f"{inside['hz_error']:+.2f}"))
        duration_ms, volume_pct = next(iter(settings))
        context = f"Duration {duration_ms} ms; volume {volume_pct}%.\n\n"
        tables["audio-loopback"] = prefix + context + markdown(
            ["Frequency (Hz)", "Acoustic level (dBFS)", "Internal level (dBFS)",
             "Acoustic error (Hz)", "Internal error (Hz)"], rows)
    return tables


def sweep_data(tones):
    implicit = [(args, reply) for args, reply in tones if len(args) == 2]
    explicit = [(args, reply) for args, reply in tones if len(args) == 3]
    frequency_duration = max(args[1] for args, _ in implicit)
    volume_hz_ms = {args[:2] for args, _ in explicit}
    if len(volume_hz_ms) != 1:
        raise ValueError("volume sweep must hold frequency and duration constant")
    duration_hz, volume_ms = next(iter(volume_hz_ms))
    return (
        ("audio-frequency", "Frequency (Hz)", f"Duration {frequency_duration} ms; console default volume.",
         [(args[0], reply) for args, reply in implicit if args[1] == frequency_duration]),
        ("audio-volume", "Volume (%)", f"Frequency {duration_hz} Hz; duration {volume_ms} ms.",
         [(args[2], reply) for args, reply in explicit]),
        ("audio-duration", "Duration (ms)", f"Frequency {duration_hz} Hz; console default volume.",
         [(args[1], reply) for args, reply in implicit if args[0] == duration_hz]),
    )


MIX_CAPTURES = Path("docs/audio/data")
MIX_DOCUMENT = Path("docs/audio/Mixer-Cost.md")
ONSET_DOCUMENT = Path("docs/audio/Clicks.md")
PERCENT = 100


def mix_data(path):
    lines = path.read_text(encoding="utf-8").splitlines()
    header = re.fullmatch(r"# autana sound-mix capture on ([\w-]+), ([\d-]+),.*", lines[0])
    if not header:
        raise ValueError("missing sound-mix board and date")
    records = [line.split("\t", 1) for line in lines[1:] if line and not line.startswith("#")]
    builds = [json.loads(value)["build_id"] for key, value in records if key == "buildid"]
    if len(builds) != 2 or len(set(builds)) != 1:
        raise ValueError("capture needs two matching build ids")
    metadata = dict(records)
    for name in ("capacity", "voices", "period_us", "stage mix", "stage present", "window_seconds"):
        if name not in metadata:
            raise ValueError(f"missing {name} record")
    capacity = int(metadata["capacity"])
    voices = list(map(int, metadata["voices"].split(",")))
    period = float(metadata["period_us"])
    window_seconds = float(metadata["window_seconds"])
    sounds = {key.split()[1]: value for key, value in records if key.startswith("sound ")}
    if not sounds or not voices or min(voices) < 0 or capacity < max(voices) or period <= 0 or window_seconds <= 0:
        raise ValueError("invalid sound points, capacity, period or window seconds")
    if 0 not in voices:
        raise ValueError("missing zero-voice point")
    mix_stage, present_stage = metadata["stage mix"], metadata["stage present"]
    points = {}
    playing = {}
    dropped = {}
    stats = {}
    for key, value in records:
        if key.startswith("point "):
            _, sound, count = key.split()
            window = frame_cost_window(value)
            mix = window["external"].get(mix_stage) if window else None
            if mix is None or mix[2] <= 0:
                raise ValueError(f"invalid window: {key}")
            present = window["brackets"].get(present_stage)
            if present is None:
                raise ValueError(f"missing {present_stage} stage: {key}")
            if window["total"] is None:
                raise ValueError(f"missing total: {key}")
            points.setdefault((sound, int(count)), []).append(
                (mix[0] * US_PER_MS, mix[1] * US_PER_MS, mix[2], present[0], window["total"]))
        elif key.startswith("playing ") or key.startswith("dropped "):
            kind, sound, count = key.split()
            target = playing if kind == "playing" else dropped
            target.setdefault((sound, int(count)), []).append(int(value))
        elif key == f"stats {list(sounds)[-1]} {max(voices)}":
            stats.update(re.findall(r"(stack_min_free|margin|stack|voices|queue|mixer)=(\d+)", value))
    expected = {(sound, count) for sound in sounds for count in voices if count}
    if 0 in voices:
        expected.add((next(iter(sounds)), 0))
    if points.keys() != expected:
        raise ValueError("missing or unexpected point windows")
    for point in expected:
        if playing.get(point) != [point[1]]:
            raise ValueError(f"missing or mismatched playing record: {point}")
        drops = dropped.get(point, [])
        if len(drops) != 2 or drops[0] != drops[1]:
            raise ValueError(f"missing or changed dropped count: {point}")
    silent = points[(next(iter(sounds)), 0)]
    baseline = sum(w[3] for w in silent) / len(silent)
    rows = []
    for (sound, count), windows in points.items():
        blocks = sum(w[2] for w in windows)
        average = sum(w[0] * w[2] for w in windows) / blocks
        worst = max(w[1] for w in windows)
        present = sum(w[3] for w in windows) / len(windows)
        total = sum(w[4] for w in windows) / len(windows)
        rows.append((f"{sound} ({sounds[sound]})", count, f"{average:.2f}", f"{worst:.2f}",
                     f"{average / period * PERCENT:.2f}", f"{worst / period * PERCENT:.2f}", blocks,
                     f"{present:.2f}", f"{total:.2f}", f"{present - baseline:+.2f}"))
    fields = ["stack_min_free", "margin", "stack", "voices", "queue", "mixer"]
    if any(field not in stats for field in fields):
        raise ValueError("missing highest-point stats")
    return capacity, window_seconds, period, present_stage, rows, fields, stats


def render_mix(path):
    capacity, window_seconds, period, present_stage, rows, fields, stats = mix_data(path)
    context = (f"capacity **{capacity} voices**, window **{window_seconds:g} s**, "
               f"block period **{period:.2f} \u00b5s**.\n\n")
    table = markdown(
        ["Sound", "Voices", "Average \u00b5s", "Worst \u00b5s", "Core-1 load, average %", "Core-1 load, worst %", "Blocks",
         f"{present_stage} ms/frame", "Frame total ms/frame", f"{present_stage} change ms/frame"], rows)
    return (caption(path, REGENERATE) + context
            + picture(f"audio/{path.stem}", "Mixer cost vs voice count") + details(table)
            + "\nHighest-point memory (bytes):\n\n" + markdown(fields, [[stats[field] for field in fields]]))



def drc_data(records):
    if not records or records[0][0] != ["buildid"]:
        raise ValueError("DRC capture must start with buildid")
    state = {}
    groups = {}
    primary_hz = next((int(command[1]) for command, _ in records if command[:1] == ["tone"]), None)
    for command, reply in records:
        if command[:1] == ["tune"]:
            if command[1] == "reset":
                state.pop(command[2].removeprefix("audio."), None)
            else:
                name = command[1].removeprefix("audio.")
                if name not in TUNABLES:
                    raise ValueError("unknown DRC tunable")
                state[name] = int(command[2])
        elif command[:1] == ["tone"]:
            if state.keys() != set(TUNABLES):
                raise ValueError("incomplete DRC configuration")
            key = tuple(state[name] for name in TUNABLES)
            if (state["drc"] not in (0, 1) or not 0 <= state["codec_volume"] <= VOLUME_MAX
                    or any(not 0 <= state[name] < len(DRC_LEVEL_DB) for name in TUNABLES[2:])):
                raise ValueError("invalid DRC configuration")
            hz, ms, volume = map(int, command[1:4])
            if reply["hz"] != hz or ms <= 0 or reply.get("path", "acoustic") != "acoustic":
                raise ValueError("DRC tone command and reply disagree")
            for name in ("tone_db", "peak_dbfs", "thd_db"):
                if reply[name] is None and name == "thd_db":
                    continue
                if not isinstance(reply[name], (int, float)) or not math.isfinite(reply[name]):
                    raise ValueError("invalid DRC measurement")
            groups.setdefault(key, []).append((hz, ms, volume, reply))
    volumes = sorted({volume for samples in groups.values() for hz, _, volume, _ in samples if hz == primary_hz})
    points = {}
    for key, samples in groups.items():
        codec, on, maximum, minimum, window, ramp = key
        primary = [(volume, reply) for hz, _, volume, reply in samples if hz == primary_hz]
        if len({ms for _, ms, _, _ in samples}) != 1:
            raise ValueError("DRC duration must be constant per configuration")
        levels = []
        for volume in volumes:
            values = [reply["tone_db"] for gain, reply in primary if gain == volume]
            if not values:
                raise ValueError("missing DRC tone volume")
            levels.append(median(values))
        thd = [reply["thd_db"] for volume, reply in primary
               if volume == volumes[-1] and reply["thd_db"] is not None]
        peak = max(reply["peak_dbfs"] for _, _, _, reply in samples)
        points[key] = dict(levels=levels, thd=median(thd) if thd else None, peak=peak)
    if not points:
        raise ValueError("missing DRC tone records")
    return primary_hz, volumes, points


def render_drc(path):
    _, records, _ = read_capture(path)
    primary_hz, volumes, points = drc_data(records)
    rows = []
    summary = []
    for key, point in points.items():
        codec, on, maximum, minimum, window, ramp = key
        label = f"max {DRC_LEVEL_DB[maximum]:.1f} / min {DRC_LEVEL_DB[minimum]:.1f}" if on else "off"
        static_output = (CODEC_VOLUME_MIN_DB + codec / VOLUME_MAX * (CODEC_VOLUME_MAX_DB - CODEC_VOLUME_MIN_DB)
                         - DB_AMPLITUDE_SCALE * math.log10(CODEC_DAC_SUPPLY_VOLTS / CODEC_PA_SUPPLY_VOLTS))
        values = [*[f"{level:.1f}" for level in point["levels"]],
                  f"{point['thd']:.1f}" if point["thd"] is not None else "n/a"]
        rows.append((codec, f"{static_output:.1f}", label, window, ramp, *values, f"{point['peak']:.1f}",
                     "yes" if point["peak"] >= DRC_CLIPPED_DBFS else "no"))
        if key == DRC_DEFAULT or (codec == DRC_REFERENCE_CODEC and not on):
            summary.append(("Default" if on else f"Codec {codec}%, DRC off", *values))
    headings = [*[f"Level {volume}% (dBFS)" for volume in volumes], f"THD {volumes[-1]}% (dB)"]
    return (caption(path, REGENERATE, "replies")
            + f"Levels and THD: {primary_hz} Hz; worst peak includes every frequency.\n\n"
            + picture("audio/drc-level", "Microphone output against tone input")
            + picture("audio/drc-thd", "Full-scale distortion against codec volume")
            + markdown(["Setting", *headings], sorted(summary, reverse=True))
            + "\n<details>\n<summary>Per-configuration data</summary>\n\n"
            + markdown(["Codec (%)", "Static full-scale DAC output (dBFS)", "DRC max/min (dB)", "Window", "Ramp",
                        *headings, "Worst peak (dBFS)", "Mic clipped"], rows)
            + "\n</details>\n")



def update_drc(capture, document, check=False):
    body = render_drc(capture) if capture.exists() else "No capture yet.\n"
    return replace_block(document, "audio-drc", body, check=check)


def update(capture, document, check=False, mix_captures=None, mix_document=None, drc_capture=None):
    internal_required = "audio-loopback" in blocks(document.read_text(encoding="utf-8"))
    changed = replace_blocks(document, render(capture, internal_required=internal_required), check=check)
    if drc_capture is not None:
        changed |= update_drc(drc_capture, document, check)
    if mix_captures is not None:
        captures = sorted(Path(mix_captures).glob("sound_mix_capture*.txt"))
        if not captures:
            raise ValueError("no sound-mix capture files")
        body = "\n".join(render_mix(path) for path in captures)
        changed |= replace_block(mix_document, "sound-mix-cost", body, check=check)
    return changed


ONSET_REPLY = re.compile(
    r"SOUND onset event=([a-z-]+) hz=(\d+) gain=(\d+) pre_db=(-?\d+\.\d+) "
    r"peak_db=(-?\d+\.\d+) excess_db=(-?\d+\.\d+) at_ms=(\d+)")


def onset_data(path, arm_settings=None):
    lines = path.read_text(encoding="utf-8").splitlines()
    header = re.fullmatch(r"# autana sound-onset capture on ([\w:-]+), ([\d-]+),.*", lines[0])
    if not header:
        raise ValueError("missing onset board and date")
    records = [line.split("\t", 2) for line in lines[1:] if line and not line.startswith("#")]
    if any(len(record) != 3 for record in records):
        raise ValueError("onset records need arm, command and reply")
    builds = [json.loads(reply)["build_id"] for arm, command, reply in records if (arm, command) == ("meta", "buildid")]
    statuses = [json.loads(reply) for arm, command, reply in records if (arm, command) == ("meta", "status")]
    if len(builds) != 2 or len(set(builds)) != 1 or len(statuses) != 2:
        raise ValueError("onset capture needs status and two matching build ids")
    run_records = [int(reply) for arm, command, reply in records if (arm, command) == ("meta", "runs")]
    if len(run_records) != 1 or run_records[0] <= 0:
        raise ValueError("onset capture needs a positive run count")
    settings = {arm: json.loads(reply) for arm, command, reply in records if command == "settings"}
    if not settings:
        raise ValueError("onset capture needs arm settings")
    if arm_settings is not None:
        if arm_settings and list(settings.items()) != list(arm_settings.items()):
            raise ValueError("onset capture arms differ")
        arm_settings.update(settings)
    points = {}
    probe = set()
    failures = set()
    for arm, command, reply in records:
        if command in ("sound onset amp-off", "failure sound onset amp-off"):
            continue
        if command.startswith("failure sound onset "):
            event = command.removeprefix("failure sound onset ")
            if arm not in settings or event not in ONSET_EVENTS:
                raise ValueError("invalid onset failure arm or event")
            failures.add((arm, event))
            continue
        if not command.startswith("sound onset "):
            continue
        match = ONSET_REPLY.fullmatch(reply)
        if match is None or arm not in settings or match[1] not in ONSET_EVENTS or command != f"sound onset {match[1]}":
            raise ValueError("invalid onset reply or command")
        event, hz, gain, pre, peak, excess, at = match.groups()
        if not math.isclose(float(peak) - float(pre), float(excess), abs_tol=0.11):
            raise ValueError("inconsistent excess level")
        probe.add((hz, gain))
        points.setdefault((arm, event), []).append((float(excess), int(at)))
    expected = {(arm, event) for arm in settings for event in ONSET_EVENTS}
    if (points.keys() | failures != expected
            or any(len(points.get(key, [])) > run_records[0]
                   or (len(points.get(key, [])) < run_records[0] and key not in failures) for key in expected)
            or len(probe) > 1):
        raise ValueError("missing onset runs or inconsistent probe")
    return settings, points, run_records[0]


def render_onset(path, arm_settings=None):
    settings, points, runs = onset_data(path, arm_settings)
    columns = ["Event", *settings]
    rows = []
    for event in ONSET_EVENTS:
        row = [event]
        for arm in settings:
            values = points.get((arm, event), [])
            row.append(f"{median(v[0] for v in values):.1f} ({max(v[0] for v in values):.1f})"
                       + (f"; n={len(values)}" if len(values) < runs else "")
                       if values else "unavailable; n=0")
        rows.append(row)
    return (caption(path, REGENERATE, kind="replies")
            + details(markdown(columns, rows)))


def update_onset(captures, document, check=False):
    paths = sorted(Path(captures).glob("sound_onset_capture_*.txt"))
    if not paths:
        raise ValueError("no sound-onset capture files")
    arm_settings = {}
    body = picture("audio/sound-onset", "Median onset excess by de-click time constant")
    body += ("Lines show the median of capture medians; bands span their minimum to maximum.\n\n"
             "Numeric cells show median (worst) excess dB; run counts appear only when incomplete.\n\n")
    body += "\n".join(f"Capture {index}\n\n" + render_onset(path, arm_settings)
                      for index, path in enumerate(paths, 1))
    return replace_block(document, "sound-onset", body, check=check)


def render_storage():
    seconds_per_minute = 60
    frames = sound_asset.SAMPLE_RATE_HZ * seconds_per_minute
    block_frames = sound_asset.DEFAULT_BLOCK_FRAMES
    blocks = (frames + block_frames - 1) // block_frames
    return ("> Generated by: `" + REGENERATE + "`\n\n" + markdown(
        ["Format", "Payload bytes per minute", "Bytes per stored frame"],
        [("PCM16", frames * sound_asset.PCM_BYTES, str(sound_asset.PCM_BYTES)),
         ("IMA-ADPCM", blocks * sound_asset.block_bytes(block_frames),
          f"{sound_asset.block_bytes(block_frames)} / {block_frames} (whole blocks)")]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    for flag, default in (("--capture", CAPTURE), ("--document", DOCUMENT), ("--mix-captures", MIX_CAPTURES),
                          ("--mix-document", MIX_DOCUMENT), ("--onset-document", ONSET_DOCUMENT),
                          ("--drc-capture", DRC_CAPTURE)):
        parser.add_argument(flag, type=Path, default=ROOT / default)
    args = parser.parse_args()
    try:
        changed = update(args.capture, args.document, args.check, args.mix_captures, args.mix_document, args.drc_capture)
        changed |= update_onset(args.mix_captures, args.onset_document, args.check)
        changed |= replace_block(ROOT / "docs/audio/Sound-Assets.md", "sound-storage", render_storage(),
                                 check=args.check)
        from audio_buzz_doc import CAPTURE as BUZZ_CAPTURE, DOCUMENT as BUZZ_DOCUMENT, update as update_buzz
        changed |= update_buzz(ROOT / BUZZ_CAPTURE, ROOT / BUZZ_DOCUMENT, args.check)
    except (ValueError, KeyError, OSError, RuntimeError) as error:
        parser.exit(2, f"{error}\n")
    print(f"audio tables: {'stale' if changed and args.check else 'updated' if changed else 'current'}")
    return int(args.check and changed)


if __name__ == "__main__":
    raise SystemExit(main())
