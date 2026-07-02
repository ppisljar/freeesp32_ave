// Validate a .ledc session file against the generator's parser/validator
// (mirror of the firmware config_parser.c) + a basic strobe-safety lint.
//
// Usage: node sessions/validate_session.mjs sessions/library/<file>.ledc
// Exit 0 = clean (warnings allowed), exit 1 = errors (parse/validate/round-trip/safety).
import { readFileSync } from 'node:fs';
import { parse } from '../web/src/js/gen/parse.js';
import { serialize } from '../web/src/js/gen/serialize.js';
import { validate } from '../web/src/js/gen/validate.js';

const file = process.argv[2];
if (!file) { console.error('usage: node validate_session.mjs <file.ledc>'); process.exit(2); }
const text = readFileSync(file, 'utf8');

let errors = 0, warns = 0;
const err = (m) => { console.error('  ERROR: ' + m); errors++; };
const warn = (m) => { console.warn('  warn:  ' + m); warns++; };

// 1. Parse
const { doc, diagnostics } = parse(text);
for (const d of diagnostics) {
  if (d.severity === 'error') err(`parse line ${d.line}: ${d.msg}`);
  else warn(`parse line ${d.line}: ${d.msg}`);
}
// Any line held verbatim as a 'raw' row = a parse failure the firmware would reject.
for (const r of doc.rows) if (r.kind === 'raw') err(`unparsable line held raw: ${JSON.stringify(r.text)}`);

// 2. Semantic validate (firmware clamps)
for (const d of validate(doc)) {
  if (d.severity === 'error') err(`validate row ${d.row}: ${d.msg}`);
  else warn(`validate row ${d.row}: ${d.msg}`);
}

// 3. Round-trip stability
const rt1 = serialize(doc);
const rt2 = serialize(parse(rt1).doc);
if (rt1 !== rt2) err('round-trip not stable (serialize∘parse∘serialize differs)');

// 4. Basic strobe-safety lint (guidelines §4): flag bright LED flicker in 15–25 Hz.
// Checks each LED row's flicker frequency endpoints (value + modEnd) against brightness.
const inDanger = (hz) => hz >= 15 && hz <= 25;
for (let i = 0; i < doc.rows.length; i++) {
  const r = doc.rows[i];
  if (r.kind !== 'led') continue;
  const fvals = [r.freq?.value, r.freq?.modEnd].filter((v) => typeof v === 'number');
  const bright = r.bright?.value ?? 0;
  for (const f of fvals) {
    if (inDanger(f) && bright > 50) {
      err(`row ${i}: LED flicker ${f} Hz in 15–25 Hz danger band at brightness ${bright}% (>50). Keep beta/SMR beats audio-driven; steady/dim light.`);
    } else if (inDanger(f) && bright > 0) {
      warn(`row ${i}: LED flicker ${f} Hz in 15–25 Hz band at brightness ${bright}% — confirm intended/dim.`);
    }
  }
}

console.log(`${file}: ${errors} error(s), ${warns} warning(s)`);
process.exit(errors ? 1 : 0);
