# Alpha Spectrum Filter — Refactor Plan

> Reference: `ftnoir_filter_alpha_spectrum.cpp`, `ftnoir_filter_alpha_spectrum.h`,
> `alpha-spectrum-governance.hpp`
>
> Check off each task `[x]` as it is completed. Phases must be validated (build + smoke test)
> before moving to the next one.

---

## Problems Being Solved

The filter implementation has accumulated several categories of structural mess:

| Category | Symptom |
|---|---|
| **Namespace verbosity** | `detail::alpha_spectrum::governance::index(detail::alpha_spectrum::governance::edge_for_head(...))` repeated inline 6+ times |
| **Flat `_sum`/`_avg` proliferation** | ~32 identically-structured `double rot_X_sum = 0.0;` + `const double rot_X_avg = rot_X_sum / 3.0;` pairs, not factored at all |
| **Flat drive-channel variables** | 7 heads × 2 sides = 14 individual `rot_X_drive_sum` / `pos_X_drive_sum` locals, instead of an array |
| **Repeated `edge_valid` assignment** | 6 copy-paste `status.edge_valid[edge_for_head(X)].store(is_valid_if_enabled(X, head_enabled_X))` blocks |
| **Inline weight tables** | `conflict_weight` and `pathology_weight` computed via chains of bare `if (i == N) weight = ...` — should be `constexpr` arrays |
| **Flat `status.*.store()` publish block** | ~54 individual named-field stores with no structure; rot/pos symmetry not exploited |
| **`calibration_status` struct is unstructured** | Flat atomic fields for rot/pos pairs; drive channels not in arrays |
| **Governance logic leaking into filter** | The filter knows about `governance::index`, `edge_for_head`, `governed_edge` internals |

---

## Phase 0 — Baseline & Tooling

- [ ] Confirm clean build from scratch: `cmake --build build -j$(nproc)`
- [ ] Record current `.cpp` line count and binary size for regression reference
- [ ] Run filter under opentrack to confirm baseline behaviour; note any existing diagnostics output
- [ ] Tag git state before any changes: `git stash` / commit or branch as `pre-refactor`

---

## Phase 1 — Namespace Alias & Governance API Housekeeping

**Goal:** Remove all `detail::alpha_spectrum::governance::` long-hand spellings from `filter()`.
The filter should call a clean API; the governance header is the single source of truth.

### 1a — Add convenience alias at top of `.cpp`

```cpp
namespace gov = detail::alpha_spectrum::governance;
namespace as  = detail::alpha_spectrum;
```

Replace all occurrences of `detail::alpha_spectrum::governance::` and `detail::alpha_spectrum::` in
function bodies with the aliases.

### 1b — Add `head_edge_index()` helper to governance header

```cpp
// In governance namespace:
constexpr size_t head_edge_index(tracking_head h) {
    return index(edge_for_head(h));
}
```

So call sites become: `status.edge_valid[gov::head_edge_index(h)].store(...)`

### 1c — Add `constexpr` list of all heads in declaration order

```cpp
// In alpha-spectrum-governance.hpp, outside governance sub-namespace:
inline constexpr std::array<tracking_head, tracking_head_count> all_heads = {
    tracking_head::ema, tracking_head::brownian, tracking_head::adaptive,
    tracking_head::predictive, tracking_head::chi_square, tracking_head::pareto
};
```

### Acceptance Criteria
- [ ] No bare `detail::alpha_spectrum::governance::` in any function body
- [ ] No bare `detail::alpha_spectrum::` in function bodies (aliases or unqualified names only)
- [ ] Build passes; no behaviour change

---

## Phase 2 — Collapse Drive Accumulators into Arrays

**Goal:** Replace 14 individual `double rot_X_drive_sum / pos_X_drive_sum` locals with two
`std::array<double, tracking_head_count>` accumulators indexed by `tracking_head`.

### 2a — Replace per-head drive sum declarations

Old form (7 × 2 = 14 variables):
```cpp
double rot_ema_drive_sum = 0.0;
double rot_brownian_drive_sum = 0.0;
// … 5 more …
double pos_ema_drive_sum = 0.0;
// …
```

