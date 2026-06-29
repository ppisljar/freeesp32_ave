// Talks to the external "generator" server (CONFIG_GENERATOR_SERVER_URL) for
// listing, loading and saving .ledc configs. Base URL comes from appConfig,
// resolved at boot from /api/appconfig.
import { appConfig, showMessage } from './util.js';

let currentLedcName = null;

// Exposed so config.js can tag uploaded session reports with the loaded name.
export function getCurrentLedcName() { return currentLedcName; }

export function refreshLedcDropdown() {
    return fetch(appConfig.generatorUrl + '/ledc')
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .then(data => {
            const dd = document.getElementById('ledcDropdown');
            dd.innerHTML = '';
            const files = data.files || [];
            if (files.length === 0) {
                const opt = document.createElement('option');
                opt.textContent = '(no configs on generator)';
                opt.disabled = true;
                dd.appendChild(opt);
                return;
            }
            files.forEach(f => {
                const opt = document.createElement('option');
                opt.value = f; opt.textContent = f;
                if (f === currentLedcName) opt.selected = true;
                dd.appendChild(opt);
            });
        })
        .catch(err => {
            const dd = document.getElementById('ledcDropdown');
            dd.innerHTML = '';
            const opt = document.createElement('option');
            opt.textContent = '(generator unreachable @ ' + appConfig.generatorUrl + ')';
            opt.disabled = true;
            dd.appendChild(opt);
        });
}

export function loadFromGenerator() {
    const name = document.getElementById('ledcDropdown').value;
    if (!name) { showMessage('Pick a config first', 'error'); return; }
    fetch(appConfig.generatorUrl + '/ledc/' + encodeURIComponent(name))
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); })
        .then(text => {
            document.getElementById('exampleConfig').value = text;
            currentLedcName = name;
            document.getElementById('loadedFilename').textContent = '(loaded: ' + name + ')';
            showMessage('Loaded ' + name + ' from generator', 'success');
        })
        .catch(err => showMessage('Load failed: ' + err, 'error'));
}

function putLedc(name, body) {
    return fetch(appConfig.generatorUrl + '/ledc/' + encodeURIComponent(name), {
        method: 'PUT',
        headers: { 'Content-Type': 'text/plain' },
        body: body,
    })
    .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
    .then(result => {
        const verb = result.overwritten ? 'Overwrote' : 'Created';
        showMessage(verb + ' ' + result.saved + ' on generator (' + result.bytes + ' bytes)', 'success');
        currentLedcName = result.saved;
        document.getElementById('loadedFilename').textContent = '(loaded: ' + result.saved + ')';
        return refreshLedcDropdown();
    })
    .catch(err => showMessage('Save failed: ' + err, 'error'));
}

export function saveToGenerator() {
    const body = document.getElementById('exampleConfig').value;
    if (!body.trim()) { showMessage('Config is empty', 'error'); return; }
    if (!currentLedcName) {
        // No file loaded — fall through to Save As so user names it.
        return saveAsToGenerator();
    }
    return putLedc(currentLedcName, body);
}

export function saveAsToGenerator() {
    const body = document.getElementById('exampleConfig').value;
    if (!body.trim()) { showMessage('Config is empty', 'error'); return; }
    const suggested = currentLedcName || 'untitled.ledc';
    let name = prompt('Save as (must end in .ledc):', suggested);
    if (!name) return;
    name = name.trim();
    if (!/^[A-Za-z0-9._-]+\.ledc$/.test(name)) {
        showMessage('Invalid filename. Allowed: [A-Za-z0-9._-]+.ledc', 'error');
        return;
    }
    // If filename exists in the dropdown AND it's not the currently-loaded one,
    // confirm overwrite. (Saving back to the current file is the normal Save
    // path; explicit Save-As to a different existing name should warn.)
    const existing = Array.from(document.getElementById('ledcDropdown').options)
                           .map(o => o.value).filter(Boolean);
    if (existing.includes(name) && name !== currentLedcName) {
        if (!confirm('"' + name + '" exists on the generator. Overwrite?')) return;
    }
    return putLedc(name, body);
}
