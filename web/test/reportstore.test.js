// Unit tests for the PURE report helpers (filename, compose, parse). No DOM.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { makeReportName, composeReport, parseReport,
         parseReportName, reportLabel, compareReports } from '../src/js/reportstore.js';

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

test('parseReport: recovers the Date header line', () => {
    const p = parseReport(composeReport(BODY, { title: 'T', session: 'S', comments: '', date: '2026-07-23 13:45' }));
    assert.equal(p.date, '2026-07-23 13:45');
});

// ---- filename -> {base, date} ---------------------------------------------

test('parseReportName: splits the session base from the timestamp', () => {
    const p = parseReportName('04_meditation_theta-20260723-134501.rpt');
    assert.equal(p.base, '04_meditation_theta');
    assert.equal(p.date.getFullYear(), 2026);
    assert.equal(p.date.getMonth(), 6);       // July
    assert.equal(p.date.getDate(), 23);
    assert.equal(p.date.getHours(), 13);
    assert.equal(p.date.getMinutes(), 45);
});

test('parseReportName: a name with a dash in it keeps the LAST stamp', () => {
    const p = parseReportName('my-nap-session-20260101-000000.rpt');
    assert.equal(p.base, 'my-nap-session');
});

test('parseReportName: unstamped/legacy names have no date', () => {
    const p = parseReportName('handwritten.rpt');
    assert.equal(p.base, 'handwritten');
    assert.equal(p.date, null);
});

// ---- dropdown label --------------------------------------------------------

test('reportLabel: date, .ledc name and custom title', () => {
    const e = { name: 'x-20260723-134501.rpt', base: 'x', date: new Date(2026, 6, 23, 13, 45, 1),
                session: '04_meditation_theta.ledc', title: 'Great nap' };
    const l = reportLabel(e);
    assert.match(l, /^2026-07-23 13:45/);
    assert.ok(l.includes('04_meditation_theta.ledc'));
    assert.ok(l.includes('Great nap'));
});

test('reportLabel: an auto-defaulted title (== session) is not repeated', () => {
    const e = { name: 'x.rpt', base: 'x', date: new Date(2026, 0, 2, 3, 4, 5),
                session: 'nap.ledc', title: 'nap.ledc' };
    assert.equal(reportLabel(e), '2026-01-02 03:04  \u00b7  nap.ledc');
});

test('reportLabel: falls back to the filename base when the session is unknown', () => {
    const e = { name: 'nap-20260102-030405.rpt', base: 'nap', date: null, session: '', title: '' };
    assert.equal(reportLabel(e), '(no date)  \u00b7  nap.ledc');
});

test('reportLabel: a report with comments is marked with a leading *', () => {
    const e = { name: 'x.rpt', base: 'x', date: new Date(2026, 0, 2, 3, 4, 5),
                session: 'nap.ledc', title: 'nap.ledc', hasComments: true };
    assert.equal(reportLabel(e), '* 2026-01-02 03:04  \u00b7  nap.ledc');
});

test('reportLabel: a report without comments carries no marker', () => {
    const e = { name: 'x.rpt', base: 'x', date: new Date(2026, 0, 2, 3, 4, 5),
                session: 'nap.ledc', title: 'nap.ledc', hasComments: false };
    assert.ok(!reportLabel(e).startsWith('*'));
});

// ---- ordering --------------------------------------------------------------

test('compareReports: newest first, undated last', () => {
    const mk = (name, d) => ({ name, date: d });
    const list = [
        mk('a-20260101-000000.rpt', new Date(2026, 0, 1)),
        mk('legacy.rpt', null),
        mk('c-20260301-000000.rpt', new Date(2026, 2, 1)),
        mk('b-20260201-000000.rpt', new Date(2026, 1, 1)),
    ];
    list.sort(compareReports);
    assert.deepEqual(list.map(e => e.name),
        ['c-20260301-000000.rpt', 'b-20260201-000000.rpt', 'a-20260101-000000.rpt', 'legacy.rpt']);
});
