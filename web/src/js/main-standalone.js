// Entry point for the STANDALONE build — the generator published to
// docs/generator/ on the project site, with no device anywhere.
//
// Deliberately NOT main.js with the device parts switched off. main.js boots
// eight subsystems, of which seven only exist because there is hardware on the
// other end of /api: the live-control panel, device settings, OTA firmware
// update, diagnostics, saved reports, the device config store, and the status
// strip that listens for device state. Importing them and then hiding them
// would still pull their code into the bundle, still fire their boot requests,
// and still print the two red banners the audit found ("Error loading example",
// "Settings load failed") on a page that is working perfectly.
//
// So this entry imports exactly the generator and nothing else.

import { generatorInit } from './gen/generator.js';
import { isFileProtocol } from './env.js';

function boot() {
    generatorInit();

    // The Community browser reads a session index from GitHub, which cannot
    // work from a file:// origin. Say so once, where it will be read, rather
    // than letting that one tab look broken.
    if (isFileProtocol()) {
        const s = document.getElementById('status');
        if (s) {
            s.textContent = 'Opened straight from a file — everything works '
                + 'except the Community tab, which needs a web server to reach '
                + 'GitHub. Serve this folder over HTTP if you want it.';
            s.className = 'status info';
            s.style.display = 'block';
        }
    }
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
} else {
    boot();
}
