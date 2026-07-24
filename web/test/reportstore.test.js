// Unit tests for the PURE report helpers (filename, compose, parse). No DOM.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { makeReportName, composeReport, parseReport } from '../src/js/reportstore.js';

// ---- makeReportName --------------------------------------------------------

test('makeReportName: includes the session name + a timestamp, ends .rpt', () => {
    const n = makeReportName('04_meditation_theta.ledc');
    // ".ledc" is dropped; the base name is preserved; a -YYYYMMDD-HHMMSS stamp
    // and .rpt extension are appended.
    assert.match(n, /^04_meditation_theta-\d{8}-\d{6}\.rpt$/);
});

test('makeReportName: sanitizes unsafe chars and falls back to "session"', () => {
    assert.match(makeReportName('my session/name!'), /^my-session-name-\d{8}-\d{6}\.rpt$/);
    assert.match(makeReportName(''), /^session-\d{8}-\d{6}\.rpt$/);
    assert.match(makeReportName(null), /^session-\d{8}-\d{6}\.rpt$/);
});

// ---- compose / parse round-trip -------------------------------------------

const BODY = '=== SESSION REPORT ===\nSession length so far: 12.0 s\n\n--- Last loaded config ---\n1000 440 0 50 0\n';

test('composeReport: header carries title, session, date, comments', () => {
    const txt = composeReport(BODY, { title: 'Great nap', session: '02_power_nap', comments: 'felt rested', date: '2026-07-23 13:45' });
    assert.match(txt, /^Title: Great nap\n/);
    assert.match(txt, /\nSession: 02_power_nap\n/);
    assert.match(txt, /\nDate: 2026-07-23 13:45\n/);
    assert.match(txt, /\nComments:\nfelt rested\n/);
    assert.ok(txt.endsWith(BODY));
});

test('composeReport: empty title defaults to session, empty comments -> (none)', () => {
    const txt = composeReport(BODY, { title: '', session: 'sess', comments: '', date: 'd' });
    assert.match(txt, /^Title: sess\n/);
    assert.match(txt, /\nComments:\n\(none\)\n/);
});

test('parseReport: recovers title/session/comments/body from a composed report', () => {
    const txt = composeReport(BODY, { title: 'T1', session: 'S1', comments: 'line one\nline two', date: 'd' });
    const p = parseReport(txt);
    assert.equal(p.title, 'T1');
    assert.equal(p.session, 'S1');
    assert.equal(p.comments, 'line one\nline two');
    assert.equal(p.body, BODY);
});

test('parseReport: "(none)" comments come back empty', () => {
    const p = parseReport(composeReport(BODY, { title: 'T', session: 'S', comments: '', date: 'd' }));
    assert.equal(p.comments, '');
});

test('parseReport: header-less (legacy) report -> whole text is the body', () => {
    const legacy = '=== SESSION REPORT ===\nold report with no header\n';
    const p = parseReport(legacy);
    assert.equal(p.title, '');
    assert.equal(p.comments, '');
    assert.equal(p.body, legacy);
});
