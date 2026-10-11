#!/usr/bin/env python3
"""Self-test: inject a known fault, assert the engine finds exactly that fault.

Run either of these from tools/avmeasure (the `-t .` is required: it puts the
package root on the path so the modules under test are importable):

    python3 -m unittest discover -s tests -t . -v
    python3 tests/test_avmeasure.py -v                       # same thing

THE TWO HALVES OF THIS SUITE
----------------------------
1. CRY-WOLF TEST (test_00_clean_*): a clean synthetic recording must produce
   ZERO errors and ZERO warnings. This is the most important test in the file.
   An instrument that reports phantom bugs gets ignored, and then it may as
   well not exist.

2. FAULT TESTS: each injects ONE quantified defect and asserts both that the
   right code fires AND that the reported magnitude is right to a stated
   tolerance. Asserting only "something fired" would let the detector be
   wrong by 10x and still pass.

WHAT THESE TESTS DO *NOT* PROVE
-------------------------------
The renderer drives off the same keyframe state machine as the expectation, so
these tests validate observe_wav.py and compare.py -- NOT the fidelity of
ledc_expect.py to the firmware. That fidelity rests on the file:line citations
in ledc_expect.py, on the separate semantic unit tests below (which encode
specific firmware behaviours as hand-written expectations), and on a human
reading `analyze.py expect --dump`. See README LIMITATIONS.
"""

from __future__ import annotations

import math
import os
import resource
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

import numpy as np  # noqa: E402

import compare as cmp_mod  # noqa: E402
import devicemodel as dm  # noqa: E402
import ledc_expect as le  # noqa: E402
import observe_wav as ow  # noqa: E402
import synth as sy  # noqa: E402

SESSION = os.path.join(HERE, "selftest.ledc")
DURATION = 100.0
MAP = "audioL=1,audioR=2,light1=3,light2=4"

_TMP: str | None = None
_CACHE: dict = {}


def setUpModule():
    global _TMP
    _TMP = tempfile.mkdtemp(prefix="avmeasure-test-")


def tearDownModule():
    if _TMP and os.path.isdir(_TMP):
        shutil.rmtree(_TMP, ignore_errors=True)


def run_case(name: str, faults: sy.Faults | None = None, *,
             chmap: str = MAP, duration: float = DURATION,
             cfg: cmp_mod.CompareConfig | None = None,
             lead_in: float = 2.0):
    """Render -> observe -> compare. Cached, because rendering dominates."""
    key = (name, chmap, duration, lead_in)
    if key in _CACHE:
        return _CACHE[key]
    wav = os.path.join(_TMP, f"{name}.wav")
    meta = sy.synth_wav(SESSION, wav, faults=faults,
                        cfg=sy.SynthConfig(lead_in_s=lead_in),
                        duration_s=duration)
    exp = le.build_expectation(SESSION, duration_s=duration + 2.0)
    rd = ow.WavReader(wav)
    n_ch = rd.n_channels
    rd.close()
    cm = ow.parse_channel_map(chmap, n_ch)
    obs = ow.observe_wav(wav, cm)
    rep = cmp_mod.compare(exp, obs, cfg or cmp_mod.CompareConfig())
    _CACHE[key] = (rep, meta, exp, obs)
    return _CACHE[key]


def codes(rep, severities=("error", "warning")) -> list[str]:
    return [f.code for f in rep.findings if f.severity in severities]


def get(rep, code):
    for f in rep.findings:
        if f.code == code:
            return f
    return None


def describe(rep) -> str:
    out = [f"sync t0={rep.sync.t0_s:.4f} method={rep.sync.method} "
           f"conf={rep.sync.confidence}",
           f"av_offset={rep.av_offset_ms:.1f} ms  "
           f"drift_common={rep.drift_common_ppm:.0f} ppm  "
           f"drift_diff={rep.drift_differential_ppm:.0f} ppm"]
    for f in rep.sorted_findings():
        out.append(f"  [{f.severity}] {f.code} "
                   f"ch={f.channel} t={f.t_ms} obs={f.observed} exp={f.expected}")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# 0. the cry-wolf test
# ---------------------------------------------------------------------------


class TestClean(unittest.TestCase):

    def test_00_clean_has_no_findings(self):
        rep, meta, exp, obs = run_case("clean")
        self.assertEqual(rep.sync.method in ("xcorr", "marker"), True,
                         describe(rep))
        self.assertEqual(rep.sync.confidence, "high", describe(rep))
        # t0 must be recovered to within a few ms of the known lead-in.
        self.assertAlmostEqual(rep.sync.t0_s, meta["lead_in_s"], delta=0.010,
                               msg=describe(rep))
        self.assertEqual(codes(rep), [], describe(rep))

    def test_01_clean_av_offset_inside_band(self):
        rep, meta, exp, obs = run_case("clean")
        lo, hi = dm.AV_OFFSET_BAND_MS
        self.assertTrue(lo <= rep.av_offset_ms <= hi, describe(rep))

    def test_02_clean_no_drift(self):
        rep, meta, exp, obs = run_case("clean")
        self.assertLess(abs(rep.drift_common_ppm), dm.THRESHOLD_DRIFT_PPM,
                        describe(rep))
        self.assertLess(abs(rep.drift_differential_ppm),
                        dm.THRESHOLD_DIFF_DRIFT_PPM, describe(rep))

    def test_03_clean_measurements_are_accurate(self):
        """The measured quantities must match the session, not just "not fail"."""
        rep, meta, exp, obs = run_case("clean")
        t0 = rep.sync.t0_s
        i = int(np.searchsorted(obs.t_rec, t0 + 20.0))
        A = obs.audio
        self.assertAlmostEqual(float(A.tone_l_hz[i]), 240.0, delta=0.2)
        self.assertAlmostEqual(float(A.tone_r_hz[i]), 246.0, delta=0.2)
        self.assertAlmostEqual(float(A.beat_hz[i]), 6.0, delta=0.1)
        self.assertAlmostEqual(float(A.pulse_hz[i]), 7.0, delta=0.1)
        # The timeline hard-codes mod_depth = 0.1 (config_parser.c:2357).
        self.assertAlmostEqual(float(A.pulse_depth[i]), 0.1, delta=0.03)
        L = obs.light[1]
        self.assertAlmostEqual(float(L.freq_hz[i]), 10.0, delta=0.02)
        self.assertAlmostEqual(float(L.duty_pct[i]), 50.0, delta=4.0)
        j = int(np.searchsorted(obs.t_rec, t0 + 75.0))
        self.assertAlmostEqual(float(obs.light[1].freq_hz[j]), 20.0, delta=0.05)
        self.assertAlmostEqual(float(obs.light[2].freq_hz[j]), 10.0, delta=0.05)

    def test_04_clean_with_sync_marker_has_no_findings(self):
        """The RECOMMENDED path must be at least as clean as the fallback.

        REGRESSION: the marker envelope was accumulated on the 20 Hz analysis
        grid but consumed as if it were on the 1 kHz fine grid, so --sync-tone
        dated the audio origin ~3.5 ms late and then reported the t=600 and
        t=30000 audio steps as ~7 ms EARLY -- on a bit-perfect clean recording.
        The plain cry-wolf test missed it because it does not pass a sync tone,
        i.e. the one path the README tells users to prefer was the only one
        untested. Assert BOTH that it is clean and that it is more precise than
        the no-marker estimate, which is the entire reason to author a marker.
        """
        run_case("clean")                      # ensure the WAV exists (cached)
        wav = os.path.join(_TMP, "clean.wav")
        exp = le.build_expectation(SESSION, duration_s=DURATION + 2.0)
        rd = ow.WavReader(wav)
        n_ch = rd.n_channels
        rd.close()
        cm = ow.parse_channel_map(MAP, n_ch)

        obs = ow.observe_wav(wav, cm, sync_tone_hz=3000.0)
        self.assertEqual(obs.meta.get("_sync_fs"), 44100.0 / 44,
                         "marker envelope must publish its own 1 kHz rate")
        rep = cmp_mod.compare(exp, obs,
                              cmp_mod.CompareConfig(sync_tone_hz=3000.0))
        self.assertEqual(rep.sync.method, "marker", describe(rep))
        self.assertEqual(codes(rep), [], describe(rep))
        self.assertAlmostEqual(rep.sync.t0_s, 2.0, delta=0.010, msg=describe(rep))

        # The marker's audio origin must beat the broadband fallback's.
        plain, _, _, _ = run_case("clean")
        self.assertLessEqual(
            abs(rep.sync.audio_lag_ms - rep.sync.light_lag_ms),
            abs(plain.sync.audio_lag_ms - plain.sync.light_lag_ms) + 1.0,
            f"marker: {describe(rep)}\n\nplain: {describe(plain)}")


class TestSyncHonesty(unittest.TestCase):
    """t0 is the foundation; a confidently WRONG t0 is the worst possible output.

    REGRESSION. On a session with no sharp optical feature -- a 20 s brightness
    fade-in, then one steady rate for minutes, which is the shape of nearly
    every shipped session -- the activity envelopes are near-flat plateaus. Two
    flat plateaus correlate at 0.9997 at ANY small lag, so the correlation peak
    is meaningless while still being close to 1. Confidence was gated on that
    peak, so a t0 that was 2 SECONDS wrong was published as
    "HIGH confidence (+/-5 ms)", and it cascaded into a phantom
    AV_AUDIO_LEADS_LIGHT error plus ten more findings.

    The invariant worth testing is not "t0 is always right" -- it cannot be,
    without a marker -- but "t0 is never confidently wrong".
    """

    SESSION = os.path.join(HERE, "fadein.ledc")
    DUR = 240.0
    CHMAP = "audioL=1,audioR=2,light1=3"

    @classmethod
    def setUpClass(cls):
        cls.wav = os.path.join(_TMP, "fadein.wav")
        cls.meta = sy.synth_wav(cls.SESSION, cls.wav,
                                cfg=sy.SynthConfig(lead_in_s=2.0),
                                duration_s=cls.DUR)
        cls.exp = le.build_expectation(cls.SESSION, duration_s=cls.DUR + 2.0)
        rd = ow.WavReader(cls.wav)
        n_ch = rd.n_channels
        rd.close()
        cls.cm = ow.parse_channel_map(cls.CHMAP, n_ch)

    def test_05_never_confidently_wrong_about_t0(self):
        obs = ow.observe_wav(self.wav, self.cm)
        rep = cmp_mod.compare(self.exp, obs, cmp_mod.CompareConfig())
        err = abs(rep.sync.t0_s - self.meta["lead_in_s"]) * 1000.0
        if rep.sync.confidence == "high":
            # Allowed to be high ONLY if it is actually right.
            self.assertLess(err, 50.0,
                            f"t0 off by {err:.0f} ms but reported HIGH:\n"
                            + describe(rep))
        else:
            # Otherwise the stated uncertainty must actually cover the error,
            # or the uncertainty is itself a lie.
            self.assertGreaterEqual(
                rep.sync.uncertainty_ms, err * 0.5,
                f"t0 off by {err:.0f} ms but claimed only "
                f"+/-{rep.sync.uncertainty_ms:.0f} ms:\n" + describe(rep))

    def test_06_no_hard_errors_when_t0_is_untrustworthy(self):
        """A weak t0 must not be laundered into hard errors about the device."""
        obs = ow.observe_wav(self.wav, self.cm)
        rep = cmp_mod.compare(self.exp, obs, cmp_mod.CompareConfig())
        if rep.sync.confidence in ("low", "none"):
            errs = [f.code for f in rep.findings if f.severity == "error"]
            self.assertEqual(errs, [], describe(rep))
            self.assertIn("SYNC_LOW_CONFIDENCE",
                          [f.code for f in rep.findings], describe(rep))

    def test_07_sync_marker_rescues_the_hard_case(self):
        """The README's recipe must actually fix the case it is prescribed for."""
        marked = os.path.join(_TMP, "fadein_marked.ledc")
        with open(self.SESSION) as fh:
            body = fh.read().split("\n")
        rows = []
        for line in body:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            t = s.split()
            if t[0] in ("A", "a"):
                t[1] = str(int(t[1]) + 1000)
            else:
                t[0] = str(int(t[0]) + 1000)
            rows.append(" ".join(t))
        # Marker rate == the session's own max rate, so it does not raise the
        # flicker ISR tick and therefore does not perturb what we measure.
        marker = ["0     40 50 100 255 255 255 1",
                  "A 0   3000 0 70 0 15",
                  "600   0 50 0 0 0 0 1",
                  "A 600 3000 0 0 0 15"]
        with open(marked, "w") as fh:
            fh.write("\n".join(marker + rows) + "\n")

        wav = os.path.join(_TMP, "fadein_marked.wav")
        meta = sy.synth_wav(marked, wav, cfg=sy.SynthConfig(lead_in_s=2.0),
                            duration_s=self.DUR + 1.0)
        exp = le.build_expectation(marked, duration_s=self.DUR + 3.0)
        rd = ow.WavReader(wav)
        n_ch = rd.n_channels
        rd.close()
        cm = ow.parse_channel_map(self.CHMAP, n_ch)
        obs = ow.observe_wav(wav, cm, sync_tone_hz=3000.0)
        rep = cmp_mod.compare(exp, obs,
                              cmp_mod.CompareConfig(sync_tone_hz=3000.0))
        self.assertEqual(rep.sync.confidence, "high", describe(rep))
        self.assertAlmostEqual(rep.sync.t0_s, meta["lead_in_s"], delta=0.010,
                               msg=describe(rep))
        self.assertEqual(codes(rep), [], describe(rep))


# ---------------------------------------------------------------------------
# 1. faults
# ---------------------------------------------------------------------------


