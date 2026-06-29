// Entry point. Resolves runtime config (generator URL), wires up the static
// markup's buttons to module functions (replacing the old inline onclick=
// handlers), then kicks off initial loads and the live-control panel.
import { appConfig } from './util.js';
import { loadExample, playConfig, stopConfig, clearConfig, bindFileInput } from './config.js';
import { refreshLedcDropdown, loadFromGenerator, saveToGenerator, saveAsToGenerator } from './generator.js';
import { ctrlInit } from './livecontrol.js';

function bind(id, fn) {
    const el = document.getElementById(id);
    if (el) el.addEventListener('click', fn);
}

async function boot() {
    // Generator base URL is injected by the firmware (CONFIG_GENERATOR_SERVER_URL)
    // via /api/appconfig, so the static assets stay device-independent.
    try {
        const cfg = await fetch('/api/appconfig').then(r => r.json());
        appConfig.generatorUrl = (cfg && cfg.generator_url) || '';
    } catch (e) {
        appConfig.generatorUrl = '';
    }

    bind('btnLoadExample', loadExample);
    bind('btnUpload', () => document.getElementById('configFile').click());
    bind('btnPlay', playConfig);
    bind('btnStop', stopConfig);
    bind('btnClear', clearConfig);
    bind('btnRefreshLedc', refreshLedcDropdown);
    bind('btnLoadGen', loadFromGenerator);
    bind('btnSaveGen', saveToGenerator);
    bind('btnSaveAsGen', saveAsToGenerator);
    bindFileInput();

    loadExample();         // populate textarea with the example config
    refreshLedcDropdown(); // fetch the generator's config list
    ctrlInit();            // build live-control panel + start state polling
}

boot();
