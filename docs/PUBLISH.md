# Browser deployment

The GitHub Pages workflow publishes `web/` to:

https://teaserlua.github.io/rabbids-go-home-web-port/

The repository-root `index.html` redirects to `web/` when Pages publishes from
the `main` branch. When the GitHub Actions deployment publishes the contents of
`web/` directly, the game also works at the root URL.

GitHub Pages has a 1 GB published-site limit. This project uses a small web client and
compressed WASM. Game disc contents stay outside Git history. The first visit installs
`coi-sw.js` and reloads to enable `SharedArrayBuffer` for the WASM worker runtime.

## Two sources for game files

1. **Local alternative:** On the published page press **Choose game folder** and select
   the extracted `entire partition` folder (with `sys/` and `files/`). The browser
   reads selected files in 256-KiB slices using `FileReaderSync` in its workers.
   The selected files are not uploaded. The player must select them again after
   refreshing. Chrome / Edge desktop support directory selection.
2. **Online by default:** The published page now loads the extracted partition
   automatically from the dedicated Cloudflare Worker with R2 storage. The
   game objects are under the `/game/` prefix, preserving their names:

   ```text
   https://rabbids-go-home-assets.jmhq20120212.workers.dev/game/sys/boot.bin
   https://rabbids-go-home-assets.jmhq20120212.workers.dev/game/sys/fst.bin
   https://rabbids-go-home-assets.jmhq20120212.workers.dev/game/sys/main.dol
   https://rabbids-go-home-assets.jmhq20120212.workers.dev/game/files/RGH.BF.part00
   ```

   The published `web/game-config.json` is set to:

   ```json
   { "assetBase": "https://rabbids-go-home-assets.jmhq20120212.workers.dev/" }
   ```

   This configuration is committed and deployed with GitHub Pages. The browser
   reads the remote game files using byte ranges, with no folder selection.

## Object storage requirements

The dedicated Cloudflare bucket `rabbids-go-home-web` has been created and
tested with synthetic objects. See [CLOUDFLARE-R2.md](CLOUDFLARE-R2.md) for
its configured CORS policy, development URL, two resumable upload methods,
and the deployed `rabbids-go-home-assets` Cloudflare Worker that serves the
R2 files to GitHub Pages. All 205 game objects were uploaded and the CDN
is enabled in `web/game-config.json`. The browser loads them automatically.

Cloudflare R2 with a custom public domain is one compatible option. Set bucket CORS
to allow `GET`, `HEAD`, and the request header `Range` from
`https://teaserlua.github.io` and expose `Content-Length`, `Content-Range`,
and `Accept-Ranges` response headers. Verify these responses in the browser:

* `HEAD /game/sys/boot.bin` returns 200 with a readable `Content-Length`.
* `GET /game/sys/boot.bin`, with `Range: bytes=0-31`, returns HTTP 206 and
  `Content-Range: bytes 0-31/<total>`; returned bytes must match the local file.
* Assets must be fetched with CORS and accessible from a cross-origin isolated
  document (COOP `same-origin`, COEP `require-corp`).

Do not use GitHub Pages for the full disc; its site size limit is 1 GB. GitHub
Releases have a separate 2 GiB-per-asset limit, but release-download URLs are
not a tested CORS-enabled random-access asset CDN. Git LFS is not a solution
to serving game data through GitHub Pages.

## Static-host limitations

The original `serve.py` provides a local `/dsu/` UDP bridge, `/log` file logging,
and a `/nand` backup endpoint. These endpoints cannot run on GitHub Pages.
The published site keeps saves in the browser's IndexedDB; DSU Remote requires
using the local `serve.py` launcher. Keyboard and gamepad controls are supported.

Game content and executable-derived binaries may be protected by Ubisoft's
copyright. Only make these public if you have the rights to distribute them.