class TestFaults(unittest.TestCase):

    def test_10_constant_av_offset(self):
        inject = 120.0
        rep, meta, exp, obs = run_case(
            "offset", sy.Faults(av_offset_ms=inject))
        self.assertIn("AV_CONST_OFFSET", codes(rep), describe(rep))
        f = get(rep, "AV_CONST_OFFSET")
        want = inject + dm.av_offset_pipeline_ms(exp.led_backend)
        self.assertAlmostEqual(f.observed, want, delta=20.0, msg=describe(rep))
        # A constant offset must be ONE finding, not one per audio event.
        self.assertEqual(codes(rep).count("AV_CONST_OFFSET"), 1, describe(rep))
        self.assertNotIn("EVENT_LATE", codes(rep), describe(rep))

    def test_11_audio_leads_light_is_always_a_bug(self):
        rep, meta, exp, obs = run_case(
            "lead", sy.Faults(av_offset_ms=-60.0))
        self.assertIn("AV_AUDIO_LEADS_LIGHT", codes(rep), describe(rep))
        f = get(rep, "AV_AUDIO_LEADS_LIGHT")
        self.assertEqual(f.severity, "error")
        want = -60.0 + dm.av_offset_pipeline_ms(exp.led_backend)
        self.assertAlmostEqual(f.observed, want, delta=20.0, msg=describe(rep))

    def test_12_recorder_clock_drift_is_not_a_device_bug(self):
        rep, meta, exp, obs = run_case("drift", sy.Faults(clock_ppm=500.0))
        self.assertIn("CLOCK_DRIFT_RECORDER",
                      codes(rep, ("error", "warning", "info")), describe(rep))
        self.assertAlmostEqual(rep.drift_common_ppm, 500.0, delta=120.0,
                               msg=describe(rep))
        # Crucially: a COMMON drift must NOT be reported as a device-side
        # audio/light divergence, and must not spray per-event findings.
        self.assertNotIn("AV_DIFFERENTIAL_DRIFT", codes(rep), describe(rep))
        self.assertNotIn("FLICKER_RATE", codes(rep), describe(rep))
        self.assertNotIn("TONE_FREQ", codes(rep), describe(rep))

    def test_13_differential_drift_is_a_device_bug(self):
        rep, meta, exp, obs = run_case(
            "diffdrift", sy.Faults(audio_extra_ppm=400.0))
        self.assertIn("AV_DIFFERENTIAL_DRIFT", codes(rep), describe(rep))
        f = get(rep, "AV_DIFFERENTIAL_DRIFT")
        self.assertEqual(f.severity, "error")
        self.assertAlmostEqual(f.observed, 400.0, delta=120.0, msg=describe(rep))

    def test_14_late_event(self):
        late_ms = 180.0
        rep, meta, exp, obs = run_case(
            "late", sy.Faults(delay_ms={30000: late_ms}))
        lates = [f for f in rep.findings if f.code == "EVENT_LATE"]
        self.assertTrue(lates, describe(rep))
        hit = [f for f in lates if abs((f.t_ms or 0) - 30000) < 1000]
        self.assertTrue(hit, f"no EVENT_LATE at t=30000\n{describe(rep)}")
        self.assertAlmostEqual(hit[0].delta, late_ms, delta=35.0,
                               msg=describe(rep))

    def test_15_dropped_event(self):
        rep, meta, exp, obs = run_case(
            "drop", sy.Faults(drop_times_ms=(60000,)))
        # The dropped entry was ch1's 10 -> 20 Hz step, so the consequence is a
        # wrong flicker rate from 60 s onward on channel 1 and nowhere else.
        fr = [f for f in rep.findings
              if f.code == "FLICKER_RATE" and f.channel == 1]
        self.assertTrue(fr, describe(rep))
        f = fr[0]
        self.assertAlmostEqual(f.t_ms / 1000.0, 60.0, delta=3.0,
                               msg=describe(rep))
        self.assertAlmostEqual(f.observed, 10.0, delta=0.3, msg=describe(rep))
        self.assertAlmostEqual(f.expected, 20.0, delta=0.3, msg=describe(rep))
        self.assertFalse([x for x in rep.findings
                          if x.code == "FLICKER_RATE" and x.channel == 2],
                         describe(rep))

    def test_16_wrong_flicker_frequency(self):
        rep, meta, exp, obs = run_case(
            "wrongfreq", sy.Faults(light_freq_scale={1: 1.06}))
        fr = [f for f in rep.findings
              if f.code == "FLICKER_RATE" and f.channel == 1]
        self.assertTrue(fr, describe(rep))
        f = fr[0]
        self.assertAlmostEqual(f.observed / f.expected, 1.06, delta=0.01,
                               msg=describe(rep))
        self.assertFalse([x for x in rep.findings
                          if x.code == "FLICKER_RATE" and x.channel == 2],
                         describe(rep))

    def test_17_wrong_audio_frequency(self):
        rep, meta, exp, obs = run_case(
            "wrongtone", sy.Faults(audio_freq_scale=1.03))
        tf = [f for f in rep.findings if f.code == "TONE_FREQ"]
        self.assertTrue(tf, describe(rep))
        self.assertAlmostEqual(tf[0].observed / tf[0].expected, 1.03,
                               delta=0.008, msg=describe(rep))

    def test_18_dead_light_channel(self):
        rep, meta, exp, obs = run_case("dead", sy.Faults(dead_lights=(2,)))
        nv = [f for f in rep.findings if f.code == "CHANNEL_NEVER_ON"]
        self.assertTrue(nv, describe(rep))
        self.assertEqual(nv[0].channel, 2, describe(rep))
        self.assertEqual(nv[0].severity, "error")
        self.assertEqual([f.channel for f in nv], [2], describe(rep))

    def test_19_noise_does_not_cause_false_positives(self):
        """Robustness: a noisy but correct recording must still be clean.

        -34 dBFS of broadband noise is far more than a decent interface and a
        decent photodiode front-end will produce, so if the detectors survive
        this they will survive a real rig."""
        rep, meta, exp, obs = run_case("noisy", sy.Faults(noise_db=-34.0))
        self.assertEqual(rep.sync.confidence, "high", describe(rep))
        self.assertEqual(codes(rep), [], describe(rep))

    def test_20_single_audio_channel_degrades_gracefully(self):
        rep, meta, exp, obs = run_case(
            "mono", chmap="audioL=1,-,light1=3,light2=4")
        self.assertFalse(obs.audio.beat_available)
        self.assertIn("BEAT_UNAVAILABLE",
                      codes(rep, ("error", "warning", "info")), describe(rep))
        # Never guess: there must be no beat VALUE finding at all.
        self.assertNotIn("BEAT_FREQ", [f.code for f in rep.findings],
                         describe(rep))
        # The tone and level checks must still work on the one ear we have.
        self.assertTrue(np.isfinite(obs.audio.tone_l_hz).any())

    def test_21_no_light_channels_lowers_sync_confidence(self):
        """Without an optical reference, t0 inherits the audio dispatch jitter
        and the tool must say so rather than pretend."""
        rep, meta, exp, obs = run_case(
            "nolight", chmap="audioL=1,audioR=2")
        self.assertIn(rep.sync.confidence, ("low", "none"), describe(rep))
        self.assertIn("SYNC_LOW_CONFIDENCE", codes(rep), describe(rep))

    def test_22_ac_coupled_sensor_still_measures_rate(self):
        """An audio interface high-passes its inputs around 5-20 Hz, which
        destroys the DC level of a slow flicker envelope. Rate must survive
        (it is carried by the edges); duty is allowed to degrade."""
        rep, meta, exp, obs = run_case("accouple",
                                       sy.Faults(light_hp_hz=12.0))
        t0 = rep.sync.t0_s
        i = int(np.searchsorted(obs.t_rec, t0 + 20.0))
        self.assertAlmostEqual(float(obs.light[1].freq_hz[i]), 10.0, delta=0.05,
                               msg=describe(rep))
        self.assertNotIn("CHANNEL_NEVER_ON", codes(rep), describe(rep))


class TestSilentMisses(unittest.TestCase):
    """The worst failure class: a fault is present and NOTHING is reported.

    Every test here corresponds to a case where the tool previously printed
    "Nothing to report: the recording matches the .ledc within every threshold"
    (or omitted the finding entirely) with a real, injected, quantified defect
    in the recording. A tool that goes quiet on a real fault certifies a broken
    device as healthy, which is worse than having no tool at all.
    """

    def _render(self, name, session, faults=None, dur=100.0, chmap=MAP,
                tone=3000.0, cfg=None):
        wav = os.path.join(_TMP, f"{name}.wav")
        sy.synth_wav(session, wav, faults=faults,
                     cfg=sy.SynthConfig(lead_in_s=2.0), duration_s=dur)
        exp = le.build_expectation(session, duration_s=dur + 2.0)
        rd = ow.WavReader(wav)
        n_ch = rd.n_channels
        rd.close()
        cm = ow.parse_channel_map(chmap, n_ch)
        obs = ow.observe_wav(wav, cm, sync_tone_hz=tone)
        rep = cmp_mod.compare(exp, obs, cfg or cmp_mod.CompareConfig(
            sync_tone_hz=tone))
        return rep, exp, obs

    def test_70_sensitivity_is_monotone_not_a_band(self):
        """A WORSE fault must never be more likely to be missed than a mild one.

        MEASURED before: an audio volume step injected 200 ms late was reported
        at 197 ms, while the SAME step 700 ms and 1500 ms late produced zero
        errors, zero warnings and exit 0 -- because a realisation outside the
        fixed +/-400 ms window returned NaN and the NaN was dropped silently
        for every event that was not a cold activation.
        """
        for late in (200.0, 700.0, 1500.0, 4000.0):
            rep, _, _ = self._render(f"mono_late_{int(late)}", SESSION,
                                     sy.Faults(delay_ms={30000: late}))
            hit = [f for f in rep.findings
                   if f.code in ("EVENT_LATE", "EVENT_NOT_LOCATED")
                   and abs((f.t_ms or 0) - 30000) < 1500]
            self.assertTrue(hit, f"{late:.0f} ms late went UNREPORTED:\n"
                                 + describe(rep))
            self.assertEqual(hit[0].severity, "error",
                             f"{late:.0f} ms late was not an error:\n"
                             + describe(rep))

    def test_71_led_value_change_lateness_is_reachable(self):
        """EVENT_LATE on an LED value change must be reachable, and linear.

        MEASURED before: the verdict tolerance was `25 + slack + 10*period`
        (1025 ms at 10 Hz) while the realisation search was a fixed 400 ms, so
        the reportable band was EMPTY at every rate the 82 shipped sessions
        use. 120 / 300 / 450 / 800 ms-late 10 -> 20 Hz steps ALL reported
        "the recording matches the .ledc within every threshold".
        """
        for late in (300.0, 800.0):
            rep, _, _ = self._render(f"led_late_{int(late)}", SESSION,
                                     sy.Faults(delay_ms={60000: late}))
            hit = [f for f in rep.findings
                   if f.code == "EVENT_LATE" and f.channel == 1
                   and abs((f.t_ms or 0) - 60000) < 100]
            self.assertTrue(hit, f"LED step {late:.0f} ms late went "
                                 f"UNREPORTED:\n" + describe(rep))
            # ...and with the right MAGNITUDE. A detector that fires but is
            # wrong by 2x sends someone to the wrong place.
            self.assertAlmostEqual(hit[0].delta, late, delta=60.0,
                                   msg=describe(rep))

    def test_72_all_mapped_light_channels_dark(self):
        """The single most likely first-real-recording failure.

        MEASURED before: with 3 of 4 zones dead all three were reported, but
        with ALL FOUR dead there were ZERO CHANNEL_NEVER_ON findings and exit
        0, because contrast is normalised by the brightest MAPPED channel --
        which, when every mapped channel is dark, IS the noise floor. The
        report instead invented "audio lags light by a CONSTANT 2038 ms".
        """
        rep, _, obs = self._render("all_dark", SESSION,
                                   sy.Faults(dead_lights=(1, 2)))
        nv = sorted(f.channel for f in rep.findings
                    if f.code == "CHANNEL_NEVER_ON")
        self.assertEqual(nv, [1, 2], describe(rep))
        for f in rep.findings:
            if f.code == "CHANNEL_NEVER_ON":
                self.assertEqual(f.severity, "error", describe(rep))
        # And it must NOT fabricate an A/V offset off a noise-only reference.
        self.assertFalse(math.isfinite(rep.av_offset_ms),
                         f"av_offset {rep.av_offset_ms} was reported with no "
                         f"optical signal at all:\n" + describe(rep))
        self.assertTrue(all(not O.available for O in obs.light.values()))

    def test_73_dropped_entry_is_reported_at_its_own_timestamp(self):
        """A dropped step must produce an EVENT-level finding AT its timestamp.

        MEASURED before: dropping the t=30000 audio volume step was only
        visible as "AMPLITUDE: audio level was -5.5 dB" with t_ms = 1648, i.e.
        28 seconds before the fault, because the NaN realisation was discarded
        and the single per-session gain fit spread the error over both halves.
        """
        rep, _, _ = self._render("drop30", SESSION,
                                 sy.Faults(drop_times_ms=(30000,)))
        hit = [f for f in rep.findings
               if f.code == "EVENT_NOT_LOCATED"
               and abs((f.t_ms or 0) - 30000) < 100]
        self.assertTrue(hit, describe(rep))