New form:
```cpp
std::array<double, as::tracking_head_count> rot_drive_sum{};
std::array<double, as::tracking_head_count> pos_drive_sum{};
```

### 2b — Replace all accumulation sites inside the per-axis loop

Old: `rot_ema_drive_sum += ...;`
New: `rot_drive_sum[as::index(as::tracking_head::ema)] += ...;`

Or with a local alias inside the loop:
```cpp
auto& rot_drive = rot_drive_sum;
auto& pos_drive = pos_drive_sum;
```

### 2c — Replace the 14 `_avg` local declarations with a single averaging step

```cpp
std::array<double, as::tracking_head_count> rot_drive_avg, pos_drive_avg;
for (size_t h = 0; h < as::tracking_head_count; ++h) {
    rot_drive_avg[h] = rot_drive_sum[h] / 3.0;
    pos_drive_avg[h] = pos_drive_sum[h] / 3.0;
}
```

### 2d — Replace the 14 `status.rot/pos_X_drive.store()` calls in the publish block

Requires a corresponding change to `calibration_status` (see Phase 5). For now, keep individual
named fields but populate from the array:
```cpp
status.rot_ema_drive.store(rot_drive_avg[as::index(as::tracking_head::ema)], ...);
// etc. — still named at status level, but sourced from array
```

### Acceptance Criteria
- [ ] `rot_ema_drive_sum` … `pos_mtm_drive_sum` local variables are gone
- [ ] Drive accumulation inside the per-axis loop uses the array form
- [ ] The 14 `_avg` temporaries after the loop are gone (or at least collapsed)
- [ ] Build passes; no behaviour change

---

## Phase 3 — Collapse Remaining `_sum`/`_avg` Pairs

**Goal:** All remaining `rot_X_sum` / `pos_X_sum` / `_avg` pairs are either moved into arrays or
structured into a small local aggregate.

The remaining channels (after Phase 2 extracts the drive channels):

| Sum variable | Avg name | Description |
|---|---|---|
| `rot_brownian_raw_sum` | `rot_brownian_raw_avg` | Brownian energy (raw) |
| `rot_brownian_filtered_sum` | `rot_brownian_filtered_avg` | Brownian energy (filtered) |
| `rot_predictive_error_sum` | `rot_predictive_error_avg` | Predictive error |
| `pos_brownian_raw_sum` | `pos_brownian_raw_avg` | " (pos) |
| `pos_brownian_filtered_sum` | `pos_brownian_filtered_avg` | " (pos) |
| `pos_predictive_error_sum` | `pos_predictive_error_avg` | " (pos) |
| `rot_jitter_sum` | (inline at store, `/3.0`) | Jitter |
| `pos_jitter_sum` | (inline at store, `/3.0`) | " (pos) |
| `rot_objective_sum` | `rot_objective_avg` | Innovation objective |
| `pos_objective_sum` | `pos_objective_avg` | " (pos) |
| `rot_neck_weight_error_sum` | `rot_neck_weight_error_avg` | Neck coupling error |
| `pos_neck_weight_error_sum` | `pos_neck_weight_error_avg` | " (pos) |
| `rot_neck_invalid_ratio_sum` | `rot_neck_invalid_ratio_avg` | Invalid sample ratio |
| `pos_neck_invalid_ratio_sum` | `pos_neck_invalid_ratio_avg` | " (pos) |
| `invariant_correction_magnitude_sum` | `invariant_correction_magnitude` | Invariant correction |
| `pos_predictive_translation_error_sum` | `pos_predictive_translation_error_avg` | Translational error |
| `pos_residual_sum` | `pos_residual_avg` | Anomaly residual |
| `pos_head_transition_count` | `pos_head_transition_ratio` | Head transitions |

### 3a — Introduce a `per_axis_sums` local struct (or named aggregate)

```cpp
struct axis_averages {
    double brownian_raw, brownian_filtered, predictive_error;
    double jitter, objective;
    double neck_weight_error, neck_invalid_ratio;
};
axis_averages rot_avg{}, pos_avg{};
```

