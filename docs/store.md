# The store

When **Build PAPPs** succeeds on `main`, **Publish store** (`.github/workflows/publish-store.yml`) does two things:

1. **Releases.** Each app version gets one GitHub Release: tag `<name>-v<version>`, asset `<name>-<version>.papp`, with its sha256 in the release notes. A version already published with the same binary is skipped. If the binary changed but the version didn't, the job fails. Bump `version` in `apps/<name>/papp.json` to publish a new build.
2. **Catalog.** `tools/make_catalog.py` writes `index.html` and `store.json` and deploys them to GitHub Pages: **https://nonasuomy.github.io/papp-conversions/**
   Each `.papp` is also copied into the Pages site, and the catalog links to that copy. Devices download from GitHub Pages (Let's Encrypt, ISRG Root X1), which the ESP-IDF certificate bundle verifies. `github.com` release downloads chain to Sectigo's newer ECC root and fail verification on-device (`esp-x509-crt-bundle: Failed to verify certificate`, seen with ESP-IDF 6.1). The Release stays the versioned archive (`release_url` in `store.json`).

## How the device reads it

The ESPHome `papp_loader` fetches `catalog_url` (HTTPS, up to 5 redirects, at most 64 KB). Every `href` ending in `.papp` becomes a button in the LVGL list, labelled with the file name, for example `psram_lvgl-0.1.0.papp`. The version is in the name, so a new release shows up as a new label after a refresh. Tapping a button streams the copy on GitHub Pages into PSRAM. Nothing is cached on the device.

```yaml
papp_loader:
  catalog_url: https://nonasuomy.github.io/papp-conversions/
```

`store.json` has the same list with title, size, sha256 and pinned source, for tools and people.

## One-time repository setup (host)

- **Settings → Pages → Build and deployment → Source: GitHub Actions.**
- To publish by hand: **Actions → Publish store → Run workflow**. Optionally give it a Build PAPPs run id.
