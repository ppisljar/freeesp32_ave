#!/usr/bin/env python3
"""avmeasure -- did the device actually play what the .ledc prescribed?

Single CLI entry point:

  analyze.py analyze --ledc S.ledc --wav rec.wav --map "audioL=1,audioR=2,light1=3"
  analyze.py synth   --ledc S.ledc --out rec.wav [--fault ...]
  analyze.py expect  --ledc S.ledc [--dump]
  analyze.py lint    --ledc S.ledc

Run `analyze.py <command> --help` for the options of each.
Dependencies: python3 + numpy. Nothing else, on purpose -- this has to run on
a clean machine with no setup, at a bench, possibly offline.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time

import numpy as np

import devicemodel as dm
import ledc_expect as le
from timeline import Report

# ---------------------------------------------------------------------------
# Human-readable report
# ---------------------------------------------------------------------------

_SEV_TAG = {"error": "ERROR  ", "warning": "WARN   ", "info": "info   "}


def _fmt_t(ms: float | None) -> str:
    if ms is None or (isinstance(ms, float) and math.isnan(ms)):
        return "          "
    s = ms / 1000.0
    return f"{int(s) // 60:>3}:{s % 60:06.3f}"


def render_report(rep: Report, *, show_info: bool = True,
                  width: int = 92) -> str:
    L: list[str] = []
    bar = "=" * width
    L.append(bar)
    L.append("avmeasure report")
    L.append(bar)
    L.append(f"  .ledc        {rep.ledc}")
    L.append(f"  observation  {rep.observation}  (front-end: {rep.front_end})")
    L.append("")

    # --- sync first, always. A wrong t0 invalidates everything below. ---
    s = rep.sync
    L.append("-" * width)
    L.append("SYNC -- the recording's device t=0")
    L.append("-" * width)
    L.append(f"  t0            {s.t0_s:.4f} s into the recording")
    L.append(f"  method        {s.method}")
    L.append(f"  confidence    {s.confidence.upper()}  "
             f"(+/-{s.uncertainty_ms:.0f} ms)")
    # These are t0 restated per domain: "+1999.7 ms" is not a two-second lag,
    # it is where each domain puts device t=0 in the recording. Say so, and say
    # when a domain could not be measured at all -- a blank line there used to
    # read as "zero".
    if math.isfinite(s.light_lag_ms):
        L.append(f"  t0 from light {s.light_lag_ms:+.1f} ms into the recording "
                 f"(reference domain: LED anchors are immune to dispatch lag)")
    else:
        L.append("  t0 from light NOT MEASURED (no sharp optical activation "
                 "edge was found)")
    if math.isfinite(s.audio_lag_ms):
        L.append(f"  t0 from audio {s.audio_lag_ms:+.1f} ms into the recording")
    else:
        L.append("  t0 from audio NOT MEASURED (no entry makes a timeable "
                 "sound at its own timestamp)")
    if s.detail:
        for part in s.detail.split("; "):
            L.append(f"                {part}")
    if s.confidence in ("low", "none"):
        L.append("  !! A wrong t0 makes every timing number below meaningless.")
        L.append("     Add a sync marker to the .ledc (see README) and re-record.")
    L.append("")

    L.append("-" * width)
    L.append("GLOBAL OFFSET AND CLOCK")
    L.append("-" * width)
    if math.isfinite(rep.av_offset_ms):
        lo, hi = dm.AV_OFFSET_BAND_MS
        floor = max(dm.AV_OFFSET_FLOOR_MS, s.uncertainty_ms)
        verdict = ("OK" if lo <= rep.av_offset_ms <= hi
                   else ("AUDIO LEADS LIGHT -- always a bug"
                         if rep.av_offset_ms < -floor else
                         ("audio leads, but inside the measurement floor"
                          if rep.av_offset_ms < lo else "OUTSIDE BAND")))
        L.append(f"  A/V const offset {rep.av_offset_ms:+.1f} ms  "
                 f"(expected {lo:.0f}..{hi:.0f} ms)   {verdict}")
        # Print the ARITHMETIC. A careful reader subtracts the two origins above
        # to sanity-check this figure and gets a different answer, because the
        # modelled pipeline term is added silently. One line removes the doubt.
        pipe = float(rep.stats.get("av_pipeline_ms",
                                   dm.av_offset_pipeline_ms()))
        L.append(f"                   = {rep.av_offset_ms - pipe:+.1f} ms "
                 f"measured residual (audio origin - light origin) "
                 f"{pipe:+.1f} ms modelled pipeline")
        L.append("                   positive = audio lags light, which is the "
                 "healthy direction")
    else:
        # Say WHY. "need both a light sensor and an audio channel" sent people
        # to recheck a channel map that was already correct, when the real
        # reason was a session whose LEDs never flicker (a DC lamp is invisible
        # to an AC-coupled input) or a t0 too coarse to support the claim.
        reason = next((f.message for f in rep.findings
                       if f.code == "AV_NOT_MEASURABLE"), None)
        if reason:
            L.append("  A/V const offset NOT REPORTED -- see AV_NOT_MEASURABLE "
                     "below")
        elif not math.isfinite(s.light_lag_ms) and not math.isfinite(s.audio_lag_ms):
            L.append("  A/V const offset not measurable: neither domain yielded "
                     "an origin")
        else:
            L.append("  A/V const offset not measurable: only one of the two "
                     "domains yielded an origin (an offset is their difference)")
    if math.isfinite(rep.drift_common_ppm):
        L.append(f"  recorder clock   {rep.drift_common_ppm:+.0f} ppm "
                 f"(the component COMMON to audio and light -> capture "
                 f"interface, fitted out)")
    if math.isfinite(rep.drift_differential_ppm):
        L.append(f"  A/V diff. drift  {rep.drift_differential_ppm:+.0f} ppm "
                 f"(audio timebase minus light timebase; device-side if "
                 f"non-zero)")
    for k in ("clock_ppm_light", "clock_ppm_audio"):
        if k in rep.stats and math.isfinite(rep.stats[k]):
            L.append(f"    {k:<18} {rep.stats[k]:+.1f} ppm "
                     f"from {rep.stats.get(k.replace('clock_ppm', 'clock_n'), 0)} "
                     f"samples")
    L.append("")

    findings = [f for f in rep.sorted_findings()
                if show_info or f.severity != "info"]
    n_err = sum(1 for f in rep.findings if f.severity == "error")
    n_warn = sum(1 for f in rep.findings if f.severity == "warning")
    L.append("-" * width)
    L.append(f"FINDINGS -- {n_err} error(s), {n_warn} warning(s)")
    L.append("-" * width)
    if not findings:
        L.append("  Nothing to report: the recording matches the .ledc within "
                 "every threshold.")
    for f in findings:
        head = f"  [{_SEV_TAG.get(f.severity, f.severity)}] {_fmt_t(f.t_ms)} "
        tag = f.code
        if f.channel is not None:
            tag += f" ch{f.channel}"
        L.append(head + tag)
        for line in _wrap(f.message, width - 16):
            L.append("                            " + line)
        if f.t_end_ms is not None and f.t_end_ms != f.t_ms:
            L.append(f"                            "
                     f"window {_fmt_t(f.t_ms)} .. {_fmt_t(f.t_end_ms)}")
        if f.detail:
            for line in _wrap("why/where: " + f.detail, width - 16):
                L.append("                            " + line)
        L.append("")
    L.append(bar)
    return "\n".join(L)


def _wrap(text: str, width: int) -> list[str]:
    words = text.split()
    lines, cur = [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > width:
            lines.append(cur)
            cur = w
        else:
            cur = f"{cur} {w}" if cur else w
    if cur:
        lines.append(cur)
    return lines or [""]


def report_to_json(rep: Report) -> dict:
    s = rep.sync
    return {
        "ledc": rep.ledc,
        "observation": rep.observation,
        "front_end": rep.front_end,
        "sync": {
            "t0_s": s.t0_s, "method": s.method, "confidence": s.confidence,
            "uncertainty_ms": s.uncertainty_ms, "detail": s.detail,
            "light_lag_ms": None if math.isnan(s.light_lag_ms) else s.light_lag_ms,
            "audio_lag_ms": None if math.isnan(s.audio_lag_ms) else s.audio_lag_ms,
            "xcorr_peak": None if math.isnan(s.xcorr_peak) else s.xcorr_peak,
            "xcorr_margin": None if math.isnan(s.xcorr_margin) else s.xcorr_margin,
        },
        "av_offset_ms": None if math.isnan(rep.av_offset_ms) else rep.av_offset_ms,
        "av_offset_band_ms": list(dm.AV_OFFSET_BAND_MS),
        "drift_common_ppm": (None if math.isnan(rep.drift_common_ppm)
                             else rep.drift_common_ppm),
        "drift_differential_ppm": (None if math.isnan(rep.drift_differential_ppm)
                                   else rep.drift_differential_ppm),
        "counts": {
            "error": sum(1 for f in rep.findings if f.severity == "error"),
            "warning": sum(1 for f in rep.findings if f.severity == "warning"),
            "info": sum(1 for f in rep.findings if f.severity == "info"),
        },
        "stats": {k: (None if isinstance(v, float) and math.isnan(v) else v)
                  for k, v in rep.stats.items()},
        "findings": [f.to_dict() for f in rep.sorted_findings()],
    }


# ---------------------------------------------------------------------------
# Fault spec parsing for `synth`
# ---------------------------------------------------------------------------

_FAULT_HELP = """\
--fault may be repeated. Accepted forms:
  offset:MS              audio delayed MS ms relative to light (negative = leads)
  drift:PPM              recorder clock error, applied to BOTH domains
  audio-drift:PPM        differential drift, audio only (a device-side bug)
  drop:T_MS[,T_MS...]    entries at those timestamps never fire
  late:T_MS:MS           entries at T_MS fire MS ms late
  light-freq:CH:FACTOR   LED channel CH flickers at FACTOR x the right rate
  audio-freq:FACTOR      every audio carrier scaled by FACTOR
  beat-drift:HZ_PER_1000S  the binaural detune grows, so the beat drifts
  dead-light:CH[,CH...]  LED channel(s) never light up
  noise:DBFS             white noise at DBFS added to every channel
  ac-couple:HZ           simulate an AC-coupled sensor input (high-pass)
