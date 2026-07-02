# Diagnostics over WiFi — Plan & Contract

> **STATUS (2026-07-02): Layers 1–3 implemented, build clean.**
> - **Layer 1 (reset reason) + Layer 2 (log ring + `/api/logs`): DONE, OTA-flashed
>   to 10.0.0.162, and VERIFIED over HTTP** — `/api/state` returns the `diag`
>   block (`reset_reason:"SW"`, uptime, free_heap/psram, log_bytes, coredump), and
>   `/api/logs` returns the full boot log with no serial attached.
> - **Layer 3 (crash core dumps): code + config + partition DONE, build clean, but
>   NOT yet flashed — it needs ONE wired `flash_all.sh`** (adds the `coredump`
>   partition @ 0x3C0000 256 KB in the free tail; OTA can't change the partition
>   table). ⚠️ Do NOT OTA the current coredump-enabled build onto the device — it
>   would run but can't store dumps until the partition exists. New module
>   `main/diagnostics.c/.h`; endpoints in `web_server.c` (`/api/logs`,
>   `/api/coredump`, `/api/coredump/erase`, `diag` in `/api/state`); init at top of
>   `app_main`. sdkconfig: `ESP_COREDUMP_ENABLE_TO_FLASH` + ELF + CRC32.
> - **Layer 4 (Diagnostics web page): DONE + deployed + verified.** New tab
>   (`web/src/js/diagnostics.js`, `#page-diagnostics`, nav + CSS) shows reset
>   reason / uptime / heap / PSRAM / log tail + Copy/Clear, a core-dump
>   download+erase card, and a **Reboot Device** button (POST /api/reboot).
>   `/` serves HTTP 200 with the Diagnostics tab present.
> - **Layer 3 now FLASHED** — user ran `flash_all` (coredump partition + build),
>   and the app was re-OTA'd, so all 3 layers are live on 10.0.0.162.
> - **BUG FOUND+FIXED during Layer 4:** `web_server.c` `config.max_uri_handlers`
>   was 24 but registrations grew to 28 (diagnostics ×3 + tts ×1), so the LAST
>   handlers — including the catch-all `/*` static handler — silently failed to
>   register and `/` returned 404. Raised to 40. **Invariant: keep
>   max_uri_handlers > the number of httpd_register_uri_handler() calls.**
>
> Goal: recover full serial-monitor parity
> over WiFi now that the device is updated via OTA and normally has no UART
> attached. Three independent layers, composed into one **Diagnostics** web page.
> Layers 1–2 ship over OTA; Layer 3 needs a ONE-TIME wired `flash_all.sh` (adds a
> `coredump` partition — OTA cannot change the partition table). Build-only per
> step; flashing/hardware is the orchestrator's/user's job (subagents must NOT
> flash).

## Why

"Logs over WiFi" is really **two problems with opposite constraints**:

- **Runtime logs** (`ESP_LOGx` during normal operation) — the network stack is
  up, so these can be captured live and served/streamed.
- **Crash logs** (panic backtrace + register/stack dump) — when the CPU panics
  the scheduler and TCP/IP stack are frozen, so **nothing can transmit at crash
  time**. The only reliable capture is a *core dump written to flash*, retrieved
  over WiFi on the next (healthy) boot.

They need different mechanisms, hence the layers below.

## Platform facts (verified)

- 4 MB flash. Partitions end at `0x3C0000` → **256 KB free tail** (`0x3C0000`–
  `0x400000`) for a `coredump` partition; adding it there moves NO existing
  partition offset (backward-compatible layout).
- Coredump currently disabled: `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y`.
- PSRAM enabled (`CONFIG_SPIRAM=y`) — ring buffer for logs goes there.
- Web server: `httpd_uri_t` + `httpd_register_uri_handler` pattern in
  `web_server.c` (~L168–224). `app_main` in `esp32_audioplayer.c:243`,
  `nvs_flash_init` at `:248`.
- Existing idioms to reuse: `lock_free_comm.c`, `memory_pool.c`, PSRAM
  `heap_caps_malloc(MALLOC_CAP_SPIRAM)`.

---

## Layer 1 — Reset-reason breadcrumb  (trivial · OTA-able · no repartition)

**Goal:** instantly answer "did it crash, and why" after any reboot.

1. In `app_main` (early, before heavy init), call `esp_reset_reason()` and stash
   the value (`main/diagnostics.c` global, or extend an existing status struct).
   Map to a string (`POWERON`, `PANIC`, `INT_WDT`, `TASK_WDT`, `BROWNOUT`, `SW`,
   `DEEPSLEEP`, …).
2. Surface it in the web API: add fields to `GET /api/state` (or a new
   `GET /api/health`) — `reset_reason`, plus uptime (`esp_timer_get_time`) and
   `esp_get_free_heap_size()` / `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`.
3. **Success:** after a forced panic (e.g. a debug endpoint that dereferences
   NULL, gated behind a build flag), the next boot's `/api/state` reports
   `reset_reason: "PANIC"`.

## Layer 2 — Live runtime log tail over WiFi  (moderate · OTA-able · no repartition)

**Goal:** reproduce the normal-operation serial stream over HTTP.

