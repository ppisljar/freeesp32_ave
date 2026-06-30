// Unit tests for the Text view's pure diagnostics-merge helper.
// (UI behavior itself is exercised in the browser; this guards the merge math.)

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { mergeDiagnostics } from '../src/js/gen/views/text.js';

test('mergeDiagnostics: maps validate row index to 1-based line', () => {
    const out = mergeDiagnostics([], [{ row: 0, severity: 'warn', msg: 'a' }]);
    assert.deepEqual(out, [{ line: 1, severity: 'warn', msg: 'a' }]);
});

test('mergeDiagnostics: row -1 (whole-doc) becomes line null', () => {
    const out = mergeDiagnostics([], [{ row: -1, severity: 'warn', msg: 'too many' }]);
    assert.equal(out[0].line, null);
});

test('mergeDiagnostics: passes through parse diagnostics by line', () => {
    const out = mergeDiagnostics([{ line: 3, severity: 'error', msg: 'bad' }], []);
    assert.deepEqual(out, [{ line: 3, severity: 'error', msg: 'bad' }]);
});

test('mergeDiagnostics: de-duplicates identical (line,severity,msg)', () => {
    const parse = [{ line: 2, severity: 'error', msg: 'dup' }];
    const valid = [{ row: 1, severity: 'error', msg: 'dup' }]; // row 1 -> line 2
    const out = mergeDiagnostics(parse, valid);
    assert.equal(out.length, 1);
});

test('mergeDiagnostics: sorts by line then errors before warns', () => {
    const out = mergeDiagnostics(
        [{ line: 5, severity: 'warn', msg: 'w5' }],
        [
            { row: 0, severity: 'warn', msg: 'w1' },   // line 1
            { row: 0, severity: 'error', msg: 'e1' },  // line 1
            { row: -1, severity: 'warn', msg: 'glob' } // line null -> last
        ]
    );
    assert.deepEqual(out.map(d => [d.line, d.severity]), [
        [1, 'error'], [1, 'warn'], [5, 'warn'], [null, 'warn'],
    ]);
});
