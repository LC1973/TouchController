# RGB Panel Display Issues — Diagnosis & Fix History

This file exists because the screen-roll/flicker/corruption bug on this board has been
chased before and partially "fixed" more than once, only to come back. Read this
before touching `initRgbPanel()`, `lvgl_flush_cb()`, or anything in `board_pinout.h`
under `BOARD_PROFILE_1024X600`.

## ⏸️ CURRENT STATUS: "Attempt E" architecture is the confirmed baseline; a
## separate, rarer, longer-duration corruption is UNRESOLVED — investigation
## PAUSED by user decision (2026-08-27/28), not abandoned

"Attempt E" (below) fixed the frequent, short-lived roll/flicker/corruption that
motivated this whole file — confirmed clean immediately after that fix. **Keep it as
the baseline; don't revert its architecture choices (see the warning further down).**

**A separate problem is still unsolved on top of that baseline:** intermittent
corruption that recurs roughly every 10-11 hours of uptime (observed independently
on at least two occasions under different configurations) and self-clears after
some time. **Six specific hypotheses have been tested and ruled out** (flash
debug logging, WiFi reconnect blocking, TCI's reconnect loop, Overview/Prop label
throttling, the periodic self-heal repaint) — see "Ongoing investigation:
long-duration corruption episodes" near the end of this file for the full trace.
**The user has paused active work on this "for now."** If picked back up, read
that whole section first — especially the "If/when this is picked back up" list
at the very end of it, which recommends a scheduled-reboot mitigation as the next
concrete step rather than another speculative fix.

However, as of 2026-08-23 a *different* corruption pattern was still occurring on
top of that fixed baseline: long stable stretches, then a corrupted roll/garbled-
text state persisting for an extended period (10+ minutes) before self-clearing.
Chased through several application-level hypotheses (all individually ruled out or
found insufficient: flash debug logging, WiFi reconnect blocking, per-second uptime
ticking, unthrottled heap/PSRAM label, a WiFi-RSSI-rounding sign bug) before the
picture got more complicated: corruption turned out NOT to be Overview-tab-
exclusive after all (also seen on the Propagation tab once watched for longer), and
the "switch tabs to clear it" trick that worked once stopped working on a later
occurrence. Web research (2026-08-26) then confirmed this class of bug — PSRAM-
framebuffer RGB LCD corruption on ESP32-S3 — is a **known, still-unresolved
architectural limitation**, not an application bug to keep hunting for: CPU and
GDMA share PSRAM bandwidth 50/50 with no priority scheme, and multiple open
Espressif/community issues describe the same symptom with no driver-level fix.
**First mitigation attempt (periodic display self-heal: full LVGL repaint +
`esp_lcd_rgb_panel_restart()` every 30s) did NOT resolve it** — recurred on the
Power tab, persistent (not brief), described as garbled/scrambled rather than a
clean shift. That combination (self-heal running every 30s but not clearing it,
plus "garbled" rather than "displaced") points away from GDMA timing desync (what
`esp_lcd_rgb_panel_restart()` fixes) and toward **stale/torn CPU cache writes to
the PSRAM frame buffer** — a documented failure mode in this exact driver
(multiple open Espressif issues). Added an explicit `esp_cache_msync()` cache
writeback after every frame buffer write in `lvgl_flush_cb()` — **but combined
with the still-active periodic repaint, this made things WORSE: corruption
recurred within ~5 minutes**, the fastest onset yet. Most likely cause: the
periodic full-screen repaint (large, guaranteed, recurring PSRAM-heavy burst) was
itself a new source of triggers, not a cure — never tested in isolation before
being combined with the cache fix, so the two changes were confounded. Repaint
removed, keeping only the cheap DMA-only `esp_lcd_rgb_panel_restart()` every 30s
plus the `esp_cache_msync()` fix — but then **escalated further (2026-08-27):
10h uptime, vertical roll AND corruption on every panel simultaneously**, despite
both the automatic `CONFIG_LCD_RGB_RESTART_IN_VSYNC` (fires ~every vsync) and the
manual periodic restart running the whole time. Four fix attempts in a row have
each been plausible and none has held up.

**Strategy change (2026-08-27): paused further speculative fixes, switched to
structured data collection.** Added a web-based panel-check reporting tool
(`/api/panelcheck`, prompted every 5 minutes via `script.js` from any page on the
site) that logs a full device-state snapshot whenever the user reports the
display as good or bad — deliberately without touching the touch panel's own UI,
since navigating it can itself clear a corruption episode. Goal: get actual
evidence before attempting another fix, rather than continuing to guess.

**TCI reconnect loop (fetched via `curl http://192.168.1.200/log` — reachable
directly from the dev machine) was a strong-looking lead (reconnecting every 5s
against an unreachable host, matching a "corrupted 5 seconds after boot" report)
but was ALSO ruled out** after the user disabled it and corruption still
occurred — the fifth specific hypothesis eliminated in a row (after flash
logging, WiFi STA-reconnect blocking, Overview label chattiness, and the
periodic self-heal). **Current: found and fixed a second instance of the
"unthrottled per-second label" bug class, this time on the Prop tab**
(`prop_updated_lbl`, the "Solar: Xs PSK: Ys" freshness readout — same bug as the
old Overview uptime label, missed at the time because Prop wasn't yet known to be
affected). Genuine fix, kept regardless of outcome, but treat with the same
calibration as the four fixes before it — none has yet been confirmed to change
the corruption's actual frequency. **Not yet tested on hardware.** See "Ongoing
investigation: long-duration corruption episodes" near the end of this file for
the full trace, including the research citations and the diagnostic tool's
design reasoning.

