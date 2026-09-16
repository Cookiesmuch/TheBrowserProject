# TheBrowserProject

A Windows-native, extreme-performance browser, built as a genuine
**Chromium source fork** — using every available Windows API to push
further than a stock browser can.

## Vision

The long-term goal is a browser that goes well beyond a themed Chromium:

- **Per-page VPN egress** — route individual tabs/pages through different
  VPN endpoints.
- **An FDM-crushing download manager** — multi-connection, resumable
  downloads, with native torrent support planned.
- **A Steam-overlay-style command bar** replacing the omnibox, plus a
  floating tab sidebar summoned with **Win+T**, instead of a fixed tab
  strip.
- **NPU-driven command parsing** — local, on-device NLU for the command bar
  via ONNX, not just string matching.
- **Quad-MFX video decode** — splitting hardware video decode across
  multiple Intel Media Fixed Function engines for higher throughput.
- **Full DRM/HDR pipeline** support.
- **Tiered 50GB RAM caching** for aggressive page/asset caching beyond what
  Chromium does by default.
- **IoRing/BypassIO storage** — modern Windows async I/O APIs for the cache
  and download paths.
- **Game-mode core throttling** — yielding CPU cores to foreground games
  while the browser runs in the background.

This is multi-year scope. The table below tracks what's actually real vs.
still a roadmap issue.

| Feature | Status |
|---|---|
| Chromium fork + patch/overlay build pipeline | ✅ Milestone 0 |
| Floating tab shell | 🚧 Milestone 1 |
| Command bar (basic verbs) | 🚧 Milestone 1 |
| Win+T floating sidebar | 🚧 Milestone 1 |
| Multi-connection HTTP downloader (stub) | 🚧 Milestone 1 |
| Native torrent support | 📋 Roadmap |
| Per-page VPN egress | 📋 Roadmap |
| NPU/ONNX command parsing | 📋 Roadmap |
| Quad-MFX video decode | 📋 Roadmap |
| DRM/HDR pipeline | 📋 Roadmap |
| Tiered 50GB RAM cache | 📋 Roadmap |
| IoRing/BypassIO storage | 📋 Roadmap |
| Game-mode core throttling | 📋 Roadmap |

See the [issue tracker](https://github.com/Cookiesmuch/TheBrowserProject/issues)
for the full breakdown of roadmap items.

## Architecture: why a Chromium fork

We looked at three approaches:

- **CEF (Chromium Embedded Framework)**, even self-compiled, wraps
  Chromium's `content` layer for *embedding* a web view inside a
  non-browser app (what Steam/Spotify do). It deliberately excludes the
  `//chrome` browser-UI layer, extension infrastructure, and the
  engine-level (Blink/V8/media) patch surface this project actually needs.
  Building CEF from source also requires the same `gclient sync` (~100GB)
  and GN/Ninja build as a raw fork — it saves nothing on infrastructure.
- **A from-scratch engine (e.g. Rust-based)** would take years before
  matching baseline web compatibility.
- **A genuine Chromium source fork** is what real full-featured
  Chromium-based browsers (Vivaldi, Brave, Arc, Edge) actually are. It's
  the only path that gets both full engine-level patch access and a
  functioning browser from day one.

We don't commit Chromium source into this repo (~100GB, and it's Google's
own history). Instead, following the model Brave/Vivaldi/
ungoogled-chromium use:

- `chromium.version` pins an upstream Chromium tag.
- `patches/` holds an ordered series of `*.patch` files applied to a fresh
  checkout.
- `overlay/` holds wholesale new files (new `chrome/browser/...`
  components) copied into the checkout before patches apply.
- `scripts/` automates bootstrap → apply → build on a self-hosted Windows
  CI runner (standard GitHub-hosted runners, at ~14GB disk / 7GB RAM,
  cannot do this build at all).

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full patch/overlay workflow.

## Developer setup

- Windows 11
- ~100GB free disk
- 16GB+ RAM minimum, 32GB+ recommended

Everything else — Visual Studio 2022 + the C++ workload, the exact Windows
SDK version this Chromium revision needs, Windows long-path support, and a
Defender exclusion for the checkout — is handled for you:

```powershell
.\scripts\setup.ps1
```

This is an interactive wizard (relaunches itself elevated if needed, asks
before anything slow/expensive like installing VS or starting the ~100GB
sync) that walks through prerequisites, sync, patches, and the build, then
smoke-tests the result. Safe to re-run — every step checks current state
and skips what's already done. Pass `-SkipBuild` to stop after sync/patches
without committing to the multi-hour compile yet.

Once set up, day-to-day you only need the individual steps directly:

```powershell
.\scripts\bootstrap.ps1       # re-sync after bumping chromium.version
.\scripts\apply-patches.ps1   # re-copy overlay/, re-apply patches/ after editing them
.\scripts\build.ps1           # gn gen + autoninja
```

Binary lands at `chromium/src/out/Release/TheBrowserProject.exe`.

## Getting a build to test

Every push to a PR branch triggers `beta.yml`, which builds and refreshes a
rolling prerelease on the
[Releases page](https://github.com/Cookiesmuch/TheBrowserProject/releases) —
that's the primary way to grab a runnable build without building it
yourself.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the patch/overlay workflow, local
build instructions, and self-hosted runner setup.
