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
