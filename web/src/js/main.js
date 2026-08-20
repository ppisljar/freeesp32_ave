// Entry point. Resolves runtime config (generator URL), wires up the static
// markup's buttons to module functions (replacing the old inline onclick=
// handlers), then kicks off initial loads and the live-control panel.
import { appConfig } from './util.js';
import { loadExample, playConfig, stopConfig, clearConfig, bindFileInput } from './config.js';
import { refreshConfigList, loadSelected, saveCurrent, saveAsDialog } from './configstore.js';
import { ctrlInit } from './livecontrol.js';
import { settingsInit } from './settings.js';
import { refreshReportList, viewReport, deleteReport, saveReportEdits, syncReportFromHash,
         toggleDeviceLog, downloadDeviceLog, toggleBrowserLog, downloadBrowserLog } from './reportstore.js';
import { initTabs } from './nav.js';
import { generatorInit } from './gen/generator.js';
import { firmwareInit } from './firmware.js';
import { diagnosticsInit } from './diagnostics.js';

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
    // Load a config as soon as it's picked in the dropdown (no separate Load
    // button — mirrors the Reports dropdown's select-to-view behaviour).
    const ledcDd = document.getElementById('ledcDropdown');
    if (ledcDd) ledcDd.addEventListener('change', loadSelected);
    bind('btnSaveGen', saveCurrent);
    bind('btnSaveAsGen', saveAsDialog);
    bind('btnReportDelete', deleteReport);
    bind('btnReportRefresh', refreshReportList);
    bind('btnReportSave', saveReportEdits);
    bind('btnReportLogView', toggleDeviceLog);
    bind('btnReportLogDownload', downloadDeviceLog);
    bind('btnReportBLogView', toggleBrowserLog);
    bind('btnReportBLogDownload', downloadBrowserLog);
    // View a report by selecting it in the dropdown (no separate View button).
    const reportDd = document.getElementById('reportDropdown');
    if (reportDd) reportDd.addEventListener('change', viewReport);
    // "#reports/<src>:<file>.rpt" names the open report: honour it on load and
    // on back/forward.
    window.addEventListener('hashchange', syncReportFromHash);
    bindFileInput();
    initTabs();            // top menu: home / live / reports / settings

    loadExample();         // populate textarea with the example config
    refreshConfigList();   // list configs from generator + device + browser
    ctrlInit();            // build live-control panel + start state polling
    settingsInit();        // build device-settings panel + load current values
    generatorInit();       // build the Generator tab (shared model + preview)
    firmwareInit();        // wire up the Firmware Update tab (OTA flow)
    diagnosticsInit();     // wire up the Diagnostics tab (logs / coredump / reboot)
    refreshReportList();   // list saved reports from device + browser
    syncReportFromHash();  // open the report named in the URL, if any
}

boot();
