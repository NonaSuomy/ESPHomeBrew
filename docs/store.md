# The store

When **Build PAPPs** succeeds on `main`, **Publish store** (`.github/workflows/publish-store.yml`) does two things:

1. **Releases.** Each app version gets one GitHub Release: tag `<name>-v<version>`, asset `<name>-<version>.papp`, with its sha256 in the release notes. A version already published with the same binary is skipped. If the binary changed but the version didn't, the job fails. Bump `version` in `apps/<name>/papp.json` to publish a new build.
2. **Catalog.** `tools/make_catalog.py` writes `index.html` and `store.json` and deploys them to GitHub Pages: **https://nonasuomy.github.io/esphomebrew/**
   Each `.papp` is also copied into the Pages site, and the catalog links to that copy. Devices download from GitHub Pages (Let's Encrypt, ISRG Root X1), which the ESP-IDF certificate bundle verifies. `github.com` release downloads chain to Sectigo's newer ECC root and fail verification on-device (`esp-x509-crt-bundle: Failed to verify certificate`, seen with ESP-IDF 6.1). The Release stays the versioned archive (`release_url` in `store.json`).

## How the device reads it

The ESPHome `papp_loader` fetches `catalog_url` (HTTPS, up to 5 redirects, at most 64 KB). Every `href` ending in `.papp` becomes a button in the LVGL list, labelled with the file name, for example `lvgl-0.3.0.papp`. The version is in the name, so a new release shows up as a new label after a refresh. Tapping a button streams the copy on GitHub Pages into PSRAM. Nothing is cached on the device.

```yaml
papp_loader:
  catalog_url: https://nonasuomy.github.io/esphomebrew/
```

`store.json` has the same list with title, size, sha256 and pinned source, for tools and people.

## App data (game files)

Some apps need files on the card before they run: Doom needs an IWAD, Quake needs `id1/pak0.pak`. Such an app lists them under `data` in its `papp.json` (see [building.md](building.md#app-data)). Only files that may be redistributed are listed. Today that means the shareware `doom1.wad`, PrBoom's `prboom.wad`, the Quake shareware `pak0.pak` and the Duke Nukem 3D shareware `duke3d.grp`. Commercial game data and save files are never published.

Publish store downloads each file from its source repository at the pinned commit. It checks the size and sha256 (and fails on any mismatch), then publishes the file on Pages under `data/<app>/<target>`. The Python tools CI does the same download and check on every pull request that touches `apps/`.

Each app with data also gets a plain-text list next to its `.papp`: `doom-0.2.1.papp` gets `doom-0.2.1.files`.

```
# papp-data 1
# doom 0.2.1: <why these files may be redistributed>
4196020 1d7d43be…c771 roms/doom/doom1.wad https://nonasuomy.github.io/esphomebrew/data/doom/roms/doom/doom1.wad
143312 b4dd3642…1d1d roms/doom/prboom.wad https://nonasuomy.github.io/esphomebrew/data/doom/roms/doom/prboom.wad
```

Each line has four fields: size, sha256, the target path under the device's data root, and the URL. Lines starting with `#` are comments. The loader reads this list before it launches the app and downloads whatever the card is missing. `store.json` carries the same information under each app's `data`.

The apps currently open their files at fixed `/sd/roms/<game>/…` paths, so the data root has to be `/sd` for them to find it.

## One-time repository setup (host)

- **Settings → Pages → Build and deployment → Source: GitHub Actions.**
- To publish by hand: **Actions → Publish store → Run workflow**. Optionally give it a Build PAPPs run id.
