// Top tab navigation. Pages are toggled by the menu; the active tab is
// reflected in the URL hash so reload and the browser back/forward buttons
// work. A page may append its own state after a slash — the Reports page uses
// "#reports/<src>:<file>.rpt" to name the open report — so only the first
// segment selects the tab.
const TABS = ['home', 'generator', 'live', 'reports', 'settings', 'firmware', 'diagnostics'];

function tabFromHash() {
    return (location.hash.replace('#', '').split('/')[0]) || 'home';
}

function show(tab) {
    if (!TABS.includes(tab)) tab = 'home';
    document.querySelectorAll('.tab').forEach(b => b.classList.toggle('active', b.dataset.tab === tab));
    document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.id === 'page-' + tab));
    // The Generator's table is far wider than the 800px reading column the
    // other pages want, so give that one page the whole window rather than
    // making every page wide.
    document.querySelector('.container')?.classList.toggle('is-wide', tab === 'generator');
}

export function initTabs() {
    document.querySelectorAll('.tab').forEach(b =>
        b.addEventListener('click', () => { location.hash = b.dataset.tab; }));
    window.addEventListener('hashchange', () => show(tabFromHash()));
    show(tabFromHash());
}