The working architecture, in one paragraph: `num_fbs=1` + `bounce_buffer_size_px`
(single PSRAM frame buffer, ESP-IDF's bounce-buffer anti-tearing scheme), LVGL using
small **separate** partial-render strip buffers in PSRAM (NOT pointers into the
panel's own frame buffer), plain `esp_lcd_panel_draw_bitmap()` + `lv_disp_flush_ready()`
in `lvgl_flush_cb()` with **no manual vsync blocking**, and `LCD_FREQ_WRITE=30000000`
with the original porches in `board_pinout.h`. This is exactly the architecture from
this project's very first commit (`477709e`) — see "Attempt E" below for the full
trace of how it was rediscovered and what had to change (the rotator bearing-line
feature) to work under it.

**If this regresses again:** do not re-introduce `esp_lcd_rgb_panel_get_frame_buffer()`,
`disp_drv.direct_mode`, `num_fbs>1`, a vsync semaphore/ISR in `lvgl_flush_cb()`, or
change `LCD_FREQ_WRITE`/the porches in `board_pinout.h`'s `BOARD_PROFILE_1024X600`
block, without first reading this whole file. Attempts A-D (below) each tuned one of
those exact knobs, on the *previous, now-abandoned* zero-copy architecture, and each
one failed on real hardware in a different way (roll, flicker, glitching, or a
visibly slow redraw) — they are kept here as a record of dead ends, not options to
retry.

## Symptoms reported (2026-08-22)

- Screen roll: bottom rows shift down a few pixels and reappear at the top.
- Flickering of the whole screen, described as "like low refresh".
- Text/UI occasionally corrupted or not drawn properly.
- Worse at startup; sometimes self-corrects for a while, then returns at an
  unpredictable moment.

## Hardware/driver background

This board drives a 1024x600 panel over the ESP32-S3's parallel RGB LCD peripheral
(`esp_lcd_new_rgb_panel`), with the frame buffer(s) living in octal PSRAM
(`flags.fb_in_psram = 1`). Unlike a SPI/QSPI "smart" panel with its own on-glass RAM,
an RGB-parallel TFT has **no memory of its own** — the ESP32 must continuously stream
every pixel, every frame, for as long as the picture is to stay visible. That has two
consequences that explain almost all the symptoms:

1. **PSRAM is a shared, finite-bandwidth resource.** Both the LCD peripheral's GDMA
   (reading pixel data out to the panel) and the CPU (LVGL rendering into the frame
   buffer, WiFi, JSON parsing, etc. — see `lv_mem_psram.h`, which deliberately routes
   *all* LVGL heap allocations to PSRAM) contend for the same PSRAM controller. If the
   CPU starves the DMA for too long, the GDMA can lose sync with the LCD peripheral's
   internal pixel counter. This does not corrupt data so much as desynchronise
   *position*: the peripheral keeps clocking out pixels on schedule, but GDMA is
   feeding it stale/wrapped data, so the image appears to roll/shift — permanently,
   until something restarts the DMA. This is a **documented Espressif erratum**, not
   specific to this project:
   > "If your draw buffers are in PSRAM ... both CPUs will be accessing PSRAM via
   > cache, sharing its bandwidth. This significantly increases the memory copy time
   > in the DMA EOF ISR ... resulting in a screen shift." — ESP-IDF RGB LCD docs,
   > "Avoiding Tearing Effects" / known issues section.
   ([docs.espressif.com rgb_lcd.html](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html))

2. **Refresh rate is bandwidth-limited, and this panel is currently run very slow.**
   With the current porch values (`BOARD_PROFILE_1024X600` in `board_pinout.h`),
   total pixels/frame = (1024+160+20+140) × (600+12+3+20) = 853,440. At the current
   `LCD_FREQ_WRITE = 14000000`, that's **~16.4 Hz** actual panel refresh. That is low
   enough to be visibly flickery on its own, on top of and separate from any
   tearing/shift bug. It was deliberately lowered from a higher value specifically to
   reduce PSRAM bandwidth pressure and hold off issue (1) — a real tradeoff, not an
   oversight.

Espressif ships two independent, well-tested mitigations for (1):

- **Double/triple buffering** (`num_fbs = 2` or `3`, no bounce buffer): LVGL always
  renders into the buffer that is *not* currently being scanned out, so the CPU write
  and the GDMA read never touch the same memory concurrently. Docs call this "the
  simplest method" to prevent tearing.
- **Bounce buffer** (`bounce_buffer_size_px` with a single frame buffer): GDMA reads
  from a small internal-SRAM staging buffer instead of PSRAM directly; a CPU ISR
  keeps that staging buffer topped up from the PSRAM frame buffer. Higher achievable
  pclk, but *more* CPU/PSRAM contention per the docs, and cannot run while the PSRAM
  cache is disabled (e.g. during flash writes).

These are presented in Espressif's own basic examples as alternatives — but real
hardware testing on this panel (see "Attempt A" below) showed they actually fix two
*different* problems and this board needs both together; don't take "presented as
alternatives in the docs" as "mutually exclusive." `CONFIG_LCD_RGB_RESTART_IN_VSYNC`
(auto-recovers a shifted image at the next
vsync) is **already enabled by default** in this project's framework build
(`framework-arduinoespressif32-libs/esp32s3/qio_opi/include/sdkconfig.h:841`) — that's
not a lever we can pull further from Arduino-framework PlatformIO without switching to
the ESP-IDF framework (sdkconfig isn't exposed to a precompiled Arduino core).

## What's been tried in this repo, chronologically

1. **LovyanGFX `Bus_RGB`** (original driver) — bypasses `esp_lcd_new_rgb_panel()`
   entirely and drives GDMA directly. Abandoned: causes DMA reads of stale PSRAM data
   from a cache-coherency bug on ESP32-S3 rev v0.2
   (`ESP_ROM_HAS_CACHE_WRITEBACK_BUG`). Commit `477709e`.

2. **Switched to `esp_lcd_new_rgb_panel()` with a bounce buffer**, `num_fbs = 2`.
   Commit `477709e`. This is the combination — bounce buffer *and* double buffering
   together — that Espressif's own reference example does **not** use (see below).

3. **Reverted to `num_fbs = 1`** (single PSRAM frame buffer, LVGL `direct_mode`,
   still with the bounce buffer). Commit `d8eafe4`, May 2026. Commit message
   attributed this to: "with num_fbs=2 the DMA pointer flip at vsync could start the
   bounce-buffer ISR scanning from a wrong row offset, producing a circular frame
   shift." **However**, the same commit also fixed a real bug in `lvgl_flush_cb()`:
   before this commit, `esp_lcd_panel_draw_bitmap()` was called on *every* partial
   flush, not gated behind `lv_disp_flush_is_last()`. Calling it once per partial
   dirty-rect (rather than once per full LVGL refresh cycle) would repeatedly queue a
   buffer-flip request mid-frame — which is a far more likely explanation for "DMA
   pointer flip... wrong row offset" than a hardware/driver race. In other words: the
   double-buffer attempt was very likely broken by *our own* flush_cb bug, and got
   blamed on the buffer count instead. That flush_cb bug was fixed in the same
   commit, but only ever shipped alongside `num_fbs = 1`, so the fix was never
   retested with double buffering.

   Going to single buffer removed the *symmetric* risk (LVGL now had only one buffer
   to possibly desync), but did nothing about mechanism (1) above — bandwidth
   starvation causing GDMA/LCD desync — which can happen with one buffer just as
   with two. It also made LVGL draw directly into the live, actively-scanned frame
   buffer, which is the worst case for tearing/text corruption (mechanism (1),
   maximally exposed): CPU pixel writes and GDMA reads now routinely target the same
   memory at the same time, not just contend for aggregate bandwidth.

4. **Lowered `LCD_FREQ_WRITE` to 14 MHz and widened blanking porches**, to buy the
   bounce-buffer ISR "massive rests" between lines. Reduces bandwidth pressure (helps
   mechanism (1)) at the direct cost of a very low ~16.4 Hz refresh (mechanism (2),
   the "like low refresh" flicker complaint).

5. **`LV_INV_BUF_SIZE` raised to 256** (`lv_conf.h`). Documented reason: with the
   single-buffer/direct_mode setup, LVGL falling back to a full-screen invalidate
   (its default behaviour once more than 32 dirty rects accumulate in one refresh)
   triggered a 1.2 MB PSRAM→PSRAM memcpy that itself starved the bounce-buffer ISR —
   i.e. mechanism (1) again, self-inflicted by LVGL's own overflow fallback. Still a
   reasonable safety margin to keep even after the changes below.

6. **Dead code discovered while investigating (2026-08-22, this session):**
   `updateBearingLinesDirect()` (draws the rotator bearing line directly into the
   raw framebuffer, bypassing LVGL, for the azimuthal map on the Rotator tab) has an
   early-return guard `if (!buf1 || !buf2 || ...) return;`. Since step 3 above, `buf2`
   is never assigned (stays `nullptr` for the lifetime of the program in
   single-buffer mode) — so this function has been silently returning immediately on
   every call. **The rotator bearing line has not been drawing.** This is a
   consequence of reverting to single-buffer without fully reverting the code that
   was written for the double-buffer world (the dual-buffer write loop, the guard,
   the "write to both buffers" comments were all left in place but neutered).

## Attempt A: double buffer, no bounce buffer (2026-08-22, superseded — see Attempt B)

Restored `num_fbs = 2` (double PSRAM frame buffer), **without** a bounce buffer, and
fixed the two direct-framebuffer-write call sites to write to both buffers again:

- `initRgbPanel()`: `num_fbs = 2`, `bounce_buffer_size_px` removed.
- Setup code: `esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &buf1, &buf2)` (was `1`,
  `&buf1` only, with `buf2` never touched); `lv_disp_draw_buf_init(&draw_buf, buf1,
  buf2, ...)` (was `NULL` for the second buffer). This also fixes the dead-code bug
  in item 6 above as a side effect — `buf2` is real again.
- `updateBearingLinesDirect()`: restored the `for (fb_idx 0..1) { fb = buf1 or buf2
  }` loop that writes bearing-line pixels into both buffers (was hardcoded to
  `buf1` only).
- `drawAzimuthalMap()`: restored the second `lv_timer_handler()` call so LVGL has a
  chance to propagate the new map image into *both* hardware buffers before direct
  writes start layering bearing lines on top (was calling it once).
- `lvgl_flush_cb()` was **not** changed — it already correctly gates
  `esp_lcd_panel_draw_bitmap()` + the vsync wait behind `lv_disp_flush_is_last()`,
  which is the fix from step 3 above and is the correct pattern per Espressif's own
  `esp_lvgl_port` RGB display driver (confirmed by reading
  `esp_lvgl_port_disp.c`/`lvgl8` in `espressif/esp-bsp` — same flush_cb structure:
  draw_bitmap + drain-then-block-on-vsync-semaphore, only on the last flush).
- `LCD_FREQ_WRITE` was **deliberately left at 14 MHz** for this pass, to isolate the
  double-buffering change as the only variable under test. See "Next steps" below
  for raising it once this is confirmed stable.

This matches Espressif's reference configuration exactly: their `esp_lvgl_port`
`rgb_lcd` example uses `num_fbs = 2`, `direct_mode = true`, and **no**
`bounce_buffer_size_px` (confirmed by reading
`esp-bsp/components/esp_lvgl_port/examples/rgb_lcd/main/main.c`).

**Why this combination should not repeat the May 2026 "content shift" regression:**
that regression very plausibly came from the flush_cb bug (draw_bitmap called on
every partial flush) described in step 3, which has already been fixed and is being
kept as-is. The only change in this pass is the buffer count / buffer pointers
themselves, applied on top of the already-fixed flush_cb.
**(This theory turned out to be wrong — see Attempt B's hardware result below.)**

### Attempt A — hardware test result (2026-08-22)

Flashed via OTA and tested on the physical panel. Result: **vertical roll is gone.**
But: **whole-screen flicker, and "lots of left-right oddities"** (per-scanline
horizontal glitching/smearing) appeared in its place.

This is a clean, diagnostic result, not a failure — it isolates the two mechanisms
described above from each other:

- Vertical roll fixed → confirms mechanism (1) (CPU write / GDMA read racing on the
  *same* buffer, causing frame-level desync) really was the roll's cause, and double
  buffering really does fix *that specific* mechanism, as the docs promise.
- New horizontal glitching + flicker appeared → with the bounce buffer removed, GDMA
  now reads PSRAM *directly* during scanout. Double buffering only guarantees the
  buffer GDMA is reading isn't the one the CPU is writing to *this frame* — it does
  nothing about PSRAM access latency jitter *within* a scanline (CPU/WiFi/LVGL still
  share the same PSRAM bus/controller in aggregate, even when reading/writing
  different buffers). A stall mid-line makes the LCD peripheral clock out
  stale/repeated pixel data for the rest of that line — a horizontal artifact — and
  at ~16 Hz refresh, frequent small glitches read as overall flicker.

In short: double buffering and the bounce buffer solve two *different* problems
(frame-level race vs. within-scanline bandwidth jitter), not two alternative fixes
for the same problem as the "Espressif ships two independent mitigations... not
something to combine" framing above assumed. This panel/workload needs both.

## Attempt B: double buffer + bounce buffer combined (2026-08-22, current)

Re-added `bounce_buffer_size_px = LCD_WIDTH * 20` to the `num_fbs = 2` config from
Attempt A. Nothing else changed. Rationale: bounce buffer insulates GDMA from PSRAM
jitter (should fix the new horizontal glitching/flicker) while double buffering
keeps preventing the CPU/GDMA same-buffer race (should keep the vertical roll fixed).

This is exactly the combination flagged as risky in Attempt A's original writeup,
because a prior attempt at `num_fbs=2` + bounce buffer (May 2026, commit `d8eafe4`)
was blamed for a "content shift" artifact and reverted. The reasoning for trying it
again anyway: that regression is much better explained by a *since-fixed* flush_cb
bug (`esp_lcd_panel_draw_bitmap()` called on every partial flush instead of gated
behind `lv_disp_flush_is_last()` — see item 3 under "What's been tried" above) than
by the buffer/bounce combination itself, and the current `lvgl_flush_cb()` already
has that fix. Attempt A's clean vertical-roll fix (no shift artifact at all) is
itself evidence the flush_cb fix is doing its job correctly with `num_fbs=2` — adding
the bounce buffer back on top of that shouldn't reopen the old bug.

### Attempt B — hardware test result (2026-08-22)

Flashed via OTA and tested on the physical panel. Result: **both the vertical roll
and the distorted text are back.** Worse than Attempt A, which had no roll and no
text distortion (only flicker/horizontal glitching).

**This disproves the "it was just the flush_cb bug" theory above.** The reasoning
for trying bounce buffer + `num_fbs=2` again — that the May 2026 regression was
actually caused by a flush_cb bug that's since been fixed, not by the buffer
combination itself — is now contradicted by direct reproduction on this exact
toolchain with that flush_cb fix already in place. Whatever the real mechanism is
(most likely: the bounce-buffer refill ISR's internal tracking of "which of the 2
PSRAM buffers is currently front" doesn't handle the buffer-flip event correctly —
plausible given the bounce buffer and the double-buffer flip are two separate
pieces of driver state that both need to change atomically at the same vsync, and
nothing in the public API guarantees that), **`bounce_buffer_size_px` combined with
`num_fbs=2` (or 3) is confirmed broken on this project's toolchain
(`framework-arduinoespressif32-libs @ 5.5.4+sha.735507283d`, pioarduino
platform-espressif32 55.3.38). Do not retry this combination without a materially
different approach** (e.g. a different IDF/Arduino-ESP32 version, or filing/finding
an upstream bug report first).

## Attempt C: revert to double buffer only (2026-08-22, current baseline)

Removed `bounce_buffer_size_px` again, back to exactly Attempt A's configuration
(`num_fbs=2`, no bounce buffer) — confirmed identical firmware size to Attempt A
(1,805,319 bytes) after rebuild, i.e. a clean revert with no drift. This is the best
confirmed state so far: **no vertical roll, no text distortion**, with the
known-remaining issue being flicker + horizontal glitching from GDMA reading PSRAM
directly (see Attempt A's analysis above for the mechanism).

**Not yet re-verified on hardware after the revert** — needs an OTA push to confirm
we're back to Attempt A's behaviour (expected: roll and text distortion gone again,
flicker/horizontal glitching present).

## How to verify

1. Flash `touchpanel-ota` (or serial) and watch the screen through a full boot + at
   least 10–15 minutes of normal use across all tabs (the map/rotator tab
   specifically, since it exercises the direct-framebuffer path).
2. Check the Rotator tab's azimuthal map: the green bearing line should draw (dead
   code fixed in Attempt A — a good sanity check that `buf2` is alive and both
   buffers are being written).
3. Confirm vertical roll and text distortion are gone (matching Attempt A, not
   Attempt B). Note whether flicker/horizontal glitching is present, and how bad.
4. If it's clean for an extended period (30+ min) with normal WiFi/HTTP traffic
   happening (PropProxy poll, link monitor poll, rotator memory fetch — exactly the
   kind of concurrent PSRAM/CPU load that provokes bandwidth contention), the roll
   fix is solid and only the flicker/glitching remains to be addressed.

## Triple buffering: considered and rejected (2026-08-22, not implemented)

`num_fbs = 3` was the first thing tried for the remaining flicker/glitching, on the
theory that more buffering slack reduces how bursty CPU-side PSRAM writes are. It
was investigated but **not implemented**, for two concrete reasons found before any
code was written:

1. **LVGL v8's `direct_mode` zero-copy scheme cannot use 3 buffers.**
   `lv_disp_draw_buf_t` (`lv_hal_disp.h:52-53`) only has `buf1`/`buf2` fields, and
   `lv_refr.c`'s sync logic only ever checks those two. Simply requesting a 3rd
   frame buffer from the panel driver and not passing it to LVGL would leave that
   buffer completely unused — no benefit, just ~1.2 MB of wasted PSRAM. Real triple
   buffering needs `full_refresh = 1` with a buffer LVGL owns separately from the
   panel's own buffers, handed to `esp_lcd_panel_draw_bitmap()` each frame.

2. **That in turn runs into how the RGB panel driver actually handles a foreign
   (non-owned) source buffer.** Checked against the ESP-IDF 5.5 source for
   `esp_lcd_panel_rgb.c` (matching this project's
   `framework-arduinoespressif32-libs @ 5.5.4`): when `draw_bitmap`'s source buffer
   is *not* one of the panel's own `fbs[]`, the driver memcpy's it into
   `fbs[cur_fb_index]` — and `cur_fb_index` only advances when the source buffer
   *is* one of its own buffers (the zero-copy path). With a foreign LVGL buffer,
   there's a real risk `cur_fb_index` just stays pinned to one buffer forever,
   meaning the extra buffers from `num_fbs=3` would never actually get used —
   only verifiable by flashing it and checking, not by reading the code path with
   confidence.

Combined with the fact that switching to `full_refresh` also requires reworking
`updateBearingLinesDirect()` / `fbFillRect()` / `fbDrawText()` (currently poke pixels
directly into the hardware buffers, bypassing LVGL entirely — under `full_refresh`
those pokes would get overwritten by the very next redraw cycle unless rewritten to
update `map_buf` and go through `lv_obj_invalidate()` instead), this was judged too
large and too uncertain a change to burn an OTA test cycle on without a clearer
payoff. Revisit only if the ESP-IDF source can be read directly (not just fetched
via an intermediary) to confirm the buffer-selection behaviour first.

## Attempt D: lower pclk (2026-08-22, superseded — see Attempt E)

`LCD_FREQ_WRITE` lowered from 14 MHz to 11 MHz in `board_pinout.h`
(`BOARD_PROFILE_1024X600`), on top of Attempt C's `num_fbs=2`/no-bounce-buffer
config. Refresh drops from ~16.4 Hz to ~12.9 Hz (853,440 total pclks/frame).

Rationale: the remaining flicker/horizontal glitching is GDMA underrunning against
PSRAM latency jitter while reading the frame buffer directly (no bounce buffer to
insulate it — see Attempt A's analysis). Lowering pclk reduces GDMA's *sustained*
PSRAM read-rate requirement, which directly targets that mechanism — unlike the
original plan (documented earlier in this file) to *raise* pclk once double
buffering was in place, which was reasoning about the wrong flicker source (assumed
low-refresh perceptual flicker was the only mechanism; it isn't, once the bounce
buffer is off the table).

**Real, acknowledged risk:** this trades one flicker mechanism for less of another.
Lower refresh rate can itself look worse, independent of whether the underrun
glitching improves. This needs a direct A/B comparison against 14 MHz on hardware —
there's no way to predict which nets out better without testing.

### Attempt D — hardware test result (2026-08-22)

Flashed and tested. Result: **worse.** The refresh rate drop (~12.9Hz) was itself
obviously visible as a slow, "watching it paint" full-screen redraw, AND the
left-right shifting/glitching persisted anyway. So this was a clean negative on
both counts: lowering pclk did not meaningfully reduce the glitching, and made the
low-refresh flicker markedly worse. This is useful negative evidence — it suggests
the glitching is not primarily an *average*-bandwidth problem (which pclk directly
controls), more likely caused by individual CPU-side stalls long enough to starve
GDMA's tiny internal FIFO regardless of pclk in the range tested (14 vs 11MHz is
only a 21% difference — a much larger drop might behave differently, but at the
cost of refresh rate low enough to be unusable, so not worth pursuing further on
this architecture).

At this point the user mentioned the display used to work and couldn't recall which
commit broke it — which led to Attempt E below, a different avenue entirely rather
than continued knob-tuning on the same architecture.

## Attempt E: revert to the original architecture (2026-08-23)

Traced git history (only 4 commits total; this repo's history starts mid-refactor)
to find what "used to work." The very first commit, `477709e` ("WIP: Replace
LovyanGFX with esp_lcd_new_rgb_panel"), used an architecture **completely
different** from everything tuned in Attempts A-D:

- `num_fbs = 1` + `bounce_buffer_size_px = LCD_WIDTH * 10` (single buffer + bounce,
  not double buffer with or without bounce).
- `LCD_FREQ_WRITE = 30000000` (30MHz, ~32.75Hz refresh) with different porches
  entirely (`HSYNC_PULSE_WIDTH=162`, `HSYNC_BACK_PORCH=152`, `VSYNC_PULSE_WIDTH=45`,
  etc. — not tuned variations of the 14MHz/11MHz timings used all session).
- LVGL configured with small **separate** partial-render buffers in PSRAM
  (`LCD_WIDTH * LVGL_BUF_LINES`, `LVGL_BUF_LINES=48` — a few dozen rows, not the
  full screen), allocated via plain `heap_caps_malloc`, **not** pointers into the
  panel's own frame buffer via `esp_lcd_rgb_panel_get_frame_buffer()`.
- `disp_drv.direct_mode` **not set at all** — plain LVGL partial-rendering mode.
- `lvgl_flush_cb()` with **no vsync blocking whatsoever**: just
  `esp_lcd_panel_draw_bitmap()` + `lv_disp_flush_ready()`, called for every partial
  flush. No `s_vsync_sem`, no `vsync_isr_cb`, no `esp_lcd_rgb_panel_register_event_
  callbacks()` at all.

**All of that — the zero-copy `direct_mode` scheme, the vsync-semaphore blocking in
flush_cb, `esp_lcd_rgb_panel_get_frame_buffer()`, and the reduced 14MHz timing —
was introduced together in `b492e07` ("Restore full-featured main.cpp"), a commit
whose message is entirely about application features (remote RSSI, peer uptime,
rotator memories, power confirmation) and never mentions the display driver.** Line
count jumped from 4,027 to 6,480 in that one commit. The most plausible explanation:
a full-featured `main.cpp` (and its `board_pinout.h`) was pasted back in from a
separate branch/backup, and the display-driver rewrite + timing downgrade came
along for the ride, untested as a deliberate change in its own right. Every symptom
chased this entire session (roll, shift, flicker, glitching), across every buffer/
pclk combination tried in Attempts A-D, was built on top of that same architecture.

**What changed to revert to it:**

- `board_pinout.h` (`BOARD_PROFILE_1024X600`): all RGB timing constants restored to
  `477709e`'s values (30MHz, original porches).
- `initRgbPanel()`: `num_fbs = 1`, `bounce_buffer_size_px = LCD_WIDTH * 10`.
- Removed `s_vsync_sem`, `vsync_isr_cb`, `s_pending_flush_drv`, and the
  `esp_lcd_rgb_panel_register_event_callbacks()` call entirely.
- `lvgl_flush_cb()`: back to the simple `draw_bitmap()` + `flush_ready()` form, no
  `lv_disp_flush_is_last()` gating, no blocking.
- `setup()`: `buf1`/`buf2` now `heap_caps_malloc(LCD_WIDTH * LVGL_BUF_LINES * ...,
  MALLOC_CAP_SPIRAM)` (added `#define LVGL_BUF_LINES 48`), with an internal-RAM
  fallback if PSRAM alloc fails (matching `477709e`). Removed
  `esp_lcd_rgb_panel_get_frame_buffer()` and `disp_drv.direct_mode`.

**One thing that could not be reverted as-is:** the rotator bearing-line feature
(`updateBearingLinesDirect()`, `fbFillRect()`, `fbDrawText()`) did not exist at
`477709e` — it was added later, specifically built around the zero-copy scheme
(poking pixels directly into the *hardware* frame buffer at absolute screen
coordinates, tracked via a lazily-resolved `map_fb_x`/`map_fb_y`). With `buf1`/
`buf2` now small partial strip buffers instead of full-screen buffers, that
approach is invalid — those buffers no longer correspond to fixed screen regions.
Reworked instead to write into `map_buf` (the existing `MAP_SIZE`x`MAP_SIZE` pixel
buffer already used as the canvas image source, already written this way by
`drawAzimuthalMap()` elsewhere in the same file) at `MAP_SIZE`-relative
coordinates, then call `lv_obj_invalidate(canvas_map)` to let LVGL redraw it
through the normal partial-buffer pipeline. `fbFillRect()`/`fbDrawText()` were
narrowed from generic screen-buffer helpers to `map_buf`-only helpers (bounds now
checked against `MAP_SIZE`, not `LCD_WIDTH`/`LCD_HEIGHT`) since they're only ever
called on `map_buf` now. `map_fb_x`/`map_fb_y` and their lazy-resolve logic were
removed entirely — no longer needed, since writes target the buffer directly, not
absolute screen coordinates. This also incidentally fixes two things: the tab-
visibility guard comment ("direct writes must never run while another tab is on
screen") is no longer a real hazard (writes never touch a live screen buffer now),
and a previously-dead-code bug is moot (`buf2` being null in the old single-buffer
setup meant `updateBearingLinesDirect()`'s `if (!buf1 || !buf2 ...)` guard always
returned early — the bearing line was silently never drawing; not applicable to
this new implementation, which doesn't reference `buf1`/`buf2` at all).

### Attempt E — hardware test result (2026-08-23): ✅ CONFIRMED WORKING

Flashed via OTA and tested on the physical panel. User's exact words: **"that works
perfectly."** No vertical roll, no flicker, no text corruption. Builds cleanly
(`pio run -e touchpanel-serial`, SUCCESS, RAM 19.1%, Flash 27.5%, effectively
identical footprint to Attempts A-D — the fix was architectural, not a size/resource
change). Rotator tab bearing-line rework verified working end to end as part of the
same test. This is now the baseline configuration — see the top of this file.

## If this configuration regresses again

1. **First, diff against this fix.** `git log -- src/main.cpp include/board_pinout.h`
   and check what changed in `initRgbPanel()`, `lvgl_flush_cb()`, the `setup()` LVGL
   init block, or `board_pinout.h`'s `BOARD_PROFILE_1024X600` block since the commit
   that recorded this working state (search the log for this file's introduction).
   Nine times out of ten a regression here will be an edit to one of those that
   reintroduces something this file already warns about (`direct_mode`, `num_fbs>1`,
   a vsync semaphore in flush_cb, or a `LCD_FREQ_WRITE`/porch change) — check there
   before investigating anything else.
2. If the config genuinely hasn't changed but symptoms reappear anyway (e.g. after a
   toolchain/platform update), re-read Attempts A-D above for what was already ruled
   out on the *previous* architecture, and don't re-run those same experiments here
   without a specific reason — this is a different, simpler architecture and old
   findings about `num_fbs=2`/bounce-buffer combinations don't automatically apply.
3. The `num_fbs=2` findings from Attempts A-D (double buffering fixes the
   frame-level CPU/DMA race; combining bounce buffer with `num_fbs>1` is broken on
   this toolchain) remain true facts about *that* architecture and could be
   revisited on top of *this* confirmed-working single-buffer+bounce baseline if a
   specific reason to want double buffering comes up (e.g. a future feature needs
   `direct_mode`) — but that's a deliberate, tested opt-in, not a default to drift
   back into.
4. If a persistent, hard shift shows up despite `CONFIG_LCD_RGB_RESTART_IN_VSYNC`
   being on (confirmed enabled by default in this framework's sdkconfig): call
   `esp_lcd_rgb_panel_restart(s_panel_handle)` manually (e.g. from a periodic
   health-check in `loop()`) as a belt-and-braces recovery — the ESP-IDF header
   docs explicitly describe this as safe to call anytime to "save the screen from
   a permanent shift."
5. Switching the `platformio.ini` framework from `arduino` to a hybrid
   `framework = espidf, arduino` would unlock sdkconfig options like
   `CONFIG_SPIRAM_XIP_FROM_PSRAM` — a bigger, riskier change; only worth it if
   something genuinely new comes up, not as a first response to a regression.

## Ongoing investigation: long-duration corruption episodes (2026-08-23+)

This is a **separate investigation from Attempts A-E above.** Those fixed a
different, more frequent failure mode (roll/flicker/corruption tied to the display
driver architecture itself, described at the top of this file). This section covers
a corruption pattern still occurring *on top of* the Attempt E baseline: long stable
periods, then a switch to a corrupted (roll + garbled text) state that itself
persists for an extended period — 10+ minutes — before self-clearing. Not a
per-frame blip; something is holding the display in a bad state for a long stretch,
then something ends that state.

### Ruled out: flash debug logging

`DebugLogger`'s periodic SPIFFS flush (`ESP32-SharedLibraries/lib/DebugLogger/
DebugLogger.cpp`) was the leading suspect from the initial code review — it's the
only periodic, unconditional flash write in the app, runs on the same task as
`lv_timer_handler()`, and its first flush timing (~60s after boot, static
`lastFlushTime=0` + 60000ms `flushInterval`) matched the user's earliest-observed
onset closely. A runtime toggle was added (Settings page checkbox + a quick
on/off button on the Log page — see `debugLoggingEnabled` in `main.cpp`,
`DebugLogger::enableSpiffs()`/`disableSpiffs()` in the shared library) specifically
to test this in isolation.

**Result: disabled, and the corruption still occurred.** This rules out DebugLogger
as the (sole) cause. It's also a poor mechanistic fit for the *shape* of this
particular symptom anyway — a single flash write is a brief event (low tens of ms),
not something that would hold the display in a bad state for 10+ minutes. Worth
remembering DebugLogger's flush is still a real, if unproven, contributor to
whatever caused the *original* frequent corruption Attempts A-E addressed — the
toggle stays in the codebase either way, it just isn't the explanation for *this*
pattern.

### Ruled out: `WiFiManager`'s blocking reconnect logic

`WiFiManager::attemptSTAConnection()` (`ESP32-SharedLibraries/lib/WiFiManager/
WiFiManager.cpp:168-180`) does a genuine blocking wait:
```cpp
WiFi.disconnect();
delay(100);
WiFi.begin(configuredSSID.c_str(), configuredPassword.c_str());
unsigned long connectStart = millis();
while (WiFi.status() != WL_CONNECTED && millis() - connectStart < RECONNECT_TIMEOUT)  // 15000ms
{
    delay(500);
}
```
Called from `handleSTA_Mode()` → `WiFiManager::loop()` → `main.cpp:7054`, in the
**same `loop()` function** that calls `lv_timer_handler()` six lines earlier
(`main.cpp:7048`). While blocked here, no LVGL flush cycles happen at all.

This only fires when the device's own WiFi connection has genuinely dropped (normal
operation is a cheap 30s status check that does nothing while connected —
`WiFiManager.cpp:107-111`). Once disconnected, it retries with exponential backoff
(5s, 10s, 20s, 40s, then capped at 60s — `WiFiManager.cpp:295-303`), up to
`MAX_RECONNECT_ATTEMPTS=10` before falling back to AP mode. Summed out, that's
roughly 7-8 minutes of "blocked up to 15s, wait out the backoff, blocked again" —
a strong match for the reported 10+ minute duration, and it would recur only during
genuine WiFi outages (router hiccup, AP roaming, brief interference), matching
"didn't happen often." There's also existing precedent in this codebase for WiFi
driver activity specifically disrupting this panel's timing (`main.cpp` comment:
"the bounce-buffer ISR runs at interrupt level 7 (~2000 times/sec) and causes WiFi
beacon misses when modem sleep is enabled") — i.e. a known bidirectional
sensitivity between WiFi stack activity and the RGB panel's bounce-buffer ISR on
this exact SoC/panel combination. A full disconnect/reassociate cycle is heavier
WiFi-stack activity than steady-state beacon reception and a plausible trigger for
the same kind of disruption, repeated across every retry.

**Result (2026-08-23, tested by user): disabled auto-reconnect, corruption still
occurred.** This rules out `WiFiManager`'s blocking reconnect as the (sole) cause —
same status as DebugLogger above. Two strong, mechanistically-plausible hypotheses
in a row have now been individually disabled and the symptom persisted through
both, which means either the cause is something not yet identified, or (worth
taking seriously) no single cause — see "Next hypotheses" below, particularly the
reframing under point 1.

**Toggle stays in the codebase** (default on, no behaviour change for normal
operation) — it's a legitimate diagnostic tool and the underlying blocking-call
concern is real regardless of whether it explains *this* symptom.

**Toggle added for isolation testing:** `WiFiManager::setAutoReconnect(bool)` /
`isAutoReconnectEnabled()` (new public API, `WiFiManager.h`/`.cpp`), gated in both
`handleSTA_Mode()` and `handleAP_Mode()` — when disabled, WiFi that drops just stays
dropped (no auto-recovery) instead of running the blocking retry loop. Wired up in
`main.cpp` as `wifiAutoReconnectEnabled`, persisted via `loadConfig()`/`saveConfig()`
the same way as `debugLoggingEnabled`, with a checkbox on the Settings page ("WiFi
Configuration" table). Default **on** (preserves existing behaviour); this is a
diagnostic knob, not intended for normal operation — if WiFi drops with this off,
the device stays disconnected until manually re-enabled or rebooted.

### Next hypotheses (2026-08-23+, neither tested yet)

1. **Reframing worth taking seriously: the *duration* may not mean "sustained ongoing
   cause for 10+ minutes."** Both ruled-out hypotheses assumed something actively bad
   was happening continuously for the whole visible window. An alternative: a single
   brief trigger (a few frames, maybe under a second) corrupts the frame buffer or
   desyncs GDMA timing, and `CONFIG_LCD_RGB_RESTART_IN_VSYNC` either doesn't fully
   clean it up or only fixes the timing-desync half (vertical roll) while leaving
   actually-wrong pixel bytes in the buffer (the garbled text). Under the current
   "Attempt E" architecture, LVGL only repaints a region when something invalidates
   it — a static or slow-changing screen could easily go 10+ minutes without any
   widget in the corrupted area being touched again, so the *visible* corruption
   would persist not because anything is still going wrong, but because nothing has
   redrawn over it yet. This reframes the search from "what causes 10 minutes of
   badness" to "what causes a brief glitch" (much broader field of candidates,
   including routine background load already known to stress this driver — see the
   "Hardware/driver background" section at the top of this file) plus "what finally
   triggers a repaint of that region" (tab switch, a widget's data finally changing,
   etc. — which would explain the self-clearing without needing a distinct root
   cause to *end*).

   **If this reframing is right, a practical mitigation exists independent of ever
   finding the original trigger:** periodically force a full-screen repaint (e.g.
   `lv_obj_invalidate(lv_scr_act())` every 30-60s) and/or periodically call
   `esp_lcd_rgb_panel_restart(s_panel_handle)` as belt-and-braces timing resync
   (already on the "next steps" list above, item 4, for a different reason). This
   wouldn't fix a root cause, but would bound the worst-case visible duration of any
   future occurrence to well under a minute instead of 10+.

   **Offered to the user 2026-08-23, declined for now** — wants to keep chasing the
   actual cause before adding a mitigation that would also make it harder to tell
   whether a future fix attempt actually worked (a periodic repaint would mask the
   symptom's natural duration). Revisit this offer if the investigation stalls.

2. **A peer device (not the touch panel's own WiFi) becoming slow/unreachable for an
   extended window.** `pollVfoFast()` (300ms), `pollRotatorFast()` (300ms/2s), and
   `pollAllPeers()` (5s) all run on a separate FreeRTOS task pinned to core 0
   (`pollTaskFn`, `main.cpp:6854`, `xTaskCreatePinnedToCore(..., 0)` at line 7102) —
   confirmed via `xTaskCreatePinnedToCore` to be a genuinely different core from
   `loop()`/`lv_timer_handler()` (core 1, Arduino's default), so CPU scheduling
   contention is not the mechanism here — but the PSRAM bus is still shared hardware
   across both cores regardless of task/core separation (the same erratum discussed
   throughout this file). If a specific peer (rotator, antenna controller) goes
   through its own outage, every poll to it falls through to its timeout ceiling
   (250-1000ms depending on function — see `HTTP_TIMEOUT_*` defines, `main.cpp:
   796-800`) instead of a fast success, elevating sustained JSON-parse/PSRAM-touching
   activity on core 0 for as long as that peer stays unreachable. Untested. Would
   correlate with a specific peer showing unreachable in `/api/peers/status` or the
   on-screen status indicators during the bad window — worth checking next time it
   happens, no code change needed to check this, just look at the existing UI/API.
3. **Tab-dependent, `rotatorMoving` stuck true.** If the rotator's last-known
   `motorRunning` state was `true` right as that peer became unreachable, the swoosh
   animation keeps `mapDirty` firing every 60ms indefinitely (`main.cpp`, "Keep
   swoosh animating while rotating" block) for as long as the Rotator tab stays
   active — but only if the Rotator tab is the one on screen.

   **User's answer (2026-08-23): "I have only noticed on the Overview tab"** (with
   the caveat they hadn't specifically watched other tabs yet, so not conclusive).
   This actually rules OUT this specific hypothesis as-is (it needs the Rotator tab
   active) — noted here rather than deleted since the answer is informative either
   way. If corruption also turns out to happen on other tabs once checked, this is
   fully dead. If it turns out to be Rotator-tab-exclusive after all, revisit.

   **Overview tab reviewed for anything unique** (`create_overview_tab()`,
   `main.cpp:4014+`): reboot button, WiFi/uptime/build/heap labels, and a peer table
   (up to `MAX_PEER_ROWS=8` rows: name/IP/site/uptime/build/status per discovered
   peer, populated in `update_ui()` `main.cpp:6277-6370`). Nothing exotic — no
   canvas/chart/gauge (those are Rotator/Propagation-tab-only). Peer-table text
   updates are properly change-gated via `lbl_set()` (`main.cpp:5364-5369`, compares
   old vs. new text before touching the widget) — not a naive unconditional
   redraw-everything-every-300ms loop. The one thing that IS true: Overview's peer
   table has the most continuously-*changing* content of any tab during idle viewing
   (live uptime counters, status ages) versus Rotator (static unless actively
   rotating), Propagation (30s cadence), Power/Antenna (mostly static) — so it
   plausibly has the highest baseline rate of small legitimate redraws of any single
   screen. Whether that's causally relevant or just where the user happens to be
   looking most is unresolved — asked the user which part of the screen the
   corruption actually appears in (peer table rows vs. header/labels vs. whole
   visible area) to help distinguish; answer not yet received.

### Confirmed: Overview-tab-exclusive (2026-08-23)

**User watched all tabs and confirmed: corruption ONLY occurs on the Overview tab,
never on any other.** This is now a confirmed, deliberately-tested finding, not
just an initial impression — a real, causal, tab-specific result to build on.

This directly points at what's structurally different about Overview vs. every
other tab: **`lbl_uptime` (`main.cpp:6244`, in `update_ui()`) was the only widget
anywhere in the app that unconditionally changes on a ~1-second cadence, forever,
for as long as the device is powered.** Every other tab's content is either
event-driven (Rotator: only redraws while actively rotating) or on a slow,
change-gated cadence (Propagation: 30s). Two other places in the same function
already had this exact class of problem fixed — the peer-table uptime formatter
(`main.cpp:6320-6338`) drops seconds once a peer's uptime passes an hour, and the
WiFi RSSI label has an explicit comment ("fluctuates ±2 dBm every poll, causing 3Hz
dirty marks") about deliberately rounding to suppress noise-driven redraws.
`lbl_uptime` was the one label that kept ticking every second in all three of its
formatting branches (h>0, m>0, else) and was evidently missed when that pattern was
applied elsewhere.

This reframes the "long stable, then long corrupted, self-clearing, doesn't happen
often" pattern as a probability/exposure question rather than a discrete external
cause switching on and off: if there's a narrow, rare per-redraw chance of hitting
whatever timing window causes a glitch (the same PSRAM-bandwidth-contention
sensitivity discussed throughout this file), Overview got roughly one "attempt" per
second, continuously, all day — every other tab only got an attempt when something
actually changed. A long clean stretch and a long bad stretch on Overview would
just be runs of luck in a low-probability repeated trial; other tabs get so few
attempts per unit time that the same underlying rarity would rarely if ever surface
visibly there. Consistent with DebugLogger and WiFi-reconnect both being ruled out
(neither has anything to do with which tab is on screen).

**Fix applied (not yet tested on hardware):** `lbl_uptime` now shows seconds only
for the first minute after boot, then drops to minute granularity
(`"Uptime: %lus"` under 60s, `"Uptime: %luh %lum"` / `"Uptime: %lum"` from 60s on)
— a ~60x reduction in how often that unconditional redraw fires once past the first
minute of uptime. The peer-table uptime formatter (`main.cpp:6332-6339`) was
updated to match the same rule (previously kept seconds all the way to the 1-hour
mark; now drops them at 60s like everything else). This is a genuine test, not
a cosmetic-only change or a mitigation papering over the symptom: if the
corruption drops off or stops with this alone, it confirms redraw *frequency* is
the trigger, and this becomes the actual fix (nobody needs live-ticking seconds on
an uptime counter). If it makes no measurable difference, this hypothesis is
cleanly ruled out too, and the next thing to look at is `lbl_overview_hw`
(heap/PSRAM free, `main.cpp:6253-6258`) — updated every 300ms cycle and *not*
change-gated by anything beyond `lbl_set`'s own string-diff, so it could still
redraw fairly often if free heap/PSRAM genuinely fluctuates from normal allocation
churn (JSON parsing, String operations) even without a deterministic tick like the
uptime counter had.

**How to verify:** flash this build, leave the device on the Overview tab (matches
how the user normally observes this) for an extended period, same as before. Watch
specifically for whether the corruption still occurs at all, and if so, whether it
seems less frequent than before.

### Result (2026-08-26): uptime throttle alone was insufficient, but a major new finding

User ran overnight (8h55m uptime) with the uptime throttle in place. **Corruption
still occurred on the Overview tab.** So `lbl_uptime`'s per-second tick was not the
sole trigger — either it was one contributor among several, or coincidental.

**Much more important: switching to another tab and back to Overview clears the
corruption instantly.** This is new information (not established before) and
strongly confirms the "reframing" theory from earlier in this section: the
corruption is stale/wrong content sitting in the frame buffer, not an ongoing
active process. A tab switch forces LVGL to fully repaint the tab's content on
return, which is sufficient to fix it immediately. This means "10+ minutes" was
never 10+ minutes of something actively wrong — it was 10+ minutes of nothing
happening to touch the affected region, on a tab that (even after the uptime fix)
still has more going on than most.

**Next candidate, raised by the user and matching the pattern exactly: `lbl_overview_hw`**
(heap/PSRAM free, `main.cpp:6260+`). Unlike every other dynamic value on this tab
(RSSI rounds to 5dBm, both uptimes now throttled to drop seconds after the first
minute), this one had zero smoothing — raw `ESP.getFreeHeap()`/`ESP.getFreePsram()`
in KB, sampled every 300ms. Since `lv_mem_psram.h` routes ALL of LVGL's own
allocations through PSRAM, and several pollers allocate JSON buffers on cadences
from 300ms to 5s, this figure very plausibly shifts by more than 1KB on most
300ms samples — meaning it could have been redrawing *more* often than the old
per-second uptime tick, not less.

**Fix applied (not yet tested on hardware):** rounded both figures to the nearest
10KB before formatting (`main.cpp:6260-6268`) — same "round to suppress
noise-driven redraws" pattern as the RSSI label. This is the last remaining
unthrottled frequently-sampled value on the Overview tab; if corruption persists
even after this, the "it's about which specific label updates often" line of
investigation is likely exhausted and worth stepping back from (see "if this
doesn't pan out" below).

**How to verify:** same as before — flash, leave on Overview tab, watch for
occurrences. Given the tab-switch-clears-it finding, also worth simply switching
away and back if it's spotted again, to confirm that still reliably clears it (a
quick, non-destructive way to confirm the *mechanism* even while still chasing
the *trigger*).

**If this doesn't pan out:** the "one specific label is too chatty" angle will have
been thoroughly tried (RSSI, both uptimes, heap/PSRAM — that's everything
dynamic on this tab). At that point the more likely explanation is aggregate
redraw *rate* on Overview generally (multiple labels each individually fine, but
their combined update cadence still gives more "attempts" than other tabs), which
isn't fixable by throttling one more label — the practical options become: (a) the
periodic full-repaint/`esp_lcd_rgb_panel_restart()` mitigation offered and declined
earlier in this section (now with much stronger justification, given the confirmed
tab-switch-clears-it mechanism means a periodic forced repaint would be a real fix
for the visible symptom, not just a guess), or (b) accepting Overview's redraw
pattern as inherently more exposed and moving on unless a specific new lead shows up.

### Found and fixed (2026-08-26): WiFi RSSI rounding bug — real bug, likely contributor

User noticed the WiFi RSSI figure visibly "ticking" and asked whether it was worth
checking, despite the existing "round to nearest 5dBm" comment suggesting it was
already throttled. It was — but incorrectly. The old code:
```cpp
int rssi5 = (WiFi.RSSI() / 5) * 5;
```
C/C++ integer division truncates *toward zero*, not floor, for negative operands.
For a negative RSSI (always the case — WiFi RSSI is a negative dBm value), this
produces inconsistent, non-uniform bucket edges: e.g. -58 and -59 land in the -55
bucket, while -60/-61/-62 land in the -60 bucket. A real signal hovering right at
one of those edges (very plausible — any given stable signal sits at *some* value,
and edges recur every 5 dBm) would flip the *displayed* bucket on perfectly
ordinary ±1 dBm measurement noise, defeating the entire point of rounding. This is
a genuine, previously-unnoticed bug, not a red herring — fixed to proper signed
round-to-nearest-5 (`main.cpp:6281-6293`):
```cpp
int rssi = WiFi.RSSI();
int rssi5 = ((rssi + (rssi >= 0 ? 2 : -2)) / 5) * 5;
```
This is now the fourth label on the Overview tab found to have been redrawing more
often than intended (after the device uptime, peer-table uptime, and — new find —
this RSSI figure was *supposed* to already be fixed but wasn't, due to the sign
bug). Not yet isolated in a hardware test of its own (bundled with the color-coding
change below in the same build) — if the Overview corruption still recurs after
this build, this fix and the heap/PSRAM rounding fix from the previous section are
both in place simultaneously, so a future recurrence won't distinguish which (if
either) mattered. Worth remembering if further isolation is ever needed.

### Also added (2026-08-26): colour-coded status values (not corruption-related)

User asked for green/amber/red colour-coding on the WiFi RSSI, heap, and PSRAM
figures, independent of the corruption investigation — a readability improvement,
not a diagnostic step. Implemented via LVGL's inline label recolor feature
(`lv_label_set_recolor()` + `#RRGGBB text#` markup within the existing combined
label strings, so no new widgets/layout changes needed):
- WiFi RSSI: green >= -60dBm, amber -60 to -70dBm, red < -70dBm.
- Heap free: green >= 50KB, amber 20-50KB, red < 20KB.
- PSRAM free: green >= 1024KB, amber 256-1024KB, red < 256KB.

Thresholds are estimates, not calibrated against real steady-state figures from
this device — easy to adjust in `update_ui()` (`main.cpp`, the `lbl_overview_hw`
and `lbl_wifi` blocks) once actual typical values are observed. Worth noting since
this is a genuinely separate concern from the corruption investigation above:
because inline recolor changes the label's *text content* (the colour code is part
of the string), it goes through the exact same `lbl_set()` string-diff gate as
everything else — a colour-category change alone (independent of the underlying
number changing) will now also trigger a redraw, e.g. RSSI moving from -59 to -61
recolors green→amber even though the rounded-to-5 displayed number might stay the
same. This is a small *increase* in potential redraw triggers on top of the fixes
above, done deliberately at the user's request — worth keeping in mind if isolating
future corruption causes on this tab specifically.

### Overturned: it's NOT Overview-tab-exclusive, and repaint doesn't always clear it (2026-08-26)

After the heap/PSRAM + RSSI fixes above, corruption recurred again. Two things
changed the picture significantly:

1. **Corruption also seen on the Propagation tab**, not just Overview. The earlier
   "Overview-only" finding was accurate as far as it went, but based on limited
   observation — with more time watching, it's not tab-exclusive. This retroactively
   weakens (without fully invalidating) the whole "which specific label is too
   chatty" line of investigation: Overview genuinely does have more going on than
   most tabs and the fixes above were all real, worthwhile bug fixes, but they were
   never going to be a complete explanation if the fault isn't confined to that tab.
2. **The tab-switch-clears-it trick didn't work this time.** Previously 100%
   reliable. This is the more important signal — see the research below for why
   this makes sense.

### Web research (2026-08-26): this is a known, unresolved architectural limitation

Given four label-level fixes hadn't resolved it and the symptom no longer fit
"one chatty tab," searched for prior art rather than continuing to guess
application-level causes. Findings, consistent across multiple independent
sources:

- **PSRAM bandwidth is split 50/50 between CPU and GDMA with no priority scheme.**
  From an ESP32 forum thread on the exact bottleneck: "When both the CPU and GDMA
  request access to the PSRAM, the bandwidth is divided 50-50 between the CPU cache
  and GDMA... as soon as the CPU accesses PSRAM, the bandwidth is reduced to 50%,
  which can lead to corruption if the LCD needs more than that." Their conclusion:
  "The only way to solve this would be some sort of hardware priority scheme which
  allow GDMA to complete its job without being interrupted" — which doesn't exist.
  ([LCD_CAM + GDMA external RAM bottleneck](https://www.esp32.com/viewtopic.php?t=33312))
- **A specific, matching open Espressif issue: "LCD display corruption caused by
  Wi-Fi activities"** (espressif/esp-idf #12342). Reporter's exact trigger:
  "if Wi-Fi is connected and has some activities (e.g. communicating with host,
  performing OTA update, etc)" — routine traffic, not disconnects/reconnects (which
  this project already ruled out separately). Still open as of this research, no
  driver-level fix. This project generates WiFi traffic constantly (HTTP polling
  every 300ms-30s across several peers), so this was never actually tested in
  isolation — but per the next point, isolating it further isn't expected to lead
  to an application-level fix anyway.
  ([espressif/esp-idf#12342](https://github.com/espressif/esp-idf/issues/12342))
- **This project's own architecture already implements the standard partial
  mitigation** — keeping WiFi's own buffers in internal DRAM instead of PSRAM
  specifically to reduce this contention (`lv_mem_psram.h`'s stated rationale). But
  every LVGL widget update, on *any* tab, still goes through PSRAM by design
  (`LV_MEM_CUSTOM_ALLOC` routes all of LVGL through PSRAM, to free DRAM for WiFi in
  the first place) — so this was never going to be fully eliminated by protecting
  WiFi's buffers alone, and isn't fixable by throttling more labels since the
  contention is architectural, not tied to any one widget.
- **Two distinct fault types likely explain why repaint sometimes doesn't work.**
  One source notes `CONFIG_LCD_RGB_RESTART_IN_VSYNC` "can lead to single-frame
  desyncs if the interrupt is late enough, as the LCD controller may have already
  read the first data bytes before DMA is reset." That implies content corruption
  (wrong bytes in the buffer — fixed by a repaint, since it overwrites bad bytes
  with correct ones) and timing/position desync (buffer content is fine, but the
  display controller is reading from the wrong place — NOT fixed by a repaint,
  only by an actual DMA restart) are both possible under this one umbrella symptom.
  Consistent with the tab-switch trick working once and not the next time — likely
  two different underlying events, not one.
- Other corroborating (not newly actionable) reports: PSRAM framebuffer cache-flush
  timing issues ([espressif/esp-idf#13293](https://github.com/espressif/esp-idf/issues/13293)),
  an unresolved 2-framebuffer tearing report with the same "is buffer swapping my
  responsibility?" confusion this project went through in Attempts A-D
  ([espressif/esp-idf#13805](https://github.com/espressif/esp-idf/issues/13805)).

**Conclusion: this is not a fixable-by-more-code-review application bug.** It's a
known, still-open limitation of driving PSRAM-backed RGB LCD framebuffers on the
ESP32-S3 that Espressif's own community hasn't fully solved either. Continuing to
hunt for "the one remaining trigger" at the application level has diminishing
returns — the label-throttling fixes already made were genuine improvements and
stay, but the strategy shifts here from elimination to mitigation.

### Mitigation implemented (2026-08-26): periodic display self-heal — NOT YET HARDWARE-TESTED

Added a periodic health-check in `loop()` (`main.cpp`, `PANEL_HEALTH_INTERVAL_MS =
30000`, right after `DebugLogger::periodicFlush()`):
```cpp
if (millis() - lastPanelHealthCheck > PANEL_HEALTH_INTERVAL_MS)
{
    lastPanelHealthCheck = millis();
    esp_lcd_rgb_panel_restart(s_panel_handle);
    lv_obj_invalidate(lv_scr_act());
}
```
Both calls are deferred/non-blocking internally (restart takes effect at the next
vsync; invalidate just marks dirty for the next `lv_timer_handler()` pass) — neither
stalls `loop()`. Deliberately combines both fault types identified above: the
restart re-syncs GDMA/scan-out timing, the repaint overwrites any actually-wrong
pixel bytes. 30s interval chosen as a reasonable balance — caps worst-case visible
corruption to ~30s (down from 10+ minutes) while adding only one extra full
repaint per 30s on top of the already-frequent 300ms update cycle.

This does not fix the root cause (per the research above, there may not be one
available to fix within this application) — it bounds the symptom. If corruption
is still visibly disruptive even with this in place, the next lever is shortening
`PANEL_HEALTH_INTERVAL_MS` (more overhead, faster self-heal) rather than more
root-cause hunting.

**How to verify:** flash, use normally, confirm any future occurrence is brief
(within ~30s) rather than lasting minutes. The corruption *may still flash briefly*
every so often — that's expected and is the point of this change (bounded, not
eliminated).

### Result (2026-08-26): self-heal did NOT resolve it — new diagnostic clue, different fix

Recurred on the **Power tab** (a third tab, further confirming this was never
tab-exclusive) while the periodic self-heal was active. User confirmed it was
**persistent** (lasted well over a minute, not brief) despite `esp_lcd_rgb_panel_
restart()` + a full repaint firing every 30s during that window — meaning the
self-heal was not actually clearing it when it ran, not just failing to run often
enough. Also confirmed the visual character: **garbled/scrambled — wrong colors,
nonsense — not a clean shift/displacement.**

This combination is diagnostic. A clean shift (rows displaced but individually
legible) is what `esp_lcd_rgb_panel_restart()` is specifically documented to fix
(GDMA reading from the wrong *position*; buffer content itself is fine). Garbled
noise instead means **actually wrong bytes in memory** — and if a full repaint
(which rewrites that memory with fresh correct data) doesn't clear it, that points
at the corruption being *introduced fresh by the act of writing*, not sitting
stale and unrepainted. Every write is equally exposed, including the healing
repaint's own writes.

**Root cause candidate: stale/torn CPU cache writes to the PSRAM frame buffer.**
`lvgl_flush_cb()` copies each rendered strip into the panel driver's PSRAM frame
buffer via `esp_lcd_panel_draw_bitmap()`, which (for a foreign/external source
buffer, which is what our small LVGL strip buffers are under this architecture)
does a plain CPU `memcpy`. That write goes through the CPU's data cache — on
ESP32-S3, a write isn't necessarily committed to the underlying PSRAM chip
immediately, it can sit in cache until evicted or explicitly flushed. The
bounce-buffer ISR reads PSRAM *directly* (that's the whole point of the bounce
buffer — feeding GDMA from a fast SRAM copy refilled from PSRAM), on its own
schedule tied to the pixel clock, with no coordination with the CPU's cache state.
If it reads a region whose most recent write is still sitting in cache, it can see
stale or partially-written bytes — which, interpreted as RGB565 pixels, looks
exactly like noise: wrong colors, garbled, nonsense. This is a documented,
still-open failure mode in this exact driver — see the GitHub issues in
References below, one of which described the identical mechanism ("screen
corrupted... because the last bytes are stuck in the CPU cache, not yet committed
to the framebuffer").

**Fix applied (2026-08-26): explicit cache writeback after every frame buffer
write.** `s_hw_fb` (`main.cpp`, near `s_panel_handle`) holds the panel driver's
internal PSRAM frame buffer address, fetched once at startup via
`esp_lcd_rgb_panel_get_frame_buffer()` — used *only* for computing the address
range to flush, not for rendering (LVGL still renders into its own separate
buf1/buf2 strip buffers, unchanged from the rest of "Attempt E"). `lvgl_flush_cb()`
now calls `esp_cache_msync()` (from `esp_mm/esp_cache.h`, confirmed present and
linkable in this toolchain) immediately after `esp_lcd_panel_draw_bitmap()`,
forcing the just-written rows out of CPU cache and into PSRAM before returning:
```cpp
if (s_hw_fb)
{
    size_t rowBytes = (size_t)LCD_WIDTH * sizeof(lv_color_t);
    void *flushAddr = (uint8_t *)s_hw_fb + (size_t)area->y1 * rowBytes;
    size_t flushSize = (size_t)(area->y2 - area->y1 + 1) * rowBytes;
    esp_cache_msync(flushAddr, flushSize, ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}
```
Flushes full rows (not just the changed columns) rather than computing a precise
per-row sub-range — simpler and safer addressing (guaranteed cache-line-aligned,
since `LCD_WIDTH * 2 bytes` is always a multiple of the 64-byte cache line; a
precise per-column flush would need a loop with alignment math per row, more
surface area for an addressing bug for a marginal precision gain). `esp_cache_msync`
is documented as cache-safe and thread-safe, and defaults to the writeback (C2M)
direction needed here.

Whether the underlying ESP-IDF `esp_lcd_panel_draw_bitmap()` already does this
writeback internally in this exact framework version (5.5.4) is unconfirmed — only
headers are vendored, not the driver's `.c` source, which is precompiled into
`libesp_lcd.a`. If it already does, this call is a harmless no-op most of the
time; if it doesn't (plausible — this is exactly the gap several open GitHub
issues describe), this directly fixes it. Confirmed to link successfully (no
undefined-reference errors), so the symbol is genuinely present in this toolchain.

**The periodic self-heal mitigation (restart + repaint every 30s) was NOT
removed** — kept as a belt-and-braces measure for the timing-desync half of the
problem (which this cache fix doesn't address) and as a backstop in case some
other corruption mechanism is still unaccounted for.

**Not yet tested on hardware.**

**How to verify:** flash, use normally over an extended period across multiple
tabs. If corruption stops recurring (or becomes markedly rarer), the stale-cache
theory is confirmed. If it still recurs and still looks garbled/scrambled, this
theory is wrong (or the fix has a bug) and the next thing to check is whether
`esp_cache_msync()` is actually being reached and returning `ESP_OK` (add a
one-time `debugLogf` on non-OK return to check for `ESP_ERR_INVALID_ARG`, which
would indicate an alignment assumption in this fix is wrong for some edge-case
`area` geometry).

### Regression (2026-08-26): recurred within ~5 minutes — worse than before, repaint removed

With both the periodic self-heal (restart + full repaint every 30s) and the cache
fix active together, corruption recurred within roughly 5 minutes of boot —
notably faster than any prior report (previous fastest onset was ~60s, and that
was under the old, since-superseded architecture; typical gaps between episodes on
"Attempt E" had been much longer than 5 minutes). Two speculative, unvalidated
changes had been stacked on top of each other (self-heal, cache fix) without ever
testing either in isolation, making it impossible to tell whether one, both, or
neither was net-helpful — and this result suggests at least one may have been net-
*harmful*.

**Most likely culprit: the periodic full-screen repaint itself.**
`lv_obj_invalidate(lv_scr_act())` every 30s forces a complete redraw of the whole
1024x600 screen — a large, guaranteed, recurring burst of exactly the kind of
CPU-touching-PSRAM activity that the research conclusion says competes with GDMA
for bandwidth. It was added *to fix* corruption on the theory that a forced
repaint heals stale content, but if the actual mechanism is "CPU PSRAM activity at
the wrong moment causes corruption," a mandatory large PSRAM-heavy event every 30
seconds is a plausible new *source* of triggers, not a cure — over 5 minutes,
that's roughly 10 guaranteed high-risk events self-inflicted by this code that
would not otherwise have happened.

**Fix: removed the repaint, kept the restart.** `esp_lcd_rgb_panel_restart()` is a
cheap, DMA-level-only operation with no PSRAM read/write burst of its own, so it
stays as a low-cost belt-and-braces measure for GDMA timing desync specifically.
The `esp_cache_msync()` per-flush fix also stays (small, proportional, targets
what was actually just written — not a new recurring burst like the repaint was).
Current state in `loop()`:
```cpp
if (millis() - lastPanelHealthCheck > PANEL_HEALTH_INTERVAL_MS)
{
    lastPanelHealthCheck = millis();
    esp_lcd_rgb_panel_restart(s_panel_handle);
}
```

**This isolates the two remaining changes (restart-only self-heal, cache-writeback
fix) so a future occurrence can actually be attributed.** If corruption is still
frequent/fast with just these two lightweight changes, the cache-writeback theory
is probably wrong (or buggy) and it's worth reverting that too to get a true
"nothing added, pure Attempt E baseline" measurement before trying anything else —
see the AskUserQuestion options offered to the user at this point for the
alternative paths not taken (strip back further, or revert everything to
baseline).

**Lesson for future work on this file: test one change at a time.** Stacking
multiple unvalidated speculative fixes together (as happened here) makes results
uninterpretable — a regression could be caused by either, an improvement could be
one masking the other's harm, and a "no change" result rules out nothing. Prefer
flashing and observing after each individual change before adding the next one,
even though that's slower.

**Not yet tested on hardware.**

### Escalation (2026-08-27): worse overnight symptom, strategy change to data collection

10h uptime, woke up to corruption AND vertical roll present **on every panel
simultaneously** — a different, more severe signature than the tab-specific
garbled corruption chased above. Important negative result: `CONFIG_LCD_RGB_
RESTART_IN_VSYNC` fires automatically on essentially every vsync (many thousands
of times over 10h) and the manual `esp_lcd_rgb_panel_restart()` had been running
every 30s all night too — both apparently failed to clear whatever this was. That
rules out "one-off desync, just needs a restart" for this particular episode; it
was either being continuously re-caused, or is a class of desync no restart call
can fix at all.

Given four fix attempts in a row (label throttling, periodic self-heal, cache
writeback, restart-only) have each been plausible but none has held up, and the
last one (self-heal + cache fix together) measurably made things worse before
being partially walked back — decided to stop guessing at fixes for now and
**switch to structured data collection** instead, to get actual evidence rather
than more theories.

### Diagnostic tool added (2026-08-27): web-based panel-check reporting

Two deliberate design constraints, both reasoned through before building:

1. **Never touch the touch panel's own UI to report a problem.** Tab-switching on
   the device has been observed to sometimes *clear* a corruption episode —
   requiring the user to navigate to a specific screen to report it would risk
   destroying the evidence before it could be logged. Reporting happens purely
   from a browser hitting an HTTP endpoint, with no interaction with the LVGL UI
   at all.
2. **Force a regular check-in cadence rather than relying on ad-hoc clicks.**
   Sporadic "looks fine" reports don't establish a real timeline — the gaps
   between them could hide the entire event. `script.js` now prompts a
   `confirm()` dialog roughly every 5 minutes, from whichever page of the site is
   open, tracked via `localStorage` (not a JS variable) so the cadence survives
   page navigation and the `/log` page's own 15s auto-reload.

**Implementation:**
- `data/script.js`: `checkPanelPrompt()`, polled every 10s, checks a
  `localStorage`-held due-time; when due, shows a blocking `confirm()` ("is the
  display showing correctly right now?") and POSTs the result to
  `/api/panelcheck?status=good|bad`, then resets the due-time to +5 minutes
  (set *before* showing the dialog, so a slow response doesn't compress the next
  interval).
- `main.cpp` `handlePanelCheck()` (registered at `/api/panelcheck`): logs a single
  self-contained snapshot line via `debugLogf()` — status, uptime, free heap/PSRAM,
  WiFi connection state + RSSI, which tab is currently active on the touch panel
  (`lv_tabview_get_tab_act(tabview)`), and how many discovered peers are currently
  reachable. Deliberately a compact single line (not full peer names/details,
  which risked exceeding `DebugLogger::logf()`'s internal 256-byte buffer with
  several peers) — full peer detail is already available on demand via the
  existing `/api/peers/status` endpoint if a specific report needs more context.

**Important operational note: flash debug logging must be enabled for this data
to persist and be reviewable later.** It was switched off earlier in this
investigation to test (and rule out) DebugLogger's periodic flush as a cause of
the *original* frequent corruption — that hypothesis is confirmed dead, so
re-enabling it is safe. The in-memory log view (`/log` page) does NOT survive a
reboot, so if flash logging is off, overnight reports could be lost before being
reviewed the next day. **Check the Settings page checkbox or the Log page's
quick-toggle button before relying on this.**

**Honest expectation-setting, unchanged from when this was proposed:** this may
still come back inconclusive — "nothing obviously different in the snapshot at
those moments" is a real possible outcome given every specific single-cause
hypothesis tried so far has already been ruled out. That would still be useful
information (strengthens the case that this needs a different strategy entirely,
e.g. scheduled reboot as a pragmatic recovery mechanism rather than continued
root-cause hunting) — not a wasted effort either way.

**Not yet tested on hardware. No further code-level "fixes" attempted pending
this data.**

### Strong new lead from live log data (2026-08-27): TCI reconnect loop every 5 seconds

With flash logging re-enabled, fetched the device's `/log` directly (it's
reachable on the LAN from this dev machine — `curl http://192.168.1.200/log`)
rather than asking the user to copy/paste it. Immediately visible: **`[TCI]
Connection: disconnected` repeating on an almost exact 5-second cadence,
continuously** (09:50:50, :55, 09:51:00, :05, :10, :15, :20, :25, :30, :35,
:40, :45...). Checked `lib/TCIService/TCIService.cpp:20`:
`_reconnectInterval(5000)` — the `WebSocketsClient` is configured to attempt a
full reconnect every 5 seconds whenever disconnected, and it clearly could not
reach the configured `tciHost`/`tciPort`.

**This is a strong match for "corrupted 5 seconds after the screen showed"** (the
user's exact words, reported independently before this log was checked) — and a
better mechanistic fit than anything examined previously for a specific reason:
every other periodic activity in the app (VFO/rotator/peer polls) is a *quick,
usually-successful* HTTP GET to a reachable device. A failing WebSocket reconnect
attempt is different — TCP connect + handshake attempt against an unreachable
host involves heavier, more variable WiFi/lwIP-stack activity (connection
attempt, timeout/refusal handling) than a fast successful poll, and it was
running on the single tightest, most persistent cadence of anything in the whole
application — every 5 seconds, forever, for as long as TCI can't connect. This
is also the closest match yet to the "LCD display corruption caused by Wi-Fi
activities" GitHub issue (#12342, cited earlier) — routine WiFi *traffic*, which
had never actually been tested in isolation before now (the WiFi-related things
ruled out earlier were the blocking *STA reconnect* logic and *flash debug
logging*, not this).

**Test in progress: user disabled TCI via the existing `tciEnabled` Settings
page checkbox** (2026-08-27, ~10:02) — zero code change needed, the toggle
already existed. Confirmed via live log fetch that it took effect cleanly: last
TCI log line is `[TCI] Direct TCI disabled or no host configured`, and the
every-5-second reconnect spam stopped immediately after.

**How to interpret the result:** if corruption becomes markedly rarer or stops
with TCI disabled, that's strong confirmation and points at either (a) fixing
the actual TCI host configuration so it connects successfully instead of
endlessly retrying, or (b) backing off the reconnect interval significantly
(5s is aggressive) if a permanent "TCI not always available" situation is
expected. If corruption still occurs at a similar rate with TCI disabled, this
lead is a false one — but even then, the panel-check web tool (see above)
should now be capturing rich state snapshots (including WiFi status) at each
future occurrence regardless of which specific cause turns out to be right.

### Result: TCI ruled out too (2026-08-27)

Corruption still occurred with TCI disabled — confirmed via live log fetch
(`curl http://192.168.1.200/log`, reachable directly from this dev machine) that
the disable had genuinely taken effect (no more `[TCI]` reconnect spam; last
entry `[TCI] Direct TCI disabled or no host configured`) and that recent PROP/LINK
polling looked completely normal. This is now the fifth specific hypothesis
tested and ruled out in a row (flash logging, WiFi STA-reconnect blocking,
Overview label chattiness, the periodic self-heal, TCI reconnect loop). No
`[PANELCHECK]` entries had been logged yet at this point either — the web
check-in tool only runs while a browser tab is open on the site, and none had
been kept open continuously.

### New finding, prompted by the user asking specifically about the Prop tab (2026-08-27)

Checked `create_propagation_tab()`/`update_ui()`'s Prop-tab section directly.
Two things found:

1. **The 12 bands' TX/RX distance bars (24 `lv_bar_set_value()` calls per
   propagation poll, ~every 30s) are NOT a bug** — checked LVGL's own
   `lv_bar_set_value()` source directly (`lv_bar.c:95`): `if(bar->cur_value ==
   value) return;` — it already skips unchanged values internally. In practice
   most of the 24 probably do genuinely change most cycles (real PSK Reporter
   spot data), so this is still a real, legitimate burst of ~24 widget updates
   concentrated in one `update_ui()` call every 30s — but it's live data the user
   wants accurate, not something to throttle away like a clock tick. Noted as a
   possible future refinement (stagger the 24 updates across a few frames instead
   of all in one burst, without reducing data freshness) if everything else is
   exhausted, not attempted now.
2. **`prop_updated_lbl` (the "Solar: Xs  PSK: Ys" freshness readout) was found
   ticking every second, unconditionally, forever** — explicitly commented
   "always update (ticks every second)" in the code, and its `fmtAge()` formatter
   never dropped to coarser precision (`"%dm%02ds"` even past the 1-minute mark).
   **This is the exact same bug class as the old Overview uptime label** —
   missed when that fix was applied, because at the time corruption was believed
   to be Overview-exclusive (later found not to be). Fixed the same way: seconds
   shown only under 60s, then minutes-only (`main.cpp`, `fmtAge` inside the Prop
   tab's `update_ui()` section).

**Take this with appropriate calibration given the track record:** four
label/timing-throttle fixes in this file (Overview uptime, peer-table uptime,
heap/PSRAM rounding, WiFi RSSI rounding-bug) were all genuine, worthwhile
individual fixes, and none of them, individually or together, has yet been
confirmed to meaningfully change the corruption's frequency. This is a real bug
of the same class, on a tab specifically confirmed to be affected, found by the
user asking a good targeted question rather than more blind guessing — but
history in this file says: fix it, keep it, don't assume it's *the* answer
without a hardware-confirmed result.

### Result (2026-08-27/28): no meaningful change — investigation paused

Tested with TCI disabled AND the Prop-tab label fix both in place. Looked
promising for a while (34 min clean, reported as a positive sign at the time,
flagged as not yet meaningful given the history of clean stretches up to ~10h
before a prior failure). **User checked again at ~11h uptime and found it
corrupted again** — essentially the same timing as the worst prior overnight
episode (10h). Confirms neither TCI-off nor the Prop-tab fix meaningfully changed
the actual pattern. By 21h uptime it had cleared again on its own (consistent
with the established self-clearing behaviour throughout this investigation).

**Six specific hypotheses have now been tested and individually ruled out**:
flash debug logging, WiFi STA-reconnect blocking, TCI's reconnect loop, and —
while never fully eliminated the way the others were — Overview/Prop label
throttling and the periodic self-heal (repaint) each failed to change the pattern
despite being reasoned through carefully. The cache-writeback fix
(`esp_cache_msync`) has never been isolated/tested on its own, so its effect
(if any) is still genuinely unknown.

**User has decided to pause active investigation here (2026-08-27/28).** Not
abandoning it — explicitly "for now." Everything implemented this session stays
in place (see "Ongoing investigation" summary above for the current full list):
the display architecture fix (still solid — this is a *different*, less frequent
problem than the one it fixed), all the label/timing throttle fixes (harmless,
genuine improvements regardless), the cache-writeback fix, the restart-only
30s self-heal, the toggles (`debugLoggingEnabled`, `wifiAutoReconnectEnabled`,
user's own `tciEnabled` — check current states before assuming), and the
panel-check web diagnostic tool.

**If/when this is picked back up:**
1. **Don't repeat any of the six ruled-out hypotheses** without a genuinely new
   reason to suspect them again.
2. **The scheduled-reboot mitigation was proposed but never implemented** — a
   nightly or uptime-based `ESP.restart()` as a pragmatic recovery mechanism
   rather than continued root-cause hunting, given the pattern (~10-11h
   self-clearing corruption cycles) is now well-established across multiple
   independent occurrences. Given corruption has now been observed recurring
   around the 10-11h mark on at least two separate occasions under different
   configurations, a reboot scheduled comfortably before that window (e.g. every
   6-8h, or nightly) would likely keep the device usable even without ever
   finding the root cause. This is the most concrete "next step" if/when
   resumed, not another speculative fix.
3. **The panel-check web tool is still live** (`/api/panelcheck`, prompted every
   5 min via `script.js`) but had captured zero data points as of the pause —
   it only fires while a browser tab is open on the site. If left running with a
   tab open over a longer period, it might eventually catch something useful for
   free; if that's not going to happen, it's harmless to leave in place either way.

## Resumed (2026-09-02): severity escalated to ~2/3 corrupted, prior mitigations flashed with no effect

User resumed after running the device for a few days. **New characterization: corrupted
roughly 2/3 of the time**, same visual character as before (garbled/shifted, self-clears
eventually) — a large escalation from the ~10-11h self-clearing cycle this file paused on.

Checked the live device before doing anything else: it was running a build dated
2026-08-31, i.e. **the plain "Attempt E" architecture fix only** ([`960be3e`](960be3e))
— none of the 2026-08-26/27 mitigation work (cache-writeback fix, restart-only self-heal,
the four label-throttle fixes, the panel-check tool) had actually been flashed; all of it
had been sitting as uncommitted changes in the working tree since the investigation was
paused. So the "no data yet" state this file paused on was still true days later — nothing
about this build had ever been tested on hardware.

**Flashed that full pending build (2026-09-02).** No improvement — user reports it's now
"not any different," i.e. still corrupting roughly 2/3 of the time. This means the cache-
writeback fix and the restart-only self-heal, isolated from the repaint that made things
worse, are **not sufficient to fix (or even visibly reduce) this** — first real hardware
result for that specific combination. Combined with the label-throttle fixes also not
moving the needle, the "one specific redraw source" framing looks weaker than ever; the
architectural PSRAM-bandwidth-contention explanation from the 2026-08-26 web research
remains the best-supported one so far, but at 2/3-corrupted this is now bad enough that
isolating specific redraw *sources* — not just their frequency — is worth another pass
before falling back to pure mitigation (scheduled reboot).

**No `[PANELCHECK]` data was collected during this window either** — confirmed via direct
`curl` of the device's `/log` and `/script.js`: the build that was actually running had
no panel-check code in it at all (older than when that tool was added). So the "2/3
corrupted" figure is the user's own subjective sense, not something with a captured
timeline yet. The now-flashed build has the panel-check tool for the first time on real
hardware — worth using it this round if a tab can be left open.

### New hypothesis (2026-09-02, user's own): the Prop tab's periodic update

User's gut feeling, based on their own observation pattern, is that the Prop tab's
periodic redraw is implicated. This hasn't been specifically tested before — the
2026-08-27 Prop-tab work (see "New finding, prompted by the user asking specifically
about the Prop tab" above) only fixed one chatty label (`prop_updated_lbl`) and
explicitly declined to touch the 12-band TX/RX bar update (24 `lv_bar_set_value()`
calls concentrated in one `update_ui()` call, ~every 30s) because it's live data with
internal change-detection already (`lv_bar.c`'s `if(bar->cur_value == value) return;`)
— reasoned to be legitimate, not throttled away, but never actually isolated as a
variable on real hardware either.

**Added, to test this without a firmware rebuild each time:**
- **"Enable Prop Panel Updates" checkbox** (Settings page, new "Propagation Panel"
  table) — `propUpdatesEnabled`, default on. When off: `pollPropagationProxy()`
  returns immediately (no network fetch, no new data parsed) **and**
  `update_propagation_tab()` returns immediately (no widget touched at all, not even
  the already-change-gated ones) — the tab simply freezes at its last-known values.
  Deliberately gates both the poll and the redraw, not just one, so disabling this
  fully removes the Prop tab from the PSRAM/CPU activity picture for an isolation
  test.
- **"Update Interval (seconds)" field** (same table) — `propPollIntervalMs`,
  replaces the old hardcoded `PROP_PROXY_POLL_INTERVAL` (was `#define`d at 30000ms,
  not configurable). Default unchanged at 30s; range 5-600s in the UI. Slowing this
  down (without fully disabling) tests whether *frequency* of the Prop burst matters
  even if the burst itself isn't removed — a middle ground between "no change" and
  "fully off" that's useful if fully disabling turns out to be a step too far to
  isolate.

Both persist via the existing `loadConfig()`/`saveConfig()` NVS round-trip, same
pattern as `tciEnabled`/`debugLoggingEnabled`. Built and verified compiling clean on
`touchpanel-serial` (RAM 19.1%, Flash 27.6% — same footprint as before; this is a
config/gating change, not an architecture change). **Not yet tested on hardware.**

**How to use this for isolation:** disable the checkbox, observe whether the 2/3
corruption rate drops. If it does, that's a strong, specific, actionable finding —
the fix becomes "slow down or restructure the Prop tab's update," not another
architecture-level mitigation. If corruption continues unchanged with the Prop tab
fully frozen, this specific hypothesis is cleanly ruled out (unlike the label-timing
fixes, this is a full removal of the suspected source, not a partial throttle, so a
null result here is a stronger negative than those were) — and worth then trying the
interval slider on the Overview/Power/Antenna tabs' own update cadence next, since
2/3-corrupted no longer looks consistent with any single-tab theory anyway.

## Major finding (2026-09-02): settings-save NVS write is a fully reproducible trigger

While testing the new Prop panel toggle (above), the user noticed something much bigger:
**the display corrupted the moment they clicked Save.** Follow-up testing isolated it
precisely:

- **Resaving the Settings page with no actual changes**: only a brief flicker/redraw,
  no corruption.
- **Toggling the Prop panel checkbox (either direction) and saving**: corrupted **every
  time**.
- **Follow-up: other unrelated settings changes also corrupt on save** (not exhaustively
  tested, but confirmed beyond just the Prop checkbox) — ruling out anything Prop-specific
  and pointing at something common to any settings change.

**Root cause: `saveConfig()`'s flash write.** `saveConfig()` (`main.cpp`, called from
`handleSave()` and the Log page's debug-toggle handler) does two flash writes on every
save: `Preferences::putString()` into NVS, and a mirror copy to `/config.json` on SPIFFS.
ESP32's NVS driver has a built-in optimization that silently skips the actual flash
erase/program when the new value is byte-identical to what's already stored — which
explains the whole pattern in one mechanism: a no-op resave skips the NVS write entirely
(only the smaller, always-happens SPIFFS mirror write fires, matching the milder
"flicker"), while any genuine value change forces a real NVS erase/program, which is
consistently what corrupts the display.

This is the **first fully reproducible trigger found in this entire investigation** —
everything before this required waiting hours and hoping. It's also strong, direct,
on-demand confirmation of the flash-write-disrupts-PSRAM-access mechanism that the
2026-08-26 web research only supported indirectly (the earlier flash-write suspect,
DebugLogger's periodic SPIFFS flush, was ruled out as the *sole* cause — but that never
contradicted flash writes being *a* contributing mechanism, and this result confirms
they are).

**Does NOT explain the ambient "2/3 corrupted" pattern on its own** — settings saves are
a rare, deliberate user action, not something happening continuously in the background.
The two are almost certainly separate: this is a newly-found, now-understood, *fixable*
bug; the ambient corruption is still most likely the broader PSRAM-bandwidth-contention
architectural issue from the 2026-08-26 research. Worth keeping both threads distinct
going forward.

**Fix applied (2026-09-02): reactive self-heal right after `saveConfig()`'s flash
writes**, instead of trying to avoid the write (not possible when a value genuinely
changes) or waiting for the passive 30s self-heal timer to eventually catch it:
```cpp
esp_lcd_rgb_panel_restart(s_panel_handle);
lv_obj_invalidate(lv_scr_act());
```
placed at the end of `saveConfig()`, after both the NVS and SPIFFS writes. This combines
a GDMA timing resync with a full repaint (to overwrite any actually-wrong bytes, not
just fix positional desync) — the same two-part reasoning as the 2026-08-26 mitigation
work. **Important difference from the periodic full-repaint that was tried and reverted
for making things worse** (recurring every 30s regardless of whether anything actually
happened, itself a new PSRAM-heavy trigger): this fires once, reactively, only
immediately after a disruption is now known to have just occurred — a much lower risk of
being a new trigger itself. Built and verified compiling clean (RAM 19.1%, Flash
27.6% — unchanged). **Not yet tested on hardware.**

**How to verify:** flash, then repeat the exact reproduction steps — change a setting
and Save, several times, ideally several different fields. If corruption stops occurring
on save (or self-clears within roughly a frame instead of persisting), the fix works. If
it still corrupts the same way, the restart+repaint isn't sufficient to counteract
whatever the flash write actually does to the frame buffer/GDMA state, and the next
avenue is investigating whether the write can be deferred (e.g. off the request-handling
path, onto the background poll task) — though note ESP32 flash program/erase operations
are documented to briefly suspend cached code/data access on *both* cores regardless of
which task issues them, so moving the call to a different task may not avoid the
disruption, only relocate which task blocks during it.

## Follow-up (2026-09-02): reactive self-heal did NOT clear it; three timed trials point away from "Prop-specific" and back to randomness

Flashed the reactive self-heal fix (previous section) and added `[SAVE]` timing
checkpoints throughout `saveConfig()`/`handleSave()`/`handleRoot()` to get an exact
timeline via `/log` instead of guessing. Three save attempts, reproduced live and
pulled from the device log:

| Attempt | Change | NVS bytes | NVS write | SPIFFS write | Total `loop()` stall | Result |
|---|---|---|---|---|---|---|
| 1 | Prop field (interval) | 1396 | 51ms | 180ms | 263ms | **Clean** |
| 2 | Prop panel: disabled | 1397 | 11ms | 24ms | 66ms | **Corrupted** |
| 3 | Prop panel: enabled | 1396 | 50ms | 164ms | 244ms | **Corrupted** |

Two findings from this table, both negative results that rule things out:

1. **The reactive self-heal (`esp_lcd_rgb_panel_restart()` + `lv_obj_invalidate
   (lv_scr_act())`, added right after the writes in `saveConfig()`) did not clear
   the corruption** — user confirmed it stayed corrupted, same as before the fix,
   not a brief flash. Whatever's happening isn't cleared by a restart+repaint the
   way the 2026-08-26 mitigations assumed it would be for this trigger.
2. **Attempt 1 and attempt 3 are near-identical in every timing/size respect**
   (same 1396 bytes, ~50ms NVS write both times, ~170-180ms SPIFFS write both
   times, ~250ms total stall both times) **yet one was clean and one corrupted.**
   This rules out write duration/size as the deciding factor — if it were, these
   two should have behaved the same. Combined with attempt 1 *also* being a Prop
   field change (not something else), this weakens the "Prop panel is special"
   theory considerably: all three trials touched Prop, and it split 1 clean / 2
   corrupted, which doesn't look like a deterministic Prop-specific trigger.

**Current best explanation: any settings save that actually changes a value blocks
`loop()` (and therefore `lv_timer_handler()`) for tens to a few hundred ms — far
longer than a routine ~sub-ms UI tick — and that stall has a real, roughly-random
chance of landing in a bad PSRAM-timing window, per the same architecture-level
GDMA/PSRAM bandwidth-contention limitation described in the 2026-08-26 web
research.** Not deterministic (attempt 1 proves that), but a long stall is a much
bigger single "roll of the dice" than normal operation gets — consistent with 2 of
3 trials corrupting. This reframes the actionable fix from "clear it after" (tried,
didn't work) to "minimize how long `loop()` is blocked in the first place."

**Fix applied (2026-09-02): deferred the SPIFFS mirror write off the blocking
path.** It was the larger and more variable of the two writes (24-180ms) and is
only a fallback/backup copy (`loadConfig()`'s SPIFFS fallback when NVS is empty;
`handleDownloadConfig()`'s config-backup endpoint) — not the authoritative store,
so a short delay before it lands doesn't matter. `saveConfig()` now just stages the
JSON (`s_pendingSpiffsJson`, `s_spiffsConfigDirty`, guarded by the existing
`g_dataMutex`) instead of writing it synchronously; `pollTaskFn()` (the existing
core-0 background task already used for exactly this "don't stall loop() with I/O"
purpose — see its own header comment) picks it up and does the actual write within
one ~20ms tick. The NVS write stays synchronous (it's the authoritative store and
the smaller/cheaper of the two — 11-51ms observed). Expected effect: cuts the
worst-case blocking stall from ~260ms down to roughly 60-80ms (JSON serialize +
NVS write + overhead only). The reactive self-heal calls stay in place as
belt-and-braces even though they didn't clear the specific corruption observed —
removing them isn't indicated by anything found here.

Built and verified compiling clean (RAM 19.1%, Flash 27.6% — unchanged). **Not yet
tested on hardware.**

**How to verify:** flash, repeat the same reproduction (change a Prop field, save,
several times). If corruption becomes markedly less frequent (not necessarily
zero, if the theory of "reduced but nonzero odds per stall" is right), that
supports the stall-duration theory. If it's still ~2/3 corrupting despite the much
shorter stall, that's a real negative result — it would mean even a ~60-80ms stall
is "long enough" to hit the bad window about as often, and the next thing to check
is whether the NVS write itself (not just SPIFFS) can be made faster or moved, or
whether this needs to fall back to the scheduled-reboot mitigation already
recommended in this file for the broader ambient corruption.

## Major finding (2026-09-02): hidden WiFi-driver flash write, independent of app code

While setting up a "disable every write and see if it's stable" test (user's request
after the ambient corruption recurred with no user action at all — see previous
section), the user asked a direct, good question: is it actually true that nothing
writes to flash except a Settings-page save? Checking that claim properly (rather
than repeating the earlier, incomplete inventory) found a real gap.

**Arduino-ESP32's `WiFi.persistent` defaults to `true`, and this codebase never set
it to `false` anywhere** (`main.cpp` or any shared library). That default means
**every call to `WiFi.begin()` silently writes the SSID/password to the WiFi
driver's own internal NVS namespace** — entirely separate from this app's own
`"tcfg"` config namespace, and entirely outside anything either `saveConfig()` or
`DebugLogger` touch. `WiFi.begin()` is called from `WiFiManager::begin()`
(unconditionally, once per boot) and from `attemptSTAConnection()` /
`handleAP_Mode()`'s scan-back path (on every reconnect, whenever
`wifiAutoReconnectEnabled` is on — the app-level gating on that flag correctly
skips these paths when disabled, but does not touch `WiFi.persistent`).

**This is the single most likely explanation yet for the *ambient* corruption**
(as opposed to the save-triggered one investigated above): it is the only flash
write in this entire system that can fire completely unprompted — a router hiccup,
brief interference, or AP roaming triggers a reconnect, which writes to flash, with
zero user action and no correlation to anything visible in the app's own log
(previous hypotheses like the TCI reconnect loop and WiFiManager's *blocking* wait
were tested and ruled out — but nobody had checked for a *hidden* write hazard
inside a reconnect that completes quickly and normally).

**Fix applied (2026-09-02):** `WiFi.persistent(false);` added to
`WiFiManager::begin()` (`ESP32-SharedLibraries/lib/WiFiManager/WiFiManager.cpp`),
right before `WiFi.mode(WIFI_STA)` — the standard, well-known fix for this class of
issue, safe here because this app already persists its own WiFi config separately.
Note this is a **shared library** used by other projects beyond TouchController —
worth being aware of if cross-checking other boards using `WiFiManager`, though the
fix itself has no known downside (it only stops a redundant internal copy the app
never reads back). Built and verified compiling clean (RAM 19.1%, Flash 27.6% —
unchanged, only `WiFiManager.cpp.o` recompiled). **Not yet tested on hardware.**

**Combined with the already-in-place fix from this session** (flash debug logging
turned off, the only other automatic write path), a build with this fix should now
have genuinely zero flash writes outside of an explicit Settings-page save —
finally a real, complete "no writes" baseline to test the ambient corruption
against.

**How to verify:** flash, leave running with flash debug logging off and without
touching Settings. If ambient corruption stops (or becomes much rarer) — especially
if it stops correlating with any visible WiFi disconnect/reconnect blip in the
in-memory `/log` — this is confirmed as a major (possibly *the*) cause of the
ambient pattern. If it still corrupts with zero writes now genuinely eliminated,
that rules out flash writes as the ambient cause entirely and points back to the
broader PSRAM/GDMA bandwidth-contention research from 2026-08-26 (WiFi traffic
itself, LVGL rendering, etc. — non-write-related contention).

## References

- [ESP-IDF RGB LCD docs — Avoiding Tearing Effects / known issues](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html)
- [`esp_lcd_panel_rgb.h`](C:/BuildCache/platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/esp_lcd/rgb/include/esp_lcd_panel_rgb.h) — struct fields, `esp_lcd_rgb_panel_restart()` doc comment
- [espressif/esp-bsp `esp_lvgl_port` — RGB avoid-tearing example](https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/examples/rgb_lcd/main/main.c)
- [espressif/esp-bsp `esp_lvgl_port_disp.c` (lvgl8) — flush_cb pattern this project's `lvgl_flush_cb()` matches](https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c)
- `framework-arduinoespressif32-libs/esp32s3/qio_opi/include/sdkconfig.h` — confirms `CONFIG_LCD_RGB_RESTART_IN_VSYNC=1` and `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=1` are already on by default in this project's toolchain
- Commit `d8eafe4` "Fix screen-shift artifact..." — the num_fbs=2→1 revert this session partially reopens
- Commit `477709e` "WIP: Replace LovyanGFX with esp_lcd_new_rgb_panel..." — original bounce-buffer introduction
- `esp-idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c` at the `v5.5` tag — `rgb_panel_draw_bitmap()`'s foreign-buffer/`cur_fb_index` handling, referenced (not directly read) when rejecting the triple-buffering attempt above
- [ESP32 Forum — LCD_CAM + GDMA external RAM bottleneck](https://www.esp32.com/viewtopic.php?t=33312) — CPU/GDMA PSRAM bandwidth is split 50/50 with no priority scheme; root justification for the periodic self-heal mitigation
- [espressif/esp-idf#12342 — LCD display corruption caused by Wi-Fi activities](https://github.com/espressif/esp-idf/issues/12342) — matching open issue, routine WiFi traffic (not reconnect) as trigger, still unresolved
- [espressif/esp-idf#13293 — PSRAM Framebuffers are not flushed after allocation](https://github.com/espressif/esp-idf/issues/13293) — corroborating cache-coherency-timing report
- [espressif/esp-idf#13805 — esp_lcd RGB panel buffer issue](https://github.com/espressif/esp-idf/issues/13805) — corroborating unresolved 2-framebuffer tearing report
- [`esp_cache.h`](C:/BuildCache/platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/esp_mm/include/esp_cache.h) — `esp_cache_msync()` API used for the explicit cache-writeback fix in `lvgl_flush_cb()`