1. **Capture:** install `esp_log_set_vprintf(diag_vprintf)` as the FIRST thing in
   `app_main` (before other init, so early boot logs are captured). `diag_vprintf`
   must:
   - still forward to the original UART vprintf (so a wired console keeps working
     during development), then
   - `vsnprintf` into a small stack buffer and copy the bytes into a **lock-free
     byte ring buffer in PSRAM** (e.g. 32 KB). MUST be non-blocking and safe from
     any task context — no mutex that a logging task could already hold, no heap
     alloc, no network I/O in the hook. Drop-oldest on overflow.
   - Guard against reentrancy (a log call from within the hook).
2. **Serve — pull first:** `GET /api/logs` returns the current ring contents
   (oldest→newest) as `text/plain`. Optional `?clear=1` to reset after read.
   Simple, robust, enough for "show me the last 32 KB".
3. **Serve — live (optional, phase 2b):** WebSocket `/ws/logs` (esp_http_server
   WS support) or UDP syslog to a host. A low-priority drain task pushes new ring
   bytes to connected sinks. Defer unless live tail is needed.
4. **Caveats to document:** logs before WiFi joins are buffered but only visible
   once `/api/logs` is reachable; ISR-context `ESP_EARLY_LOGx` (via
   `esp_rom_printf`) bypasses the vprintf hook — minor, accept it.
5. **Success:** `curl http://<ip>/api/logs` returns recent `ESP_LOGx` lines
   matching what serial would show; ring wraps without crashing under load; no
   deadlock when a mutex-holding task logs.

## Layer 3 — Crash core dumps over WiFi  (moderate · needs ONE wired flash)

**Goal:** retrieve the full panic backtrace + task stacks over WiFi.

1. **Partition:** add to `partitions.csv` (in the free tail, nothing else moves):
   ```
   coredump,  data, coredump, ,       0x40000,
   ```
   (256 KB; could be smaller, but tail is free so use it.)
2. **Config:** `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`,
   `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`,
   `CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y` (or SHA). Set stack size / max tasks as
   needed.
3. **Serve:** `GET /api/coredump` — use `esp_core_dump_image_get(&addr, &size)`
   to check for a stored dump; if present, stream the partition bytes as
   `application/octet-stream` (chunked). `GET /api/coredump/status` returns
   `{present: bool, size}`. `POST /api/coredump/erase` calls
   `esp_core_dump_image_erase()` after retrieval.
4. **Boot integration:** on boot, if `esp_core_dump_image_check()` finds a dump,
   log a WARNING and set a flag exposed in `/api/state` (`coredump_present:true`)
   so the Diagnostics page can prompt to download.
5. **Host retrieval flow (documented):**
   ```
   curl http://<ip>/api/coredump -o coredump.bin
   espcoredump.py info_corefile  -c coredump.bin build/esp32_audioplayer.elf
   espcoredump.py dbg_corefile   -c coredump.bin build/esp32_audioplayer.elf
   ```
   **REQUIREMENT:** keep the exact matching `.elf` for every OTA'd build —
   symbolization needs the binary the crash came from. Add a small `ota_flash.sh`
   note / archive step (copy `build/esp32_audioplayer.elf` beside the pushed
   `.bin`, tagged by version).
6. **Success:** force a panic; after reboot `/api/state` shows
   `coredump_present:true`; downloaded dump + matching ELF yields a symbolized
   backtrace via `espcoredump.py`.

## Layer 4 — Diagnostics web page  (ties it together · OTA-able)

Add a **Diagnostics** tab to the web UI (vanilla-JS `web/src/js/…`, esbuild):
- Reset reason + uptime + free heap/PSRAM (poll `/api/state`).
- Live log pane (poll `/api/logs`, or WS if built) with a "Clear" and "Copy".
- Core-dump card: shows `coredump_present`, "Download core dump" button, and the
  exact `espcoredump.py` command line (with the running version string filled
  in), plus an "Erase" button after download.
- Rebuild web assets (`cd web && npm run build`) so SPIFFS `storage` image
  updates; ship via `flash_web.sh` (storage-only, OTA-able) — no app reflash
  needed for UI-only changes.

## Sequencing & flashing

1. Implement **Layer 1 + Layer 2 + Layer 4(partial)** → build clean → **OTA**
   (`./ota_flash.sh <ip>`). No partition change, so pure OTA.
2. Implement **Layer 3** (partition + config + endpoints) → build clean → this
   REQUIRES one wired `flash_all.sh` (new partition table). Do it in the next
   wired session, or now while the device is handy.
3. Finish **Layer 4** core-dump card → rebuild web → `flash_web.sh` (OTA).

## Risks & notes

- **vprintf hook is the sharp edge:** it runs in arbitrary task context. Keep it
  allocation-free, lock-free, reentrancy-guarded, non-blocking. A bug here can
  deadlock or crash the whole device on the next log call. Unit-test the ring in
  isolation; add a compile flag to disable capture if it misbehaves.
- **Panic path must stay minimal:** do NOT try to add WiFi/HTTP work to the panic
  handler — core dump to flash is the whole point; retrieval happens after
  reboot.
- **ELF/version matching** for core dumps — a mismatched ELF gives garbage
  backtraces. Archive ELFs per OTA.
- **Flash wear:** core dump writes only on panic (rare); log ring is RAM-only.
  No wear concern.
- **Security:** `/api/logs` and `/api/coredump` may expose internal detail; the
  device is LAN/SoftAP only, but note it. Consider gating behind the existing
  auth if any is added later.
- **Report:** on completion write `reports/diagnostics_over_wifi_report.md`.
