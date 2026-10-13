// Fidelity regression tests for gen/serialize.js.
//
// serialize() is the narrowest waist in the Generator: every view render, every
// save, every download and every push-to-device goes through it, so a token it
// drops is dropped EVERYWHERE at once, silently, with no parse error to notice.
// Each case below is a corruption that actually shipped — the comment names the
// firmware line that makes the two spellings mean different things.
//
// Table-driven on purpose: adding a case is one line. `want` omitted means the
// output must be BYTE-IDENTICAL to the input, which is the only assertion
// strong enough for a positional format.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { serialize, serializeForDevice } from '../src/js/gen/serialize.js';
import { audioRow, bg, bgRow, cell, NYQUIST } from '../src/js/gen/model.js';

// ---------------------------------------------------------------------------
// Text round-trip table. { name, src, want? }  —  want defaults to src.
// ---------------------------------------------------------------------------
const CASES = [
    // --- audio freq_r: the present bit, both directions ---------------------
    // An explicit `0` sets AUD_SET_FREQR (config_parser.c:1834) and "collapses
    // the channel back to mono" (comment at config_parser.c:1181); an absent
    // token leaves the bit clear and the live right carrier alone
    // (config_parser.c:1182). Deciding from the VALUE deleted the `0`, so a
    // binaural beat the author ENDED kept beating.
    { name: 'explicit freq_r 0 (end a beat) survives', src: 'A 10000 200 0 80 0 1 0' },
    { name: 'explicit freq_r 0 survives with a wave_type after it', src: 'A 10000 200 0 80 0 1 0 1' },
    { name: 'explicit freq_r 0 survives with pulse fields after it', src: 'A 0 200 0 80 0 1 0 - 50' },
    // Reverse direction: a `-` must NOT come back as `0` — that would go mono.
    { name: '`-` freq_r is not rewritten as 0 (wave_type forces the slot)', src: 'A 10000 200 0 80 0 1 - 1' },
    { name: '`-` freq_r is not rewritten as 0 (pulse field forces the slot)', src: 'A 0 200 0 80 0 1 - - 50' },
    // Controls: the minimal-trailing-token form must not grow a token.
    { name: 'control: absent freq_r stays absent', src: 'A 0 200 -100 60 0 1' },
    { name: 'control: a real freq_r still round-trips', src: 'A 0 200 -100 60 0 1 208' },
    { name: 'control: a ramped freq_r keeps its prefix', src: 'A 0 200 0 60 0 1 >210' },
    // A trailing `-` is the one spelling that canonicalizes: `-` and an omitted
    // trailing column are the SAME instruction ("leave unchanged",
    // ledc_format.md "One rule, two spellings"), and the model cannot tell them
    // apart, so the shorter one wins. What matters is that it is not `0`.
    { name: 'a trailing `-` freq_r canonicalizes to an omitted column, never to 0',
      src: 'A 10000 200 0 80 0 1 -', want: 'A 10000 200 0 80 0 1' },

    // --- audio wave_type: `-` is not 0 --------------------------------------
    // `0` in that column is the SINE waveform, not a placeholder; the firmware
    // skips the token only on a dash (config_parser.c:1845). Because the format
    // is positional, `-` is the ONLY way to say "change just the duty and keep
    // the timbre", so writing `0` turned a noise/square channel into a sine.
    { name: '`-` wave_type is not rewritten as 0 (sine)', src: 'A 0 250 0 80 10 4 260 - 25' },
    { name: '`-` wave_type with only a late field set', src: 'A 0 250 0 80 10 1 260 - - - - - 0.2:45000' },
    { name: 'control: an explicit wave_type 0 stays 0', src: 'A 0 250 0 80 10 4 260 0 25' },
    { name: 'control: a noise wave_type round-trips', src: 'A 0 0 0 20 0 9 0 5' },

    // --- marker inline comments --------------------------------------------
    // parse.js:281 keeps it; serializeMarker dropped it, so a marker's `# why`
    // died on the first save from any view. The device never sees it either way
    // (comments are stripped at config_parser.c:1523).
    { name: 'marker inline comment survives', src: 'M 1000 TONE_ON # 3rd block, eyes closed' },
    { name: 'marker inline comment survives alongside mod= and kv',
      src: 'M 1000 TONE_ON mod=audio block=3 # predicted onset is what we align to' },
    { name: 'control: marker without a comment gains nothing', src: 'M 1000 TONE_ON mod=led' },
    { name: 'control: LED/audio inline comments still survive',
      src: '0 10 50 80 0 128 255 1 # blue inner\nA 0 200 -100 60 0 1 # left ear' },

    // --- controls that must stay byte-identical -----------------------------
    { name: 'control: legacy 5-field LED line', src: '0 8 50 30 9' },
    { name: 'control: canonical 8-field LED line', src: '0 8 50 30 0 0 255 9' },
    { name: 'control: LED `-` fields', src: '5000 - - - - - - 1' },
    { name: 'control: BG line', src: 'BG sdcard://bg/river.wav -25 80' },
    { name: 'control: speech line', src: 'S 1000 default 80 "breathe out"' },
    { name: 'control: comments and blanks', src: '# header\n\nA 0 200 -100 60 0 1\n' },
];

