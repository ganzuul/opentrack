#pragma once

#include "options/options.hpp"

namespace detail::alpha_spectrum {

using namespace options;

struct settings_alpha_spectrum : opts
{
    value<int> ui_complexity_mode;
    value<bool> advanced_mode_ui;
    value<bool> adaptive_mode;
    value<bool> qualities_mode_ui;
    value<bool> quality_stillness;
    value<bool> quality_continuity;
    value<bool> quality_robustness;
    value<bool> quality_decisiveness;
    value<bool> quality_pathology_defense;
    value<bool> quality_recovery_pace;
    value<bool> ema_enabled;
    value<bool> brownian_enabled;
    value<bool> predictive_enabled;
    value<bool> chi_square_enabled;
    value<bool> pareto_enabled;
    value<bool> outlier_quarantine_enabled;
    value<bool> mtm_enabled;

    value<slider_value> rot_alpha_min;
    value<slider_value> rot_alpha_max;
    value<slider_value> rot_curve;

    value<slider_value> pos_alpha_min;
    value<slider_value> pos_alpha_max;
    value<slider_value> pos_curve;

    value<slider_value> rot_deadzone;
    value<slider_value> pos_deadzone;
    value<slider_value> brownian_head_gain;
    value<slider_value> adaptive_threshold_lift;
    value<slider_value> predictive_head_gain;
    value<slider_value> chi_square_head_gain;
    value<slider_value> pareto_head_gain;
    value<slider_value> outlier_quarantine_strength;
    value<slider_value> mtm_shoulder_base;
    value<slider_value> ngc_kappa;
    value<slider_value> ngc_nominal_z;
    value<slider_value> anti_inertia_budget_max;
    value<slider_value> anti_inertia_recovery_rate;
    value<slider_value> anomaly_threshold;
    value<slider_value> invariant_correction_gain;
    value<slider_value> predictive_translation_gain;

    settings_alpha_spectrum() :
        opts("alpha-spectrum-filter"),
        ui_complexity_mode(b, "ui-complexity-mode", 1),
        advanced_mode_ui(b, "advanced-mode-ui", false),
        adaptive_mode(b, "adaptive-mode", false),
        qualities_mode_ui(b, "qualities-mode-ui", false),
        quality_stillness(b, "quality-stillness", false),
        quality_continuity(b, "quality-continuity", false),
        quality_robustness(b, "quality-robustness", false),
        quality_decisiveness(b, "quality-decisiveness", false),
        quality_pathology_defense(b, "quality-pathology-defense", false),
        quality_recovery_pace(b, "quality-recovery-pace", false),
        ema_enabled(b, "ema-enabled", true),
        brownian_enabled(b, "brownian-enabled", true),
        predictive_enabled(b, "predictive-enabled", true),
        chi_square_enabled(b, "chi-square-enabled", true),
        pareto_enabled(b, "pareto-enabled", true),
        outlier_quarantine_enabled(b, "outlier-quarantine-enabled", true),
        mtm_enabled(b, "mtm-enabled", true),
        rot_alpha_min(b, "rot-alpha-min", { .085, .005, .4 }),
        rot_alpha_max(b, "rot-alpha-max", { .218, .02, 1.0 }),
        rot_curve(b, "rot-curve", { 4.26, .2, 8.0 }),
        pos_alpha_min(b, "pos-alpha-min", { .085, .005, .4 }),
        pos_alpha_max(b, "pos-alpha-max", { .218, .02, 1.0 }),
        pos_curve(b, "pos-curve", { 4.26, .2, 8.0 }),
        rot_deadzone(b, "rot-deadzone", { .156, 0.0, .3 }),
        pos_deadzone(b, "pos-deadzone", { 1.041, 0.0, 2.0 }),
        brownian_head_gain(b, "brownian-head-gain", { 1.0, 0.0, 2.0 }),
        adaptive_threshold_lift(b, "adaptive-threshold-lift", { 0.15, 0.0, 0.6 }),
        predictive_head_gain(b, "predictive-head-gain", { 1.0, 0.0, 2.0 }),
        chi_square_head_gain(b, "chi-square-head-gain", { 1.0, 0.0, 2.0 }),
        pareto_head_gain(b, "pareto-head-gain", { 1.0, 0.0, 2.0 }),
        outlier_quarantine_strength(b, "outlier-quarantine-strength", { 0.6, 0.0, 1.0 }),
        mtm_shoulder_base(b, "mtm-shoulder-base", { 0.5, 0.0, 1.0 }),
        ngc_kappa(b, "ngc-kappa", { 0.078, 0.0, 0.3 }),
        ngc_nominal_z(b, "ngc-nominal-z", { 0.85, 0.3, 2.0 }),
        anti_inertia_budget_max(b, "anti-inertia-budget-max", { 1.0, 0.1, 5.0 }),
        anti_inertia_recovery_rate(b, "anti-inertia-recovery-rate", { 0.15, 0.01, 2.0 }),
        anomaly_threshold(b, "anomaly-threshold", { 1.5, 0.1, 10.0 }),
        invariant_correction_gain(b, "invariant-correction-gain", { 0.0, 0.0, 2.0 }),
        predictive_translation_gain(b, "predictive-translation-gain", { 1.0, 0.0, 2.0 })
    {
    }
};

} // ns detail::alpha_spectrum

using detail::alpha_spectrum::settings_alpha_spectrum;
