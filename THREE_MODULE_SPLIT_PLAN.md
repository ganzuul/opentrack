# Three-Module Split Plan (Temporary)

Status: active
Owner: maintainers
Created: 2026-03-08
Remove when: all module checklists below are complete and merged

## Purpose

Track separation and delivery of three currently floating modules so each can be reviewed, tested, and merged independently:

1. OpenHMD tracker (`tracker-openhmd`)
2. Fusion tracker (`tracker-fusion`)
3. Alpha Spectrum filter (`filter-alpha-spectrum`)

This file is intentionally temporary and should be deleted once all three modules are complete.

## Operating Rules

1. Keep each module in its own PR scope.
2. Avoid cross-module logic in a single commit unless required for compile/link.
3. If a cross-module change is required, put it in a tiny prep commit and reference it from all related PRs.
4. Prefer feature toggles and safe defaults when hardware coverage is limited.
5. Preserve Linux build friendliness while validating changes.

## Branch/PR Strategy

Use either separate branches from `master` or a stacked model.

Suggested branch names:

- `topic/openhmd-dk1-validation`
- `topic/fusion-highrate-robustness`
- `topic/filter-alpha-spectrum-mtm`

Suggested PR titles:

- `tracker-openhmd: quaternion/axis fixes and DK1 validation notes`
- `tracker-fusion: high-rate integration hardening and buffer safety`
- `filter-alpha-spectrum: MTM temporal debt + commutator stabilization`

## Pre-Split Hygiene (Run Before Creating Module Branches)

Goal: ensure the branch is current with upstream and that commits include only intended module files.

1. Sync with upstream
- `git fetch origin`
- `git checkout master`
- `git rebase origin/master`

2. Verify local diff before split
- `git status --short`
- `git diff --name-only`

3. Park temporary/unrelated changes
- `git stash push -m "temp: unrelated changes" -- <path1> <path2> ...`

4. Create module branch from updated `master`
- `git checkout -b topic/<module-name>`

5. Commit only module-owned files
- `git add <module paths only>`
- `git commit -m "<module scoped message>"`

6. Confirm commit scope
- `git show --name-only --stat HEAD`

7. Repeat per module (do not reuse mixed branches)

If `git status --short` shows files outside intended module scope, do not commit until they are either stashed, dropped, or moved to the correct branch.

## File Mapping (Planned Ownership)

OpenHMD tracker PR should contain only OpenHMD-specific changes, for example:

- `tracker-openhmd/**`
- docs specific to OpenHMD behavior

Fusion PR should contain only fusion-specific changes, for example:

- `tracker-fusion/**`
- fusion UI/settings text and behavior

Alpha Spectrum PR should contain only filter-specific changes, for example:

- `filter-alpha-spectrum/**`

Cross-cutting files are allowed only if strictly required for build integration and should be minimal.

## Validation Matrix

Given available hardware is Rift DK1 only, validate with layered evidence:

1. Hardware validation (DK1)
- Basic motion sanity (yaw/pitch/roll direction and magnitude)
- Stability during rapid head movement
- Recovery after temporary tracking disturbance

2. Replay/trace validation
- Record representative sample streams
- Replay before/after changes
- Compare drift, jitter, and lag characteristics

3. Build validation
- Linux configure/build succeeds
- Module target `.so` artifacts produced
- No unrelated module regressions introduced

4. Runtime safety
- Feature toggles/defaults do not break existing baseline behavior
- Fallback behavior remains valid when high-rate or specific tracker path is unavailable

## Module Checklists

### 1) OpenHMD Tracker

- [ ] Scope isolated to `tracker-openhmd` and required tiny integration edits only
- [ ] Quaternion component ordering and axis conventions documented
- [ ] DK1 hardware sanity test completed
- [ ] Replay or logged-trace comparison captured
- [ ] PR includes explicit hardware limitation note
- [ ] PR merged

### 2) Fusion Tracker

- [ ] Scope isolated to `tracker-fusion` and required tiny integration edits only
- [ ] High-rate polling/buffer behavior validated
- [ ] Edge-case handling verified (empty samples, overflow, stale data)
- [ ] Settings/UI behavior validated
- [ ] PR merged

### 3) Alpha Spectrum Filter

- [ ] Scope isolated to `filter-alpha-spectrum` and required tiny integration edits only
- [ ] MTM transition debt logic validated
- [ ] Depth commutator lift path validated
- [ ] Stabilization/amortization behavior validated
- [ ] Linux build for module target verified
- [ ] PR merged

## Merge Order Guidance

Default suggested order (adjust if dependencies require):

1. Fusion (infrastructure/telemetry stability)
2. OpenHMD (tracker-specific behavior)
3. Alpha Spectrum (filter behavior)

If Alpha Spectrum is fully independent, it can merge earlier.

## Completion Criteria (Delete This File)

Delete `THREE_MODULE_SPLIT_PLAN.md` when all are true:

1. OpenHMD PR merged
2. Fusion PR merged
3. Alpha Spectrum PR merged
4. No remaining temporary TODOs tied to this split effort

After deletion, reference completion in the final PR description or release notes.
