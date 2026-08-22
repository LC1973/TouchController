# RGB Panel Display Issues — Diagnosis & Fix History

This file exists because the screen-roll/flicker/corruption bug on this board has been
chased before and partially "fixed" more than once, only to come back. Read this
before touching `initRgbPanel()`, `lvgl_flush_cb()`, or anything in `board_pinout.h`
under `BOARD_PROFILE_1024X600`.

## ✅ CURRENT STATUS: WORKING (confirmed on hardware 2026-08-23, "Attempt E")

The display is confirmed clean on real hardware as of "Attempt E" below: no vertical
roll, no flicker, no text corruption. **This is the known-good configuration — treat
it as the baseline to preserve, not a starting point for further "improvement."**

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

## References

- [ESP-IDF RGB LCD docs — Avoiding Tearing Effects / known issues](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html)
- [`esp_lcd_panel_rgb.h`](C:/BuildCache/platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/esp_lcd/rgb/include/esp_lcd_panel_rgb.h) — struct fields, `esp_lcd_rgb_panel_restart()` doc comment
- [espressif/esp-bsp `esp_lvgl_port` — RGB avoid-tearing example](https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/examples/rgb_lcd/main/main.c)
- [espressif/esp-bsp `esp_lvgl_port_disp.c` (lvgl8) — flush_cb pattern this project's `lvgl_flush_cb()` matches](https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c)
- `framework-arduinoespressif32-libs/esp32s3/qio_opi/include/sdkconfig.h` — confirms `CONFIG_LCD_RGB_RESTART_IN_VSYNC=1` and `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=1` are already on by default in this project's toolchain
- Commit `d8eafe4` "Fix screen-shift artifact..." — the num_fbs=2→1 revert this session partially reopens
- Commit `477709e` "WIP: Replace LovyanGFX with esp_lcd_new_rgb_panel..." — original bounce-buffer introduction
- `esp-idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c` at the `v5.5` tag — `rgb_panel_draw_bitmap()`'s foreign-buffer/`cur_fb_index` handling, referenced (not directly read) when rejecting the triple-buffering attempt above
