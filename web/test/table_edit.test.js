// Tests for the Table view's EDIT paths — the grid used to accept edits it
// then threw away.
//
// Every case here is a model-level assertion against the pure helpers the grid
// writes through, usually checked all the way out to the serialized text, so a
// "the UI showed it applied" bug cannot hide behind a repainted widget. No DOM,
// so they run in plain Node like test/table_view.test.js.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    setRowField, toggleMaskBit, markerSummary, cloneRow,
    freqRCellOf, applyFreqRCell,
    newRowOfKind, insertTimeOf, insertKindAfter, appendTimeOf,
} from '../src/js/gen/views/table.js';
import {
    ledRow, audioRow, markerRow, speechRow, commentRow, bgRow, bg, cell,
} from '../src/js/gen/model.js';
import { parse } from '../src/js/gen/parse.js';
import { serialize } from '../src/js/gen/serialize.js';

const firstRow = (text) => parse(text).doc.rows[0];
const lineOf = (row) => serialize({ rows: [row] }).trim();

// ---- (1) a legacy 5-field LED row must not swallow colour/pulse edits ------
// serializeLed() drops R/G/B and env/phase/attack/jitter while `legacy5` is
// set, so an edit to one of those has to retire the flag or the file keeps the
// old line and the next re-parse erases the edit from the model too.

test('a legacy 5-field LED line parses with legacy5 set and round-trips as 5 fields', () => {
    const row = firstRow('1000 10 50 80 1\n');
    assert.equal(row.legacy5, true);
    assert.equal(lineOf(row), '1000 10 50 80 1');
});

test('picking a colour on a legacy 5-field row reaches the FILE', () => {
    const row = firstRow('1000 10 50 80 1\n');
    setRowField(row, 'r', { ...row.r, value: 255 });
    setRowField(row, 'g', { ...row.g, value: 0 });
    setRowField(row, 'b', { ...row.b, value: 0 });
    assert.equal(row.legacy5, false, 'the 5-field form cannot carry a colour');
    assert.equal(lineOf(row), '1000 10 50 80 255 0 0 1');
});

test('a single changed colour channel is enough to retire legacy5', () => {
    const row = firstRow('1000 10 50 80 1\n');
    setRowField(row, 'g', { ...row.g, value: 128 });
    assert.equal(row.legacy5, false);
    assert.match(lineOf(row), /^1000 10 50 80 255 128 255 1$/);
});

test('a colour RAMP on an unchanged value still counts as an edit', () => {
    // Same 255, new interp: the ramp is the edit, and a 5-field line has no
    // slot to put it in.
    const row = firstRow('1000 10 50 80 1\n');
    setRowField(row, 'r', cell(255, 'lin'));
    assert.equal(row.legacy5, false);
    assert.equal(lineOf(row), '1000 10 50 80 >255 255 255 1');
});

test('turning on a pulse field retires legacy5 too', () => {
    for (const [field, value] of [['env', 1], ['phase', cell(90)],
                                  ['attack', cell(5)], ['jitter', { amp: 2 }]]) {
        const row = firstRow('1000 10 50 80 1\n');
        setRowField(row, field, value);
        assert.equal(row.legacy5, false, field + ' is dropped while legacy5 is set');
        assert.notEqual(lineOf(row), '1000 10 50 80 1');
    }
});

test('committing the colour that is ALREADY there leaves the 5-field line alone', () => {
    // Opening the colour popover and clicking Done fires a commit with the
    // unchanged white. Retiring the flag there would rewrite the 23 five-field
    // rows in the shipped library just for being looked at.
    const row = firstRow('1000 10 50 80 1\n');
    setRowField(row, 'r', cell(255));
    setRowField(row, 'g', { ...row.g });
    setRowField(row, 'b', { value: 255, interp: 'none', modEnd: undefined, modPeriodMs: undefined });
    assert.equal(row.legacy5, true);
    assert.equal(lineOf(row), '1000 10 50 80 1');
});

test('clearing an already-absent pulse field leaves the 5-field line alone', () => {
    const row = firstRow('1000 10 50 80 1\n');
    setRowField(row, 'phase', undefined);
    setRowField(row, 'env', undefined);
    assert.equal(row.legacy5, true);
    assert.equal(lineOf(row), '1000 10 50 80 1');
});