for (const c of CASES) {
    const want = (c.want === undefined) ? c.src : c.want;
    test('serialize fidelity: ' + c.name, () => {
        const { doc, diagnostics } = parse(c.src + '\n');
        assert.deepStrictEqual(diagnostics.filter(d => d.severity === 'error'), [],
                               'fixture must parse cleanly');
        const once = serialize(doc);
        assert.equal(once, want + '\n');
        // The canonical form must also be a fixed point, or a second save would
        // drift again.
        assert.equal(serialize(parse(once).doc), once, 'serialization must be idempotent');
        assert.deepStrictEqual(parse(once).doc, doc, 'the model must survive the round-trip');
    });
}

// ---------------------------------------------------------------------------
// Overflowing numeric literals. `1e400` parses to Infinity (parse.js keeps what
// parseFloat returned); fmt() used to write that back as '0', turning the
// author's MAXIMUM into a MINIMUM. We emit the largest finite double instead:
// the sign and the "as big as it gets" meaning survive, and the firmware then
// applies its own per-field clamp (s_clamp_freq, audio_generator.c:471, pins an
// over-Nyquist carrier at 22050 Hz rather than at DC).
// ---------------------------------------------------------------------------
const HUGE = String(Number.MAX_VALUE); // 1.7976931348623157e+308

// `pick` names the field that overflowed, so the "still a maximum" assertion
// looks at the right column instead of at whichever one happens to be truthy.
const OVERFLOW_CASES = [
    { name: 'an overflowing freq is not written back as 0',
      src: 'A 0 1e400 0 80 0 1', want: 'A 0 ' + HUGE + ' 0 80 0 1', pick: r => r.freq },
    { name: 'an overflowing freq written long-hand is not written back as 0',
      src: 'A 0 9' + '9'.repeat(37) + 'e999 0 80 0 1', want: 'A 0 ' + HUGE + ' 0 80 0 1', pick: r => r.freq },
    { name: 'an overflowing NEGATIVE pan keeps its sign',
      src: 'A 0 200 -1e400 80 0 1', want: 'A 0 200 -' + HUGE + ' 80 0 1', pick: r => r.pan },
    { name: 'an overflowing LED brightness is not written back as 0',
      src: '0 10 50 1e400 0 0 255 1', want: '0 10 50 ' + HUGE + ' 0 0 255 1', pick: r => r.bright },
    { name: 'an overflowing ramp target keeps its prefix',
      src: 'A 0 >1e400 0 80 0 1', want: 'A 0 >' + HUGE + ' 0 80 0 1', pick: r => r.freq },
];

for (const c of OVERFLOW_CASES) {
    test('serialize fidelity: ' + c.name, () => {
        const out = serialize(parse(c.src + '\n').doc);
        assert.equal(out, c.want + '\n');
        // Re-reading it must yield a FINITE number that is still a maximum, not
        // a zero: that is the whole point of the choice made in fmt().
        assert.equal(serialize(parse(out).doc), out, 'must be byte-stable on re-save');
        const v = c.pick(parse(out).doc.rows[0]).value;
        assert.ok(Number.isFinite(v), 're-parsed value must be finite');
        assert.ok(Math.abs(v) > NYQUIST, 're-parsed value must still be a maximum, not 0');
    });
}

