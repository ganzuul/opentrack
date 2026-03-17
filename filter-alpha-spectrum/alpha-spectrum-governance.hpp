// "Copyright (C) 2026 ganzuul. This file is part of Alpha Spectrum. Alpha Spectrum is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3..."

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace detail::alpha_spectrum {

enum class tracking_head : size_t {
    ema = 0,
    brownian,
    adaptive,
    predictive,
    chi_square,
    pareto,
    head_count
};

constexpr size_t tracking_head_count = static_cast<size_t>(tracking_head::head_count);

// Per-head fixed Rényi α defining each head's likelihood kernel geometry.
// α=1.0: Gaussian/Shannon.  α<1: heavy-tail.  α>1: min-entropy biased.
inline constexpr std::array<double, tracking_head_count> head_alpha_table = {
    1.0,  // ema       — Gaussian baseline
    0.7,  // brownian  — light heavy-tail
    1.2,  // adaptive  — slightly min-entropy biased
    0.9,  // predictive — near-Gaussian with tolerance
    2.5,  // chi_square — tight gate
    0.4,  // pareto    — heavy-tail saccade catch
};

constexpr size_t index(tracking_head h)
{
    return static_cast<size_t>(h);
}

namespace governance {

enum class canvas_node : size_t
{
    core = 0,
    ema,
    brownian,
    adaptive,
    predictive,
    chi_square,
    pareto,
    shoulder,
    error_gate,
    quality,
    node_count
};

constexpr size_t canvas_node_count = static_cast<size_t>(canvas_node::node_count);

constexpr size_t index(canvas_node node)
{
    return static_cast<size_t>(node);
}

// All canvas edges -- sole source of truth for the node graph topology.
// Entries 0..governed_edge_count-1 map 1:1 to calibration_status::edge_valid[].
// Remaining entries are additional visual edges tracked separately.
enum class canvas_edge : size_t
{
    // Earned head->shoulder edges (0-5: edge_valid[] index == tracking_head index)
    ema_to_shoulder = 0,
    brownian_to_shoulder,
    adaptive_to_shoulder,
    predictive_to_shoulder,
    chi_square_to_shoulder,
    pareto_to_shoulder,
    // Earned shoulder output (6)
    shoulder_to_error_gate,
    // Always-on distributor edges (core->head; share edge_valid index with their head)
    core_to_ema,
    core_to_brownian,
    core_to_adaptive,
    core_to_predictive,
    core_to_chi_square,
    core_to_pareto,
    // Quality discriminant path (governed by UI setting, not edge_valid[])
    quality_to_shoulder,
    edge_count
};

constexpr size_t canvas_edge_count = static_cast<size_t>(canvas_edge::edge_count);

// Number of slots in calibration_status::edge_valid[].
// Equals the count of validity-gated canvas_edge entries (indices 0..governed_edge_count-1).
constexpr size_t governed_edge_count = 7;

constexpr size_t index(canvas_edge edge)
{
    return static_cast<size_t>(edge);
}

constexpr canvas_edge edge_for_head(tracking_head head)
{
    return static_cast<canvas_edge>(static_cast<size_t>(head));
}

// core->head distributor edge for a given tracking head.
// Ordinal contract: core_to_ema is the base; heads follow in tracking_head order.
constexpr canvas_edge core_edge_for_head(tracking_head head)
{
    return static_cast<canvas_edge>(index(canvas_edge::core_to_ema) + static_cast<size_t>(head));
}

static_assert(index(canvas_edge::ema_to_shoulder)        == static_cast<size_t>(tracking_head::ema));
static_assert(index(canvas_edge::brownian_to_shoulder)   == static_cast<size_t>(tracking_head::brownian));
static_assert(index(canvas_edge::adaptive_to_shoulder)   == static_cast<size_t>(tracking_head::adaptive));
static_assert(index(canvas_edge::predictive_to_shoulder) == static_cast<size_t>(tracking_head::predictive));
static_assert(index(canvas_edge::chi_square_to_shoulder) == static_cast<size_t>(tracking_head::chi_square));
static_assert(index(canvas_edge::pareto_to_shoulder)     == static_cast<size_t>(tracking_head::pareto));
static_assert(governed_edge_count == tracking_head_count + 1,
              "governed_edge_count must cover all heads plus shoulder output");
// core_edge_for_head() ordinal contract: core_to_ema through core_to_pareto follow heads in order
static_assert(index(canvas_edge::core_to_ema) == governed_edge_count,
              "core_to_ema must immediately follow the governed edges block; update governed_edge_count if adding governed edges");
static_assert(index(canvas_edge::core_to_pareto)    == index(canvas_edge::core_to_ema) + static_cast<size_t>(tracking_head::pareto));

enum class neck_edge_test : uint32_t
{
    finite_sample = 1u << 0,
    finite_evidence = 1u << 1,
    evidence_range = 1u << 2,
    finite_likelihood = 1u << 3,
    nonnegative_likelihood = 1u << 4,
    finite_weight = 1u << 5,
    weight_range = 1u << 6,
    renyi_channel_consistency = 1u << 7,
};

constexpr uint32_t mask(neck_edge_test test)
{
    return static_cast<uint32_t>(test);
}

constexpr uint32_t neck_edge_required_mask =
    mask(neck_edge_test::finite_sample) |
    mask(neck_edge_test::finite_evidence) |
    mask(neck_edge_test::evidence_range) |
    mask(neck_edge_test::finite_likelihood) |
    mask(neck_edge_test::nonnegative_likelihood) |
    mask(neck_edge_test::finite_weight) |
    mask(neck_edge_test::weight_range) |
    mask(neck_edge_test::renyi_channel_consistency);

static_assert(neck_edge_required_mask == 0xFFu, "neck_edge_required_mask must cover all 8 neck_edge_test bits");

// Ordered list of all tracking heads, suitable for range-for iteration.
inline constexpr std::array<tracking_head, tracking_head_count> all_heads = {
    tracking_head::ema,
    tracking_head::brownian,
    tracking_head::adaptive,
    tracking_head::predictive,
    tracking_head::chi_square,
    tracking_head::pareto,
};

// ---------------------------------------------------------------------------
// Canvas edge visual metadata -- sole source of truth for advanced_node_canvas.
// One entry per canvas_edge value, in the same ordinal order.
//
// edge_valid_idx: index into calibration_status::edge_valid[], or -1 for
//                 quality_to_shoulder (governed by UI setting instead).
// use_health_color: true means the active color is computed at runtime from
//                   neck health diagnostics; active_color field is ignored.
// dashed: initial pen style (overwritten on first frame refresh).
//
// C++26 reflection note: when bloomberg/clang-p2996 merges to mainline,
// replace the canvas construction/update loops with:
//   [: expand(std::meta::enumerators_of(^^canvas_edge)) :]
// ---------------------------------------------------------------------------
struct canvas_color { uint8_t r, g, b; };

struct canvas_edge_meta
{
    canvas_node  from;
    canvas_node  to;
    canvas_color active_color;  // ignored when use_health_color = true
    bool         use_health_color;
    bool         dashed;
    // edge_valid[index(this edge)] is the sole validity source for every entry.
    // The filter writes all entries; the canvas reads them uniformly.
    // See: filter.cpp edge_valid writes, canvas_edge_registry, static_asserts above.
};

inline constexpr std::array<canvas_edge_meta, canvas_edge_count> canvas_edge_registry = {{
    // ema_to_shoulder        (edge_valid written by filter: earned head->shoulder)
    { canvas_node::ema,        canvas_node::shoulder,   {  0,  0,  0}, true,  false },
    // brownian_to_shoulder
    { canvas_node::brownian,   canvas_node::shoulder,   {  0,  0,  0}, true,  false },
    // adaptive_to_shoulder
    { canvas_node::adaptive,   canvas_node::shoulder,   {  0,  0,  0}, true,  false },
    // predictive_to_shoulder
    { canvas_node::predictive, canvas_node::shoulder,   {  0,  0,  0}, true,  false },
    // chi_square_to_shoulder
    { canvas_node::chi_square, canvas_node::shoulder,   {  0,  0,  0}, true,  false },
    // pareto_to_shoulder
    { canvas_node::pareto,     canvas_node::shoulder,   {  0,  0,  0}, true,  false },
    // shoulder_to_error_gate (edge_valid written by filter: neck health test)
    { canvas_node::shoulder,   canvas_node::error_gate, {220,140, 80}, false, false },
    // core_to_ema            (edge_valid written by filter: head_enabled_arr)
    { canvas_node::core,       canvas_node::ema,        {110,140,220}, false, false },
    // core_to_brownian
    { canvas_node::core,       canvas_node::brownian,   {110,140,220}, false, false },
    // core_to_adaptive
    { canvas_node::core,       canvas_node::adaptive,   {110,140,220}, false, false },
    // core_to_predictive
    { canvas_node::core,       canvas_node::predictive, {110,140,220}, false, false },
    // core_to_chi_square
    { canvas_node::core,       canvas_node::chi_square, {110,140,220}, false, false },
    // core_to_pareto
    { canvas_node::core,       canvas_node::pareto,     {110,140,220}, false, false },
    // quality_to_shoulder    (edge_valid written by filter: *s.qualities_mode_ui)
    { canvas_node::quality,    canvas_node::shoulder,   {170,110,210}, false, true  },
}};

static_assert(canvas_edge_registry.size() == canvas_edge_count,
              "canvas_edge_registry must have exactly one entry per canvas_edge");

// Mode count (semantic bins: B1-B4, T1-T4, P1-P4).
constexpr size_t mode_count = 12;

// Per-bin conflict sensitivity weights (indexed by bin 0–11).
// Layout: B1=0  B2=1  B3=2  B4=3  T1=4  T2=5  T3=6  T4=7  P1=8  P2=9  P3=10  P4=11
inline constexpr std::array<double, mode_count> bin_conflict_weights = {
    0.05, // B1 baseline
    0.55, // B2 plausibility gate
    0.05, // B3
    0.05, // B4 recovery wall
    0.05, // T1
    0.05, // T2
    0.85, // T3 decisive
    0.95, // T4 continuity
    0.60, // P1 pathology detect
    0.05, // P2 quarantine
    0.05, // P3 true-death
    0.05, // P4 re-entry
};

} // namespace governance

// ---------------------------------------------------------------------------
// Preset schema — canonical description of a curated Simplified view config.
// YAML files bundled as Qt resources are parsed into this struct at runtime.
// A key value of -1.0 means "leave the setting at its current default".
// ---------------------------------------------------------------------------

struct preset_def
{
    std::string name;
    std::string description;
    bool adaptive_mode = false;
    // Indexed by tracking_head — which ensemble heads are active.
    std::array<bool, tracking_head_count> head_enabled {};
    // Geometry parameters; -1.0 = "do not override".
    double rot_alpha_min = -1.0;
    double rot_alpha_max = -1.0;
    double rot_curve     = -1.0;
    double pos_alpha_min = -1.0;
    double pos_alpha_max = -1.0;
    double pos_curve     = -1.0;
    double rot_deadzone  = -1.0;
    double pos_deadzone  = -1.0;
};

} // namespace detail::alpha_spectrum
