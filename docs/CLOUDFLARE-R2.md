# Cloudflare R2 for the browser build

The project has a dedicated Cloudflare R2 Standard bucket called
`rabbids-go-home-web` (created Oct 11, 2026). Its CORS policy permits the
GitHub Pages origin to issue `HEAD` and `GET Range` requests.

**Verified:** the public development R2 URL
`https://pub-07eddd2929d4451986469ff38f9b85f4.r2.dev/` responded correctly
to `HEAD` and `Range` requests for synthetic 256-byte fixture files. Chrome
successfully fetched byte ranges across origins from the deployed GitHub Pages
site while `crossOriginIsolated` remained enabled. The tests were NOT run with
the original game files on R2.

Cloudflare limits `.r2.dev` usage to development. For reliable public hosting,
connect a custom domain to the bucket in **R2 > rabbids-go-home-web > Settings >
Custom Domains**. Update `web/game-config.json` with the chosen public domain
*only after the files have been uploaded and their HTTP checks pass*.

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
after splitting. The full directory must never be placed in the Git repository.
The copyrighted disc content is not provided in this repository or in this
deployment; publication requires permission from its rights holder.