"""


def parse_faults(specs: list[str]):
    from synth import Faults
    f = Faults()
    for spec in specs or []:
        parts = spec.split(":")
        name = parts[0].strip().lower()
        try:
            if name == "offset":
                f.av_offset_ms = float(parts[1])
            elif name == "drift":
                f.clock_ppm = float(parts[1])
            elif name in ("audio-drift", "audio_drift"):
                f.audio_extra_ppm = float(parts[1])
            elif name == "drop":
                f.drop_times_ms = tuple(int(x) for x in parts[1].split(","))
            elif name == "late":
                f.delay_ms[int(parts[1])] = float(parts[2])
            elif name in ("light-freq", "light_freq"):
                f.light_freq_scale[int(parts[1])] = float(parts[2])
            elif name in ("audio-freq", "audio_freq"):
                f.audio_freq_scale = float(parts[1])
            elif name in ("beat-drift", "beat_drift"):
                f.beat_drift_hz_per_1000s = float(parts[1])
            elif name in ("dead-light", "dead_light"):
                f.dead_lights = tuple(int(x) for x in parts[1].split(","))
            elif name == "noise":
                f.noise_db = float(parts[1])
            elif name in ("ac-couple", "ac_couple"):
                f.light_hp_hz = float(parts[1])
            else:
                raise ValueError(f"unknown fault '{name}'")
        except (IndexError, ValueError) as e:
            raise SystemExit(f"bad --fault '{spec}': {e}\n\n{_FAULT_HELP}")
    return f


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def cmd_analyze(a) -> int:
    import compare as cmp_mod
    import observe_wav as ow

    t_start = time.time()
    exp = le.build_expectation(
        a.ledc, grid_hz=a.grid_hz, keep_speech=a.keep_speech,
        led_backend=a.led_backend)

    rd_probe = ow.WavReader(a.wav)
    n_ch = rd_probe.n_channels
    sr = rd_probe.sample_rate
    rd_probe.close()
    cm = ow.parse_channel_map(a.map, n_ch)

    if not a.quiet:
        print(f"# .ledc      {a.ledc}  ({len(exp.schedule)} entries, "
              f"{exp.duration_s / 60:.1f} min demanded)", file=sys.stderr)
        print(f"# recording  {a.wav}  ({n_ch} ch @ {sr} Hz)", file=sys.stderr)
        print(f"# map        {cm.describe()}", file=sys.stderr)

    last = [0.0]

    def progress(frac):
        if a.quiet:
            return
        if frac - last[0] >= 0.1:
            last[0] = frac
            print(f"# reading... {frac * 100:3.0f}%", file=sys.stderr)

    obs = ow.observe_wav(
        a.wav, cm, grid_hz=a.grid_hz, light_lp_hz=a.light_lp,
        light_floor=a.light_floor, spec_win_s=a.spec_win,
        spec_hop_s=a.spec_hop, sync_tone_hz=a.sync_tone,
        max_seconds=a.max_seconds, block_frames=a.block_frames,
        progress=progress)

    cfg = cmp_mod.CompareConfig(
        t0_s=a.t0, sync_tone_hz=a.sync_tone, event_late_ms=a.event_late_ms,
        flicker_rel=a.flicker_rel, min_run_s=a.min_run)
    rep = cmp_mod.compare(exp, obs, cfg)
    rep.stats["elapsed_s"] = round(time.time() - t_start, 2)
    rep.stats["observation_meta"] = {k: v for k, v in obs.meta.items()
                                     if not k.startswith("_")}

    if a.json:
        out = json.dumps(report_to_json(rep), indent=2, default=_json_default)
        if a.json != "-":
            with open(a.json, "w") as fh:
                fh.write(out + "\n")
            if not a.quiet:
                print(f"# json -> {a.json}", file=sys.stderr)
        else:
            print(out)
    if not a.json or a.json != "-":
        print(render_report(rep, show_info=not a.no_info))

    n_err = sum(1 for f in rep.findings if f.severity == "error")
    n_warn = sum(1 for f in rep.findings if f.severity == "warning")
    if rep.sync.method == "failed":
        return 3
    # EXIT CODES (see also build_parser's epilog and the README):
    #   0  nothing to report
    #   1  at least one ERROR -- or, with --strict, any finding at all
    #   2  operator error (bad path, bad --map, bad arguments); raised by main()
    #   3  sync failed, so nothing below t0 could be judged
    #
    # EVENT_LATE, EVENT_EARLY and AV_CONST_OFFSET are now ERRORS, so the faults
    # the tool is primarily aimed at do reach $?. They used to be warnings, and
    # the README's own quick-start fault demo (`--fault offset:120 --fault
    # late:30000:60`) printed a real 129 ms A/V desync plus a 58 ms late event
    # and then exited 0. The honesty property is preserved: all three are in
    # compare._T0_DERIVED_CODES, so a weak t0 demotes them back to warnings
    # with a stated reason, and then only --strict escalates.
    if n_err:
        return 1
    if getattr(a, "strict", False) and n_warn:
        return 1
    return 0


def _json_default(o):
    if isinstance(o, (np.integer,)):
        return int(o)
    if isinstance(o, (np.floating,)):
        v = float(o)
        return None if math.isnan(v) else v
    if isinstance(o, np.ndarray):
        return o.tolist()
    if isinstance(o, (np.bool_,)):
        return bool(o)
    return str(o)


def cmd_synth(a) -> int:
    import synth as sy
    faults = parse_faults(a.fault)
    cfg = sy.SynthConfig(sample_rate=a.sample_rate, lead_in_s=a.lead_in,
                         seed=a.seed)
    meta = sy.synth_wav(a.ledc, a.out, faults=faults, cfg=cfg,
                        duration_s=a.duration, keep_speech=a.keep_speech,
                        led_backend=a.led_backend)
    if a.json:
        print(json.dumps(meta, indent=2, default=_json_default))
    else:
        print(f"wrote {meta['out']}  "
              f"{meta['n_channels']} ch @ {meta['sample_rate']} Hz  "
              f"{meta['total_s']:.1f} s")
        print(f"  --map \"{meta['map']}\"")
        print(f"  device t=0 at {meta['lead_in_s']:.3f} s into the file")
        if meta["faults"]:
            print(f"  injected faults: {meta['faults']}")
        if meta["clipped_samples"]:
            print(f"  WARNING: {meta['clipped_samples']} samples clipped; "
                  f"lower the session volumes or audio_master")
    return 0


def cmd_expect(a) -> int:
    exp = le.build_expectation(a.ledc, grid_hz=a.grid_hz,
                               keep_speech=a.keep_speech,
                               led_backend=a.led_backend)
    if a.json:
        print(json.dumps({
            "source": exp.source,
            "duration_s": exp.duration_s,
            "led_backend": exp.led_backend,
            "schedule": [
                {"t_demanded_ms": s.t_demanded_ms, "t_dispatch_ms": s.t_dispatch_ms,
                 "kind": s.kind, "line_no": s.line_no, "executed": s.executed,
                 "drop_reason": s.drop_reason}
                for s in exp.schedule],
            "events": exp.events,
            "light_channels": sorted(exp.light),
            "audio_channels": sorted(exp.audio),
            "lints": [f.to_dict() for f in exp.lints],
        }, indent=2, default=_json_default))
    else:
        print(le.dump_expectation(exp, every_s=a.every))
    return 0


def cmd_lint(a) -> int:
    exp = le.build_expectation(a.ledc, grid_hz=4.0, keep_speech=a.keep_speech,
                               led_backend=a.led_backend)
    import compare as cmp_mod
    lints = cmp_mod._dedupe(exp.lints)
    if a.json:
        print(json.dumps([f.to_dict() for f in lints], indent=2,
                         default=_json_default))
        return 1 if any(f.severity == "error" for f in lints) else 0
    if not lints:
        print(f"{os.path.basename(a.ledc)}: no static problems found")
        return 0
    print(f"{os.path.basename(a.ledc)}: {len(lints)} finding(s)")
    for f in sorted(lints, key=lambda x: x.sort_key()):
        print(f"  [{_SEV_TAG.get(f.severity, '')}] "
              f"{'line ' + str(f.line_no) if f.line_no else '         '}  "
              f"{f.code}")
        for line in _wrap(f.message, 84):
            print(f"        {line}")
        if f.detail:
            for line in _wrap("why/where: " + f.detail, 84):
                print(f"        {line}")
    return 1 if any(f.severity == "error" for f in lints) else 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="avmeasure", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--ledc", required=True, help="session .ledc file")
        sp.add_argument("--keep-speech", action="store_true",
                        help="model a raw POST of the file. By DEFAULT the S "
                             "rows are stripped, because the browser strips "
                             "them before POSTing (serialize.js:159-163) so "
                             "that is what the device actually receives.")
        sp.add_argument("--led-backend", default=dm.DEFAULT_LED_BACKEND,
                        choices=sorted(dm.LED_BACKEND_LATENCY_MS),
                        help="active LED backend; it changes the edge-latency "
                             "budget by ~2.5 ms (default: %(default)s)")
        sp.add_argument("--grid-hz", type=float, default=20.0,
                        help="analysis grid rate (default: %(default)s)")
        sp.add_argument("--json", nargs="?", const="-", default=None,
                        help="machine-readable output ('-' for stdout, or a path)")

    an = sub.add_parser("analyze", help="diff a recording against a .ledc")
    common(an)
    an.add_argument("--wav", required=True, help="multi-channel recording")
    an.add_argument("--map", required=True,
                    help='channel map, e.g. "audioL=1,audioR=2,light1=3,light4=4" '
                         'or positional "AL,AR,L1,L4". Prefix an index with ! to '
                         'invert that channel.')
    an.add_argument("--t0", type=float, default=None,
                    help="override the solved device t=0 (seconds into the file)")
    an.add_argument("--sync-tone", type=float, default=None,
                    help="frequency of the sync-marker tone, if the .ledc has "
                         "one (see README). Greatly improves t0 precision.")
    an.add_argument("--light-lp", type=float, default=200.0,
                    help="light-envelope low-pass cutoff in Hz; must sit above "
                         "the flicker rate and below the LED's own PWM carrier "
                         "(default: %(default)s)")
    an.add_argument("--light-floor", type=float, default=0.002,
                    help="peak-to-peak amplitude below which a light channel is "
                         "treated as dark (default: %(default)s)")
    an.add_argument("--spec-win", type=float, default=0.25,
                    help="audio spectral window in seconds (default: %(default)s)")
    an.add_argument("--spec-hop", type=float, default=0.2,
                    help="audio spectral hop in seconds (default: %(default)s)")
    an.add_argument("--event-late-ms", type=float,
                    default=dm.THRESHOLD_EVENT_LATE_MS,
                    help="lateness threshold (default: %(default)s)")
    an.add_argument("--flicker-rel", type=float, default=dm.THRESHOLD_FLICKER_REL,
                    help="relative flicker-rate threshold (default: %(default)s)")
    an.add_argument("--min-run", type=float, default=2.0,
                    help="a value error must persist this many seconds to be "
                         "reported (default: %(default)s)")
    an.add_argument("--max-seconds", type=float, default=None,
                    help="only analyse the first N seconds")
    an.add_argument("--block-frames", type=int, default=1 << 15,
                    help="streaming block size in frames. It does NOT bound peak memory and shrinking it does not help -- MEASURED on a 1.908 GB / 6 ch / 60-minute file: 297,959,424 B at the 32768 default and 301,629,440 B at 4096, i.e. a smaller block is very slightly WORSE. What dominates is the per-channel edge lists and the 1 kHz audio level series, both of which scale with DURATION, not with this. Detection quality does not depend on it either (the light trigger uses its own 4 s window). (default: %(default)s = 0.74 s at 44.1 kHz)")
    an.add_argument("--no-info", action="store_true",
                    help="suppress informational findings")
    an.add_argument("--strict", action="store_true",
                    help="exit 1 on WARNINGS as well as errors. Use this in a "
                         "script or a CI gate: a warning means the instrument "
                         "saw something it could not raise to an error, which "
                         "on a bench is still a reason to look.")
    an.add_argument("--quiet", action="store_true", help="no progress on stderr")
    an.set_defaults(func=cmd_analyze)

    sy = sub.add_parser("synth", help="render a synthetic recording from a .ledc",
                        epilog=_FAULT_HELP,
                        formatter_class=argparse.RawDescriptionHelpFormatter)
    common(sy)
    sy.add_argument("--out", required=True, help="output WAV path")
    sy.add_argument("--fault", action="append", default=[],
                    help="inject a fault (repeatable); see the list below")
    sy.add_argument("--duration", type=float, default=None,
                    help="device-time duration to render (default: whole session)")
    sy.add_argument("--lead-in", type=float, default=2.0,
                    help="silence before device t=0 (default: %(default)s)")
    sy.add_argument("--sample-rate", type=int, default=44100)
    sy.add_argument("--seed", type=int, default=12345)
    sy.set_defaults(func=cmd_synth)

    ex = sub.add_parser("expect", help="print the demanded model for review")
    common(ex)
    ex.add_argument("--dump", action="store_true",
                    help="(default behaviour; kept for symmetry)")
    ex.add_argument("--every", type=float, default=60.0,
                    help="sample interval for the dump, seconds "
                         "(default: %(default)s)")
    ex.set_defaults(func=cmd_expect)

    ln = sub.add_parser("lint", help="static .ledc problems, no recording needed")
    common(ln)
    ln.set_defaults(func=cmd_lint)
    return p


def main(argv=None) -> int:
    """Entry point. Ordinary operator mistakes exit 2, never 1.

    A typo'd --wav path used to print twelve lines of stack ending in
    FileNotFoundError and exit 1 -- the SAME code as "the device is wrong". At a
    bench at 1am a traceback reads as "the tool crashed", and a script could not
    tell a mistyped filename from a real fault. The messages themselves were
    already good; they were just buried under stack frames and sharing an exit
    code with the thing the tool exists to report.
    """
    a = build_parser().parse_args(argv)
    try:
        return a.func(a)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
    except (ValueError, OSError) as e:
        name = getattr(e, "filename", None)
        msg = str(e) or e.__class__.__name__
        if name and name not in msg:
            msg = f"{msg}: {name}"
        print(f"error: {msg}", file=sys.stderr)
        print("(this is an input problem, not a device finding -- exit 2)",
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