All per-axis accumulation inside the loop writes into `rot_avg.X_sum` (or use a parallel sums
struct); after the loop, a single averaging pass fills each avg from sum/3.0.

### 3b — Eliminate the block of 16–18 `const double X_avg = X_sum / 3.0;` lines after the loop

Replace with the structured averaging pass from 3a.

### 3c — Update all downstream usages that reference the old named `_avg` variables

The publish block and MTM update block reference these; redirect to `rot_avg.X` / `pos_avg.X`.

### Acceptance Criteria
- [ ] All `_sum` / `_avg` temporaries replaced by the new structs or arrays
- [ ] The block of ~18 `const double X_avg = X_sum / 3.0;` lines is gone
- [ ] Build passes; no behaviour change

---

## Phase 4 — Replace Head `edge_valid` Copy-Paste with a Loop

**Goal:** Eliminate the 6 copy-paste `status.edge_valid[...].store(is_valid_if_enabled(...))` blocks.

### 4a — Build the `head → enabled` lookup inline

```cpp
const std::array<bool, as::tracking_head_count> head_enabled = {
    ema_enabled, brownian_enabled, adaptive_mode,
    predictive_enabled, chi_square_enabled, pareto_enabled
};
```

### 4b — Replace the 6-block expansion with a loop

```cpp
for (size_t h = 0; h < as::tracking_head_count; ++h) {
    const auto head = static_cast<as::tracking_head>(h);
    status.edge_valid[gov::head_edge_index(head)].store(
        is_valid_if_enabled(head, head_enabled[h]),
        std::memory_order_relaxed);
}
```

### 4c — Remove the individual `head_enabled_ema` … `head_enabled_pareto` booleans

They are replaced by the array declared in 4a.

### Acceptance Criteria
- [ ] Six `status.edge_valid[...].store(...)` call blocks reduced to one loop
- [ ] `head_enabled_ema` … `head_enabled_pareto` local variables are gone
- [ ] Build passes; no behaviour change

---

## Phase 5 — Promote Bin Weight Tables to `constexpr` Arrays

**Goal:** Replace the inline `if (i == N) weight = ...` chains in the per-bin loop with
`constexpr` weight arrays declared in the governance header.

### 5a — Add `constexpr` bin weight arrays to `alpha-spectrum-governance.hpp`

```cpp
namespace governance {

// Conflict sensitivity weight per semantic bin (B1–B4, T1–T4, P1–P4)
inline constexpr std::array<double, 12> bin_conflict_weights = {
    0.05,  // B1
    0.55,  // B2 plausibility gate
    0.05,  // B3
    0.05,  // B4 recovery wall
    0.05,  // T1
    0.05,  // T2
    0.85,  // T3 decisive
    0.95,  // T4 continuity
    0.60,  // P1 pathology detect
    0.05,  // P2 quarantine
    0.05,  // P3 true-death
    0.05,  // P4 re-entry
};

// Pathology sensitivity weight per semantic bin
// NOTE: P2, P4 weights are budget-ratio-dependent; these are static baseline weights.
//       Dynamic scaling is applied at call site.
inline constexpr std::array<double, 12> bin_pathology_weights = {
    0.05,  // B1
    0.05,  // B2
    0.05,  // B3
    0.35,  // B4 recovery wall
    0.05,  // T1
    0.05,  // T2
    0.05,  // T3
    0.05,  // T4
    1.00,  // P1 pathology detect
    0.75,  // P2 quarantine (scaled by (1 - budget_ratio) at call site)
    0.90,  // P3 true-death (scaled by anomaly_active at call site)
    0.70,  // P4 re-entry  (scaled by (1 - budget_ratio) at call site)
};

} // namespace governance
```

> **Note:** The weights for P2, P3, P4 have dynamic components (`budget_ratio`, `anomaly_active`).
> Document these at the call site but keep the base values here.

### 5b — Replace the inline `if` chains in the per-bin loop

Old:
```cpp
double conflict_weight = 0.05;
if (i == 1) conflict_weight = 0.55;
if (i == 6) conflict_weight = 0.85;
// …
```

