// Entry point. Resolves runtime config (generator URL), wires up the static
// markup's buttons to module functions (replacing the old inline onclick=
// handlers), then kicks off initial loads and the live-control panel.
import { appConfig } from './util.js';
import { loadExample, playConfig, stopConfig, clearConfig, bindFileInput } from './config.js';
import { refreshConfigList, loadSelected, saveCurrent, saveAsDialog } from './configstore.js';
import { ctrlInit } from './livecontrol.js';
import { settingsInit } from './settings.js';
import { refreshReportList, viewReport, deleteReport } from './reportstore.js';
import { initTabs } from './nav.js';

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

    bind('btnUpload', () => document.getElementById('configFile').click());
    bind('btnPlay', playConfig);
    bind('btnStop', stopConfig);
    bind('btnClear', clearConfig);
    bind('btnRefreshLedc', refreshConfigList);
    bind('btnLoadGen', loadSelected);
    bind('btnSaveGen', saveCurrent);
    bind('btnSaveAsGen', saveAsDialog);
    bind('btnReportDelete', deleteReport);
    bind('btnReportRefresh', refreshReportList);
    // View a report by selecting it in the dropdown (no separate View button).
    const reportDd = document.getElementById('reportDropdown');
    if (reportDd) reportDd.addEventListener('change', viewReport);
    bindFileInput();
    initTabs();            // top menu: home / live / reports / settings

    loadExample();         // populate textarea with the example config
    refreshConfigList();   // list configs from generator + device + browser
    ctrlInit();            // build live-control panel + start state polling
    settingsInit();        // build device-settings panel + load current values
    refreshReportList();   // list saved reports from device + browser
}

boot();
