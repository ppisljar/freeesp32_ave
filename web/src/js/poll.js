// Polling the device without strangling it.
//
// THE BUG THIS EXISTS TO PREVENT
//
// Every poller here used to be `setInterval(asyncFn, ms)`. That schedules by
// wall clock and ignores whether the previous call finished, so the moment the
// device answers slower than the interval the requests STACK. They never
// recover on their own, because each new tick adds another one.
//
// Chrome allows 6 concurrent HTTP/1.1 connections per host, and the device's
// httpd has only 7 sockets total. Once the stacked polls hold all six, every
// other request from the page — including a user clicking "Sync speech to SD
// card" — queues behind them forever. The page looks dead while the device is
// perfectly healthy: curl from a terminal answers in 40 ms, because curl isn't
// inside Chrome's exhausted connection pool. That divergence is what makes this
// so disorienting to debug, so: poll through this module, never setInterval.
//
// THE RULES
//
// 1. Self-scheduling. The next run is scheduled only after the previous one
//    SETTLES, so overlap is structurally impossible rather than merely
//    discouraged. The interval becomes a gap between runs, not a deadline.
// 2. Bounded. Every request gets a timeout, so a connection that stalls (dozing
//    wifi, a device mid-reboot) is released instead of held until the tab
//    closes. An unbounded fetch is a leaked socket.
// 3. Stoppable. startPolling returns stop(); hidden views must call it. A
//    poller that keeps running behind a hidden tab is spending one of six
//    connections on nothing.

// Timeouts and the concurrency limit live in devicefetch.js — poll bodies must
// use deviceFetch(), not bare fetch(). This module only owns CADENCE.

// Run `fn` every `intervalMs`, measured from the END of the previous run, with
// never more than one run in flight. Returns stop().
//
// `fn` may be async; it is always awaited. A rejection is swallowed (a failed
// poll is normal — the device reboots, wifi blips) but does NOT stop the loop,
// and does not skip the next interval.
//
// options.immediate (default true) runs fn once right away, as setInterval
// callers usually did by hand.
export function startPolling(fn, intervalMs, { immediate = true } = {}) {
    let timer = null;
    let stopped = false;

    const schedule = () => {
        if (stopped) return;
        timer = setTimeout(run, intervalMs);
    };

    async function run() {
        timer = null;
        if (stopped) return;
        try {
            await fn();
        } catch (e) {
            /* transient — the next tick retries */
        }
        schedule();                 // only now, after settling
    }

    if (immediate) run(); else schedule();

    return function stop() {
        stopped = true;
        if (timer) { clearTimeout(timer); timer = null; }
    };
}
