# ODT Recomp — Working Notes

**Game:** O.D.T. Escape... Or Die Trying, PS1 NTSC-U, **SLUS-00698** (FDI / Psygnosis, 1998).
**Framework:** [psxrecomp](https://github.com/mstan/psxrecomp), PolyForm Noncommercial 1.0.0.
Submodule pinned at **`c604cea4`**; `recomp-ui` at `faa3330f`. Upstream moves fast — use
plain `git submodule update --init --recursive`, never `--remote`, unless upgrading on purpose.
**After any submodule update, re-run `scripts/apply-patches.sh`** — the update discards our patches.
**Host:** macOS 26.2, arm64, Apple clang 17, Python 3.13.

**Status: playable, widescreen.** Boots, FMV plays edge-to-edge 16:9, gameplay reachable.

---

## Disc

Dump lives in `disc/` (gitignored via `/disc/`). `game.toml` points at it with a
**relative** path, which `resolve_disc_path()` makes absolute against the *current
working directory* — so launch from the repo root (`./build-release/ODT__Recompiled`),
not by double-clicking the exe. Matches **Redump disc 2333**
exactly — both tracks, all three digests. Baked into `game.toml` `[prepare_disc]`.

| Track | Type | Sectors | Size | CRC32 |
|---|---|---|---|---|
| 1 | Data/Mode 2 | 241,767 | 568,635,984 | `7eb6560a` |
| 2 | Audio | 14,417 | 33,908,784 | `857edda8` |

ISO9660: volume `ODT`, `SYSTEM.CNF` → `BOOT = cdrom:\SLUS_006.98;1`.

**Reading assets straight from the `.bin` works and is often faster than play-testing.** Raw
sectors are 2352 bytes with data at offset 24. See "FMV letterbox fill" for the STR header scan
that characterised every movie on the disc in one pass.

---

## Build

`tools/setup_dev.sh` does **not** work on a fresh clone — it probes only for a retail
`SCPH1001.BIN` and its `regen_bios.sh` call defaults to the retail profile. Use:

```sh
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build recompiler/build                    # ~35s
git submodule update --init --recursive           # REQUIRED
bash tools/regen_bios.sh --config bios/OpenBIOS.toml
cmake -S runtime -B runtime/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_RECOMP_UI=OFF
cmake --build runtime/build --target psx-runtime --parallel   # ~18s
```

- **Clone must be recursive.** Runtime configure fails without `lib/retcomm-rbengine` (Rewind)
  and `lib/recomp-net`; `rbengine` is a submodule *of psxrecomp*, not of the game.
- **No retail BIOS needed** — `bios/openbios.bin` ships in-tree (MIT), but needs an explicit
  `--config bios/OpenBIOS.toml`.
- **Vulkan is a silent software stub on macOS** (no SDK/`glslc`); OpenGL is the real backend.
  Upstream's 60.5fps figures are Vulkan numbers — MoltenVK is the first lever if pacing gets tight.
- **`psx-beetle` is skipped** (no `libmednafen_psx.a`). That's the reference emulator / oracle;
  build it before any serious divergence hunt.
- Editing a mod manifest alone isn't enough: staging is a POST_BUILD step on the link, so
  delete the exe to force a restage.

### Test suite

`docs/TESTING.md` claims 38 tests in <5s. Actual at pin `c604cea4`: **114 registered, 111 run,
~37s**. `ctest --test-dir build-recompiler`. **Two expected failures — both test bugs, not
product bugs. Match them by NAME, not number: upstream adds tests and renumbers (these were
#29/#65 at pin `5c183967`). If either changes, something real moved:**

- **#35 `cli_generate_aot_static`** — macOS `/var` is a symlink to `/private/var`; missing
  `.resolve()` in the test's own assertion.
- **#73 `aot_overlay_discovery`** — test hardcodes Windows `.dll` in 66 places while
  `overlay_ext()` returns `.so` off Windows, so its regex matches nothing. Verified the
  production dedup logic is correct by calling it with the host extension. Fails on Linux too.

(A third, #31, fails only if the runtime hasn't been built yet — stale-shard guard. Not a bug.)

---

## Recompilation coverage

From `generated/SLUS_006.98_full.ranges` (union of per-function extents; the raw sum
double-counts to 206%):

**2,597 functions, 144,170 instructions, 93.9% union coverage of the text segment**
(576,680 of 614,400 bytes, `0x80010000`–`0x800A6000`). No gaps >1KB. The missing 6% is the
rodata tail from `0x8009D328`. Seeds: `seeds/ghidra_funcs.txt`, 1,268 entries.

---

## Overlays — MOVIES.EXE runs interpreted

Capture store is `build-release/overlay_captures.json` (+ `.json.d/`), **not** `.cache/`.
One 16-minute session: 55 records, 36 distinct load addresses.

| Region | What |
|---|---|
| `0x80002000`, `0x8000D000` | BIOS runtime-installed dispatch stubs (normal PS1 behaviour) |
| `0x80011000`–`0x80098000` | pages the game writes at runtime, tripping the dirty-RAM guard |
| `0x800B4000`–`0x800BD000` | `RSC/MENU.BIN` loaded as code (its first bytes are pointers `0x800B4614`/`0x800B45AC`) |
| `0x80171000`–`0x80186000` | **`MOVIES/MOVIES.EXE`** — a real PS-X EXE, load `0x80170000`, size `0x31000`, entry `0x801719A0`, at LBA 32345 |

MOVIES.EXE is the boot/FMV module and owns the Psygnosis legal screen. It sets its **own**
`$gp = 0x801A0868` (at `0x80171A0C`).

**All of this runs on the dirty-RAM interpreter**, because `overlay_toolchain/` is absent from a
source checkout (TinyCC is bundled only at release packaging). `gcc` *is* on PATH and
`docs/ASYNC_OVERLAY_COMPILE.md` says gcc is the development default, so
`tools/compile_overlays.py` should work offline. See `docs/COMPILING_OVERLAYS.md` §1 (shard
cache) or §2 (`--static`, baked in). Unverified: whether `--cps` matches this runtime build,
and whether the tool accepts Apple's clang-as-gcc shim.

🔒 `overlay_captures.json` holds ODT's own code read off the disc. Keep private. Gitignored via
`/build-*/`.

---

## ODT internals

- **Main EXE:** load `0x80010000`, entry `0x800119E4`, text `0x96000`. `$gp = 0x800A552C`,
  set at `0x80011A08` (`lui $gp,0x800A` / `addiu $gp,$gp,0x552C`).
- **`VSync()` at `0x801746E4`** (in MOVIES.EXE) — identified by its double-read stability loop
  and `bgez $a0` / `beq $a0,1` mode handling. Most call sites pass `-1` (non-blocking query).
- **Psy-Q SDK confirmed** — `$Id: intr.c,v 1.75 ...` and `$Id: sys.c,v 1.140 ...` RCS tags.
- **Path table** at `0x800A552C` is dev leftovers — never referenced (`$gp+0` has zero
  references; smallest offset actually used is 328). Retail loads by other means.
  Still a useful asset map: `Level%02d\Sector%02d.{all,lnk,pcl}`, `Level%02d\Level.txt`,
  `Level%02d\Inlocal\local.lnk`, `Players\hr?.{pcl,lnk}`, `Objs\boss%02d.bin`,
  `Effects\Effects.{lnk,pcl}`, `Demos\Demo%d.joy`, `\SOUNDS\MUSICS\LEVEL%02d.XA;1`.

### Where text lives

- **Main menu** — a text script inside `RSC/NTSC/GFILES.LNK`: `// Scripte du Menu`,
  `L5 $100$ "New gamE"`, `L4 $102$ "Load gamE"`, `L1 $101$ "OptionS"`.
- **Dialogue** — `LEVELnn/LEVEL.TXT`, plain ASCII, speaker-tagged
  (`X[][Captain_082.....]…`). Each level also carries its own pause menu
  (`Continue` / `Load game` / `Quit` / `Are you sure ?`).
- **`RSC/MENU.BIN`** — a *developer* level-select menu (`LOAD GAME!`,
  `MENU ODT : SELECT LEVEL !`, `Sector 00`, level names). Patches here apply cleanly and
  show nothing on screen.
- **Boot screen** — `LEGAL/LEGAL.TIM`, a PS1 TIM: 640×240, 16bpp direct, no CLUT,
  307,220 bytes at LBA 761. Siblings: `FEAT1/FEAT2/LOADING/LEVEL0-7.TIM`.
  640×240 on a 4:3 display means ~2:1 non-square pixels, so the art is drawn horizontally
  stretched — text in it is ~1.74× wider than its cap height suggests.

---

## Patching game data — works, no fork needed

`[[patch]]` in a mod manifest under `mods/preloaded/packages/`. `target = "disc_raw"`,
`offset = lba*2352 + byte_in_sector`. **Equal length, must not cross a sector boundary**;
pad short replacements with spaces. The disc image on disk is never written — patches
intercept CD reads. `[[recompiler.patch]]` in `game.toml` does the same for main-EXE
instruction words at build time.

`expected` guards the **bytes, not the meaning** — it will happily patch a string that is
never displayed. Only seeing the change on screen proves the target.

See `mods/preloaded/packages/odt.debugtext/` — 3 features, 13 patches, all confirmed on screen.

---

## Patching upstream without forking

Changes to `psxrecomp/` live as diffs in `patches/`, applied by `scripts/apply-patches.sh`
(`--revert` restores pristine). Numbers are apply order only, not identity — the script globs
`patches/*.patch`.

- A submodule's `.git` is a **file**, not a directory; detect with `git -C "$SUB" rev-parse --git-dir`.
- `apply-patches.sh` always reverts to pristine and replays the whole series.
  Patches stack on one file (0001 and 0003 both touch `gpu_gl_renderer.c`), and
  "is this already applied?" is unanswerable once a later patch sits on an
  earlier one's context - replaying is deterministic. It discards uncommitted
  edits in `psxrecomp`, so make them patches.
- When upstream moves, plain `git apply` fails on context. `git apply --3way` usually rebases
  it; regenerate the patch afterwards so plain apply works again.
- `.gitmodules` sets `ignore = dirty` for psxrecomp. Without it the applied patch shows forever
  as "modified content" (`Mm` in `git status`) and never commits away. It still reports pin
  changes, so a bump can't be missed.
- Commit the gitlink explicitly (`git add psxrecomp`) — `git commit -a` skips it when dirty.

---

## Widescreen

`[video] aspect_ratio` is **deliberately inert on PSX** (`ws_offered = false`); `WIDESCREEN.md`
is stale on this. The only route is a **trusted mod activation plugin** calling
`psx_mod_set_fixed_display_aspect(16, 9)` — see `src/odt_widescreen.c` plus
`mods/preloaded/packages/odt.widescreen/`. The plugin reaches the runtime's `EXTRAS_SOURCES`
via `CODEGEN_SETUP_SOURCES` in `CMakeLists.txt`, which is the only hook a title has.

`game.toml [widescreen]`: `native_wide = false` (ODT doesn't fill a widened target, so
squash+stretch), `full_2d = true` (classify every frame as gameplay, else menus and the
save/load overlay snap back to 4:3 mid-frame).

---

## FMV letterbox fill

**Every FMV on the disc is 320x192** — confirmed by scanning STR frame headers straight out of
the track 1 `.bin` (magic `0x0160`, type `0x8001` at sector data offset 24; width/height at
`+0x10`). 84,597 chunks at 320x192, plus one 320x256 outlier at LBA 42182. Reading the disc
beats play-testing for this: complete answer in seconds.

Bars come from **two stacking sources**:

| source | size | seen by |
|---|---|---|
| 192-line picture centred in the 236-line display band | 24 top / 20 bottom | VRAM coverage *or* pixels |
| black baked into the intro's own picture | ~16 / ~14 more | pixels only |

So the intro needs 40/32 and the splashes 24/20. The fix (`patches/0001-fmv-widescreen-fill.patch`,
one file) covers the window with the content region instead of pillarboxing, measuring bars by
accumulating **which rows have ever been lit** in the current video.

Why that works: a real bar row never lights and the map only grows, so the estimate converges on
the truth **from above** — it can over-crop while the picture fades up from black, but can never
show a bar. Before any lit frame it assumes a 20% cap (`FMV_MAX_BAR_PCT`).

**Threshold 8 is the whole trick.** Measured on ODT's movies: bar rows are *exactly* 0, MDEC
ringing puts stray pixels up to ~4 in them, and genuinely dark picture runs to ~40. At 8 a bar
row has zero lit pixels while picture rows are saturated. Too low catches ringing; too high
discards dark picture and convergence crawls.

Two further details, both load-bearing:
- `ly`/`lh` must round **away from zero**. Truncating leaves the content's bottom edge ~0.9px
  high, which shows as a 1px black sliver.
- A 2-row margin (`FMV_BAR_MARGIN`) past the measured edge. The bar boundary is an MDEC block
  edge, so the last row or two is a dark transition rather than a clean cut.

---

## HUD in widescreen

The GTE X-squash genuinely widens the 3D view (verified: the 16:9 frame contains
the 4:3 content at identical scale plus extra world at the sides - mean abs diff
5.76 for "expansion" vs 17.76 for "stretch"). Screen-space 2D gets no squash, so
the present stretches it unopposed: gauges measured w/h 1.44 against 1.07 at 4:3.

**The generic fixes do not apply here.** `hud_sprt_squash` and `auto_ui_squash`
both act on SPRT/RECT prims, and **O.D.T. emits none at all** - a gameplay frame
is ~1128 `0x34` gouraud-tex tris, ~204 `0x3C` quads, and ~16 flat-textured quads
(`0x2C`/`0x2D`/`0x2E`/`0x2F`). The HUD is those flat quads. `hud_sprt_squash` was
enabled, logged as active, and changed nothing measurable - don't retry it.

`ws_hud_pivot`'s thirds heuristic would also break the bar: a tick at x=75 falls
in the left third while the rest of the bar is in the middle, so it would anchor
to the screen edge and detach. Patch 0002 uses explicit x zones instead.

HUD geometry (320x240 space), from `gpu_frame_dump`:

| x | y | element |
|---|---|---|
| 15..53 / 23..42 | 195..231 | left gauge ring / icon |
| 50..56 | 223..231 | counter digit |
| 63..170, 158..261, 76..153 | 206..226 | bar halves + fill |
| 272..310 / 280..301 | 195..231 | right gauge ring / icon |
| 8..77 (slides) | 30..61 | lives counter: portrait + "x N" |
| 20..101 | 20..101 | L1 ability d-pad: four 31x31 cells |

The top-left region (`ODT_TL_*`, y 15..105, x<=130) covers both the lives counter
and the L1 d-pad, anchored left as a whole rather than by x zone - the lives
counter **slides in from the left** and zones split it mid-slide. Bounded to
x<=130 so the unidentified clusters at x=245..320 (menu/inventory?) are
untouched; the nearest other element starts at x=130 and is excluded by
`maxx <= 130`.

The d-pad is the cleanest proof the un-squash works: its cross is square in
source space, so the whole cluster measures w/h 1.33 stretched and 1.00 fixed.

**Zones are chosen per quad by size, never by a frame latch.** Zone anchoring is
right for the HUD and wrong for menu text (a run crossing the x=60 boundary
splits: "Press" -> "Pres" + "s"). The first attempt gated zones on the health
bar being live within 2 frames, which fails whenever the bar misses a few
frames: the bar sits between both thresholds so it looks identical either way,
and only the two edge icons visibly snap back to 4:3. Measured instead - every
HUD member is >=18 tall (icon frame 38x36, its art 19x21, bars 107x19 and
103x18) or reaches y=231 (the 6x8 lives digit), while no menu glyph exceeds 14
tall or y=222. `ODT_HUD_ELEM_MIN_H`/`ODT_HUD_FLOOR_Y` test exactly that, so no
transient can drop zones mid-scene. Main menu draws nothing in the band at all;
the text screens draw 22 prims, none wider than 28.

**Un-squashing needs `supersampling = 2`.** At 1x raster a squashed 31px sprite
has only 23 device pixels to hold 31 texels, so ~8 texel columns are dropped -
visible as clipped borders on the d-pad circles and both bottom gauges. Padding
the quad does NOT help (tried +1 and +2px): it changes which columns are lost,
not that they are. At 2x the same quad covers 46 device pixels, every texel
lands somewhere, and the borders survive. This also explains loss on the RIGHT
for both left- and right-anchored elements, which a rounding explanation cannot:
sampling density is uniform across a quad regardless of anchor.
2x leaves a faint sliver of one border column. 4x was tried (~3 samples/texel
vs ~1.5) and could not be shown to improve it - `present_shot` caps at 1920x1080
so captures cannot resolve the difference. Settled on 2x: 4x costs 4x the fill
rate for no demonstrable gain. The only guaranteed fix is a filtered downscale
(`texture_filtering = "bilinear"`), which softens every texture in the game -
rejected as too large a change for a 1px HUD artefact.

Both elements ANIMATE IN (counter from the left, d-pad from above), so they are
matched on SIZE, not on a y band. A fixed band leaves a cell unsquashed while
its neighbours are squashed, which shows as ovals mid-slide and cells clipping
each other's borders. **Screenshots cannot verify this** - the animation is a
few frames and a present_shot round-trip is ~65ms, so every capture lands on the
settled state. Needs a human to confirm.

**Zone anchoring is gated on the gameplay health bar.** Zones are right for the
HUD and wrong for menu text: on character select "Press" spans x=20..68 and
crosses the x=60 boundary, so four glyphs anchored left and the fifth centre -
rendered as `Pres    s`. No threshold fixes it (the right-hand text straddles
x=260 the same way, and the gauge at x=15..53 needs the left zone). The bar's
wide quads (w>=90; menus top out near 28) mark gameplay; without them every
bottom-band quad shares the centre anchor so a text run cannot split. Net
result: menus sit at 4:3 proportions centred, gameplay gets the 3-element
layout. Only the bottom band (y>=190) is touched at all, so menu content ABOVE
that line is still stretched - accepted, few menu screens.

`func`/`ra` do NOT discriminate HUD from world: a bar quad and a world effect
both report `func=0x000029CC ra=0x8008ACC8`. Position is the only clean signal.
All numbers come from level 0 - other levels are unverified.

---

## FMV playback is slow

Measured **~991 macroblocks/s = ~4.1 fps** (320x192 = 240 mb/frame), steady. ~15fps is the
assumed STR target but is **not verified from ODT's own data**. Jamie reports the *audio* is what
sounds wrong; likely the same root cause, but XA-ADPCM is interleaved into the same sectors so
CD sector delivery and SPU pacing are separate candidates.

**Interpreted overlays are now ruled out too.** After compiling 555 native shards (2026-10-02), dispatch during FMV is 99.1% native / 0.9% interpreted and the rate was unchanged at 4.29 fps. 15fps needs ~3600 mb/s against the ~1031 measured, so the remaining candidate is MDEC decode throughput in the runtime (IDCT/dequant), not ODT's own code. Profile that next.

The earlier prime suspect was MOVIES.EXE running interpreted (below). **The pin bump is ruled out** — old pin
`5c183967` measured 909 mb/s, new `c604cea4` 991, so the newer one is 9% *faster* despite adding
per-instruction uncached-fetch accounting. Don't re-investigate that.

---

## Debug server

Off by default: it needs **`-DPSX_DEBUG_TOOLS=ON`** at configure time (`PSX_NO_DEBUG_TOOLS`
is defined otherwise), *plus* `debug_port = 4370` in `game.toml` `[runtime]`.
`psxrecomp/tools/debug_client.py`, 303 commands. Useful: `frame`, `get_registers`,
`frame_timeseries`, `screenshot_hires path=…`, `wtrace_range` (RAM writes with RA),
`overlay_state`.

- **One request per TCP connection** — reusing a socket drops it.
- `cpu->pc` is **0 while inside statically recompiled code**; only interpreted code and BIOS
  report a real PC. So PC sampling sees overlays and BIOS only — which is itself a quick way
  to tell what is still interpreted.
- Debug builds are explicitly "laggy" (extra thread, per-block recording, rings). Rebuild
  with `-DPSX_DEBUG_TOOLS=OFF` before judging performance or FMV smoothness.

**Driving the game from the harness** (cost a lot of wasted runs to learn):

- **The pad is ACTIVE LOW.** Idle is `0xFFFF`; a pressed button is a *cleared* bit. So
  `press(buttons=1<<14)` means "every button except Cross". Send `0xFFFF & ~BTN`.
- **`mdec_state.decode_macroblocks` is per-command, not cumulative** — it sits at 240 for a
  whole movie, so "has it changed" reads as idle mid-playback. Use **`trace_total`** (uint64,
  monotonic) to detect or rate-measure FMV activity. `gpu_state.height` also flips 240 → 236
  during depth24.
- **`turbo` suppresses presents.** Screenshots go stale and menu detection goes blind; in one
  run it also skipped straight past the intro into gameplay. Never use it while capturing.
- `present_shot` can silently not land. Verify a *new* file appeared — a harness that ignores
  this re-reads the previous frame and reports the screen as unchanged.
- Detect the main menu by **template-matching a known capture** (downscale to 32x18, mean abs
  diff; menu <1, everything else >14). Brightness and orange-pixel counts both fail to separate
  it from the loading screen.
- **`supersampling` must be set in BOTH `game.toml` AND `settings.toml`**, to the
  same value. Observed, mechanism not pinned down:
  | game.toml | settings.toml | scale | result |
  |---|---|---|---|
  | 2 | 1 | 1x | HUD borders clipped |
  | absent | 2 | 2x | **frame cropped / zoomed** |
  | 2 | 2 | 2x | correct |
  Both paths write `g_video_scale` (main.cpp 13086 from game.toml, 13502 from
  settings), which also drives `SDL_RenderSetLogicalSize`, so they ought to be
  equivalent - they are not. Don't assume precedence; set both and verify.
- `settings.toml` is user state the runtime REWRITES on exit, so a manual edit can
  be silently undone (the same trap as `skip_launcher`). Confirm the effective
  value via the startup log's `internal scale Nx`, never the file.
- At supersampling 2x, `present_shot` captures at 1920x1080 instead of 960x540 -
  normalise before comparing crops or everything looks magnified.
- On a re-run, `bind(4370) FAILED` means the previous instance still holds the
  port. The harness then dies with `ConnectionRefused` from the *client* side,
  which points nowhere near the real cause - check the game log for the bind
  line. Bit three times in one session; `pkill` then wait ~5s.
- `skip_launcher = true` in `build-release/settings.toml` is **required** for autonomous runs,
  and **the runtime rewrites that file on exit**, so a manual revert gets silently undone.

**Reaching the intro FMV:** boot movies → main menu → X → character select → X → loading →
intro. The intro is *not* one of the three boot movies (the third is a trailer). Bounded
X-presses skip the boot movies, then stop pressing — the menu appears ~10s later, and pressing
through it lands in gameplay having skipped the intro entirely.

---

## Dead ends (don't redo)

- **Legal screen duration.** It is load-bound, not timer-driven: `0x801851C0` spins a poll
  loop (timeout `0x00800000`) on a CD command and exits the moment data arrives. Forcing
  single-speed CD in MOVIES.EXE's `CdSetMode` wrapper (`0x801861E8`) *did* work — it visibly
  starved the FMVs — but barely lengthened the screen. Not worth pursuing.
  Note `$s0` there carries caller flags in bits 8+ (tested at `0x80186204`/`0x8018620C`), so
  masking it to `0x7F` hangs the boot; `0xFF7F` is the only safe mask.
- **Per-frame luminance bar detection with a lock.** Measuring black rows per frame and
  locking the result cannot work: over 706 frames it locked once, and its `top` swung 28→67
  purely with scene content. It cannot tell a dark sky from a letterbox. Every added filter
  (brightness peak, content-share floor, N agreeing frames) traded one failure for another.
  Replaced by ever-lit-row accumulation, which converges from above and never oscillates.
- **Freezing the crop once it stabilises.** Stable, but it locks during the fade-in and
  over-crops (40/32 where the truth was 28/22), and while waiting it falls back to the
  *previous* video's crop — which under-crops and shows exactly the bands it was meant to fix.
- **VRAM upload coverage for bar measurement.** Worked and was exact, but is strictly dominated
  by the pixel accumulator: uncovered rows are black, so the accumulator finds them too, and the
  code took `max()` of the two. Removed — 56 lines across two extra files for nothing. (If
  reinstating: `depth24_note_upload` must count *all* depth24 uploads, not just FB-class ones —
  MDEC writes 24-halfword macroblock strips, far under the `w >= 256` filter.)

- **`psxrecomp/CLAUDE.md`** is the framework author's own agent ruleset ("BIOS first, game
  never until Phase 5", "no printf debugging"). Not instructions for this project — but any
  agent working in this repo may pick it up.

---

## Next

1. **FMV audio pacing** — the open question. See "FMV playback is slow" above.
2. **Stage A2 — compile the overlays.** MOVIES.EXE is the known interpreted module and the
   prime suspect for (1).
3. **Play-test for robustness** across all 8 levels, save/load, long-session audio. FMV fill is
   verified on the boot splashes and the intro only; in-game cutscenes are unseen.
4. **QoL:** supersampling is now upstream — `[video] supersampling` plus an
   `internal_resolution` preset in Settings → Display. 60fps last.
5. Two clean upstream PRs still available from the test triage above (`cli_generate_aot_static`,
   `aot_overlay_discovery`). `depth24_trailing_margin` landed as #432.

## 60fps (render passes)

`src/odt_60fps.cpp` redraws an in-between frame with the game's own code at
interpolated GTE transforms (`docs/RENDER_PASSES.md`). Game logic stays at 30.

| address | role |
|---|---|
| `0x800B1C2C` | frames per swap: 2 in gameplay, 1 in menus (passes off) |
| `0x800B1B20` | frame struct awaiting the VSync callback (`func_800961FC`) |
| `0x800B1C28` | frame struct being built: `0x800B1B28` / `0x800B1BA4` |

Frame struct: DRAWENV `+8`, DISPENV `+0x64` (shows the other buffer), OT `+0x78`.

- A frame is one round of a cooperative task scheduler (`func_80093230` yields),
  `0x800932B8` to the submit `0x800932A8`. Logic and draw interleave, so a pass
  replays the whole round and ends via a sentinel return at the submit.
- Passes run at the submit's entry (the game's idle spin), in `SHOWN` flip mode,
  at phases 0.5 (frame k) and 0.75 (α=0.5 in-between): the presenter sees the
  callback's flip a VBlank late and only presents those phases.
- The VSync IRQ updates timers and pad state mid-round; the plugin reapplies them
  at the same yield in the replay. `patches/0004` makes slerp exact at t=0/1.
- Check a replay at α=1: it must equal frame k+1 pixel for pixel.
- Measure with `present_image_ring_get` (distinct images) and `gl_present_ring`
  (present times); the FPS readout always says 60. Running two instances at once
  overloads a MacBook Air in heavy scenes.
- Dead ends: threshold 1 doubles game speed; crossfade interpolation is
  imperceptible; vsync with interpolation drops the game to ~25fps.