class TestUnexercisedDetectors(unittest.TestCase):
    """Detectors that had no injector and no test in EITHER direction.

    An unexercised detector in a measurement instrument is an unknown, not a
    feature: nothing showed that it fires on a real fault, and nothing showed
    that it stays quiet on a clean recording.
    """

    def test_74_beat_drift_fires_and_is_quiet(self):
        """BEAT_DRIFT -- one of the four example outputs in the brief ("beat
        frequency drifted +0.3 Hz over 20 minutes") -- had no entry in
        synth.Faults at all, so compare.py:1751-1774 had never run against a
        real beat drift."""
        clean, _, _, _ = run_case("clean")
        self.assertNotIn("BEAT_DRIFT", [f.code for f in clean.findings],
                         describe(clean))
        # 20 Hz per 1000 s over the ~59 s measurable span is ~1.2 Hz, which is
        # past the 2 x 0.25 Hz reporting bar.
        rep, meta, exp, obs = run_case(
            "beatdrift", sy.Faults(beat_drift_hz_per_1000s=20.0))
        f = get(rep, "BEAT_DRIFT")
        self.assertIsNotNone(f, describe(rep))
        self.assertEqual(f.severity, "error", describe(rep))
        span_s = (f.t_end_ms - f.t_ms) / 1000.0
        self.assertAlmostEqual(f.observed, 20.0e-3 * span_s, delta=0.35,
                               msg=f"span {span_s:.0f} s\n" + describe(rep))

    def test_75_event_drift_needs_a_real_baseline(self):
        """EVENT_DRIFT must not extrapolate a 31 s fit to a per-1000 s figure.

        MEASURED before, on a CLEAN render of 04_meditation_theta: "[ERROR]
        EVENT_DRIFT: event lateness GROWS over the session: -171 ms accumulated
        between t=0 s and t=31 s ... this points at a clock-ratio problem", with
        observed = -5527.8 ms/1000 s = -5.5 parts per THOUSAND, while the clock
        fit in the same report said +/-10 ppm. Unit-level because the gates are
        the whole point and they are cheap to state.
        """
        cfg = cmp_mod.CompareConfig()
        # (t_s, residual_ms, domain, tolerance_ms)
        short = [(t, -5.5 * t, "led", 25.0) for t in (0, 6, 12, 18, 24, 31)]
        out = cmp_mod._check_event_drift(short, cfg, 0.0)
        codes_ = [f.code for f in out]
        self.assertNotIn("EVENT_DRIFT", codes_, codes_)
        self.assertIn("EVENT_DRIFT_NOT_TESTED", codes_, codes_)
        self.assertIn("lever arm", out[0].message)
        # A loose residual must not tilt the line on its own: one led_stop
        # point with a full flicker period of legitimate slack (+/-333 ms at
        # 3 Hz) used to be enough to trip the 75 ms budget.
        wide = [(t, 0.0, "led", 25.0) for t in range(0, 601, 60)]
        wide[-1] = (600.0, 330.0, "led", 333.0)
        out2 = cmp_mod._check_event_drift(wide, cfg, 0.0)
        self.assertNotIn("EVENT_DRIFT", [f.code for f in out2],
                         [f.message for f in out2])
        # ...but a genuine, significant, long-baseline trend IS reported --
        # PROVIDED BOTH DOMAINS SHOW IT (round 3). The scheduler drives audio
        # and light off one absolute T0, so an accumulating lateness has to move
        # both; a tilt in one domain only is the spread between that domain's
        # realisation estimators, which is exactly what produced
        # "[ERROR] EVENT_DRIFT ... -76 +/-17 ppm" on a bit-perfect full-length
        # render of 91_measure_17_uniform (LED residuals only; its audio entries
        # are all volume ramps and none is timeable).
        real = ([(t, 0.4 * t, "led", 25.0) for t in range(0, 601, 50)]
                + [(t, 0.4 * t, "audio", 25.0) for t in range(0, 601, 50)])
        out3 = cmp_mod._check_event_drift(real, cfg, 0.0)
        self.assertIn("EVENT_DRIFT", [f.code for f in out3],
                      [f.message for f in out3])
        self.assertAlmostEqual(get_from(out3, "EVENT_DRIFT").observed, 400.0,
                               delta=20.0)
        # One domain only: reported, but as info, naming the gate that failed.
        one = [(t, 0.4 * t, "led", 25.0) for t in range(0, 601, 50)]
        out4 = cmp_mod._check_event_drift(one, cfg, 0.0)
        self.assertNotIn("EVENT_DRIFT", [f.code for f in out4])
        self.assertIn("EVENT_DRIFT_NOT_TESTED", [f.code for f in out4])
        self.assertIn("cross-checked", get_from(out4,
                                                "EVENT_DRIFT_NOT_TESTED").message)
        # ...and a trend the two domains DISAGREE about is the estimator spread.
        split = ([(t, 0.4 * t, "led", 25.0) for t in range(0, 601, 50)]
                 + [(t, -0.4 * t, "audio", 25.0) for t in range(0, 601, 50)])
        out5 = cmp_mod._check_event_drift(split, cfg, 0.0)
        self.assertNotIn("EVENT_DRIFT", [f.code for f in out5])


def get_from(findings, code):
    for f in findings:
        if f.code == code:
            return f
    return None


class TestNoCryWolf(unittest.TestCase):
    """Clean renders of REAL shipped sessions must stay silent.

    The cry-wolf test used to render only tests/selftest.ledc, so it could not
    see that bit-perfect renders of shipped sessions produced error-severity
    findings naming firmware subsystems: "PULSE_RATE: audio pulse rate was
    98.845 Hz where 10.000 Hz was expected" on 05_focus_smr, "EVENT_DRIFT ...
    this points at a clock-ratio problem" off a 31 s baseline on
    04_meditation_theta, and "AMPLITUDE: audio level was -8.1 dB" on the same
    session. Adding two or three shipped sessions here would have caught all
    three.

    Static `.ledc` lints (LEDC_*) are EXCLUDED from the assertion on purpose:
    they are findings about the session FILE, they are reproducible by
    `analyze.py lint` with no recording at all, and several shipped sessions
    genuinely contain the defects they name.
    """

    LIB = os.path.join(ROOT, "..", "..", "sessions", "library")
    CUT_MS = 180000
    DUR = 190.0

    @staticmethod
    def _trim_and_mark(src: str, out: str, cut_ms: int) -> str:
        """Keep entries up to cut_ms, close cleanly, prepend a sync marker.

        Trimming keeps the SHAPE of the real session (its fade-ins, its ramps,
        its LFOs) while making the capture complete, so RECORDING_INCOMPLETE --
        which is a true statement about a short capture, not a device finding --
        stays out of the way.
        """
        led, aud, masks, chans = [], [], set(), set()
        with open(src) as fh:
            for raw in fh:
                s = raw.split("#")[0].strip()
                if not s:
                    continue
                t = s.split()
                if t[0] in ("A", "a"):
                    if len(t) < 2:
                        continue
                    try:
                        tm = int(float(t[1]))
                    except ValueError:
                        continue
                    if tm > cut_ms:
                        continue
                    aud.append(" ".join(["A", str(tm + 1000)] + t[2:]))
                    if len(t) >= 7:
                        try:
                            chans.add(int(t[6]))
                        except ValueError:
                            pass
                elif t[0] in ("S", "s", "BG", "bg"):
                    continue
                else:
                    try:
                        tm = int(float(t[0]))
                    except ValueError:
                        continue
                    if tm > cut_ms:
                        continue
                    led.append(" ".join([str(tm + 1000)] + t[1:]))
                    if len(t) == 5 or 8 <= len(t) <= 12:
                        try:
                            masks.add(int(t[4] if len(t) == 5 else t[7]))
                        except ValueError:
                            pass
        union = (0
                 if not masks else
                 __import__("functools").reduce(lambda a, b: a | b, masks)) & 0xFF
        union = union or 1
        end = cut_ms + 3000
        marker = [f"0     50 50 100 255 255 255 {union}",
                  "A 0   3000 0 70 0 15",
                  f"600   0 50 0 0 0 0 {union}",
                  "A 600 3000 0 0 0 15"]
        close = [f"{end} 0 50 0 0 0 0 {m}" for m in sorted(masks)]
        close += [f"A {end} 200 0 0 0 {c}" for c in sorted(chans)]
        with open(out, "w") as fh:
            fh.write("\n".join(marker + led + aud + close) + "\n")
        return out

    def _run(self, basename: str):
        src = os.path.join(self.LIB, basename + ".ledc")
        if not os.path.exists(src):
            self.skipTest("session library not present")
        marked = os.path.join(_TMP, basename + "_cw.ledc")
        self._trim_and_mark(src, marked, self.CUT_MS)
        wav = os.path.join(_TMP, basename + "_cw.wav")
        sy.synth_wav(marked, wav, cfg=sy.SynthConfig(lead_in_s=2.0),
                     duration_s=self.DUR, light_channels=[1, 2, 3, 4])
        exp = le.build_expectation(marked, duration_s=self.DUR + 2.0)
        rd = ow.WavReader(wav)
        n_ch = rd.n_channels
        rd.close()
        cm = ow.parse_channel_map(
            "audioL=1,audioR=2,light1=3,light2=4,light3=5,light4=6", n_ch)
        obs = ow.observe_wav(wav, cm, sync_tone_hz=3000.0)
        rep = cmp_mod.compare(exp, obs,
                              cmp_mod.CompareConfig(sync_tone_hz=3000.0))
        measured = [f.code for f in rep.findings
                    if f.severity in ("error", "warning")
                    and not f.code.startswith("LEDC_")]
        self.assertEqual(rep.sync.confidence, "high", describe(rep))
        self.assertAlmostEqual(rep.sync.t0_s, 2.0, delta=0.010,
                               msg=describe(rep))
        self.assertEqual(measured, [], describe(rep))
        os.remove(wav)
        return rep

    def test_80_clean_fade_in_session(self):
        """01_sleep_onset: a 15 s brightness fade-in and an 18-minute rate
        glide 10 -> 2 Hz. The shape that broke the sync solver."""
        self._run("01_sleep_onset")

    def test_81_clean_steady_isochronic_session(self):
        """05_focus_smr: a DC lamp (freq 0) plus a 300 Hz audio isochronic.

        This is the PULSE_RATE alias case: 2f = 600 Hz against the old
        501.136 Hz envelope rate aliased to 98.86 Hz and was reported as a
        hard error, 'the pulse accumulator is per-sample and sample-exact
        (audio_generator.c:1385), so a rate error here is a value error'.
        """
        rep = self._run("05_focus_smr")
        self.assertNotIn("PULSE_RATE", [f.code for f in rep.findings],
                         describe(rep))

    def test_82_clean_two_channel_panned_binaural(self):
        """04_meditation_theta: two slots on the same carrier, panned, plus a
        brightness LFO. The EVENT_DRIFT and AMPLITUDE phantom case."""
        rep = self._run("04_meditation_theta")
        self.assertNotIn("EVENT_DRIFT", [f.code for f in rep.findings],
                         describe(rep))
        # ...and it must SAY the level was not graded there rather than
        # silently skipping it.
        self.assertIn("AMPLITUDE_NOT_GRADED", [f.code for f in rep.findings],
                      describe(rep))

    def test_83_clean_high_rate_session(self):
        """07_genus_40hz: 40 Hz on four zones, the tool's headline case."""
        self._run("07_genus_40hz")

    def test_84_pulse_rate_is_right_at_every_library_carrier(self):
        """The AM rate must come out right for every carrier the library uses.

        Isolated at the estimator, because the end-to-end path is slow: 940 of
        the 1381 audio entries in sessions/library sit on a carrier whose 2f
        aliased into the 0.4-150 Hz search band on the old envelope
        (200 Hz x272, 250 x131, 300 x80, 204/208 x112, 220 x43), and every one
        of them read a double-digit phantom rate.
        """
        sr = 44100.0
        grid = np.arange(0, int(10 * 20)) / 20.0
        for carrier in (200.0, 204.0, 208.0, 220.0, 250.0, 300.0, 392.0):
            at = ow.AudioTracker(sr, grid, True, False)
            n = int(8.0 * sr)
            t = np.arange(n) / sr
            am = 1.0 - 0.1 + 0.1 * (np.mod(t * 10.0, 1.0) < 0.5)
            x = (np.sin(2 * np.pi * carrier * t) * am * 0.3).astype(np.float64)
            step = 1 << 14
            for i in range(0, n, step):
                at.push(i, x[i:i + step], None)
            got = [f for f in at.pulse_hz if np.isfinite(f)]
            self.assertTrue(got, f"no pulse rate at carrier {carrier}")
            self.assertAlmostEqual(
                float(np.median(got)), 10.0, delta=0.15,
                msg=f"carrier {carrier} Hz -> pulse {np.median(got):.3f} Hz, "
                    f"expected 10.000")


# ---------------------------------------------------------------------------
# 2. firmware-semantics unit tests (hand-written expectations)
# ---------------------------------------------------------------------------


