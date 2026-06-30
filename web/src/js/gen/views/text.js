// Generator Text view (Phase 2).
//
// A first-class raw `.led`/`.ledc` editor kept in sync with the shared model —
// the always-available escape hatch. It is a projection of the single `doc`
// owned by gen/generator.js:
//
//   - When the model changes elsewhere (another view / Load), it writes
//     serialize(doc) into the textarea, but ONLY when the textarea is not
//     focused, so it never fights the user's caret.
//   - On textarea input it debounces ~250 ms, then parse()s the text:
//       * clean  -> replace the controller's doc (ctx.setDoc), badge "✓ synced"
//                   (or "~ formatted" when valid but not canonical whitespace).
//       * errors -> KEEP the last-good doc (do not corrupt the model), badge
//                   "⚠ N error(s)", and render gutter markers + tooltips on the
//                   offending lines. parse() itself already holds bad lines as
//                   `raw` rows, so the good lines still round-trip on the next
//                   clean edit.
//   - A "Problems" strip beneath the editor merges validate(doc) + parse
//     diagnostics; each entry jumps to its line.
//
// This module is decoupled from generator.js: the controller passes a small
// ctx { getDoc, setDoc } so there is no circular import.

import { parse } from '../parse.js';
import { serialize } from '../serialize.js';
import { validate } from '../validate.js';

const DEBOUNCE_MS = 250;

// ---- Pure helper (unit-tested) --------------------------------------------
// Merge parse diagnostics (keyed by 1-based `line`) with validate diagnostics
// (keyed by `row` = index into doc.rows; -1 means a whole-document problem).
// parse() emits exactly one row per source line, so row index i maps to line
// i+1 in both the typed text and serialize(doc). Returns a de-duplicated,
// line-sorted list of { line:Number|null, severity, msg }.
export function mergeDiagnostics(parseDiags, validateDiags) {
    const out = [];
    const seen = new Set();
    const push = (line, severity, msg) => {
        const key = (line == null ? '-' : line) + '|' + severity + '|' + msg;
        if (seen.has(key)) return;
        seen.add(key);
        out.push({ line: (line == null ? null : line), severity, msg });
    };
    for (const d of (parseDiags || [])) push(d.line, d.severity, d.msg);
    for (const d of (validateDiags || [])) {
        const line = (d.row === undefined || d.row === null || d.row < 0) ? null : d.row + 1;
        push(line, d.severity, d.msg);
    }
    const sev = s => (s === 'error' ? 0 : 1);
    out.sort((a, b) => {
        const la = a.line == null ? Infinity : a.line;
        const lb = b.line == null ? Infinity : b.line;
        if (la !== lb) return la - lb;
        return sev(a.severity) - sev(b.severity);
    });
    return out;
}

// Collapse a merged diagnostics list into a per-line lookup for the gutter.
//   line -> { severity:'error'|'warn', msgs:[...] }  (error wins over warn)
function diagsByLine(merged) {
    const map = new Map();
    for (const d of merged) {
        if (d.line == null) continue;
        const cur = map.get(d.line);
        if (!cur) {
            map.set(d.line, { severity: d.severity, msgs: [d.msg] });
        } else {
            cur.msgs.push(d.msg);
            if (d.severity === 'error') cur.severity = 'error';
        }
    }
    return map;
}