test('editing a field the 5-field form DOES carry keeps it legacy', () => {
    const row = firstRow('1000 10 50 80 1\n');
    setRowField(row, 'freq', cell(12));
    setRowField(row, 'duty', cell(60));
    setRowField(row, 'bright', cell(90));
    setRowField(row, 'mask', 3);
    assert.equal(row.legacy5, true, 'time/freq/duty/bright/mask all survive 5 fields');
    assert.equal(lineOf(row), '1000 12 60 90 3');
});

test('the two 5-field lines that actually ship survive a look at the grid', () => {
    // sessions/test/t12_format_edges.ledc:73-74, verbatim. Opening the Table on
    // this file paints a Color swatch and four pulse cells for each of them; as
    // long as nothing is CHANGED, both lines must come back out as they went in.
    for (const line of ['1000 10 50 100 3', '1000 7 50 100 12']) {
        const row = firstRow(line + '\n');
        assert.equal(row.legacy5, true);
        setRowField(row, 'r', { ...row.r });          // popover opened, Done clicked
        setRowField(row, 'jitter', undefined);        // "off" re-committed as off
        assert.equal(lineOf(row), line);
    }
});

test('setRowField leaves a canonical LED row and an audio row untouched', () => {
    const led = firstRow('1000 10 50 80 255 255 255 1\n');
    setRowField(led, 'r', cell(0));
    assert.equal(led.legacy5, false);
    const aud = audioRow({ time: 0, channel: 1 });
    setRowField(aud, 'env', 2);
    assert.equal(aud.env, 2);
    assert.equal(aud.legacy5, undefined, 'audio rows have no legacy form');
});

// ---- (2) markers render and duplicate as markers ---------------------------

test('markerSummary describes the marker instead of printing undefined', () => {
    const row = firstRow('M 30000 STIM_START mod=led condition=alpha10 block=3\n');
    assert.equal(markerSummary(row), 'STIM_START mod=led condition=alpha10 block=3');
    assert.doesNotMatch(markerSummary(row), /undefined/);
});

test('markerSummary of a bare marker is still the label', () => {
    const row = firstRow('M 0 BASELINE_START\n');
    assert.equal(markerSummary(row), 'BASELINE_START');
});

test('markerSummary never renders undefined, even for an empty row', () => {
    assert.doesNotMatch(markerSummary(markerRow()), /undefined/);
});

test('Duplicate on a marker produces a MARKER, not a label-less raw row', () => {
    const row = firstRow('M 30000 STIM_START mod=led condition=alpha10\n');
    const copy = cloneRow(row);
    assert.equal(copy.kind, 'marker');
    assert.equal(lineOf(copy), lineOf(row));
    assert.equal(lineOf(copy), 'M 30000 STIM_START mod=led condition=alpha10');
});

test('a duplicated marker carries its own kv pairs', () => {
    const row = firstRow('M 0 X a=1 b=2\n');
    const copy = cloneRow(row);
    copy.kv[0].v = '9';
    copy.kv.push({ k: 'c', v: '3' });
    assert.equal(lineOf(row), 'M 0 X a=1 b=2', 'the original is not aliased to the copy');
    assert.equal(lineOf(copy), 'M 0 X a=9 b=2 c=3');
});

test('Duplicate keeps fields the marker model may gain later', () => {
    // parse.js parseMarker drops bare tokens (`eq <= 0 -> continue`) and the
    // 5th+ kv pair today, so they never reach the model and the grid cannot
    // show them — the fix for that belongs in parse.js + serialize.js. What IS
    // in our hands is that the copy must not be a SECOND place they get lost:
    // cloneRow spread-copies a marker, so whatever field name the model gains
    // rides along. `extraTokens` below is an illustrative name, not a contract.
    const row = markerRow({ time: 10, label: 'X', kv: [{ k: 'a', v: '1' }] });
    row.extraTokens = ['junk', 'e=5'];
    assert.deepEqual(cloneRow(row).extraTokens, ['junk', 'e=5']);
});

