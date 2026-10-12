# docs/ — the project site

Hand-written static HTML, one shared stylesheet (`assets/site.css`) and one
shared script (`assets/site.js`). No build step, no templating, and **no
external requests** — every page opens correctly from a local checkout with the
network unplugged. That is the one property the whole site is built around, so
it rules out web fonts, CDN scripts and remote images. The only non-CSS asset
the stylesheet references is an inline SVG data URI for the film grain.

## Layout

| Path | What it is |
|---|---|
| `index.html` | Landing page. Full-bleed presentation sections; no contents rail. |
| `user-guide/` | Operator guide, one page per task. |
| `sessions/` | `.ledc` authoring: a page per modality, the techniques guide, the complete reference, the authoring guide. |
| `hardware/` | Builder guide for both builds. |
| `assets/site.css` | The whole design system. |
| `assets/site.js` | Contents rail, heading anchors, film grain. |
| `assets/ui/` | Web-UI screenshots (see "Re-shooting the screenshots"). |
| `assets/hw/` | Photographs of the built devices. |

**Links are relative** (`../assets/site.css`, `sessions/reference.html`) with no
leading slash, so the site works whether Pages serves from `/docs` or from the
repository root. Do not introduce a root-absolute `/docs/...` href; it ties the
site to one publishing source and breaks the other.

## Adding a page

Copy the `<head>`, the `.site-nav` block and the `.layout` wrapper from an
existing inner page verbatim, moving `aria-current="page"` onto the nav entry
for the section you are in. The skeleton is:

```html
<div class="layout">
  <aside class="toc" id="toc"></aside>
  <main class="page">
    <div class="hero"><h1>…</h1><p class="lead">…</p></div>
    …
  </main>
</div>
<script src="../assets/site.js"></script>
```

**The contents rail builds itself** from the `h2` and `h3` elements in
`main.page`, so there is nothing to maintain by hand and it cannot drift out of
step with the headings. Give a heading an explicit `id` when something links to
it — hand-written ids always win, because they are what cross-page links point
at. Mark a heading `class="no-toc"` to keep it out of the rail (used for
headings inside cards and other furniture).

## Styling rules

Style only with the classes the stylesheet already defines: `hero`, `lead`,
`eyebrow`, `card` / `cards`, `linklist`, `note`, `warn`, `spec-table`,
`scroll-x`, `figure` / `figcap`, `shot` / `shot-bar`, `photo` / `photos`,
`part` / `partgrid`, `stat` / `stats`, `modality` / `modalities`, `band` /
`band-inner`, `btn`, `kbd`, `pill`.

**Never put a colour literal in a page.** Every colour is a custom property on
`:root` in `site.css`; a hard-coded hex is the one thing guaranteed to look
wrong. Inline `<svg>` figures draw with `currentColor` and the `--fig-*` /
`--amber` / `--cyan` / `--violet` tokens, and each needs `role="img"`, a
`<title>`, and a real prose description in its `.figcap` so the content
survives when the SVG does not.

**Motion.** This documents a photic entrainment device, so the site must never
read as flicker. There is exactly one animation — a 40-second drift on the
landing aurora, about 0.025 Hz — and everything is disabled under
`prefers-reduced-motion`. Do not add a second one without reading the note at
the top of `site.css`.

## Re-shooting the screenshots

`assets/ui/*.png` are captures of the device web UI taken against a local mock
server rather than real hardware, so they can be re-shot at any time without a
board. Serve `web/dist` with a stub that answers `/api/*`, open it in a browser
and capture each tab. Two things the mock has to get right or every value reads
zero:

- `/api/state` → `audio[]` is indexed so that **`A1` is `audio[1]`**, not
  `audio[0]`; index 0 is `.ledc` channel 0, which the live panel never shows.
- `/api/configs` → `{"files": ["name.ledc", …]}`, a list of **names**, not
  objects. `/api/logs` is **plain text**, not JSON.

Setting names must match `main/settings.c` exactly; the UI renders an empty box
for anything it cannot find.