class TestFirmwareSemantics(unittest.TestCase):

    def _parse_one(self, text: str):
        p = os.path.join(_TMP, "frag.ledc")
        with open(p, "w") as fh:
            fh.write(text)
        return p

    def test_30_c_atof_and_atol(self):
        # atof must stop at junk and return 0.0 on no match -- this is exactly
        # how ">250" in the freqR column silently becomes mono.
        self.assertEqual(le.c_atof(">250"), 0.0)
        self.assertEqual(le.c_atof("250.5x"), 250.5)
        self.assertEqual(le.c_atof("abc"), 0.0)
        self.assertEqual(le.c_atol("1000.5"), 1000)
        self.assertEqual(le.c_atol("-"), 0)

    def test_31_u8_cast_is_not_clamped_for_duty(self):
        # config_parser.c:1425 -- duty 300 -> 44, duty 256 -> 0.
        self.assertEqual(le.c_u8(300), 44)
        self.assertEqual(le.c_u8(256), 0)
        # ...but R/G/B ARE clamped (config_parser.c:1384-1394).
        self.assertEqual(le.clamp_u8_field(300), 255)

    def test_32_flicker_quantization_matches_hand_computation(self):
        # 7.83 Hz: f_mHz = 7830, D = 1e9//7830 = 127713 us. Tick =
        # (7830//1000)*250 = 1750 Hz -> T = 1e6//1750 = 571 us. ceil(127713/571)
        # = 224 ticks -> 127904 us -> 7.8184 Hz. Documented in the budget.
        self.assertEqual(dm.flicker_tick_hz_for(7.83), 1750)
        self.assertEqual(dm.flicker_tick_period_us(1750), 571)
        self.assertAlmostEqual(dm.quantized_flicker_hz(7.83, 1750), 7.8184,
                               places=3)
        # Sub-1 Hz falls through the integer divide to the 1000 Hz floor.
        self.assertEqual(dm.flicker_tick_hz_for(0.5), 1000)
        # The realized rate is ONE-SIDED: never above the demanded value.
        for f in (0.5, 1.0, 2.0, 7.83, 10.0, 11.96, 40.0, 99.9):
            tick = dm.flicker_tick_hz_for(f)
            self.assertLessEqual(dm.quantized_flicker_hz(f, tick), f + 1e-9,
                                 f"{f} Hz quantized ABOVE the demand")

    def test_33_quantization_vector_matches_scalar(self):
        fs = np.array([0.0, 0.5, 1.0, 7.83, 10.0, 11.96, 39.0, 40.0, 100.0])
        ticks = np.array([dm.flicker_tick_hz_for(float(f)) for f in fs])
        vec = dm.quantized_flicker_hz_vec(fs, ticks)
        for f, t, v in zip(fs, ticks, vec):
            self.assertAlmostEqual(v, dm.quantized_flicker_hz(float(f), int(t)),
                                   places=9)

    def test_34_a_without_space_misparses(self):
        """config_parser.c:1368-1370 -- token 0 is DISCARDED but the line is
        STILL PARSED, with every remaining field shifted one place left.

        This used to be modelled as a DELETED line, so the expectation omitted
        an entry the device actually executes (typically one that starts a
        channel at a bogus time). The root CLAUDE.md still documents
        `A1500 binaural 40.0 6.0` as the syntax, so a hand-written session is a
        realistic source of this.
        """
        p = self._parse_one("A1500 200 0 50 10 1\n")
        pr = le.parse_ledc(p)
        self.assertTrue(any(f.code == "LEDC_AUDIO_GLUED_TIME"
                            for f in pr.lints))
        self.assertEqual(len(pr.entries), 1, pr.entries)
        e = pr.entries[0]
        # Fields shift left: time<-200, freq<-0, pan<-50, vol<-10, mod<-1,
        # and there is no sixth token so the channel falls back to 0.
        self.assertEqual(e.time_ms, 200)
        self.assertEqual(e.freq.value, 0.0)
        self.assertEqual(e.pan.value, 50.0)
        self.assertEqual(e.vol.value, 10.0)
        self.assertEqual(e.mod.value, 1.0)
        self.assertEqual(e.channel, 0)

    def test_35_dash_in_mask_kills_the_line(self):
        # config_parser.c:1474 -- atoi("-") == 0 and :1513 then rejects.
        p = self._parse_one("1000 10 50 60 255 255 255 -\n")
        pr = le.parse_ledc(p)
        self.assertEqual(pr.entries, [])
        self.assertTrue(any(f.code == "LEDC_LED_MASK_ZERO" for f in pr.lints))

    def test_36_led_token_counts(self):
        # Exactly 5, or 8..12. 6, 7 and 13+ are skipped (config_parser.c:1405).
        for toks, ok in (("1000 10 50 60 1", True),
                         ("1000 10 50 60 255 1", False),
                         ("1000 10 50 60 255 255 1", False),
                         ("1000 10 50 60 255 255 255 1", True),
                         ("1000 10 50 60 255 255 255 1 0 0 0 0", True),
                         ("1000 10 50 60 255 255 255 1 0 0 0 0 0", False)):
            pr = le.parse_ledc(self._parse_one(toks + "\n"))
            self.assertEqual(bool(pr.entries), ok, toks)

    def test_37_legacy_five_token_line_sets_rgb_white(self):
        pr = le.parse_ledc(self._parse_one("1000 10 50 60 15\n"))
        e = pr.entries[0]
        self.assertEqual((e.r.value, e.g.value, e.b.value), (255, 255, 255))
        self.assertTrue(e.present & le.LED_SET_R)
        self.assertEqual(e.mask, 15)

    def test_38_first_batch_hoisting(self):
        # config_parser.c:696 -- the first batch fires at wall-clock 0 whatever
        # its timestamp. Audio is affected; LED is not (its anchor is absolute).
        p = self._parse_one(
            "5000 10 50 60 255 255 255 1\n"
            "A 5000 240 0 50 0 1\n"
            "9000 10 50 60 255 255 255 1\n")
        exp = le.build_expectation(p, duration_s=12.0)
        self.assertTrue(any(f.code == "LEDC_FIRST_BATCH_HOIST"
                            for f in exp.lints))
        aud = [e for e in exp.events if e["kind"] == "audio_start"]
        self.assertEqual(aud[0]["t_eff_ms"], 0)
        led = [e for e in exp.events if e["kind"] == "led_start"]
        self.assertAlmostEqual(led[0]["t_eff_ms"],
                               5000 + dm.DMA_LAG_US / 1000.0, places=2)

    def test_39_batch_cap_drops_entries(self):
        # config_parser.c:698 -- entries 51+ at one timestamp never execute.
        lines = "".join(f"1000 10 50 {b} 255 255 255 1\n" for b in range(60))
        exp = le.build_expectation(self._parse_one(lines), duration_s=3.0)
        self.assertTrue(any(f.code == "LEDC_BATCH_CAP" for f in exp.lints))
        self.assertEqual(sum(1 for s in exp.schedule if not s.executed), 10)

    def test_40_entry_cap_drops_at_parse_time(self):
        lines = "".join(f"{i * 10} 10 50 60 255 255 255 1\n" for i in range(130))
        pr = le.parse_ledc(self._parse_one(lines))
        self.assertEqual(len(pr.entries), dm.MAX_ENTRIES)
        self.assertEqual(pr.dropped_over_cap, 30)

    def test_41_stable_sort_by_time(self):
        # config_parser.c:443 -- the file need not be time-ordered.
        p = self._parse_one(
            "9000 10 50 60 255 255 255 1\n"
            "1000 20 50 60 255 255 255 1\n")
        exp = le.build_expectation(p, duration_s=12.0)
        self.assertEqual([s.t_demanded_ms for s in exp.schedule], [1000, 9000])

    def test_42_audio_before_led_within_a_batch(self):
        # config_parser.c:739 -- three passes by TYPE, not by file order.
        p = self._parse_one(
            "1000 10 50 60 255 255 255 1\n"
            "A 1000 240 0 50 0 1\n"
            "5000 10 50 60 255 255 255 1\n")
        exp = le.build_expectation(p, duration_s=7.0)
        batch = [s.kind for s in exp.schedule if s.t_demanded_ms == 1000]
        self.assertEqual(batch, ["audio", "led"])

    def test_43_animate_on_start_ramp(self):
        # The prefix lives on THIS entry; the target comes from the NEXT one.
        p = self._parse_one(
            "A 0 240 0 >0 0 1\n"
            "A 10000 240 0 80 0 1\n")
        exp = le.build_expectation(p, duration_s=12.0)
        A = exp.audio[1]
        i = lambda t: int(np.searchsorted(A.t, t))  # noqa: E731
        self.assertAlmostEqual(A.amp[i(0.0)], 0.0, places=3)
        self.assertAlmostEqual(A.amp[i(5.0)], 0.4, places=2)
        self.assertAlmostEqual(A.amp[i(10.0)], 0.8, places=2)
        self.assertAlmostEqual(A.amp[i(11.0)], 0.8, places=2)

    def test_44_quadratic_ramp_is_ease_in_out(self):
        p = self._parse_one(
            "A 0 240 0 *0 0 1\n"
            "A 10000 240 0 100 0 1\n")
        exp = le.build_expectation(p, duration_s=12.0)
        A = exp.audio[1]
        # p=0.25 -> 2*0.25^2 = 0.125 ; p=0.5 -> 0.5 ; p=0.75 -> 1-2*0.0625=0.875
        self.assertAlmostEqual(A.amp[int(np.searchsorted(A.t, 2.5))], 0.125,
                               places=2)
        self.assertAlmostEqual(A.amp[int(np.searchsorted(A.t, 5.0))], 0.5,
                               places=2)
        self.assertAlmostEqual(A.amp[int(np.searchsorted(A.t, 7.5))], 0.875,
                               places=2)

    def test_45_ramp_to_zero_trap(self):
        # config_parser.c:2480 -- a core-field ramp reads the next entry's
        # struct member with NO present-bit check, so a '-' there ramps to 0.
        p = self._parse_one(
            "A 0 240 0 >50 0 1\n"
            "A 10000 240 0 - 0 1\n")
        exp = le.build_expectation(p, duration_s=12.0)
        A = exp.audio[1]
        self.assertAlmostEqual(A.amp[int(np.searchsorted(A.t, 10.0))], 0.0,
                               places=2)
        self.assertTrue(any(f.code == "LEDC_RAMP_TO_ZERO" for f in exp.lints))

    def test_46_channels_8_to_15_never_ramp(self):
        # config_parser.c:2465 -- 1u<<8 truncated to uint8_t is 0.
        p = self._parse_one(
            "A 0 240 0 >0 0 11\n"
            "A 10000 240 0 80 0 11\n")
        exp = le.build_expectation(p, duration_s=12.0)
        A = exp.audio[11]
        self.assertAlmostEqual(A.amp[int(np.searchsorted(A.t, 5.0))], 0.0,
                               places=3, msg="ch11 must STEP, not ramp")
        self.assertTrue(any(f.code == "LEDC_AUDIO_CH8_NO_RAMP"
                            for f in exp.lints))
        # ...while channel 7 does ramp.
        p2 = self._parse_one(
            "A 0 240 0 >0 0 7\n"
            "A 10000 240 0 80 0 7\n")
        exp2 = le.build_expectation(p2, duration_s=12.0)
        self.assertAlmostEqual(
            exp2.audio[7].amp[int(np.searchsorted(exp2.audio[7].t, 5.0))],
            0.4, places=2)

    def test_47_wave_type_reset_bug(self):
        # config_parser.c:2358 -- wave_type is latched unconditionally, so a
        # later entry omitting it resets a noise channel to sine.
        p = self._parse_one(
            "A 0 200 0 50 0 3 0 5\n"
            "A 10000 200 0 50 0 3\n"
            "A 20000 200 0 50 0 3\n")
        exp = le.build_expectation(p, duration_s=22.0)
        A = exp.audio[3]
        self.assertEqual(int(A.wave[int(np.searchsorted(A.t, 5.0))]), 5)
        self.assertEqual(int(A.wave[int(np.searchsorted(A.t, 15.0))]), 0)
        self.assertTrue(any(f.code == "LEDC_WAVE_RESET" for f in exp.lints))

    def test_48_carrier_ramp_glides_the_beat(self):
        """main/audio_generator.c:995-1013 -- freq_r is the ABSOLUTE right-ear
        frequency on the SWEEP path too, so ramping the left carrier MOVES the
        beat.

        THIS TEST USED TO ASSERT THE OPPOSITE, and so did the model: the right
        carrier was recomputed as current_freq + (freqR - freq), holding the
        detune and the beat constant across a glide. That is what the firmware
        did until the comment now at :995-1013 was written -- "every
        carrier-sweep line in the shipped library documents an intended beat
        change ... 29 of 83 sessions were silently not performing their central
        move" -- and it is verified on hardware: carrier 200->203 with freqR=210
        glides the beat 9.847 -> 7.000 Hz with freqR pinned at 210.000.
        LEDC_BEAT_NOT_SWEPT, which described the old behaviour and fired at
        error severity on 29 shipped sessions, is gone with it.
        """
        p = self._parse_one(
            "A 0 240 0 55 0 1 250 0\n"
            "A 60000 >240 0 55 0 1 250 0\n"
            "A 120000 248 0 55 0 1 250 0\n")
        exp = le.build_expectation(p, duration_s=130.0)
        A = exp.audio[1]
        g = lambda t: A.beat_hz[int(np.searchsorted(A.t, t))]  # noqa: E731
        r = lambda t: A.freq_r_hz[int(np.searchsorted(A.t, t))]  # noqa: E731
        self.assertAlmostEqual(g(10.0), 10.0, places=3)
        # The carrier moves 240 -> 248 over the ramp; freqR stays pinned at 250,
        # so the beat glides 10 -> 2 and is ~6 Hz at the midpoint.
        self.assertAlmostEqual(A.freq_l_hz[int(np.searchsorted(A.t, 90.0))],
                               244.0, delta=0.3)
        self.assertAlmostEqual(r(90.0), 250.0, places=3,
                               msg="freqR is absolute and must not move")
        self.assertAlmostEqual(g(90.0), 6.0, delta=0.3,
                               msg="the beat must GLIDE with the carrier")
        self.assertAlmostEqual(g(125.0), 2.0, places=3)
        self.assertFalse(any(f.code == "LEDC_BEAT_NOT_SWEPT"
                             for f in exp.lints),
                         "the lint described behaviour that no longer exists")

    def test_48b_final_led_entry_is_cancelled(self):
        # config_parser.c:2104-2111 -- the end-of-timeline stop runs in the same
        # dispatch as the last batch, before the final entry's cycle anchor
        # arrives, so that entry never emits an edge.
        p = self._parse_one(
            "0 10 50 60 255 255 255 1\n"
            "5000 10 50 60 255 255 255 1\n")
        exp = le.build_expectation(p, duration_s=7.0)
        self.assertTrue(any(f.code == "LEDC_LAST_ENTRY_CANCELLED"
                            for f in exp.lints), [f.code for f in exp.lints])
        L = exp.light[1]
        self.assertTrue(L.active[int(np.searchsorted(L.t, 2.0))])
        self.assertFalse(L.active[int(np.searchsorted(L.t, 6.0))])
        # ...and NOT flagged when the session ends with brightness 0, which is
        # what every real session does.
        p2 = self._parse_one(
            "0 10 50 60 255 255 255 1\n"
            "5000 10 50 0 255 255 255 1\n")
        exp2 = le.build_expectation(p2, duration_s=7.0)
        self.assertFalse(any(f.code == "LEDC_LAST_ENTRY_CANCELLED"
                             for f in exp2.lints))

    def test_49_freqr_prefix_collapses_to_mono(self):
        p = self._parse_one("A 0 240 0 55 0 1 >250 0\n")
        exp = le.build_expectation(p, duration_s=3.0)
        self.assertAlmostEqual(exp.audio[1].beat_hz[10], 0.0, places=6)
        self.assertTrue(any(f.code == "LEDC_FREQR_PREFIX" for f in exp.lints))

    def test_50_led_freq_zero_stops_flicker(self):
        # config_parser.c:2729 -- freq <= 0 means STOP, not DC-on.
        p = self._parse_one(
            "0 10 50 60 255 255 255 1\n"
            "5000 0 50 60 255 255 255 1\n"
            "9000 10 50 60 255 255 255 1\n")
        exp = le.build_expectation(p, duration_s=11.0)
        L = exp.light[1]
        self.assertTrue(L.active[int(np.searchsorted(L.t, 2.0))])
        self.assertFalse(L.active[int(np.searchsorted(L.t, 6.0))])

    def test_51_led_above_100hz_is_rejected(self):
        # led_matrix_example.c:1314 -- the entry does nothing at all.
        p = self._parse_one(
            "0 120 50 60 255 255 255 1\n"
            "5000 120 50 60 255 255 255 1\n")
        exp = le.build_expectation(p, duration_s=7.0)
        self.assertTrue(any(f.code == "LEDC_LED_REJECTED" for f in exp.lints))
        self.assertFalse(exp.light[1].active.any())

    def test_52_modulation_syntax_defaults(self):
        self.assertEqual(le.parse_mod_extras("~25"), (25.0, 1000.0))
        self.assertEqual(le.parse_mod_extras("~25:30"), (30.0, 1000.0))
        self.assertEqual(le.parse_mod_extras("~25:30:8000"), (30.0, 8000.0))

    def test_53_sine_modulation_shape(self):
        # One full period is the complete start->end->start cycle.
        p = self._parse_one(
            "0 10 50 ~20:40:10000 255 255 255 1\n"
            "60000 10 50 20 255 255 255 1\n")
        exp = le.build_expectation(p, duration_s=62.0)
        L = exp.light[1]
        anchor = dm.DMA_LAG_US / 1e6
        g = lambda t: L.bright_pct[int(np.searchsorted(L.t, t))]  # noqa: E731
        # LED uses the parabola 4p(1-p); at p=0 it is the start value and at
        # p=0.5 the end value.
        self.assertAlmostEqual(g(10.0 + anchor), 20.0, delta=1.5)
        self.assertAlmostEqual(g(15.0 + anchor), 40.0, delta=1.5)
        self.assertAlmostEqual(g(20.0 + anchor), 20.0, delta=1.5)

    def test_54_speech_is_stripped_by_default(self):
        # serialize.js:159-163 -- the browser strips S rows before POSTing, so
        # the device never sees them.
        p = self._parse_one(
            "0 10 50 60 255 255 255 1\n"
            'S 1000 default 10 "hello"\n'
            "5000 10 50 60 255 255 255 1\n")
        self.assertEqual(len(le.parse_ledc(p).entries), 2)
        self.assertEqual(len(le.parse_ledc(p, keep_speech=True).entries), 3)

    def test_55_line_truncation_at_255_bytes(self):
        pad = "#" + "x" * 300
        p = self._parse_one(f"1000 10 50 60 255 255 255 1 {pad}\n")
        pr = le.parse_ledc(p)
        self.assertTrue(any(f.code == "LEDC_LINE_TRUNCATED" for f in pr.lints))

    def _sim(self, text: str):
        pr = le.parse_ledc(self._parse_one(text))
        sched, sim, sched_lints = le.build_model(pr.entries)
        return pr, sched, sim, list(pr.lints) + sched_lints + sim.lints

    def test_57_audio_ramp_starts_from_the_live_value(self):
        """audio_generator_start_sweep_locked DISCARDS the caller's literal.

        audio_generator.c:519-551 overwrites `start` with ch->current_freq /
        current_amp / current_pan / current_mod_freq whenever ch->active, and
        fill_buffer only re-derives current_* while the sweep's
        duration_samples == 0 (:831-855), so the armed sweep wins permanently.
        config_parser.c:2487 does pass audio->frequency; it never takes effect.

        The model used to use the FILE LITERAL, which for
        `A 0 200 0 20 0 0` / `A 30000 200 0 >60 0 0` / `A 90000 200 0 40 0 0`
        predicted amp stepping 0.20 -> 0.60 at t=30 s and then ramping to 0.40,
        while the device ramps 0.20 -> 0.40 across the whole window: a ~9.5 dB
        discrepancy at the step instant, reported as a device fault on
        bit-perfect playback.
        """
        pr, sched, sim, lints = self._sim(
            "A 0 200 0 20 0 0\n"
            "A 30000 200 0 >60 0 0\n"
            "A 90000 200 0 40 0 0\n")
        kfs = sim.aud_kfs[0]
        sw = kfs[1].sweeps["vol"]
        self.assertAlmostEqual(sw.v0, 20.0, places=6,
                               msg="ramp must start from the LIVE 20, not the "
                                   "file's 60")
        self.assertAlmostEqual(sw.v1, 40.0, places=6)
        # Halfway through the window the value is the midpoint of 20 -> 40.
        self.assertAlmostEqual(le._scalar(kfs[1], "vol", 60.0), 30.0, delta=0.1)
        self.assertIn("LEDC_RAMP_START_SUBSTITUTED", [f.code for f in lints])

    def test_57b_fresh_volume_ramp_starts_from_silence(self):
        """start_channel_locked sets current_amp = 0 and active = true
        (audio_generator.c:386, :477) BEFORE config_parser arms the sweep, so a
        FIRST-entry volume ramp starts from silence, not from the literal."""
        pr, sched, sim, lints = self._sim(
            "A 0 200 0 >35 0 0\n"
            "A 20000 200 0 70 0 0\n")
        sw = sim.aud_kfs[0][0].sweeps["vol"]
        self.assertAlmostEqual(sw.v0, 0.0, places=6)
        self.assertAlmostEqual(sw.v1, 70.0, places=6)
        # ...but a FREQ ramp on a fresh channel DOES start from the literal,
        # because current_freq is latched from params first.
        pr2, s2, sim2, l2 = self._sim(
            "A 0 >150 0 50 0 0\n"
            "A 20000 300 0 50 0 0\n")
        self.assertAlmostEqual(sim2.aud_kfs[0][0].sweeps["freq"].v0, 150.0,
                               places=6)

    def test_57c_pulse_field_ramps_do_start_from_the_literal(self):
        """The pulse fields are the one place the literal survives -- do not
        "fix" them to match the core fields.

        config_parser.c:2395-2402 writes this entry's duty/phase/attack into
        the channel (audio_generator_set_iso_channel / _set_phase) BEFORE
        arming the sweep at :2605, and the live-value substitution at
        audio_generator.c:541-549 then reads those same fields back -- so it
        returns the literal. EXCEPT duty 0, which audio_generator.c:210
        refuses to write (`duty_pct > 0.0f`).
        """
        pr, sched, sim, lints = self._sim(
            "A 0 200 0 50 8 0 0 0 30\n"
            "A 10000 200 0 50 8 0 0 0 >70\n"
            "A 30000 200 0 50 8 0 0 0 90\n")
        sw = sim.aud_kfs[0][1].sweeps["duty"]
        self.assertAlmostEqual(sw.v0, 70.0, places=6,
                               msg="pulse duty ramp starts from its LITERAL")
        self.assertAlmostEqual(sw.v1, 90.0, places=6)

    def test_58_led_tick_comes_from_the_spec_literal(self):
        """led_matrix_example.c:1936-1938 sizes the ISR tick from
        spec->freq_milliHz_start (the FILE literal, config_parser.c:2838), not
        from the live value the interpolator ramps from.

        Taking the live value kept a 2500 Hz tick for a channel running at
        10 Hz that receives a `>40` entry, so the model expected 39.68 Hz while
        the device raises the tick to 10000 Hz and emits 40.000 Hz -- a 0.32 Hz
        error reported against a correct device.
        """
        exp = le.build_expectation(self._parse_one(
            "0 10 50 60 255 255 255 1\n"
            "20000 >40 50 60 255 255 255 1\n"
            "60000 40 50 60 255 255 255 1\n"
            "90000 40 50 0 255 255 255 1\n"), grid_hz=4.0, duration_s=95.0)
        L = exp.light[1]
        i = int(np.searchsorted(L.t, 70.0))
        self.assertAlmostEqual(float(L.tick_hz[i]), 10000.0, places=1)
        self.assertAlmostEqual(float(L.emitted_hz[i]), 40.0, delta=1e-6)

    def test_59_out_of_range_led_entry_executes_on_the_sweep_path(self):
        """The freq>100 / duty>100 / bright>100 guards exist ONLY in
        start_flicker_masked (led_matrix_example.c:1314-1325) and
        update_flicker_params_masked (:1588-1594). start_sweep_masked
        (:1920-1939) validates nothing but duration_ms and init_freq != 0, and
        config_parser.c routes a bit to the sweep path whenever any field
        carries `>`. So a ramped out-of-range entry DOES take effect."""
        # No ramp -> rejected, and the channel keeps doing what it was doing.
        exp = le.build_expectation(self._parse_one(
            "0 10 50 60 255 255 255 1\n"
            "20000 150 50 60 255 255 255 1\n"
            "40000 10 50 0 255 255 255 1\n"), grid_hz=4.0, duration_s=45.0)
        self.assertIn("LEDC_LED_REJECTED", [f.code for f in exp.lints])
        i = int(np.searchsorted(exp.light[1].t, 30.0))
        self.assertAlmostEqual(float(exp.light[1].demanded_hz[i]), 10.0,
                               delta=1e-6, msg="a rejected entry is a no-op")
        # With a ramp -> NOT rejected: it executes at the out-of-range rate.
        exp2 = le.build_expectation(self._parse_one(
            "0 10 50 60 255 255 255 1\n"
            "20000 150 50 >60 255 255 255 1\n"
            "40000 10 50 0 255 255 255 1\n"), grid_hz=4.0, duration_s=45.0)
        codes2 = [f.code for f in exp2.lints]
        self.assertNotIn("LEDC_LED_REJECTED", codes2)
        self.assertIn("LEDC_LED_OUT_OF_RANGE", codes2)
        j = int(np.searchsorted(exp2.light[1].t, 30.0))
        self.assertAlmostEqual(float(exp2.light[1].demanded_hz[j]), 150.0,
                               delta=1e-6)

    def test_59b_stop_retains_the_rate_for_a_later_dash(self):
        """led_matrix_stop_flicker_masked (led_matrix_example.c:1532-1545)
        clears only led_state, led_dirty and active. frequency_milliHz,
        duty_cycle, brightness and rgb are RETAINED, and that retained value is
        what led_matrix_get_snapshot (:2184) reports and what
        config_parser.c:2667 substitutes for a '-' column.

        Zeroing the rate made the model resolve such a '-' to 0 = stop, keep
        the LEDs modelled dark, and then report a correctly-flickering channel
        as "never turned on"."""
        exp = le.build_expectation(self._parse_one(
            "0 12 50 60 255 255 255 1\n"
            "10000 0 50 60 255 255 255 1\n"
            "20000 - 50 60 255 255 255 1\n"
            "40000 0 50 0 255 255 255 1\n"), grid_hz=4.0, duration_s=45.0)
        L = exp.light[1]
        i = int(np.searchsorted(L.t, 30.0))
        self.assertTrue(bool(L.active[i]),
                        "the '-' must restart at the RETAINED 12 Hz")
        self.assertAlmostEqual(float(L.demanded_hz[i]), 12.0, delta=1e-6)
        self.assertNotIn("LEDC_DASH_FREQ_STOPS", [f.code for f in exp.lints])

    def test_59c_dash_freqr_reads_the_live_right_carrier(self):
        """config_parser.c:2346 reads audio_generator_get_current_freq_r_locked
        -> ch->current_freq_r (audio_generator.c:1792-1796).

        ON A BINAURAL CHANNEL that is now the CONFIGURED value: every assignment
        of current_freq_r writes params.frequency_r itself (:382, :854, :959,
        :1011, :1630), so a carrier sweep does not move it and the read-back is
        the literal. This test used to assert current_freq + (freqR - freq),
        which is the detune-preserving form the firmware no longer uses.
        """
        pr, sched, sim, lints = self._sim(
            "A 0 100 0 50 0 0 110\n"
            "A 10000 >100 0 50 0 0 110\n"
            "A 40000 300 0 50 0 0 -\n"
            "A 70000 300 0 0 0 0 -\n")
        st = sim.aud_kfs[0][2].state
        self.assertAlmostEqual(st["freq_r"], 110.0, delta=1e-6)
        self.assertAlmostEqual(st["freq"], 300.0, delta=1e-6)

    def test_59c2_dash_freqr_on_a_mono_channel_latches_the_frozen_carrier(self):
        """ON A MONO CHANNEL (params.frequency_r == 0) current_freq_r is set to
        params.frequency at start (audio_generator.c:382) and refreshed on a
        params latch (:854), but both refresh sites are gated on
        `params.frequency_r > 0.0f` or skipped while a frequency sweep is armed
        (:852-855). So while a carrier ramp is in flight current_freq_r is
        FROZEN at the pre-sweep carrier, and a '-' freqR landing mid-ramp
        latches THAT -- turning the channel binaural with a beat equal to the
        part of the carrier move completed so far.
        """
        pr, sched, sim, lints = self._sim(
            "A 0 100 0 50 0 0\n"
            "A 10000 >100 0 50 0 0\n"
            "A 40000 300 0 50 0 0 -\n"
            "A 70000 300 0 0 0 0 -\n")
        st = sim.aud_kfs[0][2].state
        self.assertAlmostEqual(st["freq_r"], 100.0, delta=1e-6,
                               msg="the frozen PRE-SWEEP carrier, not the live "
                                   "one and not the swept target")

    def test_59d_bg_line_tolerates_extra_tokens(self):
        """config_parser.c:1707-1710 fails only on token_count < 3 and ignores
        extras, so `BG http://x 0 50 note` is ACCEPTED on the device."""
        pr = le.parse_ledc(self._parse_one(
            "BG http://example.com/a.mp3 0 50 note\n"
            "0 10 50 60 255 255 255 1\n"))
        self.assertIsNotNone(pr.bg)
        self.assertEqual(pr.bg.url, "http://example.com/a.mp3")

    def test_59e_entry_cap_counts_only_lines_the_device_accepts(self):
        """config_parser.c:395-420 consumes a slot only when parse_line returns
        ESP_OK, so rejected lines (bad token count, mask 0) and browser-stripped
        S rows must not count against the 100-entry cap."""
        body = []
        for i in range(100):
            body.append(f"{i * 100} 10 50 60 255 255 255 1")
        # 20 lines the device REJECTS outright, then one it accepts.
        body += [f"{10000 + i} 10 50 60 -" for i in range(20)]
        body.append("20000 10 50 60 255 255 255 2")
        pr = le.parse_ledc(self._parse_one("\n".join(body) + "\n"))
        self.assertEqual(len(pr.entries), 100)
        # The accepted 101st entry is the ONLY one over the cap.
        cap = [f for f in pr.lints if f.code == "LEDC_ENTRY_CAP"]
        self.assertTrue(cap)
        self.assertIn("1 entry", cap[0].message)

    def test_59g_ramp_target_comes_from_a_batch_capped_entry(self):
        """find_next_audio_for_bit (config_parser.c:2187-2196) scans the RAW
        sorted array, so it takes a ramp TARGET from an entry the 50-per-batch
        cap prevents from ever executing. Requiring `executed` changed the
        ramp's endpoint outright, not by a rounding step."""
        body = ["A 0 200 0 >20 0 0", "0 10 50 60 255 255 255 1"]
        # 55 entries all at t=30000, with the audio one LAST in file order: the
        # stable sort keeps it at position 55 of that batch, past the
        # 50-per-batch cap, so it never executes -- but it is still in the
        # array find_next_audio_for_bit scans.
        body += [f"30000 10 50 {30 + i % 5} 255 255 255 1" for i in range(54)]
        body.append("A 30000 200 0 80 0 0")
        pr, sched, sim, lints = self._sim("\n".join(body) + "\n")
        dropped = [s for s in sched if not s.executed]
        self.assertTrue(dropped, "expected the batch cap to drop entries")
        self.assertEqual(dropped[-1].kind, "audio",
                         [(s.kind, s.t_demanded_ms) for s in dropped])
        self.assertIn("LEDC_BATCH_CAP", [f.code for f in lints])
        sw = sim.aud_kfs[0][0].sweeps["vol"]
        self.assertAlmostEqual(sw.v1, 80.0, places=6,
                               msg="the ramp target must come from the raw "
                                   "array even if that entry never runs")

    def test_59h_led_phase_and_attack_are_integers(self):
        """config_parser.c:1493,1501 store phase as (uint16_t)wrap_deg(d) and
        attack as (uint16_t)(a<0?0:a), and led_matrix_set_attack caps attack at
        60 ms (led_matrix_example.c:1435)."""
        pr = le.parse_ledc(self._parse_one(
            "1000 10 50 60 255 255 255 1 0 123.7 250.4 7.5:30000\n"))
        e = pr.entries[0]
        self.assertEqual(e.phase.value, 123.0)
        # ATTACK IS NOT CAPPED AT PARSE TIME. led_matrix_set_attack_masked caps
        # at 60 ms (main/led_matrix_example.c:1497) but
        # led_matrix_start_sweep_masked writes spec->attack_start /
        # attack_target straight into s->sw_attack (:2063-2067) with no cap, so
        # a RAMPED attack interpolates through uncapped endpoints. The cap lives
        # on the setter path, in _apply_led_pulse_fields.
        self.assertEqual(e.attack.value, 250.0)
        self.assertEqual(e.jitter_amp_hz, 5.0)       # 7.5 -> capped at 5 Hz
        self.assertEqual(e.jitter_period_ms, 30000.0)
        # A sub-1000 ms jitter period is REFUSED, leaving the default.
        pr2 = le.parse_ledc(self._parse_one(
            "1000 10 50 60 255 255 255 1 0 0 3 2:500\n"))
        self.assertEqual(pr2.entries[0].jitter_period_ms, 45000.0)

    def test_59f_jitter_amplitude_survives_a_glyph(self):
        """config_parser.c:111-116 routes the jitter amplitude through
        parse_value_with_interpolation, which STRIPS a leading glyph, so `>5`
        is 5.0 on the device and was 0.0 in the model."""
        self.assertEqual(le.parse_jitter_token(">5", 45000.0), (5.0, 45000.0))
        self.assertEqual(le.parse_jitter_token("5:30000", 45000.0),
                         (5.0, 30000.0))

    def test_56_led_dash_uses_the_low_bit_live_value(self):
        # config_parser.c:2651-2676 -- '-' resolves from the LOWEST set bit.
        p = self._parse_one(
            "0 10 50 60 255 255 255 1\n"
            "0 40 50 30 255 255 255 2\n"
            "5000 - - - - - - 3\n"
            "9000 10 50 60 255 255 255 3\n")
        exp = le.build_expectation(p, duration_s=11.0)
        i = int(np.searchsorted(exp.light[2].t, 6.0))
        # Channel 2 inherits channel 1's 10 Hz / 60%, not its own 40 Hz / 30%.
        self.assertAlmostEqual(exp.light[2].demanded_hz[i], 10.0, places=3)
        self.assertAlmostEqual(exp.light[2].bright_pct[i], 60.0, places=0)