// ---- (3) Duplicate on an audio row keeps depth and the freqR ramp ----------

test('Duplicate on an audio row preserves depth and the ramped right carrier', () => {
    const row = firstRow('A 1000 400 0 80 0 1 >410 0 50 4 0 3 0:0 25\n');
    assert.equal(row.depth.value, 25, 'fixture really does carry a depth');
    assert.equal(row.freqRCell.interp, 'lin', 'and a ramped freq_r');
    const copy = cloneRow(row);
    assert.equal(lineOf(copy), lineOf(row));
    assert.equal(copy.depth.value, 25);
    assert.equal(copy.freqRCell.interp, 'lin');
    copy.depth.value = 99;
    assert.equal(row.depth.value, 25, 'the copy owns its own cells');
});

test('Duplicate preserves "freq_r token absent", which is not the same as mono', () => {
    // An absent token leaves the right carrier alone (sim/resolve.js:304); a
    // copy that forgets it reads as an explicit 0 and kills the beat.
    const row = firstRow('A 2000 400 0 80 0 1\n');
    assert.equal(row.freqRPresent, false);
    assert.equal(cloneRow(row).freqRPresent, false);
});

// ---- (5) one chip click must not be able to commit mask 0 ------------------

test('toggleMaskBit toggles ordinary bits', () => {
    assert.equal(toggleMaskBit(3, 0), 2);      // channels 1+2 -> channel 2
    assert.equal(toggleMaskBit(1, 1), 3);      // channel 1 -> channels 1+2
    assert.equal(toggleMaskBit(0xFF, 7), 0x7F);
});

test('toggleMaskBit refuses to clear the LAST channel', () => {
    assert.equal(toggleMaskBit(1, 0), 1);
    assert.equal(toggleMaskBit(0x80, 7), 0x80);
});

test('the mask a chip click can produce always survives a re-parse', () => {
    // mask 0 is rejected by parse.js (and skipped by the firmware,
    // config_parser.c:1692), so committing it turns the row into a raw line.
    for (let mask = 1; mask <= 0xFF; mask++) {
        for (let bit = 0; bit < 8; bit++) {
            const row = ledRow({ time: 0, mask: toggleMaskBit(mask, bit) });
            const back = firstRow(serialize({ rows: [row] }));
            assert.equal(back.kind, 'led', 'mask ' + mask + ' bit ' + bit +
                                           ' serialized to an unparsable line');
        }
    }
});

// ---- (6) the FreqR box must round-trip a ramp ------------------------------

test('freqRCellOf surfaces a ramped freq_r as a ramp, not a bare number', () => {
    const row = firstRow('A 1000 400 0 80 0 1 >410\n');
    const c = freqRCellOf(row);
    assert.equal(c.interp, 'lin');
    assert.equal(c.value, 410);
});

test('freqRCellOf: a plain carrier is a step cell, mono is unset', () => {
    assert.deepEqual(freqRCellOf({ freqR: 410 }), cell(410));
    assert.equal(freqRCellOf({ freqR: 0 }), undefined);
});

test('editing a ramped freq_r to a step value REPLACES the ramp in the file', () => {
    // The old number box wrote only `row.freqR`, which serialize.js ignores
    // while freqRCell carries a prefix — the typed value vanished.
    const row = firstRow('A 1000 400 0 80 0 1 >410\n');
    assert.equal(applyFreqRCell(row, cell(500)), true);
    assert.equal(row.freqR, 500);
    assert.equal(row.freqRCell, undefined);
    assert.equal(lineOf(row), 'A 1000 400 0 80 0 1 500');
});

test('a ramp typed into FreqR round-trips through the file', () => {
    const row = firstRow('A 1000 400 0 80 0 1 410\n');
    assert.equal(applyFreqRCell(row, cell(420, 'lin')), true);
    assert.equal(lineOf(row), 'A 1000 400 0 80 0 1 >420');
    const back = firstRow(lineOf(row) + '\n');
    assert.equal(back.freqRCell.interp, 'lin');
    assert.equal(back.freqR, 420);
});

