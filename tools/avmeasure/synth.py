"""Synthesise a multi-channel recording from a `.ledc`, with optional faults.

WHY THIS EXISTS
---------------
There is no way to validate a measurement instrument against hardware without
already trusting the instrument. So: generate a recording whose ground truth is
known by construction, optionally break it in one specific, quantified way, and
assert that the engine finds exactly that break and nothing else. A detector
that cries wolf on a clean recording is worse than useless, so "clean -> no
findings" is as important a test as any fault case.

HONESTY ABOUT CIRCULARITY
-------------------------
The renderer drives off the same keyframe state machine as ledc_expect, so
these tests validate observe_wav.py and compare.py -- NOT the fidelity of the
expectation model to the firmware. That fidelity rests on the file:line
citations in ledc_expect.py and on `analyze.py expect --dump`, which prints the
demanded model for a human to check against the session. This is stated again
in README LIMITATIONS; it is the single most important caveat about the tool.

What the renderer DOES model independently of the sampled expectation:
  * per-sample carrier phase accumulation (so a frequency ramp is a true chirp,
    not a staircase)
  * the firmware's quantized flicker period, including the one-sided rate bias
  * the +46.439 ms LED anchor and the audio generator/DMA pipeline delay
  * a PWM optical carrier above the flicker rate, so the front-end's low-pass
    has something real to remove
  * the 10% AM depth the timeline hard-codes
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

import devicemodel as dm
import ledc_expect as le
from wavio import WavWriter

# ---------------------------------------------------------------------------
# Fault specification
# ---------------------------------------------------------------------------


@dataclass
class Faults:
    """Each field is one independently-injectable, quantified defect."""

    av_offset_ms: float = 0.0
    """Extra audio delay relative to light, ON TOP of the physical pipeline
    offset. Positive = audio lags light further. Negative = audio leads, which
    is physically impossible on a healthy device."""

    clock_ppm: float = 0.0
    """Recorder crystal error, applied to BOTH domains equally. This is what a
    real recorder does and is NOT a device bug -- the comparator must fit it
    out before reporting drift."""

    audio_extra_ppm: float = 0.0
    """Differential drift: audio clock vs light clock. This one IS a device
    bug (the device cannot do it -- both derive from the same 40 MHz crystal)
    and must be reported."""

    drop_times_ms: tuple[int, ...] = ()
    """Entries at these timestamps are removed from the rendered model, so the
    recording behaves as if they never fired."""

    delay_ms: dict[int, float] = field(default_factory=dict)
    """{timestamp_ms: delay_ms} -- entries at that timestamp happen late."""

    light_freq_scale: dict[int, float] = field(default_factory=dict)
    """{led_channel: factor} -- that channel flickers at the wrong rate."""

    audio_freq_scale: float = 1.0
    """All audio carriers scaled, i.e. a wrong tone frequency."""

    beat_drift_hz_per_1000s: float = 0.0
    """The binaural DETUNE grows linearly, i.e. the beat frequency drifts.

    One of the four example outputs in the brief is "beat frequency drifted
    +0.3 Hz over 20 minutes", and compare.py's BEAT_DRIFT detector had no
    injector at all -- so it had never been exercised in EITHER direction, and
    an unexercised detector in a measurement instrument is an unknown rather
    than a feature. The firmware holds the detune constant through a carrier
    ramp (audio_generator.c:958-961), so a growing detune is exactly the
    "carriers not tracking each other" fault the finding names.
    """

    dead_lights: tuple[int, ...] = ()
    """LED channels that never light up at all."""

    noise_db: float = -90.0
    """White noise RMS level in dBFS added to every channel."""

    light_hp_hz: float = 0.0
    """Simulate an AC-coupled sensor input (an audio interface high-passes
    around 5-20 Hz). 0 = DC-coupled, which is what the recommended rig uses."""


@dataclass
class SynthConfig:
    sample_rate: int = 44100
    lead_in_s: float = 2.0
    """Silence before device t=0, so the sync solver has a real problem to
    solve rather than being handed t0 = 0."""
    tail_s: float = 1.0
    pwm_hz: float = 430.0
    """WS2812 internal intensity-PWM rate. Above every flicker rate we care
    about, so the front-end's low-pass must remove it."""
    light_amp: float = 0.45
    audio_master: float = 8.0
    """Compensates the firmware's fixed 1/16 mix headroom so the synthetic WAV
    sits at a sane level. It is a RELATIVE scale; absolute dBFS is
    unobservable anyway (devicemodel.AUDIO_HEADROOM)."""
    block_s: float = 0.25
    sensor_noise: float = 0.0015
    seed: int = 12345


