# Rabbids Go Home — Web Port

A port of **Rabbids Go Home** (Wii, USA version `RGWE41`) that runs in a web browser, built by
**static recompilation**: the game's PowerPC code is translated ahead of time into C and compiled to
WebAssembly, and the Wii hardware it talks to (GPU, audio DSP, disc drive, Wii Remote, NAND, OS
services) is reimplemented in a small runtime. Graphics go through WebGL 2, sound through an
AudioWorklet.

*Ported by **teaser.lua**.*

> **This repository contains no game code or data.** Rabbids Go Home is © Ubisoft. Everything that
> comes from the game — the recompiled code, the shader cache, the disc files, artwork — is produced
> locally from **your own legally obtained copy** and is excluded from the repository by
> `.gitignore`. This project is not affiliated with or endorsed by Ubisoft or Nintendo.

## What you need

- Your own copy of **Rabbids Go Home for Wii (USA, RGWE41)**, extracted with Dolphin:
  right click the game → *Properties* → *Filesystem* → right click the disc → *Extract Entire Partition…*.
  You get a folder (here called `entire partition`) containing `sys/`, `files/`, `tmd.bin`, `ticket.bin`.
- To build: Linux, macOS or WSL with `python3`, `clang`, `make`, `gzip`,
  the [WASI SDK](https://github.com/WebAssembly/wasi-sdk) (tested with 25) and
  [Binaryen](https://github.com/WebAssembly/binaryen) (tested with 123).
- To play: Chrome or Edge (WebGL 2 + SharedArrayBuffer), and Python 3 or Windows PowerShell for the local server.

## Build

```sh
export W=/path/to/wasi-sdk BINARYEN=/path/to/binaryen
tools/build.sh "/path/to/entire partition"
```

`build.sh` does, in order:

1. `tools/gen.py` — recompiles `main.dol` to C in `gen/`, using the symbols of `LynWiiRetail.elf` from the disc.
2. a native build (`build/rgh`), which runs once to capture the game's AI module (`Ai2CppWiiFinal.rso`)
   exactly as the game relocates it in memory;
3. `tools/gen_rso.py` — recompiles that module to `gen_rso/`;
4. the browser build: `web/rgh.wasm` (+ `web/rgh.wasm.gz`).

A full build takes a while (the generated C is large; about 1–2 hours on a laptop) and needs about
8 GB of RAM for the final WebAssembly optimization step. Later builds are incremental.

## Play

- **Windows:** put the project folder next to `entire partition` and double click `start-rabbids-web.bat`
  (or `start-rabbids-web.ps1 -Game "C:\path\entire partition"`).
- **Any OS:** `python3 serve.py --game "/path/to/entire partition"` and open `http://localhost:8080/`.

The server only listens on `127.0.0.1` and only serves this folder and your game copy to your own
browser. Saves are kept in the browser (IndexedDB), with a backup copy written next to `web/`
(`rabbids-save-backup.bin`).

On the first start the loading bar compiles the shaders the browser has already seen; new ones are
drawn with a generic shader while they compile and are remembered for the next time.

## Controls (default, all rebindable in *Settings*)

| Wii | Keyboard | Controller (Xbox / PlayStation) |
|---|---|---|
| Pointer | mouse (click the game to capture it, **Esc** releases it) | right stick |
| A / B | left / right click, Space · Enter / X · Shift | A·Cross / B·Circle, RT·R2 |
| A + B | Q, F | — |
| Nunchuk stick | W A S D | left stick |
| Z / C | Z / C | LT·L2 / LB·L1 |
| Shake Wii Remote / Nunchuk | E / R | RB·R1, Y·Triangle / X·Square, L3 |
| + / − / HOME | P · Esc / M / H | Start / Back / Guide |
| 1 / 2 | 1 / 2 | — |
| D-pad | arrow keys | D-pad |

A phone can also be used as a Wii Remote and Nunchuk with the **DSU Remote** app (cemuhook
protocol) — see *Settings → DSU Remote*. Only addresses of your local network are accepted.

## Troubleshooting

- The page logs to `rabbids-log.txt` (next to `web/`); the previous session is kept in
  `rabbids-log-previous.txt`.
- **F9** writes what every game thread, the level loader and the audio mixer are doing at that moment
  into the log. It also happens automatically when the game looks stuck or silent.
- *Settings → Display & graphics*: internal resolution (1x is much lighter on laptops), shader mode
  (precompile everything / generic shader only), FPS counter and the info bar.

## How it works

| Part | Where |
|---|---|
| PowerPC → C recompiler (functions, jump tables, paired singles) | `tools/gen.py`, `tools/gen_rso.py`, `tools/elf.py` |
| CPU state, memory, interrupts, threads as fibers | `runtime/main.c`, `runtime/os.c`, `runtime/fiber_*.c` |
| GX command processor → render records → WebGL 2 (TEV as generated shaders + ubershader) | `runtime/gx.c`, `runtime/gxrender.c`, `web/gx-renderer.js` |
| AX audio mixing (ADPCM/PCM voices, aux buses, Wii Remote speaker) | `runtime/ax.c`, `web/audio-worklet.js` |
| IOS: disc, NAND file system, settings | `runtime/ios.c`, `runtime/disc.c`, `runtime/nandfs.c` |
| Wii Remote / Nunchuk (high level) | `runtime/wpad.c`, `web/input.js` |
| Page, loading screen, settings | `web/index.html`, `web/ui.js`, `web/rgh-host.js`, `web/rgh-worker.js` |

The title screen artwork is optional: see `web/assets/README.md`.

## License

The source code in this repository is published for reference; no license has been chosen yet,
so all rights are reserved by the author. No license is granted for any game content.