New:
```cpp
const double conflict_weight = gov::bin_conflict_weights[i];
```

For pathology, preserve any dynamic scaling factors at the loop site but source baseline weights
from the constexpr array.

### Acceptance Criteria
- [ ] No bare `if (i == N) conflict_weight = ...` chains in the per-bin loop
- [ ] No bare `if (i == N) pathology_weight = ...` chains
- [ ] Weight arrays live in `alpha-spectrum-governance.hpp` with semantic comments
- [ ] Build passes; no behaviour change

---

## Phase 6 — Structured Status Publish

**Goal:** The ~54-line flat `status.*.store()` block is replaced with a structured approach.
The filter computes a local value-aggregate, then dispatches to status in one place.

### 6a — Introduce a `filter_frame_result` local struct inside `filter()`

```cpp
struct frame_result {
    double rot_objective, pos_objective;
    double rot_jitter,    pos_jitter;

    // Brownian channel
    double rot_brownian_raw, rot_brownian_filtered, rot_brownian_delta, rot_brownian_damped;
    double pos_brownian_raw, pos_brownian_filtered, pos_brownian_delta, pos_brownian_damped;
    double rot_predictive_error, pos_predictive_error;

    // Drive channels (indexed by tracking_head)
    std::array<double, as::tracking_head_count> rot_drive, pos_drive;

    // MTM
    double rot_mode_e, pos_mode_e, rot_mode_peak, pos_mode_peak;
    double rot_mode_purity, pos_mode_purity;
    double ngc_coupling_residual;

    // Geometry
    double rot_alpha_min, rot_alpha_max, rot_curve, rot_deadzone;
    double pos_alpha_min, pos_alpha_max, pos_curve, pos_deadzone;

    // Budget / anomaly
    double anti_inertia_budget;
    double anomaly_score;
    double outlier_quarantine_activity;

    // Additional channels
    double pos_predictive_translation_error;
    double invariant_correction_magnitude;
    double rot_neck_weight_error, pos_neck_weight_error;
    double rot_neck_invalid_ratio,  pos_neck_invalid_ratio;

    // Edge validity
    std::array<bool, gov::governed_edge_count> edge_valid{};
    bool   anomaly_active;

    // Bin arrays
    std::array<double, mode_count> rot_bin_prob, pos_bin_prob;
    std::array<double, mode_count> rot_bin_delta, pos_bin_delta;
    std::array<double, mode_count> rot_bin_conflict, pos_bin_conflict;
    std::array<double, mode_count> rot_bin_pathology, pos_bin_pathology;
};
```

### 6b — Write a `publish_status(calibration_status&, const frame_result&)` free function

Defined in the governance compilation unit (or a new `filter_status_publish.cpp`).
Iterates over all known fields and calls `.store(..., std::memory_order_relaxed)`.

The drive channels are published via a small inner loop:
```cpp
for (size_t h = 0; h < as::tracking_head_count; ++h) {
    status.rot_drive[h].store(r.rot_drive[h], ...);
    status.pos_drive[h].store(r.pos_drive[h], ...);
}
```

(Requires Phase 7 for the array-ified status struct.)

### 6c — Replace the inline publish block in `filter()` with `publish_status(status, result);`

### Acceptance Criteria
- [ ] The 54-line publish block in `filter()` replaced by a single call
- [ ] `publish_status` function defined in its own translation unit or helper header
- [ ] Build passes; no behaviour change

---

## Phase 7 — Restructure `calibration_status` (Drive Fields → Arrays)

**Goal:** The 14 individual `std::atomic<double> rot_X_drive` / `pos_X_drive` fields in
`calibration_status` become two arrays indexed by `tracking_head`.

**Note:** This phase modifies the public API of `calibration_status` and therefore requires
corresponding updates in `ftnoir_filter_alpha_spectrum_dialog.cpp`.

### 7a — Replace 14 named drive fields with arrays

```cpp
// OLD (14 fields):
std::atomic<double> rot_ema_drive, rot_brownian_drive, /* … */ pos_mtm_drive;

// NEW (2 arrays of 6 + 1 for MTM, or full tracking_head_count):
std::array<std::atomic<double>, as::tracking_head_count> rot_drive{}, pos_drive{};
```