// ---------------------------------------------------------------------------
// doc.bg — the session-level override that play.js:196 relies on.
//
// It serializes `{...doc, bg: {url:'push://session', …}}` so the firmware's
// prime gate arms (config_parser.c:766 defers the whole t=0 start until the
// pushed audio has primed) and the browser-bounced narration stays
// sample-aligned to t=0. serialize() read only `bg` ROWS, so that override was
// discarded and the device started un-gated — or played the file's own BG.
// ---------------------------------------------------------------------------
const PUSH = bg('push://session', 0, 100);
const BG_CASES = [
    {
        name: 'doc.bg with no BG row of its own is emitted',
        doc: () => ({ rows: parse('A 0 200 0 80 0 1\n').doc.rows, bg: PUSH }),
        want: 'A 0 200 0 80 0 1\nBG push://session 0 100\n',
    },
    {
        name: 'doc.bg replaces the existing BG row rather than duplicating it',
        doc: () => ({ ...parse('BG sdcard://bg/river.wav -25 80\nA 0 200 0 80 0 1\n').doc, bg: PUSH }),
        want: 'BG push://session 0 100\nA 0 200 0 80 0 1\n',
    },
    {
        name: 'doc.bg replaces only the LAST BG row (the firmware is last-wins)',
        doc: () => ({ ...parse('BG sdcard://a.wav 0 50\nBG sdcard://b.wav 0 60\n').doc, bg: PUSH }),
        want: 'BG sdcard://a.wav 0 50\nBG push://session 0 100\n',
    },
    {
        name: 'control: a parsed doc (doc.bg mirrors its row) is byte-identical',
        doc: () => parse('BG sdcard://bg/river.wav -25 80\nA 0 200 0 80 0 1\n').doc,
        want: 'BG sdcard://bg/river.wav -25 80\nA 0 200 0 80 0 1\n',
    },
    {
        name: 'control: no override and no BG row emits no BG line',
        doc: () => ({ rows: parse('A 0 200 0 80 0 1\n').doc.rows, bg: null }),
        want: 'A 0 200 0 80 0 1\n',
    },
    {
        name: 'control: a BG row with doc.bg unset is still emitted from the row',
        doc: () => ({ rows: [bgRow(bg('sdcard://bg/river.wav', 0, 60))], bg: null }),
        want: 'BG sdcard://bg/river.wav 0 60\n',
    },
    {
        name: 'control: a url-less doc.bg is ignored (it would not re-parse)',
        doc: () => ({ rows: parse('A 0 200 0 80 0 1\n').doc.rows, bg: bg('', 0, 0) }),
        want: 'A 0 200 0 80 0 1\n',
    },
];

for (const c of BG_CASES) {
    test('serialize fidelity: ' + c.name, () => {
        const out = serialize(c.doc());
        assert.equal(out, c.want);
        assert.equal((out.match(/^BG /gm) || []).length, (c.want.match(/^BG /gm) || []).length,
                     'no duplicate BG lines');
        assert.equal(serialize(parse(out).doc), out, 'must be byte-stable on re-save');
    });
}

test('serialize fidelity: the play.js push:// override reaches the device body', () => {
    // Exactly play.js's speech path: bounce → rewrite BG to push:// → playDoc,
    // which calls serializeForDevice. Without the override the firmware never
    // arms its prime gate and the baked narration drifts off t=0.
    const doc = parse('BG sdcard://bg/river.wav 0 60\nA 0 200 0 80 0 1\nS 500 default 80 "breathe"\n').doc;
    const body = serializeForDevice({ ...doc, bg: bg('push://session', 0, 100) });
    assert.ok(body.includes('BG push://session 0 100'), 'the override must be in the device body');
    assert.ok(!body.includes('sdcard://bg/river.wav'), 'the overridden BG must not survive');
    assert.ok(!body.includes('\nS '), 'S rows are still stripped for the device');
    assert.equal((body.match(/^BG /gm) || []).length, 1, 'exactly one BG line');
});

// ---------------------------------------------------------------------------
// Editor-built rows (no present bit) keep the value-driven minimal form: their
// `freqR` number IS the authority, so freqR: 0 means mono and the token is only
// emitted when a later column forces the slot. Guards the bug-avoidance
// contract (#3/#4/#6) against the present-bit fix above.
// ---------------------------------------------------------------------------
const EDITOR_CASES = [
    { name: 'editor row: freqR 0 + no later field omits the slot',
      row: { channel: 1, freqR: 0 }, want: 'A 0 200 0 60 0 1' },
    { name: 'editor row: freqR 0 + a wave_type emits the explicit mono 0',
      row: { channel: 1, freqR: 0, waveType: 2 }, want: 'A 0 200 0 60 0 1 0 2' },
    { name: 'editor row: a real freqR is emitted',
      row: { channel: 1, freqR: 208 }, want: 'A 0 200 0 60 0 1 208' },
    { name: 'editor row: freqR 0 + a pulse field fills the wave slot with `-`',
      row: { channel: 1, freqR: 0, duty: cell(25) }, want: 'A 0 200 0 60 0 1 0 - 25' },
];

for (const c of EDITOR_CASES) {
    test('serialize fidelity: ' + c.name, () => {
        const row = audioRow({ time: 0, freq: cell(200), pan: cell(0), vol: cell(60),
                               mod: cell(0), ...c.row });
        const out = serialize({ rows: [row], bg: null });
        assert.equal(out, c.want + '\n');
        assert.equal(serialize(parse(out).doc), out, 'must be byte-stable on re-save');
    });
}