# ---------------------------------------------------------------------------
# 3. operational properties
# ---------------------------------------------------------------------------


class TestOperational(unittest.TestCase):

    def test_60_channel_map_forms(self):
        cm = ow.parse_channel_map("audioL=1,audioR=2,light1=3,light4=4", 4)
        self.assertEqual(cm.audio_l, 0)
        self.assertEqual(cm.audio_r, 1)
        self.assertEqual(cm.lights, {1: 2, 4: 3})
        cm2 = ow.parse_channel_map("AL,AR,L1,L4", 4)
        self.assertEqual((cm2.audio_l, cm2.audio_r, cm2.lights),
                         (0, 1, {1: 2, 4: 3}))
        cm3 = ow.parse_channel_map("audioL=1,light1=!2", 2)
        self.assertIn(1, cm3.invert)
        with self.assertRaises(ValueError):
            ow.parse_channel_map("audioL=9", 4)
        with self.assertRaises(ValueError):
            ow.parse_channel_map("bogus=1", 4)

    def test_61_inverted_light_channel_is_handled(self):
        rep, meta, exp, obs = run_case("clean")
        wav = os.path.join(_TMP, "clean.wav")
        cm = ow.parse_channel_map("audioL=1,audioR=2,light1=!3", 4)
        obs2 = ow.observe_wav(wav, cm)
        # Inverting a correct channel must NOT yield a plausible-looking rate:
        # the duty complements and the edges swap, so the tool would report a
        # duty error rather than silently agreeing.
        self.assertGreater(obs2.light[1].edges_rise.size, 10)
        i = int(np.searchsorted(obs2.t_rec, rep.sync.t0_s + 20.0))
        self.assertAlmostEqual(float(obs2.light[1].freq_hz[i]), 10.0, delta=0.1)

    def test_62_memory_ceiling(self):
        """A 60-minute recording must not be loaded into RAM.

        Rendering AND analysing a 5-minute 4-channel file in a subprocess and
        reading ru_maxrss gives a real ceiling. Memory is bounded by
        --block-frames, not by file length: the only array that grows with
        duration is the optional 1 kHz float32 audio level series (14 MB per
        hour) plus the edge lists (~6 MB per hour at 100 Hz).
        """
        script = os.path.join(_TMP, "mem.py")
        wav = os.path.join(_TMP, "mem.wav")
        with open(script, "w") as fh:
            fh.write(f"""
import sys, resource
sys.path.insert(0, {ROOT!r})
import synth as sy, ledc_expect as le, observe_wav as ow, compare as cmp_mod
sy.synth_wav({SESSION!r}, {wav!r}, duration_s=300.0)
exp = le.build_expectation({SESSION!r}, duration_s=302.0)
cm = ow.parse_channel_map({MAP!r}, 4)
obs = ow.observe_wav({wav!r}, cm)
cmp_mod.compare(exp, obs)
mb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
mb = mb / (1024*1024) if mb > 1 << 24 else mb / 1024
print(f"PEAK_RSS_MB={{mb:.1f}}")
""")
        r = subprocess.run([sys.executable, script], capture_output=True,
                           text=True, timeout=900)
        self.assertEqual(r.returncode, 0, r.stderr[-3000:])
        line = [x for x in r.stdout.split("\n") if "PEAK_RSS_MB" in x]
        self.assertTrue(line, r.stdout)
        mb = float(line[0].split("=")[1])
        print(f"\n    peak RSS for a 5-minute 4-channel analysis: {mb:.1f} MB")
        self.assertLess(mb, 250.0, f"peak RSS {mb:.1f} MB is too high")

    def test_63_cli_end_to_end(self):
        wav = os.path.join(_TMP, "cli.wav")
        jsn = os.path.join(_TMP, "cli.json")
        cli = os.path.join(ROOT, "analyze.py")
        r = subprocess.run(
            [sys.executable, cli, "synth", "--ledc", SESSION, "--out", wav,
             "--duration", "40"], capture_output=True, text=True, timeout=600)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        r = subprocess.run(
            [sys.executable, cli, "analyze", "--ledc", SESSION, "--wav", wav,
             "--map", MAP, "--quiet", "--json", jsn],
            capture_output=True, text=True, timeout=600)
        self.assertIn(r.returncode, (0, 1), r.stderr[-2000:])
        self.assertIn("SYNC", r.stdout)
        import json
        with open(jsn) as fh:
            doc = json.load(fh)
        self.assertEqual(doc["sync"]["confidence"], "high", r.stdout)
        self.assertEqual(doc["counts"]["error"], 0, json.dumps(doc, indent=2))
        # The 40 s render covers only part of the 90 s session, so the ONLY
        # warning allowed is the one that says exactly that.
        codes_seen = [f["code"] for f in doc["findings"]
                      if f["severity"] in ("error", "warning")]
        self.assertEqual(codes_seen, ["RECORDING_INCOMPLETE"],
                         json.dumps(doc, indent=2))

    def test_64_lint_runs_on_the_whole_shipped_library(self):
        """The static linter must not crash on any real session, and must find
        the known defects in the Gateway sessions."""
        import glob
        lib = os.path.join(ROOT, "..", "..", "sessions", "library")
        files = sorted(glob.glob(os.path.join(lib, "*.ledc")))
        if not files:
            self.skipTest("session library not present")
        seen = set()
        for p in files:
            exp = le.build_expectation(p, grid_hz=2.0, duration_s=60.0)
            seen.update(f.code for f in exp.lints)
        print(f"\n    linted {len(files)} shipped sessions; "
              f"codes seen: {sorted(seen)}")
        self.assertIn("LEDC_AUDIO_CH8_NO_RAMP", seen)
        # LEDC_BEAT_NOT_SWEPT is deliberately absent: the firmware change at
        # main/audio_generator.c:995-1013 made it describe behaviour that no
        # longer exists, and it was firing at error severity on 29 of these 83
        # files. See test_48_carrier_ramp_glides_the_beat.
        self.assertNotIn("LEDC_BEAT_NOT_SWEPT", seen)

    def test_66_operator_error_exits_2_not_1(self):
        """A typo must not share an exit code with "the device is wrong".

        Before: main() was `return a.func(a)`, so a mistyped --wav path printed
        twelve lines of stack ending in FileNotFoundError and exited 1 -- the
        same code as a real device finding, and a script could not tell them
        apart. The messages were already good; they were buried.
        """
        cli = os.path.join(ROOT, "analyze.py")
        wav = os.path.join(_TMP, "cli.wav")
        if not os.path.exists(wav):
            r0 = subprocess.run(
                [sys.executable, cli, "synth", "--ledc", SESSION, "--out", wav,
                 "--duration", "40"], capture_output=True, text=True,
                timeout=600)
            self.assertEqual(r0.returncode, 0, r0.stderr[-2000:])
        for args, what in (
                (["analyze", "--ledc", SESSION, "--wav", "/nope/missing.wav",
                  "--map", MAP, "--quiet"], "missing wav"),
                (["analyze", "--ledc", "/nope/missing.ledc", "--wav", wav,
                  "--map", MAP, "--quiet"], "missing ledc"),
                (["analyze", "--ledc", SESSION, "--wav", wav,
                  "--map", "bogusrole=1", "--quiet"], "bad role"),
                (["analyze", "--ledc", SESSION, "--wav", wav,
                  "--map", "audioL=0,audioR=1", "--quiet"], "0-based map"),
                (["analyze", "--ledc", SESSION, "--wav", wav,
                  "--map", "audioL=1,audioR=1", "--quiet"], "duplicate index"),
        ):
            r = subprocess.run([sys.executable, cli] + args,
                               capture_output=True, text=True, timeout=600)
            self.assertEqual(r.returncode, 2,
                             f"{what}: exit {r.returncode}\n{r.stderr[-2000:]}")
            self.assertIn("error:", r.stderr, what)
            self.assertNotIn("Traceback", r.stderr, what)
        # The 0-based mistake must be NAMED, not merely rejected.
        r = subprocess.run(
            [sys.executable, cli, "analyze", "--ledc", SESSION, "--wav", wav,
             "--map", "audioL=0,audioR=1", "--quiet"],
            capture_output=True, text=True, timeout=600)
        self.assertIn("1-based", r.stderr)

    def test_67_exit_code_tracks_the_faults_the_tool_exists_to_find(self):
        """The README's own quick-start fault demo must not exit 0.

        Before: EVENT_LATE, EVENT_EARLY and AV_CONST_OFFSET were all warning
        severity and `n_err` counted only errors, so `synth --fault offset:120
        --fault late:30000:60` then `analyze` printed a real 129 ms A/V desync
        plus a 58 ms late event and returned EXIT=0. Anyone checking $? was
        told a broken device was clean.
        """
        cli = os.path.join(ROOT, "analyze.py")
        wav = os.path.join(_TMP, "exitcode.wav")
        r = subprocess.run(
            [sys.executable, cli, "synth", "--ledc", SESSION, "--out", wav,
             "--duration", "100", "--fault", "offset:120",
             "--fault", "late:30000:60"],
            capture_output=True, text=True, timeout=900)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        r = subprocess.run(
            [sys.executable, cli, "analyze", "--ledc", SESSION, "--wav", wav,
             "--map", MAP, "--quiet", "--sync-tone", "3000"],
            capture_output=True, text=True, timeout=900)
        self.assertEqual(r.returncode, 1,
                         f"the README's fault demo exited {r.returncode}\n"
                         + r.stdout[-4000:])
        self.assertIn("AV_CONST_OFFSET", r.stdout)
        # --strict must additionally escalate warnings. A 40 s capture of a
        # 90 s session is warning-only (RECORDING_INCOMPLETE) and clean
        # otherwise, so it exits 0 normally and 1 under --strict.
        short = os.path.join(_TMP, "short.wav")
        subprocess.run(
            [sys.executable, cli, "synth", "--ledc", SESSION, "--out", short,
             "--duration", "40"], capture_output=True, text=True, timeout=600)
        base = [sys.executable, cli, "analyze", "--ledc", SESSION,
                "--wav", short, "--map", MAP, "--quiet"]
        r_plain = subprocess.run(base, capture_output=True, text=True,
                                 timeout=600)
        r_strict = subprocess.run(base + ["--strict"], capture_output=True,
                                  text=True, timeout=600)
        self.assertEqual(r_plain.returncode, 0, r_plain.stdout[-3000:])
        self.assertEqual(r_strict.returncode, 1, r_strict.stdout[-3000:])
        os.remove(wav)
        os.remove(short)

    def test_68_given_t0_is_not_labelled_high_when_it_disagrees(self):
        """An unverified hand-typed t0 must not get the strongest label.

        Before: SyncSolution was built with confidence='high',
        uncertainty_ms=0.0 UNCONDITIONALLY, so the report said "HIGH (+/-0 ms)"
        on the same screen as "t0 supplied on the command line; not verified",
        and no comparison against the solved value was made even though the
        solver had already run.
        """
        run_case("clean")
        wav = os.path.join(_TMP, "clean.wav")
        exp = le.build_expectation(SESSION, duration_s=DURATION + 2.0)
        cm = ow.parse_channel_map(MAP, 4)
        obs = ow.observe_wav(wav, cm, sync_tone_hz=3000.0)
        good = cmp_mod.compare(exp, obs, cmp_mod.CompareConfig(
            t0_s=2.0, sync_tone_hz=3000.0))
        self.assertEqual(good.sync.method, "given")
        self.assertIn("AGREES", good.sync.detail)
        bad = cmp_mod.compare(exp, obs, cmp_mod.CompareConfig(
            t0_s=3.5, sync_tone_hz=3000.0))
        self.assertNotEqual(bad.sync.confidence, "high", describe(bad))
        self.assertGreater(bad.sync.uncertainty_ms, 1000.0, describe(bad))
        self.assertIn("ONE OF THE TWO IS WRONG", bad.sync.detail)

    def test_69_block_frames_help_matches_the_measurement(self):
        """The CLI help must not contradict the measured memory behaviour.

        It used to promise "peak RSS is ~28 MB of interpreter plus roughly
        1.7 KB per frame of block", i.e. 35 MB at 4096 frames, while the
        README said the opposite and the truth (measured on a 1.908 GB /
        6 ch / 60-minute file) is 298 MB at the default and 302 MB at 4096 --
        the knob makes it very slightly worse.
        """
        import analyze as an
        p = an.build_parser()
        txt = ""
        for act in p._subparsers._group_actions[0].choices["analyze"]._actions:
            if "--block-frames" in (act.option_strings or []):
                txt = act.help or ""
        self.assertTrue(txt)
        self.assertNotIn("1.7 KB per frame", txt)
        self.assertIn("does NOT bound peak memory", txt)

    def test_65_findings_are_deduped(self):
        f = cmp_mod._dedupe([
            __import__("timeline").Finding(code="X", severity="warning",
                                           message="m", line_no=3, t_ms=10.0),
            __import__("timeline").Finding(code="X", severity="warning",
                                           message="m", line_no=9, t_ms=20.0),
        ])
        self.assertEqual(len(f), 1)
        self.assertIn("2 occurrences", f[0].message)
        self.assertIn("lines 3-9", f[0].message)



