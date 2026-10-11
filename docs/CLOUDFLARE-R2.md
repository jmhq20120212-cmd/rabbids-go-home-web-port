# Cloudflare R2 for the browser build

The project has a dedicated Cloudflare R2 Standard bucket called
`rabbids-go-home-web` (created Oct 11, 2026). Its CORS policy permits the
GitHub Pages origin to issue `HEAD` and `GET Range` requests.

The project also deploys a Cloudflare Worker (`cloudflare/rabbids-assets/`) at
`https://rabbids-go-home-assets.jmhq20120212.workers.dev/`. The Worker has an
R2 binding to the dedicated bucket and supports `GET`, `HEAD`, `Range`, and CORS.
The Worker hostname belongs to the Cloudflare account and does not change when
the GitHub username changes. The browser page is at
`https://teaserlua.github.io/rabbids-go-home-web-port/`.
The Worker endpoint was validated with real Chrome requests from GitHub Pages.
The source is in this repository; redeploy it with:

```powershell
npm exec --yes --package=wrangler -- wrangler deploy --config cloudflare/rabbids-assets/wrangler.jsonc
```

This `workers.dev` endpoint avoids the public-development rate limits of
`r2.dev`. For larger public traffic, Cloudflare Workers plan request limits
and R2 operation costs still apply.

**Verified:** the public development R2 URL
`https://pub-07eddd2929d4451986469ff38f9b85f4.r2.dev/` responded correctly
to `HEAD` and `Range` requests for synthetic 256-byte fixture files. Chrome
successfully fetched byte ranges across origins from the deployed GitHub Pages
site while `crossOriginIsolated` remained enabled. The tests were NOT run with
the original game files on R2.

Cloudflare limits `.r2.dev` usage to development. A custom domain can also
be connected to the bucket in **R2 > rabbids-go-home-web > Settings > Custom
Domains**. Use the Worker endpoint above unless a dedicated custom domain
is available. Configure `web/game-config.json` *only after the files have been
uploaded and their HTTP checks pass*.

## Uploading content you are permitted to redistribute

Wrangler OAuth authentication was already set up on the author's machine; this
avoids creating S3 access keys. The repository includes two upload methods:

* `tools/r2-wrangler-upload.mjs`: uses the Wrangler login and automatically
  divides files bigger than 280 MiB into `*.part00`, `*.part01`, etc. The game
  runtime supports this layout. All parts are 128 MiB or less; R2's single
  Wrangler upload limit is 315 MB. Uses a local state file for `--resume`.
* `tools/r2-upload.mjs`: alternate S3 multipart uploader using an R2-scoped
  access key in environment variables and the AWS SDK; also supports resumable
  multipart uploads. See comments at the start of the script.

The first method can be prepared and dry-run locally as follows:

```powershell
npm install --no-save --no-package-lock wrangler
node tools/r2-wrangler-upload.mjs --source "C:\path\to\entire partition" --bucket rabbids-go-home-web
```

After confirming the rights to publish **all** content, append
`--execute --confirm-rights --resume --public-base https://YOUR-PUBLIC-R2-DOMAIN/`
to perform a real upload. The uploader's local state is stored outside the
repository and avoids resending completed objects. Data, login tokens, save
files, and temporary upload parts should not be committed.

## Validating public delivery

Apply and inspect the policy:

```powershell
npm exec --yes --package=wrangler -- wrangler r2 bucket cors set rabbids-go-home-web --file tools/r2-cors.json
npm exec --yes --package=wrangler -- wrangler r2 bucket cors list rabbids-go-home-web
```

After authorized uploads, run:

```powershell
node tools/check-r2.mjs --base https://YOUR-PUBLIC-R2-DOMAIN/
```

Then place this JSON in the untracked `web/game-config.json`:

```json
{ "assetBase": "https://YOUR-PUBLIC-R2-DOMAIN/" }
```

Commit this non-secret configuration only when every required file is online.
The browser will then load the data directly using CORS-enabled byte ranges.
The local folder picker remains available as an alternative.

The extracted partition is approximately **1.946 GiB** across 205 objects
after splitting. All 205 objects were uploaded on October 11, 2026. The
`tools/verify-r2.mjs` check compared byte ranges of 39 objects (including
every large part and the boot files) with the originals, with 39 passes.
The full directory is stored in R2 and is not committed to the Git repository.
The author confirmed permission to publish the game contents.