export function initTextView(ctx) {
    const root = document.getElementById('genTextView');
    const ta = document.getElementById('genTextArea');
    const gutter = document.getElementById('genTextGutter');
    const badge = document.getElementById('genTextBadge');
    const problems = document.getElementById('genProblems');
    if (!ta) return { refresh() {}, show() {}, hide() {} };

    let debounceTimer = null;

    // ---- Badge ------------------------------------------------------------
    function setBadge(kind, text) {
        if (!badge) return;
        badge.textContent = text;
        badge.className = 'gen-badge gen-badge-' + kind;
    }

    // ---- Gutter -----------------------------------------------------------
    function renderGutter(text, byLine) {
        if (!gutter) return;
        const count = text.length ? text.split('\n').length : 1;
        const frag = document.createDocumentFragment();
        for (let i = 1; i <= count; i++) {
            const div = document.createElement('div');
            div.className = 'gen-gutter-line';
            const d = byLine.get(i);
            if (d) {
                div.classList.add(d.severity === 'error' ? 'has-error' : 'has-warn');
                div.title = d.msgs.join('\n');
                div.textContent = (d.severity === 'error' ? '✖ ' : '⚠ ') + i;
            } else {
                div.textContent = String(i);
            }
            frag.appendChild(div);
        }
        gutter.innerHTML = '';
        gutter.appendChild(frag);
        gutter.scrollTop = ta.scrollTop;
    }

    // ---- Problems strip ---------------------------------------------------
    function renderProblems(merged) {
        if (!problems) return;
        problems.innerHTML = '';
        if (!merged.length) {
            const ok = document.createElement('div');
            ok.className = 'gen-problem gen-problem-ok';
            ok.textContent = 'No problems.';
            problems.appendChild(ok);
            return;
        }
        for (const d of merged) {
            const item = document.createElement('div');
            item.className = 'gen-problem gen-problem-' + d.severity;
            const loc = document.createElement('span');
            loc.className = 'gen-problem-loc';
            loc.textContent = d.line == null ? 'file' : ('line ' + d.line);
            const msg = document.createElement('span');
            msg.className = 'gen-problem-msg';
            msg.textContent = d.msg;
            item.appendChild(loc);
            item.appendChild(msg);
            if (d.line != null) {
                item.classList.add('gen-problem-jump');
                item.addEventListener('click', () => jumpToLine(d.line));
            }
            problems.appendChild(item);
        }
    }

    function jumpToLine(n) {
        const lines = ta.value.split('\n');
        let pos = 0;
        for (let i = 0; i < n - 1 && i < lines.length; i++) pos += lines[i].length + 1;
        const end = pos + (lines[n - 1] ? lines[n - 1].length : 0);
        ta.focus();
        ta.setSelectionRange(pos, end);
        const lh = parseFloat(getComputedStyle(ta).lineHeight) || 16;
        ta.scrollTop = Math.max(0, (n - 1) * lh - ta.clientHeight / 2);
        gutter.scrollTop = ta.scrollTop;
    }

    // Render badge + gutter + problems for the given text & its parse result.
    function renderDiagnostics(text, parseDiags, doc) {
        const merged = mergeDiagnostics(parseDiags, validate(doc));
        renderGutter(text, diagsByLine(merged));
        renderProblems(merged);
        return merged;
    }

    // ---- Input handling ---------------------------------------------------
    function handleInput() {
        const text = ta.value;
        const { doc: parsedDoc, diagnostics: parseDiags } = parse(text);
        const parseErrs = parseDiags.filter(d => d.severity === 'error');

        // Diagnostics always reflect the CURRENT text (precise per-line feedback)
        // regardless of whether we commit the model.
        renderDiagnostics(text, parseDiags, parsedDoc);

        if (parseErrs.length) {
            // Keep the last-good model untouched — do not corrupt it.
            setBadge('error', '⚠ ' + parseErrs.length + ' error' +
                (parseErrs.length > 1 ? 's' : ''));
            return;
        }

        // Clean parse -> commit the new model.
        ctx.setDoc(parsedDoc, parseDiags);
        const canonical = serialize(parsedDoc);
        if (canonical === text) {
            setBadge('ok', '✓ synced');
        } else {
            setBadge('warn', '~ formatted');
        }
    }

    ta.addEventListener('input', () => {
        if (debounceTimer) clearTimeout(debounceTimer);
        debounceTimer = setTimeout(handleInput, DEBOUNCE_MS);
    });
    ta.addEventListener('scroll', () => { if (gutter) gutter.scrollTop = ta.scrollTop; });

    // ---- Model-changed hook ----------------------------------------------
    // Called by the controller on every onModelChanged. Write serialize(doc)
    // into the textarea ONLY when it is not focused (no caret fight); when the
    // user is actively typing, handleInput owns the UI.
    function refresh(doc) {
        if (document.activeElement === ta) return;
        const text = serialize(doc);
        ta.value = text;
        renderDiagnostics(text, [], doc);
        setBadge('ok', '✓ synced');
    }

    function show() { if (root) root.style.display = ''; refresh(ctx.getDoc()); }
    function hide() { if (root) root.style.display = 'none'; }

    return { refresh, show, hide };
}
