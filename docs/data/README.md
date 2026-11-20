# Compatibility database mirror

`compatibility.json` in this directory is a mirror of the public KytyPS5
compatibility database, served from this repository through GitHub Pages so
EmuStation5 does not depend on upstream KytyPS5 infrastructure at runtime.

- Upstream source: <https://kytyps5.github.io/data/compatibility.json>
- Public mirror URL: <https://hugoe031.github.io/EmuStation5/data/compatibility.json>
- Served via GitHub Pages from the `main` branch, `/docs` folder.

To refresh the mirror, download the upstream JSON and replace
`docs/data/compatibility.json`, then commit and push. The launcher downloads
the file on startup and caches a copy under its settings directory, so a new
release is not required to pick up data updates.