# ---------------------------------------------------------------------------
# ROUND 3. Every behaviour change in the round-3 fix pass, as a test that
# fails on the previous tree and passes on this one. The acceptance harness
# (tests/acceptance.py) is the gate; these are the unit-level anchors for the
# individual mechanisms it exercises, so a regression says WHICH one broke.
# ---------------------------------------------------------------------------


class TestRound3Estimators(unittest.TestCase):
    """Pure-function tests. No render, so they run in milliseconds."""

    SR = 44100.0

    def _refine(self, f_true, n_s=0.25, am_hz=0.0, am_depth=0.0,
                harmonics=()):
        n = int(n_s * self.SR)
        t = np.arange(n) / self.SR
        x = np.sin(2 * np.pi * f_true * t + 0.7)
        for k, a in harmonics:
            x += a * np.sin(2 * np.pi * k * f_true * t + 0.3 * k)
        if am_hz > 0:
            x *= 1.0 - am_depth * (0.5 + 0.5 * np.sin(2 * np.pi * am_hz * t))
        # f0 the way AudioTracker._tone derives it: parabolic interpolation of
        # the Hann-windowed FFT peak. Starting from a value a third of a bin off
        # is the realistic input, and it is the one whose residual image the
        # period snapping is supposed to null.
        sp = np.abs(np.fft.rfft(x * np.hanning(n)))
        fr = np.fft.rfftfreq(n, 1.0 / self.SR)
        k = int(np.argmax(sp[1:])) + 1
        a, b, c = (20 * math.log10(sp[k - 1] + 1e-30),
                   20 * math.log10(sp[k] + 1e-30),
                   20 * math.log10(sp[k + 1] + 1e-30))
        den = a - 2 * b + c
        d = 0.0 if den == 0 else max(-0.5, min(0.5, 0.5 * (a - c) / den))
        f0 = float(fr[k] + d * (fr[1] - fr[0]))
        return ow._refine_frequency(x, self.SR, f0)

    def test_70_refine_frequency_is_unbiased_under_am(self):
        """The negative-frequency image used to tilt the phase fit.

        MEASURED on full-length clean renders of 15_ganzflicker_imagery and
        30_splitfield_lucid, carrier 200 Hz against a 200.000062 Hz truth from a
        40 s FFT: -51.8 and -60.0 ppm, which WAS `clock_ppm_audio` and produced
        AV_DIFFERENTIAL_DRIFT at error severity on bit-perfect recordings.
        """
        got = self._refine(200.0, am_hz=10.0, am_depth=0.1)
        self.assertTrue(math.isfinite(got))
        self.assertLess(abs(got - 200.0) / 200.0 * 1e6, 5.0,
                        f"200 Hz + 10 Hz AM -> {got!r}")

    def test_71_refine_frequency_survives_harmonics(self):
        """`wave 7` (EEG contour) is harmonic-rich; 180 library entries use it.

        Snapping the sub-block to a whole number of HALF carrier periods nulls
        only the even images, so odd harmonics still tilted the fit.
        """
        got = self._refine(200.0, harmonics=((2, 0.4), (3, 0.25)))
        self.assertTrue(math.isfinite(got))
        self.assertLess(abs(got - 200.0) / 200.0 * 1e6, 20.0, repr(got))

    def test_72_refine_frequency_works_at_a_low_carrier(self):
        """A fixed 8 sub-blocks gave up below ~140 Hz and fell back to the raw
        FFT peak, whose interpolation error is ~1% of a 4 Hz bin.

        MEASURED on 20_gateway_focus10 (binaural 100 / 101.5 Hz): the left ear
        read +0.4 ppm because 100 Hz lands exactly on a bin, the right ear
        -540 ppm. 112 library entries sit on 100-104 Hz.
        """
        got = self._refine(101.5)
        self.assertTrue(math.isfinite(got), "declined at 101.5 Hz")
        self.assertLess(abs(got - 101.5) / 101.5 * 1e6, 60.0, repr(got))

    def test_73_pulse_of_prefers_the_fundamental(self):
        """A 4%-duty gate's first 1/d harmonics are nearly equal in amplitude,
        so which one is the argmax is decided by noise.

        MEASURED: 80.000 Hz where 40.000 was expected (27_genus_40hz_dim under
        -60 dBFS of noise), 119.982 vs 40.000 (07_genus_40hz at -40 dBFS),
        36.000 vs 6.000 (09_lucid_hypnagogic, clean).
        """
        fs = 1000.0
        t = np.arange(int(8 * fs)) / fs
        gate = ((t * 6.0) % 1.0 < 0.1).astype(np.float64)   # 6 Hz, 10% duty
        env = 1.0 - 0.1 * gate
        f, _d = ow.AudioTracker._pulse_of(env[:int(4 * fs)], fs)
        self.assertTrue(math.isfinite(f))
        self.assertAlmostEqual(f, 6.0, delta=0.15, msg=f"got {f!r}")

    def test_74_pulse_of_interpolates_at_the_band_edge(self):
        """The parabola used the SEARCH SUB-BAND, so a peak in its first bin got
        no correction at all and was snapped to the bin centre: MEASURED
        "0.500 Hz where 0.574 Hz was expected" on 18_jhana_absorption.
        """
        fs = 1000.0
        n = int(4 * fs)
        t = np.arange(n) / fs
        env = 1.0 - 0.1 * (0.5 + 0.5 * np.sin(2 * np.pi * 0.574 * t))
        f, _d = ow.AudioTracker._pulse_of(env, fs)
        self.assertTrue(math.isfinite(f))
        self.assertNotAlmostEqual(f, 0.5, places=3)
        self.assertAlmostEqual(f, 0.574, delta=0.06, msg=f"got {f!r}")

    def test_75_grid_phase_fix_centres_a_step_template(self):
        """A 20 Hz demanded series puts a step's linear-interp midpoint half a
        cell early, which a template match pays back as lateness: MEASURED
        +26.4 ms on 90_measure_sync's marker-tone stop, against a -10..+30 ms
        budget.
        """
        t_dem = np.arange(0, 10.0, 0.05)
        for t_ev in (0.60, 0.62, 1.00, 3.075):
            fix = cmp_mod._grid_phase_fix(t_dem, t_ev)
            # the template's transition midpoint, after the fix, is t_ev
            i_prev = math.ceil(t_ev / 0.05 - 1e-9) - 1
            mid = t_dem[i_prev] + 0.025 + fix
            self.assertAlmostEqual(mid, t_ev, places=9)

    def _shift(self, y_dem, y_obs, lo=-0.5, hi=0.5, dt=0.001):
        t = np.arange(0.0, 4.0, dt)

        def tmpl(tt, lag):
            return np.interp(tt - lag, t, y_dem, left=y_dem[0], right=y_dem[-1])
        return cmp_mod._shift_fit(t, y_obs, tmpl, lo, hi, dt * 4)

    def test_76_shift_fit_locates_a_step_and_a_corner(self):
        dt = 0.001
        t = np.arange(0.0, 4.0, dt)
        step = (t >= 2.0).astype(np.float64)
        lag, q, loc, edge, sig, info, b, bj = self._shift(step, np.interp(
            t - 0.12, t, step, left=0.0, right=1.0))
        self.assertTrue(loc and not edge, (lag, q, loc, edge))
        self.assertAlmostEqual(lag, 0.12, delta=0.01)
        self.assertLess(sig, 0.05)
        corner = np.clip((t - 2.0) / 1.0, 0.0, 1.0)
        lag, q, loc, edge, sig, info, b, bj = self._shift(corner, np.interp(
            t + 0.20, t, corner, left=0.0, right=1.0))
        self.assertTrue(loc and not edge, (lag, q, loc, edge))
        self.assertAlmostEqual(lag, -0.20, delta=0.03)

    def test_77_shift_fit_declines_a_featureless_ramp(self):
        """Shifting a straight line in time is exactly absorbed by the fitted
        offset, so every lag fits identically and the argmin is the search grid.
        Returning it anyway is how a mid-ramp entry got a confident timing.
        """
        dt = 0.001
        t = np.arange(0.0, 4.0, dt)
        ramp = t / 4.0
        lag, q, loc, edge, sig, info, b, bj = self._shift(ramp, ramp)
        # A straight ramp is unidentifiable: the IDENTIFIABILITY of the
        # demanded trajectory, not the goodness of fit, is what says so.
        self.assertLess(info, cmp_mod._TRAJ_INFO_MIN, (lag, q, info))

    def test_78_fit_edge_grid_is_unbiased_across_gaps(self):
        """A threshold detector loses edges during the dim half of a brightness
        LFO, so the edge list arrives in bursts; indexing across the gaps off a
        biased interval mean accumulated a period error.

        MEASURED on a zero-drift render of 91_measure_17_uniform: all four zones
        fitted +220 ppm, published as "AV_DIFFERENTIAL_DRIFT -221 ppm ... the
        LIGHT timebase is the one that moved", against +2 ppm from an
        independent 900 s spectral measurement of the same recording.
        """
        T = 0.1
        ks = np.concatenate([np.arange(0, 60), np.arange(100, 160),
                             np.arange(200, 260), np.arange(300, 360)])
        rng = np.random.default_rng(7)
        sel = 1.0 + ks * T + rng.normal(0.0, 0.0005, ks.size)
        A, Tf, lock, se = cmp_mod._fit_edge_grid(sel, T)
        self.assertTrue(math.isfinite(Tf), "declined a gapped grid outright")
        self.assertLess(abs(Tf / T - 1.0) * 1e6, 30.0,
                        f"fitted {Tf!r} for a true {T}")