test('FreqR refuses a periodic modulation visibly, changing nothing', () => {
    // The firmware parses one, warns "not implemented yet — holding", and drops
    // the interp (config_parser.c:1835-1840). Authoring it would be a lie.
    const row = firstRow('A 1000 400 0 80 0 1 >410\n');
    const before = lineOf(row);
    assert.equal(applyFreqRCell(row, cell(410, 'sine', 420, 1000)), false);
    assert.equal(lineOf(row), before, 'the refused edit left the row untouched');
});

test('clearing FreqR collapses the channel to mono, and SAYS so', () => {
    // The ramp goes, and the row still states freq_r = 0: a line that merely
    // omits the token leaves the live right carrier alone (the AUD_SET_FREQR
    // guard at config_parser.c:1182), so a beat the author just turned off
    // would keep beating.
    const row = firstRow('A 1000 400 0 80 0 1 >410\n');
    assert.equal(applyFreqRCell(row, undefined), true);
    assert.equal(row.freqR, 0);
    assert.equal(row.freqRCell, undefined);
    assert.equal(lineOf(row), 'A 1000 400 0 80 0 1 0');
});

test('typing a FreqR value marks it present so the preview stops inheriting', () => {
    const row = firstRow('A 2000 400 0 80 0 1\n');
    assert.equal(row.freqRPresent, false);
    applyFreqRCell(row, cell(410));
    assert.equal(row.freqRPresent, true, 'the file now carries the token');
    assert.equal(lineOf(row), 'A 2000 400 0 80 0 1 410');
});

// ---- Duplicate must not turn a `-` field into a zero ----------------------

test('Duplicate preserves `-` (leave unchanged) on an LED colour', () => {
    // `- - -` keeps the channel's live colour; `0 0 0` forces it black. 40 LED
    // rows in the shipped library are written exactly this way (the _SINE and
    // _TRAP variants of 21_gateway_focus12 and 25_gateway_journey, 10 each), so
    // a copy used to come out black. (48 counting sessions/test — the other 8
    // are in t12_format_edges.ledc; that is the figure cloneCell() quotes,
    // table.js:333.)
    const row = firstRow('0 4 50 >0 - - - 10 1 0\n');
    assert.equal(row.r, null, 'fixture really does carry `-`');
    assert.equal(lineOf(cloneRow(row)), '0 4 50 >0 - - - 10 1 0');
});

test('Duplicate preserves `-` on an audio volume', () => {
    const row = firstRow('A 1000 440 - - 0 1\n');
    assert.equal(row.vol, null);
    assert.equal(lineOf(cloneRow(row)), 'A 1000 440 - - 0 1');
});

// ---- (7) a new row must land in the instant you are working in ------------
// "Insert after", the Kind dropdown and the Add-row bar all seed the new row
// with a time read off a row that is already there. That read used to consider
// led/audio rows ONLY, so after a speech cue or a marker it fell through to 0
// and the new row appeared at the TOP of the file — in 01_sleep_onset's case 25
// minutes from where the author was looking, sorted above every other row by
// orderRowsForDisplay, and easy to miss entirely.

test('Insert after a speech cue lands in that instant, not at t=0', () => {
    // sessions/library/01_sleep_onset.ledc:60-63, verbatim (comments dropped):
    // the session's last instant, whose last line is the cue.
    const rows = parse(
        '1500000 2 50 0 150 20 0 15\n' +
        'A 1500000 248 0 0 0 1 250 0\n' +
        'A 1500000 0 0 0 0 3 0 5\n' +
        'S 1500000 default 10 "Two hertz, deep sleep. Rest."\n').doc.rows;
    const cue = rows[rows.length - 1];
    assert.equal(cue.kind, 'speech', 'fixture really does end on a speech cue');
    const fresh = newRowOfKind(insertKindAfter(cue), insertTimeOf(cue));
    assert.equal(fresh.time, 1500000, 'the new row inherited the cue\'s instant');
    rows.splice(rows.length, 0, fresh);
    assert.match(serialize({ rows }), /\n1500000 0 0 0 255 255 255 1\n?$/);
});

