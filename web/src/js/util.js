// Shared UI helpers and runtime app config.

// Filled in at boot from GET /api/appconfig — the firmware injects the
// generator base URL from CONFIG_GENERATOR_SERVER_URL. Kept as a mutable
// object so other modules can read appConfig.generatorUrl after boot resolves
// it (the value isn't known at import time).
export const appConfig = { generatorUrl: '' };

// Show a status banner at the top of the page.
export function showMessage(message, type) {
    const statusDiv = document.getElementById('status');
    statusDiv.textContent = message;
    statusDiv.className = 'status ' + type;
    statusDiv.style.display = 'block';
}

// Modal chooser. `options` = [{ label, value, desc? }]. Resolves with the chosen
// value, or null if cancelled (backdrop click / Cancel / Esc). Used e.g. for the
// "this session has speech" prompt on Play.
export function chooseModal(title, options, message) {
    return new Promise((resolve) => {
        const backdrop = document.createElement('div');
        backdrop.className = 'modal-backdrop';
        const box = document.createElement('div');
        box.className = 'modal-box';
        const h = document.createElement('h3');
        h.textContent = title;
        box.appendChild(h);
        if (message) {
            const p = document.createElement('div');
            p.className = 'modal-msg';
            p.textContent = message;
            box.appendChild(p);
        }
        let done = false;
        const finish = (v) => { if (done) return; done = true; backdrop.remove(); document.removeEventListener('keydown', onKey); resolve(v); };
        for (const opt of options) {
            const b = document.createElement('button');
            b.className = 'modal-opt';
            b.textContent = opt.label;
            if (opt.desc) b.title = opt.desc;
            b.addEventListener('click', () => finish(opt.value));
            box.appendChild(b);
        }
        const cancel = document.createElement('button');
        cancel.className = 'modal-cancel';
        cancel.textContent = 'Cancel';
        cancel.addEventListener('click', () => finish(null));
        box.appendChild(cancel);
        backdrop.appendChild(box);
        backdrop.addEventListener('click', (e) => { if (e.target === backdrop) finish(null); });
        const onKey = (e) => { if (e.key === 'Escape') finish(null); };
        document.addEventListener('keydown', onKey);
        document.body.appendChild(backdrop);
    });
}