# ---------------------------------------------------------------------------
# Keyframe views
# ---------------------------------------------------------------------------


class _View:
    """Evaluate one channel's keyframe list at arbitrary sample times."""

    def __init__(self, kfs: list[le.Keyframe]):
        self.kfs = kfs
        self.times = np.array([kf.t_s for kf in kfs]) if kfs else np.zeros(0)

    def _groups(self, t: np.ndarray):
        if self.times.size == 0:
            return []
        idx = np.searchsorted(self.times, t, side="right") - 1
        out = []
        for i in np.unique(idx):
            out.append((int(i), idx == i))
        return out

    def field(self, name: str, t: np.ndarray, default: float = 0.0) -> np.ndarray:
        out = np.full(t.size, default, dtype=np.float64)
        for i, m in self._groups(t):
            if i < 0:
                continue
            out[m] = self.kfs[i].value(name, t[m])
        return out

    def state_int(self, name: str, t: np.ndarray, default: int = 0) -> np.ndarray:
        out = np.full(t.size, default, dtype=np.int32)
        for i, m in self._groups(t):
            if i >= 0:
                out[m] = int(self.kfs[i].state.get(name, default))
        return out

    def active(self, t: np.ndarray) -> np.ndarray:
        """Active AND past this activation's cycle anchor.

        The anchor wait is per-activation (a stopped-and-restarted LED channel
        gets a new one), so it has to come from the keyframe, not from a single
        session-wide value.
        """
        out = np.zeros(t.size, dtype=bool)
        for i, m in self._groups(t):
            if i < 0:
                continue
            st = self.kfs[i].state
            if not st["active"]:
                continue
            vis = st.get("vis_from")
            out[m] = (t[m] >= vis) if vis is not None else True
        return out

    def anchors(self) -> list[float]:
        return [kf.state["vis_from"] for kf in self.kfs
                if kf.state.get("vis_from") not in (None, float("inf"))]

    def vis_at(self, t_s: float) -> float:
        """The activation anchor in force at `t_s`, or NaN."""
        if self.times.size == 0:
            return float("nan")
        i = int(np.searchsorted(self.times, t_s, side="right")) - 1
        if i < 0:
            return float("nan")
        st = self.kfs[i].state
        if not st.get("active"):
            return float("nan")
        v = st.get("vis_from")
        if v is None or not math.isfinite(v):
            return float("nan")
        return float(v)


# ---------------------------------------------------------------------------
# Carriers
# ---------------------------------------------------------------------------


