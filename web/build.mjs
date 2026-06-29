// Web UI build: bundle + minify the vanilla-JS modules and CSS with esbuild,
// then gzip every asset (level 9) into ./data, which the firmware's CMake
// packs into the SPIFFS "storage" partition. Only the .gz files are shipped;
// the C file server serves them with Content-Encoding: gzip and falls back
// between plain/.gz names. ./dist holds the uncompressed, human-readable
// output for inspection.
import { build } from 'esbuild';
import { gzipSync } from 'node:zlib';
import { mkdirSync, rmSync, writeFileSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = dirname(fileURLToPath(import.meta.url));
const SRC = join(ROOT, 'src');
const DIST = join(ROOT, 'dist');
const DATA = join(ROOT, 'data');

function reset(dir) { rmSync(dir, { recursive: true, force: true }); mkdirSync(dir, { recursive: true }); }
reset(DIST);
reset(DATA);

// 1. Bundle + minify JS (single output, ES module).
const js = await build({
    entryPoints: [join(SRC, 'js', 'main.js')],
    bundle: true,
    minify: true,
    format: 'esm',
    target: 'es2020',
    write: false,
});
const jsOut = js.outputFiles[0].text;
writeFileSync(join(DIST, 'app.js'), jsOut);

// 2. Minify CSS (single output).
const css = await build({
    entryPoints: [join(SRC, 'css', 'style.css')],
    bundle: true,
    minify: true,
    loader: { '.css': 'css' },
    write: false,
});
const cssOut = css.outputFiles[0].text;
writeFileSync(join(DIST, 'style.css'), cssOut);

// 3. HTML — copy as-is (gzip flattens the whitespace anyway).
const html = readFileSync(join(SRC, 'index.html'), 'utf8');
writeFileSync(join(DIST, 'index.html'), html);

// 4. gzip each asset into ./data as <name>.gz (level 9).
function gz(name, text) {
    const raw = Buffer.from(text);
    const buf = gzipSync(raw, { level: 9 });
    writeFileSync(join(DATA, name + '.gz'), buf);
    return { raw: raw.length, gz: buf.length };
}
const assets = {
    'index.html': gz('index.html', html),
    'app.js': gz('app.js', jsOut),
    'style.css': gz('style.css', cssOut),
};

console.log('Web UI built -> data/ (gzipped, packed into SPIFFS):');
let totalRaw = 0, totalGz = 0;
for (const [name, s] of Object.entries(assets)) {
    totalRaw += s.raw; totalGz += s.gz;
    console.log('  ' + name.padEnd(12) + s.raw.toString().padStart(7) + ' B  ->  ' + s.gz.toString().padStart(6) + ' B .gz');
}
console.log('  ' + 'TOTAL'.padEnd(12) + totalRaw.toString().padStart(7) + ' B  ->  ' + totalGz.toString().padStart(6) + ' B .gz');
