# docs/ — the project site

This folder is the whole site: hand-written static HTML plus one shared
stylesheet, no build step, no templating and no external requests, so any page
opens correctly from a local checkout with the network unplugged. The layout is
one directory per section — `index.html` is the landing page, `hardware/` is the
builder guide, `user-guide/` is the operator guide and `sessions/` is the
`.ledc` authoring reference — and `assets/site.css` is the only stylesheet any
of them load. To add a page, copy the `<head>` and the `.site-nav` block from an
existing page verbatim (moving `aria-current="page"` onto the nav entry for the
section you are in), wrap the content in `<main class="page">`, and style it
only with the classes the stylesheet already defines: `.hero`, `.lead`, `.card`
/ `.cards`, `.note`, `.warn`, `.spec-table` (on either the wrapping `div` or the
`table` itself — both contain the horizontal scroll), `.figure` / `.figcap`,
`.toc`, `.kbd` and `.pill`. Never add a colour literal to a page: every colour
is a custom property on `:root` in `site.css` and the
`prefers-color-scheme: dark` block only re-points those tokens, so a hard-coded
hex is the one thing guaranteed to break dark mode. Diagrams are inline `<svg>`
drawn with `currentColor` and the same tokens, each with `role="img"`, a
`<title>`, and a real prose description in its `.figcap` so the content survives
when SVG does not. One caveat to know before touching links: pages reference
each other as `/docs/...`, which requires the GitHub Pages publishing source to
be the repository root (with `_config.yml` copied there) — if Pages is instead
pointed at `/docs`, that prefix has to be stripped from every `href` in all four
sections at once; see the note at the top of `_config.yml`.