def _audio_carrier(wave: int, phase_cycles: np.ndarray,
                   rng: np.random.Generator, noise_state: dict) -> np.ndarray:
    """audio_generator.h:42-55 wave types, as a recording would see them."""
    p = np.mod(phase_cycles, 1.0)
    if wave == 0:
        return np.sin(2.0 * np.pi * p)
    if wave == 1:
        return np.where(p < 0.5, 1.0, -1.0)
    if wave == 2:
        return np.where(p < 0.5, 4.0 * p - 1.0, 3.0 - 4.0 * p)
    if wave == 3:
        return 2.0 * p - 1.0
    if wave == 4:
        return rng.standard_normal(p.size) * 0.5
    if wave in (5, 6):
        # Spectrally-shaped white: 1/sqrt(f) for pink, 1/f for brown. Done in
        # the frequency domain because a recursive one-pole would be a Python
        # loop over every sample -- 50M iterations for a 20-minute render.
        # The block boundary is not perfectly continuous, but a noise BED is
        # never the quantity under measurement; it exists so the audio
        # front-end has to cope with a real noise floor.
        n = p.size
        wn = np.fft.rfft(rng.standard_normal(n))
        k = np.arange(wn.size)
        k[0] = 1
        expo = 0.5 if wave == 5 else 1.0
        y = np.fft.irfft(wn / k ** expo, n=n)
        s = float(np.std(y))
        return (y / s) * 0.5 if s > 0 else y
    # wave 7 (EEG contour) is not modelled; a sine keeps the carrier
    # measurable so the test does not silently pass on silence.
    return np.sin(2.0 * np.pi * p)


def _pulse_gain(env: int, mod_phase: np.ndarray, duty: np.ndarray,
                attack_frac: np.ndarray, depth: float) -> np.ndarray:
    """audio_generator.c:137-184, 1366-1386.

    Unipolar gates (env 0..3): sample *= (1-depth) + depth*g
    Bipolar tremolo (env 4):   sample *= 1 + depth*sin
    With the timeline's hard-coded depth of 0.1 this is a ~0.9 dB swing, which
    is why observe_wav's AM detector has a low prominence bar.
    """
    p = np.mod(mod_phase, 1.0)
    if env == 4:
        return 1.0 + depth * np.sin(2.0 * np.pi * p)
    d = np.clip(duty, 0.0, 1.0)
    if env == 0:
        g = (p < d).astype(np.float64)
    elif env == 2:
        g = np.where(p < 0.5, 2.0 * p, 2.0 - 2.0 * p)
    elif env == 3:
        a = np.minimum(attack_frac, d * 0.5)
        g = np.zeros_like(p)
        inside = p < d
        with np.errstate(invalid="ignore", divide="ignore"):
            rise = np.where(a > 0, p / np.maximum(a, 1e-12), 1.0)
            fall = np.where(a > 0, (d - p) / np.maximum(a, 1e-12), 1.0)
        g = np.where(inside, np.clip(np.minimum(rise, fall), 0.0, 1.0), 0.0)
    else:  # env 1 = raised sine, 4p(1-p)
        g = np.clip(4.0 * p * (1.0 - p), 0.0, 1.0)
    return (1.0 - depth) + depth * g


def _led_carrier(env: int, cycle_phase: np.ndarray, duty: np.ndarray,
                 attack_frac: np.ndarray) -> np.ndarray:
    """led_matrix_example.c:608-645, 920-972. Returns 0..1 intensity."""
    p = np.mod(cycle_phase, 1.0)
    d = np.clip(duty, 0.0, 1.0)
    if env == 0:
        return (p < d).astype(np.float64)
    if env == 2:
        return np.where(p < 0.5, 2.0 * p, 2.0 - 2.0 * p)
    if env == 3:
        a = np.minimum(attack_frac, d * 0.5)
        with np.errstate(invalid="ignore", divide="ignore"):
            rise = np.where(a > 0, p / np.maximum(a, 1e-12), 1.0)
            fall = np.where(a > 0, (d - p) / np.maximum(a, 1e-12), 1.0)
        return np.where(p < d, np.clip(np.minimum(rise, fall), 0.0, 1.0), 0.0)
    # env 1: sine, using the firmware's parabola so the duty-vs-shape mismatch
    # (sine ignores duty) is reproduced rather than papered over.
    return np.clip(4.0 * p * (1.0 - p), 0.0, 1.0)


# ---------------------------------------------------------------------------
# Fault application to the entry list
# ---------------------------------------------------------------------------


