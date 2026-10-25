"""Microphone DSP capture validation and documentation rows."""
import math
from statistics import median

from audio_doc_blocks import CLIPPED_DBFS, number
from audio_mic_dsp_sweep import DEFAULTS, GATE_DB, LEVEL_DB, PRIMARY_HZ, validate_config

SUMMARY_VOLUMES = (10, 30, 100)


def feature(config):
    if config["alc"]:
        return "ALC"
    if config["gate"]:
        return "Gate"
    if config != DEFAULTS:
        return "HPF"
    return "Baseline"


def label(config):
    kind = feature(config)
    if kind == "ALC":
        return (f"max/min {LEVEL_DB[config['alc_max']]:.1f}/{LEVEL_DB[config['alc_min']]:.1f} dB, "
                f"win {config['alc_win']}, gain 0x{config['alc_gain']:02X}")
    if kind == "Gate":
        return f"{GATE_DB[config['gate_ng']]} dB, win {config['gate_win']}, depth {config['gate_depth']}"
    if kind == "HPF":
        return f"s1/s2 {config['hpf_s1']}/{config['hpf_s2']}" if config["hpf"] else "frozen"
    return "defaults"


def mic_data(records):
    if not records or records[0][0] != ["buildid"]:
        raise ValueError("microphone capture must start with buildid")
    state, groups = {"eq": DEFAULTS["eq"]}, {}
    for command, reply in records:
        if command[:1] == ["tune"]:
            if command[1] == "reset":
                state.pop(command[2].removeprefix("mic."), None)
            else:
                if not command[1].startswith("mic."):
                    raise ValueError("unknown microphone tunable")
                state[command[1].removeprefix("mic.")] = int(command[2])
        elif command[:1] == ["tone"]:
            validate_config(state)
            hz, ms, volume = map(int, command[1:4])
            if reply["hz"] != hz or ms <= 0 or reply.get("path", "acoustic") != "acoustic":
                raise ValueError("microphone tone command and reply disagree")
            for field in ("tone_db", "noise_dbfs", "peak_dbfs", "thd_db"):
                value = reply[field]
                if value is None and field == "thd_db":
                    continue
                if not isinstance(value, (int, float)) or not math.isfinite(value):
                    raise ValueError("invalid microphone measurement")
            key = tuple(state[name] for name in DEFAULTS)
            groups.setdefault(key, []).append((hz, ms, volume, reply))
    if not groups:
        raise ValueError("missing microphone tone records")
    result = []
    for key, samples in groups.items():
        config = dict(zip(DEFAULTS, key))
        if len({ms for _, ms, _, _ in samples}) != 1:
            raise ValueError("microphone duration must be constant per configuration")
        points = {}
        for hz, _, volume, _ in samples:
            replies = [reply for frequency, _, gain, reply in samples if frequency == hz and gain == volume]
            points[hz, volume] = {field: median(reply[field] for reply in replies if reply[field] is not None)
                                  if any(reply[field] is not None for reply in replies) else None
                                  for field in ("tone_db", "noise_dbfs", "thd_db")}
        noise = median(reply["noise_dbfs"] for _, _, _, reply in samples)
        snr = median(reply["tone_db"] - reply["noise_dbfs"] for hz, _, _, reply in samples if hz == PRIMARY_HZ) if any(
            hz == PRIMARY_HZ for hz, _, _, _ in samples) else None
        result.append(dict(config=config, points=points, noise=noise, snr=snr,
                           peak=max(reply["peak_dbfs"] for _, _, _, reply in samples)))
    return result


def render(path, records, caption, picture, details, markdown, regenerate):
    points = mic_data(records)
    body = caption(path, regenerate, "replies")
    body += "Noise floor is broadband RMS after settling; SNR is the median paired 1 kHz tone minus that floor.\n\n"
    for stem, description in (("level", "ALC level against tone volume"), ("noise", "Noise floor by configuration"),
                              ("hpf", "HPF level against frequency")):
        body += picture("audio/mic-dsp-" + stem, description)
    headings = ["Config", "Noise floor (dBFS)", "SNR at 1 kHz (dB)",
                *[f"Level {volume}% (dBFS)" for volume in SUMMARY_VOLUMES], "THD 100% (dB)",
                "Worst peak (dBFS)", "Clipped"]
    for kind in ("Baseline", "HPF", "Gate", "ALC"):
        rows = []
        for point in points:
            if feature(point["config"]) != kind:
                continue
            values = [point["points"].get((PRIMARY_HZ, volume), {}).get("tone_db") for volume in SUMMARY_VOLUMES]
            thd = point["points"].get((PRIMARY_HZ, SUMMARY_VOLUMES[-1]), {}).get("thd_db")
            rows.append((label(point["config"]), number(point["noise"], digits=1), number(point["snr"], digits=1),
                         *[number(value, digits=1) for value in values], number(thd, digits=1), number(point["peak"], digits=1),
                         "yes" if point["peak"] >= CLIPPED_DBFS else "no"))
        body += f"### {kind}\n\n" + details(markdown(headings, rows)) + "\n"
    return body