class TestRound3Model(unittest.TestCase):
    """ledc_expect fidelity items, as hand-written expectations."""

    def _exp(self, text: str, **kw):
        p = os.path.join(_TMP, "r3.ledc")
        with open(p, "w") as fh:
            fh.write(text)
        return le.build_expectation(p, **kw)

    def test_80_carrier_sweep_glides_the_beat(self):
        """main/audio_generator.c:995-1013: freq_r is the ABSOLUTE right-ear
        frequency on the sweep path too, so sweeping the left carrier MOVES the
        beat. The model used to hold the detune constant
        (current_freq + (frequency_r - frequency)), and the matching
        LEDC_BEAT_NOT_SWEPT lint fired at error severity on 29 of the 83 shipped
        sessions, describing behaviour that no longer exists.
        """
        exp = self._exp("A 0 200 0 70 0 1 210 0\n"
                        "A 1000 >200 0 70 0 1 210 0\n"
                        "A 11000 203 0 70 0 1 210 0\n", duration_s=14.0)
        A = exp.audio[1]
        def at(t):
            i = int(np.searchsorted(A.t, t))
            return float(A.freq_l_hz[i]), float(A.freq_r_hz[i]), float(A.beat_hz[i])
        l0, r0, b0 = at(1.2)
        l1, r1, b1 = at(10.8)
        self.assertAlmostEqual(r0, 210.0, places=3)
        self.assertAlmostEqual(r1, 210.0, places=3, msg="freqR must stay pinned")
        self.assertGreater(b0 - b1, 2.0,
                           f"beat must glide: {b0:.3f} -> {b1:.3f} Hz")
        self.assertAlmostEqual(b1, 7.0, delta=0.3)
        self.assertNotIn("LEDC_BEAT_NOT_SWEPT", [f.code for f in exp.lints])

    def test_81_isr_tick_resets_when_every_channel_stops(self):
        """led_matrix_stop_flicker_masked -> s_maybe_teardown_timer_and_task
        deletes the gptimer (main/led_matrix_example.c:1271-1285) and the next
        s_ensure_timer_and_task re-initialises s_flicker_tick_hz to
        LED_FLICKER_TICK_MIN (:1219). The model had the tick "only ever rising",
        which the README's own 40 Hz marker recipe then pinned at 10 kHz for the
        whole session.
        """
        exp = self._exp("0 40 50 100 255 255 255 15\n"
                        "600 0 50 0 0 0 0 15\n"
                        "1000 7 50 80 0 0 255 15\n", duration_s=6.0)
        L = exp.light[1]
        i = int(np.searchsorted(L.t, 3.0))
        self.assertEqual(int(L.tick_hz[i]), 1750,
                         "tick must be re-derived from 7 Hz after the teardown")
        self.assertAlmostEqual(float(L.emitted_hz[i]),
                               dm.quantized_flicker_hz(7.0, 1750), places=4)

    def test_82_stop_retains_the_LIVE_value_not_the_ramps_start(self):
        """The firmware publishes the interpolated value at every cycle boundary
        (main/led_matrix_example.c:893-904) and the stop clears only
        led_state/led_dirty/active (:1532-1545), so an interrupted ramp retains
        where it GOT TO. Freezing the sweep's v0 left a later '-' column reading
        the pre-ramp value -- 20% against the device's 90%, on an 8-point DUTY
        threshold.
        """
        # The last entry on a channel is cancelled by the end-of-timeline stop
        # (config_parser.c:2104-2111), so the '-' restart needs a later entry
        # for its state to last any time at all.
        exp = self._exp("0 10 >20 80 255 255 255 1\n"
                        "6000 10 90 80 255 255 255 1\n"
                        "6500 0 50 0 0 0 0 1\n"
                        "11000 - - - - - - 1\n"
                        "19000 10 90 80 255 255 255 1\n", duration_s=22.0)
        L = exp.light[1]
        i = int(np.searchsorted(L.t, 13.0))
        self.assertTrue(bool(L.active[i]), "the '-' entry must restart it")
        self.assertAlmostEqual(float(L.duty_pct[i]), 90.0, delta=1.0,
                               msg=f"retained duty {L.duty_pct[i]!r}")

    def test_83_led_attack_cap_is_in_the_setter_not_the_parser(self):
        """led_matrix_set_attack_masked caps at 60 ms
        (main/led_matrix_example.c:1497); led_matrix_start_sweep_masked writes
        spec->attack_start/attack_target straight into s->sw_attack
        (:2063-2067) with no cap, so a RAMPED attack interpolates through
        uncapped endpoints.
        """
        p = os.path.join(_TMP, "atk.ledc")
        with open(p, "w") as fh:
            fh.write("0 10 50 80 255 255 255 1 3 0 200\n")
        ents = le.parse_ledc(p).entries if hasattr(le, "parse_ledc") else None
        # Parse through the public path and inspect the stored cell.
        exp = le.build_expectation(p, duration_s=4.0)
        self.assertTrue(exp.light)
        # the stepped (setter) value is capped...
        sched = [e for e in exp.schedule if e.kind == "led"]
        self.assertEqual(sched[0].raw.attack.value, 200.0,
                         "the PARSER must not clamp; the setter does")



