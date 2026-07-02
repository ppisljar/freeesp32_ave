// Device Settings panel. Fetches the runtime settings from GET /api/settings,
// renders grouped form fields, and writes changes back with POST /api/settings.
// Hardware settings take effect on the next reboot ("Save & Reboot").
//
// Fields are shown/hidden based on dependencies (e.g. DotStar-only fields appear
// only when the LED backend is DotStar; codec I2C fields only when a codec is
// selected; the SD group only when SD support is compiled in). Each field has an
// info icon (ⓘ) with hover help. The LED-backend / audio-codec selectors are
// capability-driven: options not compiled into this firmware are greyed out.
import { showMessage } from './util.js';

// Current settings object as last fetched/applied (used as the partial-update
// base — we POST the whole object, but only edited fields will differ).
let current = null;

// Predicate helpers over the live form selections {led_backend, audio_codec, caps}.
const isAddressable = v => v.led_backend === 'neopixel' || v.led_backend === 'dotstar';
const isDotstar     = v => v.led_backend === 'dotstar';
const isDirect      = v => v.led_backend === 'direct';
const hasCodec      = v => v.audio_codec && v.audio_codec !== 'none';

// Field group definitions. type: 'num' | 'text' | 'pin' | 'vol' | 'select' | 'pins8' | 'password'
// Optional per-field `show(v)` controls visibility; optional group `show(v)` hides a whole group.
const GROUPS = [
    {
        id: 'led', title: 'LED',
        fields: [
            { key: 'led_backend', label: 'Backend', type: 'select',
              options: ['neopixel', 'dotstar', 'direct'], capsKey: 'led', note: 'reboot to apply',
              help: 'LED hardware type. Neopixel = WS2812/SK6812 (single data wire). DotStar = APA102 (data + clock, SPI). Direct = discrete GPIOs driven by PWM. Options not compiled into this firmware are greyed out.' },
            { key: 'led_data_pin', label: 'Data pin', type: 'pin', show: isAddressable,
              help: 'GPIO carrying the data signal to the addressable strip (Neopixel/DotStar).' },
            { key: 'led_clock_pin', label: 'Clock pin (DotStar)', type: 'pin', show: isDotstar,
              help: 'GPIO for the DotStar/APA102 SPI clock line.' },
            { key: 'led_count', label: 'LED count', type: 'num', show: isAddressable,
              help: 'Number of pixels on the addressable strip.' },
            { key: 'led_channel_map', label: 'Channel map (CSV)', type: 'text', show: isAddressable,
              help: 'Comma-separated logical flicker-channel index per pixel — maps each LED to one of the control channels. Length should equal LED count.' },
            { key: 'led_grid_width', label: 'Grid width', type: 'num', show: isAddressable,
              help: 'Logical grid columns, for matrix/visualization layout of the addressable strip.' },
            { key: 'led_grid_height', label: 'Grid height', type: 'num', show: isAddressable,
              help: 'Logical grid rows, for matrix/visualization layout of the addressable strip.' },
            { key: 'led_direct_pins', label: 'Direct pins (8)', type: 'pins8', show: isDirect,
              help: 'GPIO for each of the 8 direct PWM LED channels (channel 0 → 7).' },
            { key: 'led_direct_active_low_mask', label: 'Direct active-low mask (0-255)', type: 'num', show: isDirect,
              help: 'Bitmask: set bit N to drive direct channel N active-low (inverted) — for sinking outputs / common-anode wiring.' },
            { key: 'led_dotstar_spi_clock_hz', label: 'DotStar SPI clock (Hz)', type: 'num', show: isDotstar,
              help: 'SPI bus clock used to drive the DotStar strip, in Hz (e.g. 10000000 = 10 MHz).' },
        ],
    },
    {
        id: 'i2s', title: 'Audio I2S',
        fields: [
            { key: 'i2s_bck_pin', label: 'BCK pin', type: 'pin',
              help: 'I2S bit-clock (BCK/SCK) output GPIO.' },
            { key: 'i2s_ws_pin', label: 'WS pin', type: 'pin',
              help: 'I2S word-select (WS/LRCLK) output GPIO — selects left/right channel.' },
            { key: 'i2s_data_pin', label: 'DATA out pin', type: 'pin',
              help: 'I2S serial audio data output GPIO, to the DAC or codec.' },
            { key: 'i2s_mclk_pin', label: 'MCLK pin (-1 = off)', type: 'pin',
              help: 'I2S master clock output GPIO. Required by some I2C codecs; set -1 if unused.' },
            { key: 'i2s_din_pin', label: 'DIN pin (-1 = off)', type: 'pin',
              help: 'I2S serial audio data input GPIO (recording/loopback). Set -1 if unused.' },
            { key: 'amp_enable_pin', label: 'Amp enable pin (-1 = none)', type: 'pin',
              help: 'GPIO that enables the external power amplifier (driven high on start). -1 = no amp control.' },
            { key: 'default_volume', label: 'Default volume (0-1)', type: 'vol',
              help: 'Output volume applied at startup, from 0.0 (mute) to 1.0 (full).' },
            { key: 'audio_max_volume', label: 'Maximum volume (0-100)', type: 'num', note: 'applies immediately',
              help: 'Master output cap as a percent. Scales ALL audio uniformly — e.g. set to 10 and a config/live volume of 80 plays at 80% × 10% = 8%. 100 = no attenuation. Takes effect right away (no reboot).' },
        ],
    },
    {
        id: 'codec', title: 'Audio Codec',
        fields: [
            { key: 'audio_codec', label: 'Codec', type: 'select',
              options: ['none', 'ac101', 'es8388'], capsKey: 'codec', note: 'reboot to apply',
              help: 'External I2C-controlled audio codec. None = passive DAC / raw I2S (no I2C control). Options not compiled in are greyed out.' },
            { key: 'codec_i2c_port', label: 'I2C port (0/1)', type: 'num', show: hasCodec,
              help: 'ESP32 I2C peripheral (0 or 1) used to configure the codec.' },
            { key: 'codec_i2c_sda', label: 'I2C SDA pin', type: 'pin', show: hasCodec,
              help: 'GPIO for the codec control bus I2C data (SDA) line.' },
            { key: 'codec_i2c_scl', label: 'I2C SCL pin', type: 'pin', show: hasCodec,
              help: 'GPIO for the codec control bus I2C clock (SCL) line.' },
            { key: 'codec_i2c_freq_hz', label: 'I2C freq (Hz)', type: 'num', show: hasCodec,
              help: 'I2C bus clock for the codec control bus, in Hz (typically 100000).' },
        ],
    },
    {
        id: 'sd', title: 'SD Card', show: v => !!(v.caps && v.caps.sd),
        fields: [
            { key: 'sd_cs', label: 'CS pin', type: 'pin', help: 'SD card SPI chip-select GPIO.' },
            { key: 'sd_mosi', label: 'MOSI pin', type: 'pin', help: 'SD card SPI MOSI GPIO.' },
            { key: 'sd_miso', label: 'MISO pin', type: 'pin', help: 'SD card SPI MISO GPIO.' },
            { key: 'sd_clk', label: 'CLK pin', type: 'pin', help: 'SD card SPI clock GPIO.' },
        ],
    },
    {
        id: 'controls', title: 'Controls',
        fields: [
            { key: 'button_gpio', label: 'Button GPIO (-1 = none)', type: 'pin', note: 'reboot to apply',
              help: 'GPIO of the momentary push button (wired to GND; internal pull-up). Short press logs a live-state snapshot; long press (3 s) stops all audio/LED. Set -1 to disable.' },
        ],
    },
    {
        id: 'network', title: 'Network',
        fields: [
            { key: 'generator_url', label: 'Generator URL', type: 'text',
              help: 'Base URL of the generator server used to load/save configs and upload reports. Leave blank to disable generator features.' },
        ],
    },
    {
        id: 'wifi', title: 'WiFi',
        fields: [
            { key: 'wifi_ssid', label: 'WiFi SSID', type: 'text', note: 'reboot to apply',
              help: "Name of the WiFi network to join. If empty or unreachable, the device starts its own 'ESP32-AVE-Setup' access point (192.168.4.1) so you can reach this page." },
            { key: 'wifi_password', label: 'WiFi password', type: 'password', note: 'reboot to apply; leave blank to keep current',
              help: 'Password for the WiFi network. The stored password is never shown; leave this blank to keep the current one.' },
        ],
    },
    {
        id: 'reports', title: 'Reports',
        fields: [
            { key: 'report_storage', label: 'Store reports in', type: 'select',
              options: ['none', 'local', 'spiffs'], note: 'also uploaded to generator when a URL is set',
              help: 'Where session reports are persisted: none = display only, local = this browser (localStorage), spiffs = on the device. Reports are always also uploaded to the generator when its URL is set.' },
        ],
    },
];

