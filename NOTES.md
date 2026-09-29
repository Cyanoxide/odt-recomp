# ODT Recomp — Working Notes

**Game:** O.D.T. Escape... Or Die Trying, PS1 NTSC-U, **SLUS-00698** (FDI / Psygnosis, 1998).
**Framework:** [psxrecomp](https://github.com/mstan/psxrecomp), PolyForm Noncommercial 1.0.0.
Submodule pinned at **`5c183967`**; `recomp-ui` at `faa3330f`. Upstream moves fast — use
plain `git submodule update --init --recursive`, never `--remote`, unless upgrading on purpose.
**Host:** macOS 26.2, arm64, Apple clang 17, Python 3.13.

**Status: playable.** Boots, FMV plays, gameplay reachable — first generation, no hand-fixes.

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

`docs/TESTING.md` claims 38 tests in <5s. Actual: **100 registered, 97 run, ~34s**.
`ctest --test-dir recompiler/build`. **Two expected failures — both test bugs, not product bugs.
If either changes, something real moved:**

- **#29 `cli_generate_aot_static`** — macOS `/var` is a symlink to `/private/var`; missing
  `.resolve()` in the test's own assertion.
- **#65 `aot_overlay_discovery`** — test hardcodes Windows `.dll` in 66 places while
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

---

## Dead ends (don't redo)

- **Legal screen duration.** It is load-bound, not timer-driven: `0x801851C0` spins a poll
  loop (timeout `0x00800000`) on a CD command and exits the moment data arrives. Forcing
  single-speed CD in MOVIES.EXE's `CdSetMode` wrapper (`0x801861E8`) *did* work — it visibly
  starved the FMVs — but barely lengthened the screen. Not worth pursuing.
  Note `$s0` there carries caller flags in bits 8+ (tested at `0x80186204`/`0x8018620C`), so
  masking it to `0x7F` hangs the boot; `0xFF7F` is the only safe mask.
- **`psxrecomp/CLAUDE.md`** is the framework author's own agent ruleset ("BIOS first, game
  never until Phase 5", "no printf debugging"). Not instructions for this project — but any
  agent working in this repo may pick it up.

---

## Next

1. **Stage A2 — compile the overlays.** MOVIES.EXE is the known interpreted module.
2. **Play-test for robustness** across all 8 levels, save/load, long-session audio.
3. **QoL:** `internal scale` is 1× (supersampling off) — cheap win. Then widescreen
   (`docs/WIDESCREEN.md`: cull widening, sprite hooks, HUD compositing). 60fps last.
4. Two clean upstream PRs available from the test triage above.
