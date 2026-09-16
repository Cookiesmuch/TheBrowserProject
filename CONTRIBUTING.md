# Contributing to TheBrowserProject

TheBrowserProject is a **Chromium source fork** (not CEF, not a from-scratch
engine). We never commit Chromium source to this repo — instead we pin an
upstream revision (`chromium.version`) and layer our changes on top as a
`patches/` series plus an `overlay/` of wholesale new files, applied to a
fresh checkout by our own scripts. This mirrors how Brave, Vivaldi, and
ungoogled-chromium structure their forks.

## Repo layout

- `chromium.version` — the pinned upstream Chromium tag we build against.
- `patches/` — ordered `NNNN-description.patch` files, `git apply`'d in
  lexical order onto the pinned checkout.
- `overlay/` — new files (whole new components) copied verbatim into the
  checkout *before* patches are applied. Mirrors the `chromium/src` tree
  layout, e.g. `overlay/chrome/browser/ui/tbp_sidebar/`.
- `scripts/` — PowerShell automation (see below).
- `args.gn.template` — baseline GN build args.

## Local build

Prerequisites:
- Windows 11
- ~100GB free disk (Chromium checkout + build output)
- 16GB+ RAM minimum, 32GB+ recommended
- Visual Studio 2022 Build Tools + Windows SDK (see Chromium's own
  [Windows build instructions](https://chromium.googlesource.com/chromium/src/+/main/docs/windows_build_instructions.md)
  for exact component list)
- Git, PowerShell 7+

```powershell
# One-time (or whenever chromium.version changes): installs depot_tools,
# syncs the pinned revision. First run is ~100GB and can take hours.
.\scripts\bootstrap.ps1

# Every time you change patches/ or overlay/:
.\scripts\apply-patches.ps1

# Build:
.\scripts\build.ps1
```

The resulting binary is at `chromium/src/out/Release/chrome.exe`.

## Adding or updating a patch

1. Make your change directly in the working checkout under `chromium/src`.
2. If it's a **wholesale new file/component**, put it under `overlay/`
   instead (mirroring its path under `chromium/src`) — don't hand-edit it
   into a patch.
3. If it's a **modification to an existing Chromium file**, run:
   ```powershell
   .\scripts\export-patches.ps1 -PatchName "short-description"
   ```
   This writes `patches/NNNN-short-description.patch` from your working
   diff (auto-numbered after the last existing patch).
4. Re-run `.\scripts\apply-patches.ps1` against a **clean** checkout to
   confirm the patch applies cleanly before pushing.

## Re-syncing against a new upstream revision

1. Bump `chromium.version` to the new tag.
2. Delete (or move aside) `chromium/` and re-run `.\scripts\bootstrap.ps1`
   to sync fresh.
3. Run `.\scripts\apply-patches.ps1` — fix any patch that no longer applies
   cleanly (Chromium's own file may have moved/changed upstream).
4. Run `.\scripts\build.ps1` and confirm it still builds and launches.

## Required checks before pushing

- `scripts/apply-patches.ps1` applies every patch cleanly.
- `scripts/build.ps1` succeeds.
- New patches are numbered and ordered correctly; new standalone files live
  under `overlay/`, not squeezed into a patch.

CI (`.github/workflows/ci.yml`) re-checks all of this on our self-hosted
runner on every push and PR.

## Self-hosted runner setup

Our CI needs to actually run a Chromium build, which standard GitHub-hosted
runners (~14GB disk, 7GB RAM) cannot do. We run our own Windows runner
instead. To register a machine as a runner:

1. On GitHub: go to the repo → **Settings → Actions → Runners → New
   self-hosted runner**.
2. Select **Windows** / **x64**.
3. Follow the download + configure commands GitHub shows you, e.g.:
   ```powershell
   mkdir actions-runner ; cd actions-runner
   Invoke-WebRequest -Uri <download-url-from-github> -OutFile actions-runner.zip
   Expand-Archive -Path actions-runner.zip -DestinationPath .
   ./config.cmd --url https://github.com/Cookiesmuch/TheBrowserProject --token <token-from-github>
   ```
   When prompted for runner labels, make sure to include: `windows`,
   `chromium-build` (our workflows target
   `runs-on: [self-hosted, windows, chromium-build]`).
4. Install it as a Windows service so it survives reboots and keeps
   listening for jobs:
   ```powershell
   ./svc install
   ./svc start
   ```
5. Confirm it shows as **Idle** under Settings → Actions → Runners.
6. Make sure the machine has depot_tools' prerequisites available (Visual
   Studio 2022 Build Tools + Windows SDK) and ~100GB free disk — the same
   requirements as a local build, above.

Once registered, `ci.yml` / `beta.yml` / `release.yml` will start picking up
jobs automatically.

## Repo settings (if not already configured)

If branch protection on `main` isn't already set via the GitHub API, enable
manually under **Settings → Branches → Branch protection rules** for `main`:
- Require a pull request before merging
- Require status checks to pass before merging — require the `ci` check
- Block force pushes
- Do not allow deletion of the branch