function inputId(key) { return 'set-' + key; }
function esc(s) { return String(s).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;'); }

function helpIcon(f) {
    if (!f.help) return '';
    return ` <span class="help" title="${esc(f.help)}">ⓘ</span>`;
}

function fieldHtml(f, val, settings) {
    const id = inputId(f.key);
    let inner;
    if (f.type === 'password') {
        // The GET never echoes the stored password; wifi_password_set tells us
        // whether one exists. Render an empty field — blank on save keeps it.
        const isSet = settings && settings.wifi_password_set;
        const ph = isSet ? 'unchanged — leave blank to keep' : 'not set';
        inner = `<input type="password" id="${id}" value="" placeholder="${ph}" autocomplete="new-password" style="min-width:220px;">`;
    } else if (f.type === 'select') {
        // Capabilities-driven: when f.capsKey is set, only options present in
        // settings.supported[capsKey] are selectable; the rest are greyed out so
        // the user can see which backends/codecs this firmware was built with.
        const caps = (f.capsKey && settings && settings.supported &&
                      Array.isArray(settings.supported[f.capsKey]))
            ? settings.supported[f.capsKey] : null;
        const opts = f.options.map(o => {
            const supported = !caps || caps.includes(o);
            const dis = supported ? '' : ' disabled';
            const tag = supported ? '' : ' (not compiled in)';
            return `<option value="${o}"${o === val ? ' selected' : ''}${dis}>${o}${tag}</option>`;
        }).join('');
        inner = `<select id="${id}">${opts}</select>`;
    } else if (f.type === 'pins8') {
        const arr = Array.isArray(val) ? val : [];
        inner = '<span style="display:inline-flex;gap:4px;flex-wrap:wrap;">' +
            arr.map((v, i) =>
                `<input type="number" id="${id}-${i}" value="${v}" style="width:52px;" title="ch${i}">`
            ).join('') + '</span>';
    } else if (f.type === 'text') {
        inner = `<input type="text" id="${id}" value="${esc(val)}" style="min-width:260px;">`;
    } else if (f.type === 'vol') {
        inner = `<input type="number" id="${id}" value="${val}" step="0.05" min="0" max="1" style="width:80px;">`;
    } else { // num | pin
        inner = `<input type="number" id="${id}" value="${val}" style="width:90px;">`;
    }
    const note = f.note ? ` <span style="color:#999;font-size:11px;">(${f.note})</span>` : '';
    return `<div class="set-row" id="row-${f.key}" style="display:flex;align-items:center;gap:8px;margin:4px 0;">
        <label for="${id}" style="min-width:220px;font-size:13px;">${f.label}${helpIcon(f)}${note}</label>
        ${inner}
    </div>`;
}

function render(settings) {
    current = settings;
    const root = document.getElementById('settings-body');
    if (!root) return;
    let html = '';
    for (const g of GROUPS) {
        html += `<div class="set-group" id="group-${g.id}" style="margin:12px 0;padding:10px;background:#fafafa;border:1px solid #e0e0e0;border-radius:4px;">
            <h3 style="margin:0 0 6px 0;font-size:14px;">${g.title}</h3>`;
        for (const f of g.fields) {
            html += fieldHtml(f, settings[f.key], settings);
        }
        html += '</div>';
    }
    root.innerHTML = html;
    // Re-evaluate visibility whenever a dependency (a select) changes.
    root.addEventListener('change', applyVisibility);
    applyVisibility();
}

// Read the current form selections that drive visibility (fall back to the last
// fetched settings before the form exists).
function currentSel() {
    const beEl = document.getElementById(inputId('led_backend'));
    const cdEl = document.getElementById(inputId('audio_codec'));
    return {
        led_backend: beEl ? beEl.value : (current && current.led_backend),
        audio_codec: cdEl ? cdEl.value : (current && current.audio_codec),
        caps: (current && current.supported) || {},
    };
}

function applyVisibility() {
    const v = currentSel();
    for (const g of GROUPS) {
        const gEl = document.getElementById('group-' + g.id);
        if (gEl && g.show) gEl.style.display = g.show(v) ? '' : 'none';
        for (const f of g.fields) {
            const rEl = document.getElementById('row-' + f.key);
            if (rEl) rEl.style.display = f.show ? (f.show(v) ? '' : 'none') : '';
        }
    }
}

// Collect the form values into a settings object suitable for POST. Hidden rows
// keep their inputs in the DOM, so their values still round-trip unchanged.
function collect() {
    const out = {};
    for (const g of GROUPS) {
        for (const f of g.fields) {
            const id = inputId(f.key);
            if (f.type === 'select') {
                out[f.key] = document.getElementById(id).value;
            } else if (f.type === 'pins8') {
                const arr = [];
                for (let i = 0; i < 8; i++) {
                    const el = document.getElementById(id + '-' + i);
                    if (el) arr.push(parseInt(el.value, 10) || 0);
                }
                out[f.key] = arr;
            } else if (f.type === 'text' || f.type === 'password') {
                out[f.key] = document.getElementById(id).value;
            } else if (f.type === 'vol') {
                out[f.key] = parseFloat(document.getElementById(id).value) || 0;
            } else {
                out[f.key] = parseInt(document.getElementById(id).value, 10) || 0;
            }
        }
    }
    return out;
}

export function loadSettings() {
    return fetch('/api/settings')
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .then(render)
        .catch(err => showMessage('Settings load failed: ' + err, 'error'));
}

function postSettings() {
    const body = JSON.stringify(collect());
    return fetch('/api/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body,
    }).then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); });
}