> Decision point: include MTM drive in the array (as its own head), or keep it separate since
> MTM is not a `tracking_head` enum value. Recommendation: add `mtm` as a 7th head enum value,
> or keep a single extra `rot_mtm_drive` / `pos_mtm_drive` field. Document the decision here.

### 7b — Update all read sites in the dialog

The dialog reads `status.rot_ema_drive.load()` etc. to display diagnostics. Update to index
the arrays, or provide named accessors:
```cpp
double rot_drive(tracking_head h) const {
    return rot_drive[as::index(h)].load(std::memory_order_relaxed);
}
```

### 7c — Update `publish_status` accordingly

### Acceptance Criteria
- [ ] `calibration_status` has `rot_drive` / `pos_drive` arrays, not 14 named fields
- [ ] Dialog code updated to use array access
- [ ] Build passes; diagnostic display unchanged

---

## Phase 8 — Governance Encapsulation: Hide Index Arithmetic

**Goal:** No code outside `alpha-spectrum-governance.hpp` should call `governance::index()` or
`governance::edge_for_head()` directly. All call sites use named helpers.

### 8a — Audit all remaining `governance::index(` call sites

Run: `grep -n 'governance::index\|edge_for_head' filter-alpha-spectrum/*.cpp filter-alpha-spectrum/*.h`

### 8b — Add or use helpers for each remaining pattern

- `gov::head_edge_index(h)` (added in Phase 1b) covers the main case
- For shoulder edge: `gov::index(gov::governed_edge::shoulder_to_error_gate)` → add a named constant:
  ```cpp
  constexpr size_t shoulder_edge_index = index(governed_edge::shoulder_to_error_gate);
  ```

### 8c — Verify governance header is the sole definition point for all index arithmetic

### Acceptance Criteria
- [ ] `governance::index(` only appears in `alpha-spectrum-governance.hpp`
- [ ] `edge_for_head(` only appears in `alpha-spectrum-governance.hpp`
- [ ] Build passes

---

## Phase 9 — Final Cleanup & Validation

### 9a — Remove dead code

After all phases, search for any leftover `_sum`, `_avg`, `head_enabled_` variables or other
dead locals that were left from incremental refactoring.

### 9b — Line-count regression check

Record new `.cpp` line count and compare to baseline from Phase 0. The file should be
meaningfully shorter.

### 9c — Full build & smoke test

- `cmake --build build -j$(nproc)` — must succeed with zero warnings introduced
- Run opentrack with the filter; verify real-time diagnostics display is correct
- Verify governance inventory and errata system still functions (dialog node)

### 9d — Code review checklist

- [ ] No `detail::alpha_spectrum::governance::` long-forms in function bodies
- [ ] No repeated copy-paste blocks of 3+ identical lines
- [ ] All magic numbers (bin indices, weights, thresholds) either named or in constexpr arrays
- [ ] `calibration_status` struct has no rot/pos field pairs that could be arrays
- [ ] `filter()` function is significantly shorter and reads as sequential named phases
- [ ] Governance header is self-contained and importable without filter internals

---

## Summary Table

| Phase | Scope | Risk | Dependencies |
|---|---|---|---|
| 0 | Baseline snapshot | None | — |
| 1 | Namespace alias + governance API | Low | — |
| 2 | Drive accumulator arrays | Medium | 1 |
| 3 | Remaining `_sum`/`_avg` pairs | Medium | 1 |
| 4 | `edge_valid` head loop | Low | 1 |
| 5 | `constexpr` bin weight tables | Low | 1 |
| 6 | Structured status publish | High | 2, 3, 4, 5 |
| 7 | `calibration_status` struct reform | High | 6 |
| 8 | Governance encapsulation audit | Low | 1–7 |
| 9 | Final cleanup & validation | Low | All |

Phases 1–5 are independently executable and low-risk.
Phases 6–7 are the large structural changes and should be done together or in immediate sequence.
Phase 8 is a clean-up pass.

---

*Last updated: 2026-03-16 — initial plan*