test('Insert after a MARKER lands in the marker\'s instant', () => {
    // docs/sessions/markers.html:69-72, verbatim. A marker produces no sound
    // and no light, so its row is the only thing in the grid that says where
    // the author is: an EEG script is authored around exactly these.
    const rows = parse('M 0      BASELINE_START\n' +
                       'M 30000  STIM_START      condition=alpha10 block=3\n').doc.rows;
    const mark = rows[1];
    assert.equal(mark.kind, 'marker');
    const fresh = newRowOfKind(insertKindAfter(mark), insertTimeOf(mark));
    assert.equal(fresh.kind, 'led', 'there is no newRowOfKind("marker") to mirror');
    assert.equal(lineOf(fresh), '30000 0 0 0 255 255 255 1');
});

test('insertTimeOf: every timed kind hands over its own instant', () => {
    assert.equal(insertTimeOf(ledRow({ time: 1000 })), 1000);
    assert.equal(insertTimeOf(audioRow({ time: 2000, channel: 1 })), 2000);
    assert.equal(insertTimeOf(speechRow({ time: 1500000, text: 'Rest.' })), 1500000);
    assert.equal(insertTimeOf(markerRow({ time: 30000, label: 'STIM_START' })), 30000);
    assert.equal(insertTimeOf(firstRow('S 90000 default 60 "WAKE UP!"\n')), 90000);
});

test('insertTimeOf: a row with no time of its own seeds t=0', () => {
    // BG is a document-wide descriptor and `#`/blank/raw rows are text: none of
    // them is AT an instant, so there is nothing to inherit and 0 is honest.
    assert.equal(insertTimeOf(commentRow('# a note')), 0);
    assert.equal(insertTimeOf(bgRow(bg('push://bed', 0, 50))), 0);
    assert.equal(insertTimeOf({ kind: 'raw', text: 'nonsense' }), 0);
    assert.equal(insertTimeOf({ kind: 'blank' }), 0);
    assert.equal(insertTimeOf(undefined), 0);
    assert.equal(insertTimeOf(speechRow({})), 0, 'an un-timed cue is still 0, not NaN');
});

test('insertKindAfter: led/audio beget their own kind, everything else an LED row', () => {
    assert.equal(insertKindAfter(ledRow({ time: 0 })), 'led');
    assert.equal(insertKindAfter(audioRow({ time: 0, channel: 1 })), 'audio');
    for (const row of [speechRow({ time: 1 }), markerRow({ time: 1 }),
                       commentRow('# x'), bgRow(bg('', 0, 50)), { kind: 'blank' }]) {
        assert.equal(insertKindAfter(row), 'led', row.kind);
    }
});

test('retyping a speech cue as audio keeps the instant', () => {
    // The Kind <select> replaces the row in place with newRowOfKind(kind,
    // insertTimeOf(row)) — the same read, so the same bug lived here too: a
    // cue retyped as A used to come back as `A 0 ...`.
    const cue = firstRow('S 1500000 default 10 "Two hertz, deep sleep. Rest."\n');
    const retyped = newRowOfKind('audio', insertTimeOf(cue));
    assert.equal(retyped.kind, 'audio');
    assert.equal(lineOf(retyped), 'A 1500000 0 0 0 0 1');
});

test('Add row appends at the last instant, even past a speech/marker tail', () => {
    // In 67 of the 85 .ledc files in sessions/library the last row is a speech
    // cue (11 end on audio, 7 on LED), so the bottom of the grid — where the
    // Add-row bar is — is usually a cue.
    const rows = parse('0 10 50 20 1\n' +
                       'A 0 248 0 0 0 1 250 0\n' +
                       'S 1500000 default 10 "Two hertz, deep sleep. Rest."\n').doc.rows;
    assert.equal(appendTimeOf(rows), 1500000);
    const rowsM = parse('M 0 BASELINE_START\nM 90000 STIM_END\n').doc.rows;
    assert.equal(appendTimeOf(rowsM), 90000, 'a marker-only EEG script has no led/audio row');
});

test('appendTimeOf: nothing timed in the document is t=0', () => {
    assert.equal(appendTimeOf(parse('# just a header\n\nBG push://bed 0 50\n').doc.rows), 0);
    assert.equal(appendTimeOf([]), 0);
    assert.equal(appendTimeOf(undefined), 0);
});