function saveSettings() {
    postSettings()
        .then(s => { render(s); showMessage('Settings saved (reboot to apply hardware changes)', 'success'); })
        .catch(err => showMessage('Save failed: ' + err, 'error'));
}

function saveAndReboot() {
    postSettings()
        .then(() => fetch('/api/reboot', { method: 'POST' }))
        .then(() => showMessage('Settings saved — device is rebooting…', 'success'))
        .catch(err => showMessage('Save & Reboot failed: ' + err, 'error'));
}

function restoreDefaults() {
    if (!confirm('Restore all settings to firmware defaults?')) return;
    fetch('/api/settings/reset', { method: 'POST' })
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .then(s => { render(s); showMessage('Settings restored to defaults', 'success'); })
        .catch(err => showMessage('Restore failed: ' + err, 'error'));
}

// Build a download-friendly timestamp like 20260630-134005 from a Date.
function fileTimestamp(d) {
    const p = n => String(n).padStart(2, '0');
    return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}` +
        `-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}

// Export the fresh canonical settings (from GET, so no password) as a
// pretty-printed .json file downloaded by the browser.
function exportSettings() {
    fetch('/api/settings')
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .then(obj => {
            const blob = new Blob([JSON.stringify(obj, null, 2)], { type: 'application/json' });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `freeesp32_ave-settings-${fileTimestamp(new Date())}.json`;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
            showMessage('Settings exported (password not included)', 'success');
        })
        .catch(err => showMessage('Export failed: ' + err, 'error'));
}

