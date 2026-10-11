# Rabbids Go Home — Web Port

**Browser:** https://jmhq20120212-cmd.github.io/rabbids-go-home-web-port/
**Publishing and game file setup:** [docs/PUBLISH.md](docs/PUBLISH.md)

On GitHub Pages choose the extracted `entire partition` folder from your own disc
to play. Files are read locally without uploading them. For automatic loading
from a public HTTPS asset host, configure `web/game-config.json` as described
in the publishing guide. GitHub Pages cannot host the full game partition.

A port of **Rabbids Go Home** (Wii, USA version `RGWE41`) that runs in a web browser, built by
**static recompilation**: the game's PowerPC code is translated ahead of time into C and compiled to
WebAssembly, and the Wii hardware it talks to (GPU, audio DSP, disc drive, Wii Remote, NAND, OS
services) is reimplemented in a small runtime. Graphics go through WebGL 2, sound through an
AudioWorklet.

*Ported by **teaser.lua**.*

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
