## Summary

<!-- What does this PR do and why? -->

## Feature area

- [ ] Engine patch (`patches/`)
- [ ] New component (`overlay/`)
- [ ] Chrome UI
- [ ] Downloader
- [ ] Infra / CI / build scripts
- [ ] Docs

## Testing done

<!-- What did you run, and where (local machine / self-hosted runner)? -->

## Checklist

- [ ] `scripts/apply-patches.ps1` applies all patches cleanly against the pinned `chromium.version`
- [ ] `scripts/build.ps1` (`autoninja`) succeeds on the self-hosted runner
- [ ] New patches are named `NNNN-description.patch` and ordered correctly
- [ ] New standalone files are under `overlay/`, not hand-merged into `patches/`
- [ ] Roadmap issue linked, if this addresses one