// Open the hidden file picker to import a settings .json.
function importSettings() {
    const el = document.getElementById('settingsImportFile');
    if (el) el.click();
}

// File-change handler: parse the picked .json and load it into the form for
// review. Does NOT POST — the user reviews and clicks Save / Save & Reboot.
function onImportFile(ev) {
    const input = ev.target;
    const file = input.files && input.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = () => {
        try {
            const obj = JSON.parse(reader.result);
            if (!obj || typeof obj !== 'object' || Array.isArray(obj)) {
                throw new Error('not a settings object');
            }
            render(obj);
            applyVisibility();
            showMessage('Imported — review the fields and click Save to apply.', 'success');
        } catch (err) {
            showMessage('Import failed: ' + err, 'error');
        }
    };
    reader.onerror = () => showMessage('Import failed: could not read file', 'error');
    reader.readAsText(file);
    // Reset so re-picking the same file re-fires change.
    input.value = '';
}

function bind(id, fn) {
    const el = document.getElementById(id);
    if (el) el.addEventListener('click', fn);
}

export function settingsInit() {
    bind('btnSettingsSave', saveSettings);
    bind('btnSettingsReboot', saveAndReboot);
    bind('btnSettingsRestore', restoreDefaults);
    bind('btnSettingsReload', loadSettings);
    bind('btnSettingsExport', exportSettings);
    bind('btnSettingsImport', importSettings);
    const fileEl = document.getElementById('settingsImportFile');
    if (fileEl) fileEl.addEventListener('change', onImportFile);
    loadSettings();
}