class TestRound3TrajectoryUncertainty(unittest.TestCase):
    """The two things that made a bit-perfect render look late or early:
    a breakpoint lost to the demand's sampling grid, and an SSE valley that
    describes a precision the residual does not support."""

    def test_90_grid_sharpen_restores_a_ramp_end(self):
        """A ramp that ends BETWEEN two grid samples: linear interpolation of
        the samples does not reach the final level until the next one, so the
        template lags the recording by up to a whole cell. On
        21_gateway_focus12_TRAP that read as -252 ms (EVENT_EARLY, error, on a
        bit-perfect render)."""
        dt = 0.4
        t = np.arange(0.0, 20.0, dt)
        t_ev = 11.07                      # deliberately off-grid
        true = lambda x: np.minimum(np.maximum(x, 0.0), t_ev) / t_ev
        y = true(t)
        # BEFORE: the sampled series reaches 1.0 only at the first grid point
        # at or after t_ev, i.e. late by t_next - t_ev.
        i_next = int(np.searchsorted(t, t_ev))
        self.assertLess(float(np.interp(t_ev, t, y)), 0.995)
        self.assertAlmostEqual(float(np.interp(t[i_next], t, y)), 1.0, places=9)
        # AFTER: exact on both sides of the breakpoint.
        t2, (y2,) = cmp_mod._grid_sharpen(t, [y], t_ev)
        for probe in (t_ev - 1.3, t_ev - 0.2, t_ev - 0.01, t_ev,
                      t_ev + 0.05, t_ev + 0.9):
            self.assertAlmostEqual(float(np.interp(probe, t2, y2)),
                                   float(true(probe)), places=6,
                                   msg=f"at t={probe}")

    def test_91_grid_sharpen_leaves_an_on_grid_breakpoint_alone(self):
        t = np.arange(0.0, 10.0, 0.05)
        y = np.clip(t / 4.0, 0.0, 1.0)
        t2, ys = cmp_mod._grid_sharpen(t, [y], 4.0)
        self.assertIs(ys[0], y)
        self.assertEqual(t2.size, t.size)

    def test_92_jackknife_half_width_sees_a_correlated_residual(self):
        """A lag fitted against a residual that is smooth on the scale of the
        shift is not reproducible, and the SSE valley cannot say so: with 4000
        samples it claims tens of ms for a lag that moves by hundreds when a
        sixth of the window is dropped."""
        dt = 0.002
        t = np.arange(0.0, 8.0, dt)
        corner = np.clip((t - 4.0) / 3.0, 0.0, 1.0)

        def tmpl(tt, lag):
            return np.interp(tt - lag, t, corner, left=0.0, right=1.0)

        # (a) a real shift, white noise: both half-widths small, and they agree
        # about a measurement that IS there.
        rng = np.random.default_rng(7)
        y_ok = tmpl(t, 0.25) + rng.standard_normal(t.size) * 0.01
        lag, q, loc, edge, sig, info, b, sj = cmp_mod._shift_fit(
            t, y_ok, tmpl, -1.0, 1.0, 0.01)
        self.assertAlmostEqual(lag, 0.25, delta=0.05)
        self.assertLess(max(sig, sj), 0.15, (sig, sj))
        # (b) NO shift, but a ripple in the residual on the scale of the
        # feature -- an isochronic AM, or a noise bed's own level wander. The
        # fit still returns a lag, the valley still looks tight, and twice the
        # valley would accuse the device. Only the jackknife covers it.
        y_bad = tmpl(t, 0.0) + 0.03 * np.sin(2 * np.pi * t / 3.0)
        lag, q, loc, edge, sig, info, b, sj = cmp_mod._shift_fit(
            t, y_bad, tmpl, -1.0, 1.0, 0.01)
        self.assertGreater(abs(lag), 2 * sig,
                           f"the valley ({sig:.3f}) is supposed to be fooled "
                           f"by the fitted lag ({lag:.3f})")
        self.assertGreater(sj, abs(lag),
                           f"jackknife {sj:.3f} must cover the fitted "
                           f"{abs(lag):.3f}; valley says {sig:.3f}")


class TestRound3BrightnessModulation(unittest.TestCase):
    """`~a:b:period` breathing: presence, and WHERE the finding lands.

    Built from synthetic series rather than a render, because the point is the
    statistic's placement and its two thresholds, and a 25-minute render would
    hide both behind the front-end.
    """

    def _series(self, dem, obs_level, grid_hz=20.0):
        import timeline as tl
        n = dem.size
        t = np.arange(n) / grid_hz
        L = tl.ExpectedLight(
            channel=1, t=t, active=np.ones(n, dtype=bool),
            demanded_hz=np.full(n, 10.0), emitted_hz=np.full(n, 10.0),
            duty_pct=np.full(n, 50.0), bright_pct=dem,
            r=np.full(n, 200.0), g=np.zeros(n), b=np.full(n, 200.0),
            env=np.zeros(n, dtype=int), phase_deg=np.zeros(n),
            tick_hz=np.full(n, 1000.0))
        O = tl.ObservedLight(
            channel=1, t_rec=t, level=obs_level,
            contrast=np.full(n, 0.5), freq_hz=np.full(n, 10.0),
            duty_pct=np.full(n, 50.0),
            edges_rise=np.zeros(0), edges_fall=np.zeros(0))
        exp = tl.Expectation(source="synthetic", grid_hz=grid_hz, t=t,
                             schedule=[], light={1: L}, audio={},
                             mix=tl.ExpectedMix(
                                 t=t, dominant_l_hz=np.full(n, np.nan),
                                 dominant_r_hz=np.full(n, np.nan),
                                 beat_hz=np.full(n, np.nan),
                                 pulse_hz=np.full(n, np.nan),
                                 rms_rel=np.zeros(n),
                                 active=np.zeros(n, dtype=bool),
                                 n_active=np.zeros(n, dtype=int),
                                 dominant_channel=np.full(n, -1)),
                             bg=None, duration_s=float(t[-1]))
        obs = tl.Observation(source="synthetic", front_end="synth",
                             grid_hz=grid_hz, t_rec=t, duration_s=float(t[-1]),
                             light={1: O}, audio=None)
        return cmp_mod._check_light_modulation(
            exp, obs, cmp_mod.CompareConfig(), 0.0)

    def _lfo(self, t, lo, hi, period):
        mid, half = 0.5 * (lo + hi), 0.5 * (hi - lo)
        return mid + half * np.sin(2 * np.pi * t / period)

    def test_86_compressed_but_present_lfo_is_not_a_finding(self):
        """The front-end measures 0.45-0.65 of the demanded depth on a PERFECT
        render (observe_wav.py:367-380 + :457-461: in the dim half of the swing
        the blended upper trigger goes unreachable and the ON-segment mean is
        pulled up). Grading that as a depth error grades the estimator --
        MEASURED on a clean 04_meditation_theta, whose `~10:30:10000` read 9
        points peak-to-peak against the 20 demanded.
        """
        t = np.arange(0, 120 * 20) / 20.0
        dem = self._lfo(t, 10.0, 30.0, 10.0)
        lvl = 0.02 * self._lfo(t, 16.5, 29.5, 10.0)   # 0.65x depth, 2% gain
        self.assertEqual([f.code for f in self._series(dem, lvl)], [])

    def test_87_absent_lfo_fires_at_the_entry_not_a_window_later(self):
        """A centred running window puts the finding w seconds after the entry
        that caused it; the acceptance gate places a fault within 8 s, and the
        library's LFO periods (6-30 s) force w >= 15. Hence the FORWARD window
        (_shift_envelope_fwd)."""
        t = np.arange(0, 240 * 20) / 20.0
        dem = self._lfo(t, 10.0, 30.0, 10.0)
        lvl = 0.02 * self._lfo(t, 16.5, 29.5, 10.0)
        lvl = np.where(t < 120.0, lvl, 0.02 * 23.0)   # breathing stops at 120 s
        got = self._series(dem, lvl)
        self.assertEqual([f.code for f in got], ["BRIGHTNESS_MODULATION"],
                         str([(f.code, f.t_ms) for f in got]))
        self.assertAlmostEqual(got[0].t_ms, 120000.0, delta=1500.0,
                               msg=f"reported at {got[0].t_ms:.0f} ms")

    def test_88_breathing_where_a_steady_value_is_demanded(self):
        """The other direction: the entry that was supposed to DROP the LFO
        never ran, so the device keeps breathing. This is the drop/late case on
        36_shamanic_trance_drum, whose t=1441000 entry hands the zone from
        `~18:30:12000` to a steady 26%."""
        t = np.arange(0, 120 * 20) / 20.0
        dem = np.full(t.size, 26.0)
        lvl = 0.02 * self._lfo(t, 18.0, 30.0, 12.0)
        got = self._series(dem, lvl)
        self.assertEqual([f.code for f in got], ["BRIGHTNESS_MODULATION"],
                         str([(f.code, f.t_ms) for f in got]))

    def test_89_steady_demand_steady_measurement_is_silent(self):
        t = np.arange(0, 120 * 20) / 20.0
        dem = np.full(t.size, 26.0)
        lvl = np.full(t.size, 0.02 * 26.0) + 0.0005 * np.sin(2 * np.pi * t / 7.0)
        self.assertEqual([f.code for f in self._series(dem, lvl)], [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