def _mutate_entries(entries: list, faults: Faults) -> list:
    out = []
    for e in entries:
        if e.time_ms in faults.drop_times_ms:
            continue
        d = faults.delay_ms.get(e.time_ms)
        if d:
            # Shift the whole entry later. NOTE: on a real device a dispatch
            # delay would NOT move the LED anchor (it is absolute), so this is
            # a stronger fault than reality. As a test of the DETECTOR that is
            # fine and is stated in the README.
            import copy
            e = copy.deepcopy(e)
            e.time_ms = int(e.time_ms + round(d))
        out.append(e)
    return out


# ---------------------------------------------------------------------------
# Renderer
# ---------------------------------------------------------------------------


def synth_wav(ledc_path: str, out_path: str, *, faults: Faults | None = None,
              cfg: SynthConfig | None = None, duration_s: float | None = None,
              keep_speech: bool = False,
              led_backend: str = dm.DEFAULT_LED_BACKEND,
              light_channels: list[int] | None = None,
              realistic_latency: bool = True) -> dict:
    """Render `ledc_path` to `out_path`. Returns a metadata dict (incl. --map)."""
    faults = faults or Faults()
    cfg = cfg or SynthConfig()
    sr = cfg.sample_rate
    rng = np.random.default_rng(cfg.seed)

    pr = le.parse_ledc(ledc_path, keep_speech=keep_speech)
    entries = _mutate_entries(pr.entries, faults)
    sched, sim, _ = le.build_model(entries, led_backend=led_backend)

    last_ms = max((s.t_demanded_ms for s in sched), default=0)
    dev_dur = (duration_s if duration_s is not None
               else last_ms / 1000.0 + cfg.tail_s)

    led_list = light_channels if light_channels is not None else sorted(
        ch + 1 for ch in range(dm.NUM_LED_CHANNELS) if sim.led_kfs[ch])
    if not led_list:
        led_list = [1]
    aud_list = sorted(ch for ch in range(dm.NUM_AUDIO_CHANNELS) if sim.aud_kfs[ch])

    led_views = {ch: _View(sim.led_kfs[ch - 1]) for ch in led_list}
    aud_views = {ch: _View(sim.aud_kfs[ch]) for ch in aud_list}

    n_out = 2 + len(led_list)
    chmap = ["audioL=1", "audioR=2"] + [
        f"light{ch}={i + 3}" for i, ch in enumerate(led_list)]

    # Physical pipeline offsets, so a CLEAN synthetic recording lands inside the
    # expected band instead of looking like a bug.
    if realistic_latency:
        aud_delay_s = dm.AUDIO_ONSET_DELAY_MS / 1000.0
        led_delay_s = dm.led_edge_delay_ms(led_backend) / 1000.0
    else:
        aud_delay_s = 0.0
        led_delay_s = 0.0
    aud_delay_s += faults.av_offset_ms / 1000.0

    total_s = cfg.lead_in_s + dev_dur + cfg.tail_s
    block_n = max(64, int(cfg.block_s * sr))
    n_total = int(total_s * sr)

    # Per-channel phase accumulators, carried across blocks so a swept carrier
    # is phase-continuous (a per-block phase reset would show up as a click and
    # would corrupt the phase-slope frequency refinement).
    aud_phase = {ch: 0.0 for ch in aud_list}
    aud_phase_r = {ch: 0.0 for ch in aud_list}
    aud_modphase = {ch: 0.0 for ch in aud_list}
    aud_fade = {ch: 0.0 for ch in aud_list}
    noise_state = {ch: {} for ch in aud_list}
    led_phase = {ch: 0.0 for ch in led_list}
    # Which activation anchor each channel's flicker phase is currently zeroed
    # on. The firmware resets the cycle origin ONLY on a fresh activation
    # (led_matrix_example.c:1995-2000) and that origin is the logical anchor
    # t_ms + 46.439 ms (config_parser.c:2699-2702), so the renderer has to do
    # the same or it disagrees with the expectation by up to one flicker period.
    # MEASURED before this was fixed -- first post-dark rising edge minus
    # anchor: +0.4 ms at 10 Hz (selftest's rate, aligned by luck with its
    # 600/1000 ms boundaries), +45.4 ms at 11 Hz, +31.2 ms at 13 Hz. So
    # changing ONLY the body rate of an otherwise identical fault-free session
    # produced spurious EVENT_LATE, and a clean render of the shipped
    # test2c.ledc produced "EVENT_LATE ch4 t=40000 34 ms". The whole
    # "clean -> zero findings" property rested on one session's harmonic
    # coincidence.
    led_anchor = {ch: None for ch in led_list}
    pwm_phase = 0.0
    hp_state = {ch: (0.0, 0.0) for ch in led_list}

    noise_amp = 10.0 ** (faults.noise_db / 20.0)
    clip_warn = 0

    # Split render blocks at every keyframe so the per-block enum reads (env,
    # wave, ISR tick) are exact rather than "whatever was in force at the start
    # of the block". A session has a few hundred keyframes, so this costs
    # nothing and removes a whole class of off-by-one-block artefacts that
    # would look like real timing bugs in the test suite.
    splits = set()
    for ch, v in led_views.items():
        for ts in v.times:
            splits.add(ts + led_delay_s)
    for ch, v in aud_views.items():
        for ts in v.times:
            splits.add(ts + aud_delay_s)
    for ch, v in led_views.items():
        for ts in v.anchors():
            splits.add(ts + led_delay_s)
    split_frames = np.array(sorted(
        int(round((cfg.lead_in_s + ts * (1.0 + faults.clock_ppm * 1e-6)) * sr))
        for ts in splits if ts >= 0), dtype=np.int64)
    split_frames = split_frames[(split_frames > 0) & (split_frames < n_total)]

    with WavWriter(out_path, n_out, sr) as w:
        pos = 0
        while pos < n_total:
            n = min(block_n, n_total - pos)
            k = int(np.searchsorted(split_frames, pos, side="right"))
            if k < split_frames.size and split_frames[k] < pos + n:
                n = int(split_frames[k]) - pos
            n = max(1, n)
            t_rec = (pos + np.arange(n)) / sr

            # Recorder clock error maps recorder time to device time. Both
            # domains share it, which is exactly why a common drift is NOT a
            # device bug and must be fitted out before reporting.
            #
            # CRITICAL: the ratio has to scale the PHASE INCREMENTS as well as
            # the model lookup time. A recorder running fast samples a real
            # f-Hz signal into more samples than it should, so reconstructing
            # with the nominal rate yields f/k. Shifting only the lookup time
            # (an earlier version of this file) moves events but leaves every
            # frequency exactly right -- so the drift faults were invisible to
            # the frequency-ratio estimator they were meant to exercise.
            k_common = 1.0 + faults.clock_ppm * 1e-6
            k_audio = k_common * (1.0 + faults.audio_extra_ppm * 1e-6)
            t_dev_common = (t_rec - cfg.lead_in_s) / k_common

            out = np.zeros((n, n_out), dtype=np.float64)

            # ---- audio ------------------------------------------------
            t_dev_a = (t_rec - cfg.lead_in_s) / k_audio - aud_delay_s
            for ch in aud_list:
                v = aud_views[ch]
                act = v.active(t_dev_a)
                if not act.any():
                    continue
                fl = v.field("freq", t_dev_a) * faults.audio_freq_scale
                amp = v.field("vol", t_dev_a) / 100.0
                pan = np.clip(v.field("pan", t_dev_a) / 100.0, -1.0, 1.0)
                modf = np.maximum(v.field("mod", t_dev_a), 0.0)
                duty = np.clip(v.field("duty", t_dev_a, 50.0) / 100.0, 0.0, 1.0)
                attack_ms = np.maximum(v.field("attack", t_dev_a, 3.0), 0.0)
                phase_off = v.field("phase", t_dev_a) / 360.0
                # env and wave are enums held constant within a render
                # sub-block: the main loop splits blocks at keyframe times, so
                # taking index 0 is exact, not an approximation.
                env = int(v.state_int("env", t_dev_a, 4)[0])
                wave = int(v.state_int("wave", t_dev_a, 0)[0])
                # freq_r needs float precision, so pull it from the keyframe
                # state directly rather than through the int view.
                fr = np.zeros(n)
                drift = faults.beat_drift_hz_per_1000s * 1e-3
                for i, m in v._groups(t_dev_a):
                    if i >= 0:
                        frr = v.kfs[i].state["freq_r"]
                        # freq_r is the ABSOLUTE right-ear frequency, on the
                        # sweep path too (main/audio_generator.c:995-1013), so
                        # sweeping the left carrier MOVES the beat. This used to
                        # hold the detune constant (fl + (frr - base)), which is
                        # what the firmware did until that comment was written;
                        # ledc_expect.py's _build_audio carries the same change
                        # and the two MUST agree or every carrier-sweep session
                        # renders a beat its own expectation does not predict.
                        fr[m] = (frr + drift * t_dev_a[m]) if frr > 0 else 0.0
                binaural = (fr > 0) & (np.abs(fr - fl) > 1e-9)

                # Per-sample phase accumulation (cumsum, not a single multiply)
                # so a ramp renders as a genuine chirp.
                srk = sr * k_audio
                ph_l = aud_phase[ch] + np.cumsum(fl / srk)
                aud_phase[ch] = float(ph_l[-1]) if n else aud_phase[ch]
                frs = np.where(binaural, fr, fl)
                ph_r = aud_phase_r[ch] + np.cumsum(frs / srk)
                aud_phase_r[ch] = float(ph_r[-1]) if n else aud_phase_r[ch]
                mph = aud_modphase[ch] + np.cumsum(modf / srk)
                aud_modphase[ch] = float(mph[-1]) if n else aud_modphase[ch]

                car_l = _audio_carrier(wave, ph_l, rng, noise_state[ch])
                if binaural.any():
                    car_r = _audio_carrier(wave, ph_r, rng, noise_state[ch])
                else:
                    car_r = car_l

                gain = np.where(act, amp, 0.0)
                if modf.max() > 0:
                    af = attack_ms * modf * 0.001
                    g = _pulse_gain(env, mph + phase_off, duty, af,
                                    dm.TIMELINE_AM_DEPTH)
                    gain = gain * np.where(modf > 0, g, 1.0)

                # 5 ms fade-in on a cold start (audio_generator.c:384-390).
                if act.any() and aud_fade[ch] < 1.0:
                    k = int(0.005 * sr)
                    ramp = np.clip(aud_fade[ch] + np.arange(1, n + 1) / max(1, k),
                                   0.0, 1.0)
                    gain = gain * ramp
                    aud_fade[ch] = float(ramp[-1])

                ang = (pan + 1.0) * (np.pi / 4.0)
                gl = np.where(binaural, gain, gain * np.cos(ang))
                gr = np.where(binaural, gain, gain * np.sin(ang))
                out[:, 0] += car_l * gl * dm.AUDIO_HEADROOM * cfg.audio_master
                out[:, 1] += (car_r if binaural.any() else car_l) * gr \
                    * dm.AUDIO_HEADROOM * cfg.audio_master

            # ---- light ------------------------------------------------
            t_dev_l = t_dev_common - led_delay_s
            for i, ch in enumerate(led_list):
                col = 2 + i
                if ch in faults.dead_lights:
                    continue
                v = led_views[ch]
                act = v.active(t_dev_l)
                fdem = np.maximum(v.field("freq", t_dev_l), 0.0)
                bright = np.clip(v.field("bright", t_dev_l) / 100.0, 0.0, 1.0)
                duty = np.clip(v.field("duty", t_dev_l, 50.0) / 100.0, 0.0, 1.0)
                attack_ms = np.maximum(v.field("attack", t_dev_l, 3.0), 0.0)
                phase_off = v.field("phase", t_dev_l) / 360.0
                env = int(v.state_int("env", t_dev_l, 0)[0])

                # Quantize to the rate the firmware can actually emit.
                tick = le.tick_at(sim.tick_timeline, float(t_dev_l[0]))
                scale = faults.light_freq_scale.get(ch, 1.0)
                femit = dm.quantized_flicker_hz_vec(fdem, tick) * scale

                cph = led_phase[ch] + np.cumsum(femit / (sr * k_common))
                # Re-anchor at a fresh activation: phase 0 at the anchor sample.
                # The main loop splits render blocks at every anchor, so the
                # anchor is a block boundary and i0 is normally 0 -- the general
                # form is kept so a rounding step cannot leave a channel
                # permanently un-anchored.
                vis = v.vis_at(float(t_dev_l[0]) + 0.5 / sr)
                if math.isfinite(vis) and led_anchor[ch] != vis:
                    hit = np.nonzero(t_dev_l >= vis - 0.5 / sr)[0]
                    if hit.size:
                        i0 = int(hit[0])
                        base = cph[i0] - femit[i0] / (sr * k_common)
                        cph = cph - base
                        led_anchor[ch] = vis
                led_phase[ch] = float(cph[-1]) if n else led_phase[ch]
                af = attack_ms * femit * 0.001
                shape = _led_carrier(env, cph + phase_off, duty, af)
                # A DC channel (freq 0 reached by a RAMP, not by a plain 0) is
                # constantly on (led_matrix_example.c:723-779).
                shape = np.where(femit > 0, shape, 1.0)
                level = np.where(act, bright * shape, 0.0)

                # Optical PWM: the WS2812 sets intensity by duty-cycling its
                # own carrier, so the sensor sees a fast square whose average
                # equals `level`. This is the signal observe_wav must low-pass.
                # The PWM carrier is a DEVICE-side signal too, so the recorder
                # clock error applies to it as well. (Leaving it unscaled made
                # the carrier beat against the flicker rate at an artificial
                # rate, adding a slow wander to the measured period that had no
                # physical counterpart.)
                pw = np.mod(pwm_phase
                            + np.arange(1, n + 1) * cfg.pwm_hz / (sr * k_common),
                            1.0)
                optical = (pw < level).astype(np.float64)
                y = optical * cfg.light_amp
                if faults.light_hp_hz > 0:
                    # One-pole high-pass, i.e. an AC-coupled input. Carried
                    # state so the baseline droop is continuous.
                    rc = 1.0 / (2.0 * np.pi * faults.light_hp_hz)
                    a = rc / (rc + 1.0 / sr)
                    yprev, xprev = hp_state[ch]
                    hy = np.empty(n)
                    for k in range(n):
                        yprev = a * (yprev + y[k] - xprev)
                        xprev = y[k]
                        hy[k] = yprev
                    hp_state[ch] = (yprev, xprev)
                    y = hy
                out[:, col] = y + rng.standard_normal(n) * cfg.sensor_noise
            pwm_phase = float(np.mod(
                pwm_phase + n * cfg.pwm_hz / (sr * k_common), 1.0))

            if noise_amp > 0:
                out += rng.standard_normal(out.shape) * noise_amp
            clip_warn += int(np.count_nonzero(np.abs(out) > 1.0))
            w.write(out)
            pos += n

    return {
        "out": out_path,
        "sample_rate": sr,
        "n_channels": n_out,
        "map": ",".join(chmap),
        "lead_in_s": cfg.lead_in_s,
        "device_duration_s": dev_dur,
        "total_s": total_s,
        "light_channels": led_list,
        "audio_channels": aud_list,
        "clipped_samples": clip_warn,
        "faults": {k: v for k, v in vars(faults).items()
                   if v not in (0.0, 0, (), {}, -90.0, 1.0)},
    }
