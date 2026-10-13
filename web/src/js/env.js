// Which host is this bundle running on.
//
// The same source builds twice:
//
//   DEVICE     bundled into the SPIFFS `storage` partition and served by the
//              ESP32 itself. Every /api/... route is real.
//   STANDALONE published to docs/generator/ on the project site, where there is
//              no device at all — and where the page must also open straight
//              from a local checkout over file://, because that is a property
//              the docs site deliberately keeps (docs/README.md).
//
// The flag is a BUILD-TIME constant rather than a runtime probe (hostname
// sniffing, a failed fetch, a <meta> tag) for one reason: esbuild folds it, so
// the standalone bundle contains no device code paths to accidentally reach,
// and the device bundle is unchanged byte-for-byte in behaviour.
//
// `__AVE_STANDALONE__` is injected by web/build.mjs via esbuild's `define`. The
// typeof guard keeps the module importable from `node --test`, where nothing
// defines it.

export const STANDALONE =
    (typeof __AVE_STANDALONE__ !== 'undefined') ? !!__AVE_STANDALONE__ : false;

// True when the page is open from the filesystem rather than over HTTP. Under
// file:// the origin is opaque, so fetch() of a sibling file is blocked and
// anything that reaches the network — the Community browser, speech synthesis —
// cannot work. Worth telling the visitor rather than letting it look broken.
export function isFileProtocol() {
    try { return typeof location !== 'undefined' && location.protocol === 'file:'; }
    catch (e) { return false; }
}
