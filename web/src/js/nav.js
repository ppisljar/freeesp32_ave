// Top tab navigation. Four pages (home / live / reports / settings) toggled by
// the menu; the active tab is reflected in the URL hash so reload and the
// browser back/forward buttons work.
const TABS = ['home', 'live', 'reports', 'settings'];

function show(tab) {
    if (!TABS.includes(tab)) tab = 'home';
    document.querySelectorAll('.tab').forEach(b => b.classList.toggle('active', b.dataset.tab === tab));
    document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.id === 'page-' + tab));
}

export function initTabs() {
    document.querySelectorAll('.tab').forEach(b =>
        b.addEventListener('click', () => { location.hash = b.dataset.tab; }));
    window.addEventListener('hashchange', () => show(location.hash.replace('#', '')));
    show(location.hash.replace('#', '') || 'home');
}
