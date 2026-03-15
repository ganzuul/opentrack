#include "ftnoir_filter_alpha_spectrum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>
#include <QDebug>

alpha_spectrum::alpha_spectrum() = default;

namespace {
static double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
static double remap_with_threshold(double x, double threshold)
{
    if (x <= threshold)
        return 0.0;
    return clamp01((x - threshold) / std::max(1e-9, 1.0 - threshold));
}

// True Rényi/Tsallis likelihood from generalized alpha-spectrum.
// alpha > 15: near min-entropy regime (hard cutoff)
// alpha ~= 1: Shannon/Gaussian limit
// alpha < 1: heavy-tail regime
static double renyi_tsallis_likelihood(double mahalanobis_sq, double alpha)
{
    constexpr double eps = 1e-12;
    if (alpha > 15.0)
        return (mahalanobis_sq < 0.12) ? 1.0 : 1e-10;
    if (std::fabs(alpha - 1.0) < 1e-3)
        return std::exp(-0.5 * mahalanobis_sq) + eps;

    const double core = 1.0 - (1.0 - alpha) * 0.5 * mahalanobis_sq;
    if (core <= 0.0)
        return eps;
    return std::pow(core, 1.0 / (1.0 - alpha)) + eps;
}

static constexpr int mode_count_local = 12;
static constexpr double mode_diffusion_rc_local = .75;
static constexpr std::array<double, mode_count_local> mode_centers {
    0.0417, 0.1250, 0.2083, 0.2917,
    0.3750, 0.4583, 0.5417, 0.6250,
    0.7083, 0.7917, 0.8750, 0.9583
};

// Semantic scaffolding for bin/attitude mapping in simplified UI.
static constexpr std::array<const char*, mode_count_local> mode_semantic_names {
    "B1-continuity-wall",
    "B2-plausibility-gate",
    "B3-kinematic-restraint",
    "B4-recovery-wall",
    "T1-snapshot",
    "T2-robust",
    "T3-decisive",
    "T4-continuity",
    "P1-pathology-detect",
    "P2-quarantine",
    "P3-true-death",
    "P4-reentry"
};
static_assert(mode_semantic_names.size() == mode_centers.size());

static void initialize_uniform(std::array<double, mode_count_local>& p)
{
    p.fill(1.0 / static_cast<double>(p.size()));
}

static void diffuse_modes(std::array<double, mode_count_local>& p, double dt)
{
    const double mix = clamp01(dt / (dt + mode_diffusion_rc_local));
    const double uniform = 1.0 / static_cast<double>(p.size());
    for (double& x : p)
        x = (1.0 - mix) * x + mix * uniform;
}

static void update_modes_from_measurement(std::array<double, mode_count_local>& p, double measurement)
{
    static constexpr double sigma = 0.17;
    static constexpr double inv_two_sigma2 = 1.0 / (2.0 * sigma * sigma);
    static constexpr double epsilon = 1e-8;

    measurement = clamp01(measurement);
    double sum = 0.0;
    for (int i = 0; i < static_cast<int>(p.size()); i++)
    {
        const double err = measurement - mode_centers[i];
        const double likelihood = std::exp(-(err * err) * inv_two_sigma2) + epsilon;
        p[i] *= likelihood;
        sum += p[i];
    }

    if (sum <= 1e-12)
    {
        initialize_uniform(p);
        return;
    }

    for (double& x : p)
        x /= sum;
}

static double mode_expectation(const std::array<double, mode_count_local>& p)
{
    double e = 0.0;
    for (int i = 0; i < static_cast<int>(p.size()); i++)
        e += p[i] * mode_centers[i];
    return clamp01(e);
}

static double mode_peak_center(const std::array<double, mode_count_local>& p)
{
    int idx = 0;
    for (int i = 1; i < static_cast<int>(p.size()); i++)
        if (p[i] > p[idx])
            idx = i;
    return mode_centers[idx];
}

struct renyi_bin_state final
{
    double h0 = 0.0;
    double h1 = 0.0;
    double h2 = 0.0;
    double h_inf = 0.0;
    double purity = 0.0;
    double peak_mass = 0.0;
};

static renyi_bin_state compute_renyi_state(const std::array<double, mode_count_local>& p)
{
    constexpr double eps = 1e-12;
    renyi_bin_state rs;

    int active = 0;
    for (double x : p)
    {
        if (x > eps)
            active++;
        rs.purity += x * x;
        rs.peak_mass = std::max(rs.peak_mass, x);
        if (x > eps)
            rs.h1 -= x * std::log(x);
    }

    rs.h0 = static_cast<double>(active) / static_cast<double>(p.size());
    rs.h2 = -std::log(std::max(rs.purity, eps));
    rs.h_inf = -std::log(std::max(rs.peak_mass, eps));
    return rs;
}

static double joint_evidence_stable(double chi_conf, double pareto_w)
{
    return clamp01(chi_conf) * clamp01(pareto_w);
}

static double joint_evidence_outlier(double chi_conf, double pareto_w)
{
    return (1.0 - clamp01(chi_conf)) * (1.0 - clamp01(pareto_w));
}

template <size_t N>
static void inject_bin_mass(std::array<double, N>& p, size_t idx, double strength)
{
    if (idx >= N)
        return;

    const double s = std::clamp(strength, 0.0, 1.0);
    if (s <= 1e-12)
        return;

    for (size_t i = 0; i < N; i++)
    {
        if (i == idx)
            p[i] = p[i] + s * (1.0 - p[i]);
        else
            p[i] = (1.0 - s) * p[i];
    }

    double sum = 0.0;
    for (double x : p)
        sum += x;
    if (sum > 1e-12)
    {
        for (double& x : p)
            x /= sum;
    }
}

static double evaluate_total_topological_debt(
    const detail::alpha_spectrum::temporal_economy_state& state,
    bool rotation)
{
    const auto& debt = rotation ? state.rot_transition_debt : state.pos_transition_debt;
    double total = 0.0;
    for (size_t i = 0; i < detail::alpha_spectrum::transition_matrix_size; i++)
        total += std::max(0.0, debt[i].load(std::memory_order_relaxed));
    return total;
}

static double calculate_coupling_constant(double total_debt, double max_capacity)
{
    const double safe_capacity = std::max(max_capacity, 1e-9);
    const double debt_ratio = std::clamp(total_debt / safe_capacity, 0.0, 1.0);
    return debt_ratio * debt_ratio;
}

static double apply_amortization(double previous_resolved, double new_measurement, double coupling_constant)
{
    return new_measurement + coupling_constant * (previous_resolved - new_measurement);
}

static detail::alpha_spectrum::tracking_head dominant_head_from_shares(
    double ema_share,
    double brownian_share,
    double adaptive_share,
    double predictive_share,
    double chi_square_share,
    double pareto_share)
{
    using detail::alpha_spectrum::tracking_head;
    std::array<std::pair<tracking_head, double>, 6> values {{
        {tracking_head::ema, ema_share},
        {tracking_head::brownian, brownian_share},
        {tracking_head::adaptive, adaptive_share},
        {tracking_head::predictive, predictive_share},
        {tracking_head::chi_square, chi_square_share},
        {tracking_head::pareto, pareto_share}
    }};

    auto best = values[0];
    for (size_t i = 1; i < values.size(); i++)
    {
        if (values[i].second > best.second)
            best = values[i];
    }
    return best.first;
}

static void decay_transition_debt(detail::alpha_spectrum::temporal_economy_state& state, double rate)
{
    const double clamped = std::clamp(rate, 0.0, 1.0);
    for (size_t i = 0; i < detail::alpha_spectrum::transition_matrix_size; i++)
    {
        const double pos = state.pos_transition_debt[i].load(std::memory_order_relaxed);
        const double rot = state.rot_transition_debt[i].load(std::memory_order_relaxed);
        state.pos_transition_debt[i].store(pos * clamped, std::memory_order_relaxed);
        state.rot_transition_debt[i].store(rot * clamped, std::memory_order_relaxed);
    }
}

struct runtime_filter_controls final
{
    bool adaptive_mode;
    bool ema_enabled;
    bool brownian_enabled;
    bool predictive_enabled;
    bool chi_square_enabled;
    bool pareto_enabled;
    bool outlier_quarantine_enabled;
    bool mtm_enabled;

    double brownian_head_gain;
    double adaptive_threshold_lift;
    double predictive_head_gain;
    double chi_square_head_gain;
    double pareto_head_gain;
    double outlier_quarantine_strength;
    double predictive_translation_gain;
    double mtm_shoulder_base;
    double ngc_kappa;
    double anti_inertia_budget_max;
    double anti_inertia_recovery_rate;
    double anomaly_threshold;
    double invariant_correction_gain;

    double effective_shoulder(const renyi_bin_state& rs) const
    {
        const double t = std::clamp(rs.purity, 0.0, 1.0);
        const double blend = t * t;
        return mtm_shoulder_base + (1.0 - mtm_shoulder_base) * blend;
    }
};

static runtime_filter_controls resolve_runtime_controls(const settings_alpha_spectrum& s)
{
    runtime_filter_controls ctl {
        *s.adaptive_mode,
        *s.ema_enabled,
        *s.brownian_enabled,
        *s.predictive_enabled,
        *s.chi_square_enabled,
        *s.pareto_enabled,
        *s.outlier_quarantine_enabled,
        *s.mtm_enabled,
        *s.brownian_head_gain,
        *s.adaptive_threshold_lift,
        *s.predictive_head_gain,
        *s.chi_square_head_gain,
        *s.pareto_head_gain,
        *s.outlier_quarantine_strength,
        *s.predictive_translation_gain,
        *s.mtm_shoulder_base,
        *s.ngc_kappa,
        *s.anti_inertia_budget_max,
        *s.anti_inertia_recovery_rate,
        *s.anomaly_threshold,
        *s.invariant_correction_gain,
    };
    return ctl;
}

struct quality_overlay_snapshot final
{
    bool active = false;
    std::array<double, detail::alpha_spectrum::quality_overlay_state::value_count> delta {};
};

static quality_overlay_snapshot capture_quality_overlay_snapshot()
{
    const auto& overlay = detail::alpha_spectrum::shared_quality_overlay_state();
    quality_overlay_snapshot q;
    q.active = overlay.active.load(std::memory_order_relaxed);
    for (int i = 0; i < static_cast<int>(q.delta.size()); i++)
        q.delta[i] = overlay.delta[i].load(std::memory_order_relaxed);
    return q;
}

static runtime_filter_controls resolve_runtime_controls(const settings_alpha_spectrum& s, const quality_overlay_snapshot& q)
{
    runtime_filter_controls ctl = resolve_runtime_controls(s);
    if (!q.active)
        return ctl;

    auto add_clamped = [](double base, double delta, double min_v, double max_v) {
        return std::clamp(base + delta, min_v, max_v);
    };

    ctl.brownian_head_gain = add_clamped(ctl.brownian_head_gain, q.delta[0], 0.0, 2.0);
    ctl.adaptive_threshold_lift = add_clamped(ctl.adaptive_threshold_lift, q.delta[1], 0.0, 0.6);
    ctl.predictive_head_gain = add_clamped(ctl.predictive_head_gain, q.delta[2], 0.0, 2.0);
    ctl.predictive_translation_gain = add_clamped(ctl.predictive_translation_gain, q.delta[3], 0.0, 2.0);
    ctl.mtm_shoulder_base = add_clamped(ctl.mtm_shoulder_base, q.delta[4], 0.0, 1.0);
    ctl.ngc_kappa = add_clamped(ctl.ngc_kappa, q.delta[5], 0.0, 0.3);
    ctl.anti_inertia_budget_max = add_clamped(ctl.anti_inertia_budget_max, q.delta[6], 0.1, 5.0);
    ctl.anti_inertia_recovery_rate = add_clamped(ctl.anti_inertia_recovery_rate, q.delta[7], 0.01, 2.0);
    ctl.anomaly_threshold = add_clamped(ctl.anomaly_threshold, q.delta[8], 0.1, 10.0);
    ctl.invariant_correction_gain = add_clamped(ctl.invariant_correction_gain, q.delta[9], 0.0, 2.0);
    return ctl;
}
}

detail::alpha_spectrum::calibration_status& detail::alpha_spectrum::shared_calibration_status()
{
    static calibration_status status;
    return status;
}

detail::alpha_spectrum::quality_overlay_state& detail::alpha_spectrum::shared_quality_overlay_state()
{
    static quality_overlay_state state;
    return state;
}

void alpha_spectrum::set_tracker(ITracker* tracker)
{
    highrate_source = dynamic_cast<IHighrateSource*>(tracker);
    has_highrate_source = false;
    qDebug() << "alpha-spectrum: set_tracker called, highrate_source =" << (highrate_source ? "available" : "null");
    last_highrate_pose_valid = false;
    std::fill(gyro_integrated_rotation, gyro_integrated_rotation + 3, 0.0);
}

void alpha_spectrum::integrate_highrate_samples()
{
    std::fill(gyro_integrated_rotation, gyro_integrated_rotation + 3, 0.0);
    has_highrate_source = false;

    if (!highrate_source)
        return;

    std::vector<highrate_pose_sample> samples;
    if (!highrate_source->get_highrate_samples(samples))
        return;

    has_highrate_source = true;
    
    static int log_counter = 0;
    if (++log_counter <= 3 || log_counter % 250 == 0)
        qDebug() << "alpha-spectrum: integrated" << samples.size() << "high-rate samples";

    static constexpr double full_turn = 360.0;
    static constexpr double half_turn = 180.0;

    for (const auto& sample : samples)
    {
        if (!last_highrate_pose_valid)
        {
            last_highrate_pose[0] = sample.pose[Yaw];
            last_highrate_pose[1] = sample.pose[Pitch];
            last_highrate_pose[2] = sample.pose[Roll];
            last_highrate_pose_valid = true;
            continue;
        }

        for (int axis = 0; axis < 3; axis++)
        {
            const int pose_axis = Yaw + axis;
            double delta = sample.pose[pose_axis] - last_highrate_pose[axis];
            if (std::fabs(delta) > half_turn)
            {
                const double wrap = std::copysign(full_turn, delta);
                delta -= wrap;
            }

            gyro_integrated_rotation[axis] += delta;
            last_highrate_pose[axis] = sample.pose[pose_axis];
        }
    }
}

void alpha_spectrum::filter(const double* input, double* output)
{
    static constexpr double full_turn = 360.0;
    static constexpr double half_turn = 180.0;

    // Stage 0: first-frame bootstrap to avoid transient spikes.
    if (first_run) [[unlikely]]
    {
        first_run = false;
        timer.start();
        noise_rc = 0.0;
        rot_activity = 0.0;
        pos_activity = 0.0;
        std::copy(input, input + axis_count, last_input);
        std::copy(input, input + axis_count, last_output);
        std::fill(last_delta, last_delta + axis_count, 0.0);
        std::fill(last_noise, last_noise + axis_count, 0.0);
        std::fill(raw_brownian_energy, raw_brownian_energy + axis_count, 0.0);
        std::fill(filtered_brownian_energy, filtered_brownian_energy + axis_count, 0.0);
        std::copy(input, input + axis_count, predicted_next_output);
        coupling_residual = 0.0;
        last_coupling_residual = 0.0;
        anti_inertia_budget = 1.0;
        anomaly_score = 0.0;
        anomaly_cooldown_frames = 0;
        anomaly_active = false;
        translation_state = {};
        last_Z = std::max(std::fabs(input[TZ]), 0.3);
        std::fill(gyro_integrated_rotation, gyro_integrated_rotation + 3, 0.0);
        last_highrate_pose_valid = false;
        initialize_uniform(rot_mode_prob);
        initialize_uniform(pos_mode_prob);
        last_rot_mode_prob_snapshot = rot_mode_prob;
        last_pos_mode_prob_snapshot = pos_mode_prob;
        for (auto& x : temporal_state.pos_transition_debt)
            x.store(0.0, std::memory_order_relaxed);
        for (auto& x : temporal_state.rot_transition_debt)
            x.store(0.0, std::memory_order_relaxed);
        temporal_state.max_capacity.store(10.0, std::memory_order_relaxed);
        last_dominant_head.fill(detail::alpha_spectrum::tracking_head::ema);
        std::copy(last_output, last_output + axis_count, output);
        return;
    }

    const double dt = timer.elapsed_seconds();
    timer.start();
    const double safe_dt = std::min(dt_max, std::max(dt_min, dt));

    // Integrate high-rate gyro samples into Predictive head prediction
    integrate_highrate_samples();

    noise_rc = std::min(noise_rc + safe_dt, noise_rc_max);
    const double delta_alpha = safe_dt / (safe_dt + delta_rc);
    const double noise_alpha = safe_dt / (safe_dt + noise_rc);
    const double activity_alpha = safe_dt / (safe_dt + activity_rc);
    const double brownian_alpha = safe_dt / (safe_dt + brownian_rc);

    const runtime_filter_controls ctl = resolve_runtime_controls(s, capture_quality_overlay_snapshot());
    const bool adaptive_mode = ctl.adaptive_mode;
    const bool ema_enabled = ctl.ema_enabled;
    const bool brownian_enabled = ctl.brownian_enabled;
    const bool predictive_enabled = ctl.predictive_enabled;
    const bool chi_square_enabled = ctl.chi_square_enabled;
    const bool pareto_enabled = ctl.pareto_enabled;
    const bool outlier_quarantine_enabled = ctl.outlier_quarantine_enabled;
    const bool mtm_enabled = ctl.mtm_enabled;
    auto& status = detail::alpha_spectrum::shared_calibration_status();
    status.active.store(true, std::memory_order_relaxed);

    double rot_mode_e_prior = 0.0;
    double pos_mode_e_prior = 0.0;
    renyi_bin_state rot_rs_prior;
    renyi_bin_state pos_rs_prior;
    if (mtm_enabled)
    {
        diffuse_modes(rot_mode_prob, safe_dt);
        diffuse_modes(pos_mode_prob, safe_dt);
        rot_mode_e_prior = mode_expectation(rot_mode_prob);
        pos_mode_e_prior = mode_expectation(pos_mode_prob);
        rot_rs_prior = compute_renyi_state(rot_mode_prob);
        pos_rs_prior = compute_renyi_state(pos_mode_prob);
    }

    if (!adaptive_mode)
    {
        rot_activity = 0.0;
        pos_activity = 0.0;
    }

    const double rot_alpha_min = *s.rot_alpha_min;
    const double rot_alpha_max_source = *s.rot_alpha_max;
    const double rot_alpha_max = std::max(rot_alpha_min, rot_alpha_max_source);
    const double rot_curve = *s.rot_curve;
    const double rot_deadzone = *s.rot_deadzone;

    const double pos_alpha_min = *s.pos_alpha_min;
    const double pos_alpha_max_source = *s.pos_alpha_max;
    const double pos_alpha_max = std::max(pos_alpha_min, pos_alpha_max_source);
    const double pos_curve = *s.pos_curve;
    const double pos_deadzone = *s.pos_deadzone;
    const double brownian_head_gain = ctl.brownian_head_gain;
    const double adaptive_threshold_lift = ctl.adaptive_threshold_lift;
    const double predictive_head_gain = ctl.predictive_head_gain;
    const double chi_square_head_gain = ctl.chi_square_head_gain;
    const double pareto_head_gain = ctl.pareto_head_gain;
    const double outlier_quarantine_strength = ctl.outlier_quarantine_strength;
    const double predictive_translation_gain = ctl.predictive_translation_gain;
    const double ngc_kappa = ctl.ngc_kappa;
    const double ngc_nominal_z = *s.ngc_nominal_z;
    const double invariant_correction_gain = ctl.invariant_correction_gain;
    const double anti_inertia_budget_max = std::max(ctl.anti_inertia_budget_max, 0.1);
    const double anti_inertia_recovery_rate = std::max(ctl.anti_inertia_recovery_rate, 0.0);
    const double anomaly_threshold = std::max(ctl.anomaly_threshold, 0.1);

    anti_inertia_budget = std::clamp(anti_inertia_budget, 0.0, anti_inertia_budget_max);
    const double anti_inertia_budget_ratio = std::clamp(anti_inertia_budget / anti_inertia_budget_max, 0.0, 1.0);

    // NGC depth-commutator lift: keep measured depth on TZ and use torsion as residual coupling only.
    std::array<double, 3> bifurcated_pos {input[TX], input[TY], input[TZ]};
    {
        const double raw_x = input[TX];
        const double raw_y = input[TY];
        const double curr_z = std::max(std::fabs(input[TZ]), 0.3);
        last_Z = curr_z;

        const double nominal_depth = std::max(ngc_nominal_z, 1e-6);
        const double apparent_scale = nominal_depth / curr_z;
        const double radial_xy = std::hypot(raw_x, raw_y);
        const double kappa = ngc_kappa * ((apparent_scale - 1.0) / (nominal_depth * nominal_depth));
        const double depth_torsion = 0.5 * (kappa * radial_xy);

        bifurcated_pos = {raw_x, raw_y, input[TZ]};
        coupling_residual = std::clamp(std::fabs(depth_torsion), 0.0, 3.0);
    }
    const double coupling_residual_jump = std::fabs(coupling_residual - last_coupling_residual);

    // Phase 2 predictive translation path:
    // propagate Cartesian position with explicit velocity state and optional
    // invariant correction derived from NGC coupling residual.
    std::array<double, 3> translation_prediction {
        translation_state.x + translation_state.vx * safe_dt * predictive_translation_gain,
        translation_state.y + translation_state.vy * safe_dt * predictive_translation_gain,
        translation_state.z + translation_state.vz * safe_dt * predictive_translation_gain,
    };

    const double radial_xy = std::hypot(input[TX], input[TY]);
    const double inv_depth = 1.0 / std::max(std::fabs(input[TZ]), 0.3);
    std::array<double, 3> invariant_correction {0.0, 0.0, 0.0};
    if (radial_xy > 1e-6)
    {
        const double radial_x = input[TX] / radial_xy;
        const double radial_y = input[TY] / radial_xy;
        const double corr_xy = invariant_correction_gain * coupling_residual * inv_depth;
        invariant_correction[0] = -corr_xy * radial_x;
        invariant_correction[1] = -corr_xy * radial_y;
    }
    invariant_correction[2] = invariant_correction_gain * coupling_residual;

    for (int axis = 0; axis < 3; axis++)
        translation_prediction[axis] += invariant_correction[axis] * safe_dt;

    const double invariant_correction_magnitude =
        std::hypot(invariant_correction[0], std::hypot(invariant_correction[1], invariant_correction[2]));
    double pos_predictive_translation_error_sum = 0.0;

    // Stage 1: per-axis measurement processing.
    // - wrap handling for rotational continuity
    // - deadzone suppression
    // - innovation/noise estimation for adaptive alpha
    double rot_jitter_sum = 0.0;
    double pos_jitter_sum = 0.0;
    double rot_objective_sum = 0.0;
    double pos_objective_sum = 0.0;
    double rot_brownian_raw_sum = 0.0;
    double rot_brownian_filtered_sum = 0.0;
    double rot_predictive_error_sum = 0.0;
    double pos_brownian_raw_sum = 0.0;
    double pos_brownian_filtered_sum = 0.0;
    double pos_predictive_error_sum = 0.0;
    double rot_ema_drive_sum = 0.0;
    double rot_brownian_drive_sum = 0.0;
    double rot_adaptive_drive_sum = 0.0;
    double rot_predictive_drive_sum = 0.0;
    double rot_chi_square_drive_sum = 0.0;
    double rot_pareto_drive_sum = 0.0;
    double rot_mtm_drive_sum = 0.0;
    double pos_ema_drive_sum = 0.0;
    double pos_brownian_drive_sum = 0.0;
    double pos_adaptive_drive_sum = 0.0;
    double pos_predictive_drive_sum = 0.0;
    double pos_chi_square_drive_sum = 0.0;
    double pos_pareto_drive_sum = 0.0;
    double pos_mtm_drive_sum = 0.0;
    double pos_residual_sum = 0.0;
    int pos_head_transition_count = 0;

    for (int i = TX; i <= Roll; i++)
    {
        const bool is_rotation = i >= Yaw;
        const bool is_position = i <= TZ;
        const double measurement_value = is_position ? bifurcated_pos[i] : input[i];
        double input_value = measurement_value;
        double raw_input_value = measurement_value;
        double delta = input_value - last_output[i];

        double raw_delta = raw_input_value - last_input[i];
        if (is_rotation && std::fabs(raw_delta) > half_turn)
        {
            const double wrap = std::copysign(full_turn, raw_delta);
            raw_input_value -= wrap;
            raw_delta = raw_input_value - last_input[i];
        }

        if (is_rotation && std::fabs(delta) > half_turn)
        {
            const double wrap = std::copysign(full_turn, delta);
            delta -= wrap;
            input_value -= wrap;
        }

        const double filtered_prev = last_output[i];

        const double deadzone = is_rotation ? rot_deadzone : pos_deadzone;
        if (std::fabs(delta) < deadzone)
        {
            delta = 0.0;
            input_value = last_output[i];
        }

        last_delta[i] += delta_alpha * (delta - last_delta[i]);

        const double innovation = last_delta[i] * last_delta[i];
        last_noise[i] = noise_alpha * innovation + (1.0 - noise_alpha) * last_noise[i];
        const double norm_innovation =
            last_noise[i] < 1e-12 ? 0.0 : std::min(innovation / (9.0 * last_noise[i]), 1.0);

        if (is_rotation)
        {
            rot_jitter_sum += std::sqrt(std::max(last_noise[i], 0.0));
            rot_objective_sum += norm_innovation;
        }
        else
        {
            pos_jitter_sum += std::sqrt(std::max(last_noise[i], 0.0));
            pos_objective_sum += norm_innovation;
        }

        const double alpha_min = is_rotation ? rot_alpha_min : pos_alpha_min;
        const double alpha_max = is_rotation ? rot_alpha_max : pos_alpha_max;
        const double curve = is_rotation ? rot_curve : pos_curve;
        double& activity = is_rotation ? rot_activity : pos_activity;
        const double mode_e = is_rotation ? rot_mode_e_prior : pos_mode_e_prior;
        const double mtm_drive = mtm_enabled ? mode_e : 0.0;

        if (adaptive_mode)
        {
            const double raw_brownian = std::sqrt(std::max(raw_brownian_energy[i], 0.0));
            const double filtered_brownian = std::sqrt(std::max(filtered_brownian_energy[i], 0.0));
            const double brownian_drive =
                brownian_enabled && raw_brownian > 1e-12 ?
                    clamp01(1.0 - filtered_brownian / raw_brownian) : 0.0;
            const double instantaneous_drive = std::max(std::max(norm_innovation, brownian_drive), mtm_drive);
            activity += activity_alpha * (instantaneous_drive - activity);
            activity = std::clamp(activity, 0.0, 1.0);
        }

        // Stage 2: build hydra heads (EMA/Brownian) and compose through
        // a Rényi-style neck under MTM shoulder control.
        const double motion = std::pow(norm_innovation, curve);
        double alpha_ema = alpha_min + (alpha_max - alpha_min) * motion;

        if (adaptive_mode)
            alpha_ema += adaptive_boost * activity * (1.0 - alpha_ema);

        alpha_ema = std::clamp(alpha_ema, 0.0, 1.0);

        const double raw_brownian = std::sqrt(std::max(raw_brownian_energy[i], 0.0));
        const double filtered_brownian = std::sqrt(std::max(filtered_brownian_energy[i], 0.0));
        const double brownian_drive =
            brownian_enabled && raw_brownian > 1e-12 ?
                clamp01(1.0 - filtered_brownian / raw_brownian) : 0.0;

        const double tuned_brownian_drive = clamp01(brownian_drive * brownian_head_gain);
        double alpha_brownian = alpha_min + (alpha_max - alpha_min) * std::pow(tuned_brownian_drive, curve);
        if (adaptive_mode)
            alpha_brownian += adaptive_boost * activity * (1.0 - alpha_brownian);
        alpha_brownian = std::clamp(alpha_brownian, 0.0, 1.0);

        struct head_candidate final {
            bool enabled;
            double sample;
            double weight;
            bool is_ema;
            bool is_brownian;
            bool is_adaptive;
            bool is_predictive;
            bool is_chi_square;
            bool is_pareto;
        };

        std::array<head_candidate, hydra_head_capacity> heads {{
            {false, input_value, 0.0, false, false, false, false, false, false},
            {false, input_value, 0.0, false, false, false, false, false, false},
            {false, input_value, 0.0, false, false, false, false, false, false},
            {false, input_value, 0.0, false, false, false, false, false, false},
            {false, input_value, 0.0, false, false, false, false, false, false},
            {false, input_value, 0.0, false, false, false, false, false, false}
        }};

        int head_count = 0;
        auto add_head = [&](bool enabled, double sample, bool is_ema, bool is_brownian, bool is_adaptive, bool is_predictive, bool is_chi_square, bool is_pareto) {
            if (!enabled || head_count >= hydra_head_capacity)
                return;
            heads[head_count++] = {true, sample, 0.0, is_ema, is_brownian, is_adaptive, is_predictive, is_chi_square, is_pareto};
        };

        const double adaptive_gate_input = std::max(norm_innovation, brownian_drive);
        const double adaptive_gate = remap_with_threshold(adaptive_gate_input, adaptive_threshold_lift);
        double alpha_adaptive = alpha_min + (alpha_max - alpha_min) * std::pow(adaptive_gate, curve);
        if (adaptive_mode)
            alpha_adaptive += adaptive_boost * activity * (1.0 - alpha_adaptive);
        alpha_adaptive = std::clamp(alpha_adaptive, 0.0, 1.0);

        const double ema_head_sample = filtered_prev + alpha_ema * (input_value - filtered_prev);
        const double brownian_head_sample =
            filtered_prev + alpha_brownian * (input_value - filtered_prev);
        const double adaptive_head_sample =
            filtered_prev + alpha_adaptive * (input_value - filtered_prev);
        const double predictive_head_sample =
            filtered_prev + predictive_head_gain * (predicted_next_output[i] - filtered_prev);

        const double sigma2 = std::max(last_noise[i], 1e-8);
        const double chi_sq_threshold = 3.841458820694124;
        const double innovation_chi = ((input_value - filtered_prev) * (input_value - filtered_prev)) / sigma2;
        const double chi_confidence = clamp01((chi_sq_threshold - innovation_chi) / chi_sq_threshold);
        const double alpha_chi_square = std::clamp(alpha_min + (alpha_max - alpha_min) * chi_confidence * chi_square_head_gain, 0.0, 1.0);
        const double chi_square_head_sample = filtered_prev + alpha_chi_square * (input_value - filtered_prev);

        const double pareto_shape = 1.5;
        const double scaled_residual = std::fabs(input_value - filtered_prev) / (std::sqrt(sigma2) + 1e-9);
        const double pareto_weight = std::pow(1.0 + scaled_residual, -pareto_shape);
        const double alpha_pareto = std::clamp(alpha_min + (alpha_max - alpha_min) * pareto_weight * pareto_head_gain, 0.0, 1.0);
        const double pareto_head_sample = filtered_prev + alpha_pareto * (input_value - filtered_prev);

        const double predictive_error = std::fabs(input_value - predictive_head_sample);
        if (is_rotation)
            rot_predictive_error_sum += predictive_error;
        else
            pos_predictive_error_sum += predictive_error;

        add_head(ema_enabled, ema_head_sample, true, false, false, false, false, false);
        add_head(brownian_enabled, brownian_head_sample, false, true, false, false, false, false);
        add_head(adaptive_mode, adaptive_head_sample, false, false, true, false, false, false);
        add_head(predictive_enabled, predictive_head_sample, false, false, false, true, false, false);
        add_head(chi_square_enabled, chi_square_head_sample, false, false, false, false, true, false);
        add_head(pareto_enabled, pareto_head_sample, false, false, false, false, false, true);

        double ema_share = 0.0;
        double brownian_share = 0.0;
        double adaptive_share = 0.0;
        double predictive_share = 0.0;
        double chi_square_share = 0.0;
        double pareto_share = 0.0;
        const renyi_bin_state& rs_prior = is_rotation ? rot_rs_prior : pos_rs_prior;
        const double shoulder_gain = mtm_enabled ?
            std::clamp(ctl.effective_shoulder(rs_prior) + (1.0 - ctl.effective_shoulder(rs_prior)) * mode_e, 0.0, 1.0) :
            0.0;
        double composed_output = input_value;

        if (head_count == 0)
        {
            composed_output = input_value;
        }
        else if (!mtm_enabled || head_count == 1)
        {
            heads[0].weight = 1.0;
            composed_output = heads[0].sample;
            if (heads[0].is_ema)
                ema_share = 1.0;
            if (heads[0].is_brownian)
                brownian_share = 1.0;
            if (heads[0].is_adaptive)
                adaptive_share = 1.0;
            if (heads[0].is_predictive)
                predictive_share = 1.0;
            if (heads[0].is_chi_square)
                chi_square_share = 1.0;
            if (heads[0].is_pareto)
                pareto_share = 1.0;
        }
        else
        {
            // Rényi neck: true Renyi/Tsallis likelihood + NGC residual coupling.
            const double alpha = 0.1 + 24.9 * mode_e; // [0.1, 25.0]

            double sum = 0.0;
            for (int h = 0; h < head_count; h++)
            {
                double residual = input_value - heads[h].sample;

                if (!is_rotation)
                {
                    if (i == TX || i == TY)
                        residual += coupling_residual * 0.7;
                    else if (i == TZ)
                        residual += coupling_residual * 1.3;
                }

                const double mahalanobis_sq =
                    (residual * residual) / sigma2 + coupling_residual * coupling_residual;
                const double likelihood = renyi_tsallis_likelihood(mahalanobis_sq, alpha);
                heads[h].weight = likelihood;
                sum += heads[h].weight;
            }

            if (sum <= 1e-15)
            {
                const double uniform_w = 1.0 / static_cast<double>(head_count);
                for (int h = 0; h < head_count; h++)
                    heads[h].weight = uniform_w;
            }
            else
            {
                for (int h = 0; h < head_count; h++)
                    heads[h].weight /= sum;
            }

            // MTM shoulder composes the neck-normalized heads and controls
            // how strongly we trust the composed estimate this frame.
            double hydra_sample = 0.0;
            for (int h = 0; h < head_count; h++)
            {
                hydra_sample += heads[h].weight * heads[h].sample;
                if (heads[h].is_ema)
                    ema_share += heads[h].weight;
                if (heads[h].is_brownian)
                    brownian_share += heads[h].weight;
                if (heads[h].is_adaptive)
                    adaptive_share += heads[h].weight;
                if (heads[h].is_predictive)
                    predictive_share += heads[h].weight;
                if (heads[h].is_chi_square)
                    chi_square_share += heads[h].weight;
                if (heads[h].is_pareto)
                    pareto_share += heads[h].weight;
            }

            composed_output = filtered_prev + shoulder_gain * (hydra_sample - filtered_prev);
        }

        if (!mtm_enabled && head_count > 1)
        {
            // deterministic no-MTM path: EMA -> Brownian -> Adaptive -> Predictive.
            const bool use_ema = ema_enabled;
            const bool use_brownian = !use_ema && brownian_enabled;
            const bool use_adaptive = !use_ema && !use_brownian && adaptive_mode;
            const bool use_predictive = !use_ema && !use_brownian && !use_adaptive && predictive_enabled;
            ema_share = use_ema ? 1.0 : 0.0;
            brownian_share = use_brownian ? 1.0 : 0.0;
            adaptive_share = use_adaptive ? 1.0 : 0.0;
            predictive_share = use_predictive ? 1.0 : 0.0;
            const bool use_chi_square = !use_ema && !use_brownian && !use_adaptive && !use_predictive && chi_square_enabled;
            const bool use_pareto = !use_ema && !use_brownian && !use_adaptive && !use_predictive && !use_chi_square && pareto_enabled;
            chi_square_share = use_chi_square ? 1.0 : 0.0;
            pareto_share = use_pareto ? 1.0 : 0.0;
            if (use_ema)
                composed_output = ema_head_sample;
            else if (use_brownian)
                composed_output = brownian_head_sample;
            else if (use_adaptive)
                composed_output = adaptive_head_sample;
            else if (use_predictive)
                composed_output = predictive_head_sample;
            else if (use_chi_square)
                composed_output = chi_square_head_sample;
            else if (use_pareto)
                composed_output = pareto_head_sample;
            else
                composed_output = input_value;
        }

        const auto current_head = dominant_head_from_shares(
            ema_share,
            brownian_share,
            adaptive_share,
            predictive_share,
            chi_square_share,
            pareto_share);
        const auto previous_head = last_dominant_head[i];
        last_dominant_head[i] = current_head;

        const double sigma = std::sqrt(std::max(last_noise[i], 1e-8));
        const double normalized_residual = std::min(std::fabs(input_value - composed_output) / sigma, 4.0);
        if (is_position)
        {
            pos_residual_sum += normalized_residual;
            if (current_head != previous_head)
                pos_head_transition_count++;
        }

        if (mtm_enabled)
        {
            const double innovation_debt = 0.05 * normalized_residual;
            if (is_rotation)
                temporal_state.add_rot_debt(previous_head, current_head, innovation_debt);
            else
                temporal_state.add_pos_debt(previous_head, current_head, innovation_debt);
        }

        double coupling_constant = 0.0;
        if (mtm_enabled)
        {
            const double total_debt = evaluate_total_topological_debt(temporal_state, is_rotation);
            const double max_capacity = temporal_state.max_capacity.load(std::memory_order_relaxed);
            coupling_constant = calculate_coupling_constant(total_debt, max_capacity);
        }

        // Anti-inertia gating: reduce stiff shoulder locking when budget is depleted.
        if (is_position)
            coupling_constant *= (0.35 + 0.65 * anti_inertia_budget_ratio);

        // Bounded correction before amortization for unexpected position branch changes.
        double corrected_composed_output = composed_output;
        if (is_position)
        {
            const double residual = input_value - composed_output;
            const double anomaly_drive = std::clamp(
                0.6 * normalized_residual + 0.3 * coupling_residual_jump + 0.1 * (current_head != previous_head ? 1.0 : 0.0),
                0.0,
                4.0);
            const double corr_factor = std::clamp(invariant_correction_gain * anti_inertia_budget_ratio * anomaly_drive / 4.0, 0.0, 1.0);
            corrected_composed_output += residual * corr_factor;
        }

        const double stabilized_output = apply_amortization(filtered_prev, corrected_composed_output, coupling_constant);

        last_output[i] = stabilized_output;
        output[i] = last_output[i];

        double prediction_delta = last_output[i] - filtered_prev;
        if (is_rotation && std::fabs(prediction_delta) > half_turn)
        {
            const double wrap = std::copysign(full_turn, prediction_delta);
            prediction_delta -= wrap;
        }
        const double predicted_velocity = prediction_delta / safe_dt;
        
        // Predictive head: incorporate high-rate gyro integration for rotation axes
        if (is_rotation && has_highrate_source && (i == Yaw || i == Pitch || i == Roll))
        {
            // Use gyro-integrated rotation instead of velocity extrapolation
            const int gyro_idx = i - 3; // Map Yaw/Pitch/Roll to gyro_integrated_rotation[0/1/2]
            predicted_next_output[i] = last_output[i] + gyro_integrated_rotation[gyro_idx];
            // Reset accumulator after use
            gyro_integrated_rotation[gyro_idx] = 0.0;
        }
        else if (is_position)
        {
            const int axis = i;
            predicted_next_output[i] = translation_prediction[axis];
            pos_predictive_translation_error_sum += std::fabs(input_value - translation_prediction[axis]);
        }
        else
        {
            predicted_next_output[i] = last_output[i] + predicted_velocity * safe_dt;
        }

        double filtered_delta = last_output[i] - filtered_prev;
        if (is_rotation && std::fabs(filtered_delta) > half_turn)
        {
            const double wrap = std::copysign(full_turn, filtered_delta);
            filtered_delta -= wrap;
        }

        raw_brownian_energy[i] += brownian_alpha * (raw_delta * raw_delta - raw_brownian_energy[i]);
        filtered_brownian_energy[i] +=
            brownian_alpha * (filtered_delta * filtered_delta - filtered_brownian_energy[i]);

        if (is_rotation)
        {
            rot_brownian_raw_sum += std::sqrt(std::max(raw_brownian_energy[i], 0.0));
            rot_brownian_filtered_sum += std::sqrt(std::max(filtered_brownian_energy[i], 0.0));
            rot_ema_drive_sum += ema_share;
            rot_brownian_drive_sum += brownian_share;
            rot_adaptive_drive_sum += adaptive_share;
            rot_predictive_drive_sum += predictive_share;
            rot_chi_square_drive_sum += chi_square_share;
            rot_pareto_drive_sum += pareto_share;
            rot_mtm_drive_sum += shoulder_gain;
        }
        else
        {
            pos_brownian_raw_sum += std::sqrt(std::max(raw_brownian_energy[i], 0.0));
            pos_brownian_filtered_sum += std::sqrt(std::max(filtered_brownian_energy[i], 0.0));
            pos_ema_drive_sum += ema_share;
            pos_brownian_drive_sum += brownian_share;
            pos_adaptive_drive_sum += adaptive_share;
            pos_predictive_drive_sum += predictive_share;
            pos_chi_square_drive_sum += chi_square_share;
            pos_pareto_drive_sum += pareto_share;
            pos_mtm_drive_sum += shoulder_gain;
        }

        last_input[i] = raw_input_value;
    }

    const double rot_brownian_raw_avg = rot_brownian_raw_sum / 3.0;
    const double rot_brownian_filtered_avg = rot_brownian_filtered_sum / 3.0;
    const double rot_predictive_error_avg = rot_predictive_error_sum / 3.0;
    const double pos_brownian_raw_avg = pos_brownian_raw_sum / 3.0;
    const double pos_brownian_filtered_avg = pos_brownian_filtered_sum / 3.0;
    const double pos_predictive_error_avg = pos_predictive_error_sum / 3.0;
    const double rot_brownian_delta_avg = rot_brownian_raw_avg - rot_brownian_filtered_avg;
    const double pos_brownian_delta_avg = pos_brownian_raw_avg - pos_brownian_filtered_avg;
    const double rot_ema_drive_avg = rot_ema_drive_sum / 3.0;
    const double rot_brownian_drive_avg = rot_brownian_drive_sum / 3.0;
    const double rot_adaptive_drive_avg = rot_adaptive_drive_sum / 3.0;
    const double rot_predictive_drive_avg = rot_predictive_drive_sum / 3.0;
    const double rot_chi_square_drive_avg = rot_chi_square_drive_sum / 3.0;
    const double rot_pareto_drive_avg = rot_pareto_drive_sum / 3.0;
    const double rot_mtm_drive_avg = rot_mtm_drive_sum / 3.0;
    const double pos_ema_drive_avg = pos_ema_drive_sum / 3.0;
    const double pos_brownian_drive_avg = pos_brownian_drive_sum / 3.0;
    const double pos_adaptive_drive_avg = pos_adaptive_drive_sum / 3.0;
    const double pos_predictive_drive_avg = pos_predictive_drive_sum / 3.0;
    const double pos_chi_square_drive_avg = pos_chi_square_drive_sum / 3.0;
    const double pos_pareto_drive_avg = pos_pareto_drive_sum / 3.0;
    const double pos_mtm_drive_avg = pos_mtm_drive_sum / 3.0;
    const double pos_predictive_translation_error_avg = pos_predictive_translation_error_sum / 3.0;

    // Phase 3 anomaly score + anti-inertia budget dynamics.
    const double pos_residual_avg = pos_residual_sum / 3.0;
    const double pos_head_transition_ratio = std::clamp(pos_head_transition_count / 3.0, 0.0, 1.0);
    anomaly_score = std::clamp(
        0.6 * pos_residual_avg + 0.25 * coupling_residual_jump + 0.15 * pos_head_transition_ratio,
        0.0,
        10.0);

    if (anomaly_score > anomaly_threshold)
    {
        const double consume = (anomaly_score - anomaly_threshold) * safe_dt;
        anti_inertia_budget = std::max(0.0, anti_inertia_budget - consume);
        anomaly_active = true;
        anomaly_cooldown_frames = 6;
    }
    else
    {
        anti_inertia_budget = std::min(anti_inertia_budget_max,
                                       anti_inertia_budget + anti_inertia_recovery_rate * safe_dt * anti_inertia_budget_max);
        if (anomaly_cooldown_frames > 0)
            anomaly_cooldown_frames--;
        anomaly_active = anomaly_cooldown_frames > 0;
    }

    last_coupling_residual = coupling_residual;

    // Update translational predictor state from resolved output.
    {
        const double old_x = translation_state.x;
        const double old_y = translation_state.y;
        const double old_z = translation_state.z;
        translation_state.x = last_output[TX];
        translation_state.y = last_output[TY];
        translation_state.z = last_output[TZ];
        translation_state.vx = (translation_state.x - old_x) / safe_dt;
        translation_state.vy = (translation_state.y - old_y) / safe_dt;
        translation_state.vz = (translation_state.z - old_z) / safe_dt;
    }

    const double rot_brownian_damped =
        rot_brownian_raw_avg > 1e-12 ?
            std::clamp(1.0 - rot_brownian_filtered_avg / rot_brownian_raw_avg, -1.0, 1.0) : 0.0;
    const double pos_brownian_damped =
        pos_brownian_raw_avg > 1e-12 ?
            std::clamp(1.0 - pos_brownian_filtered_avg / pos_brownian_raw_avg, -1.0, 1.0) : 0.0;

    double rot_mode_e = 0.0;
    double pos_mode_e = 0.0;
    double rot_mode_peak = 0.0;
    double pos_mode_peak = 0.0;
    const double rot_objective_avg = rot_objective_sum / 3.0;
    const double pos_objective_avg = pos_objective_sum / 3.0;
    if (mtm_enabled)
    {
        // Entropy-native coupling: chi-square and pareto heads contribute evidence
        // into the Renyi diffusion update, not only the hydra output blend.
        const double rot_entropy_stable = joint_evidence_stable(rot_chi_square_drive_avg, rot_pareto_drive_avg);
        const double rot_entropy_outlier = joint_evidence_outlier(rot_chi_square_drive_avg, rot_pareto_drive_avg);
        const double pos_entropy_stable = joint_evidence_stable(pos_chi_square_drive_avg, pos_pareto_drive_avg);
        const double pos_entropy_outlier = joint_evidence_outlier(pos_chi_square_drive_avg, pos_pareto_drive_avg);

        double rot_measurement = brownian_enabled ?
            clamp01(0.55 * rot_objective_avg +
                0.20 * std::max(rot_brownian_damped, 0.0) +
                0.25 * rot_entropy_stable) :
            clamp01(0.75 * rot_objective_avg +
                0.25 * rot_entropy_stable);
        double pos_measurement = brownian_enabled ?
            clamp01(0.55 * pos_objective_avg +
                0.20 * std::max(pos_brownian_damped, 0.0) +
                0.25 * pos_entropy_stable) :
            clamp01(0.75 * pos_objective_avg +
                0.25 * pos_entropy_stable);

        // Entropy-space outlier quarantine: attenuate occupancy updates when
        // chi-square evidence indicates a likely spike/outlier frame.
        const double rot_quarantine = outlier_quarantine_enabled ? clamp01(outlier_quarantine_strength * rot_entropy_outlier) : 0.0;
        const double pos_quarantine = outlier_quarantine_enabled ? clamp01(outlier_quarantine_strength * pos_entropy_outlier) : 0.0;
        rot_measurement = rot_measurement + rot_quarantine * (rot_mode_e_prior - rot_measurement);
        pos_measurement = pos_measurement + pos_quarantine * (pos_mode_e_prior - pos_measurement);

        update_modes_from_measurement(rot_mode_prob, rot_measurement);
        update_modes_from_measurement(pos_mode_prob, pos_measurement);

        // Semantic bins: T2/T3 from joint stable evidence.
        inject_bin_mass(rot_mode_prob, 5, 0.10 * rot_entropy_stable);
        inject_bin_mass(rot_mode_prob, 6, 0.10 * rot_entropy_stable);
        inject_bin_mass(pos_mode_prob, 5, 0.10 * pos_entropy_stable);
        inject_bin_mass(pos_mode_prob, 6, 0.10 * pos_entropy_stable);
        // During quarantine pressure, route mass into pathology/quarantine bins.
        inject_bin_mass(rot_mode_prob, 9, 0.08 * rot_quarantine);  // P2 quarantine
        inject_bin_mass(pos_mode_prob, 9, 0.08 * pos_quarantine);  // P2 quarantine
        inject_bin_mass(rot_mode_prob, 10, 0.04 * rot_quarantine); // P3 true-death
        inject_bin_mass(pos_mode_prob, 10, 0.04 * pos_quarantine); // P3 true-death

        rot_mode_e = mode_expectation(rot_mode_prob);
        pos_mode_e = mode_expectation(pos_mode_prob);
        rot_mode_peak = mode_peak_center(rot_mode_prob);
        pos_mode_peak = mode_peak_center(pos_mode_prob);

        // Continuous debt resolution keeps stiffness bounded and self-recovering.
        static constexpr double amortization_rate = 0.95;
        decay_transition_debt(temporal_state, amortization_rate);
    }

    // Stage 4: publish status for the settings panel.
    // If MTM is disabled, objective falls back to innovation objective.
    status.rot_objective.store(mtm_enabled ? rot_mode_e : rot_objective_avg, std::memory_order_relaxed);
    status.pos_objective.store(mtm_enabled ? pos_mode_e : pos_objective_avg, std::memory_order_relaxed);
    status.rot_jitter.store(rot_jitter_sum / 3.0, std::memory_order_relaxed);
    status.pos_jitter.store(pos_jitter_sum / 3.0, std::memory_order_relaxed);
    status.rot_brownian_raw.store(rot_brownian_raw_avg, std::memory_order_relaxed);
    status.rot_brownian_filtered.store(rot_brownian_filtered_avg, std::memory_order_relaxed);
    status.rot_brownian_delta.store(rot_brownian_delta_avg, std::memory_order_relaxed);
    status.rot_brownian_damped.store(rot_brownian_damped, std::memory_order_relaxed);
    status.rot_predictive_error.store(rot_predictive_error_avg, std::memory_order_relaxed);
    status.pos_brownian_raw.store(pos_brownian_raw_avg, std::memory_order_relaxed);
    status.pos_brownian_filtered.store(pos_brownian_filtered_avg, std::memory_order_relaxed);
    status.pos_brownian_delta.store(pos_brownian_delta_avg, std::memory_order_relaxed);
    status.pos_brownian_damped.store(pos_brownian_damped, std::memory_order_relaxed);
    status.pos_predictive_error.store(pos_predictive_error_avg, std::memory_order_relaxed);
    status.rot_ema_drive.store(rot_ema_drive_avg, std::memory_order_relaxed);
    status.rot_brownian_drive.store(rot_brownian_drive_avg, std::memory_order_relaxed);
    status.rot_adaptive_drive.store(rot_adaptive_drive_avg, std::memory_order_relaxed);
    status.rot_predictive_drive.store(rot_predictive_drive_avg, std::memory_order_relaxed);
    status.rot_chi_square_drive.store(rot_chi_square_drive_avg, std::memory_order_relaxed);
    status.rot_pareto_drive.store(rot_pareto_drive_avg, std::memory_order_relaxed);
    status.rot_mtm_drive.store(rot_mtm_drive_avg, std::memory_order_relaxed);
    status.pos_ema_drive.store(pos_ema_drive_avg, std::memory_order_relaxed);
    status.pos_brownian_drive.store(pos_brownian_drive_avg, std::memory_order_relaxed);
    status.pos_adaptive_drive.store(pos_adaptive_drive_avg, std::memory_order_relaxed);
    status.pos_predictive_drive.store(pos_predictive_drive_avg, std::memory_order_relaxed);
    status.pos_chi_square_drive.store(pos_chi_square_drive_avg, std::memory_order_relaxed);
    status.pos_pareto_drive.store(pos_pareto_drive_avg, std::memory_order_relaxed);
    status.pos_mtm_drive.store(pos_mtm_drive_avg, std::memory_order_relaxed);
    status.rot_mode_expectation.store(rot_mode_e, std::memory_order_relaxed);
    status.pos_mode_expectation.store(pos_mode_e, std::memory_order_relaxed);
    status.rot_mode_peak.store(rot_mode_peak, std::memory_order_relaxed);
    status.pos_mode_peak.store(pos_mode_peak, std::memory_order_relaxed);
    status.rot_mode_purity.store(compute_renyi_state(rot_mode_prob).purity, std::memory_order_relaxed);
    status.pos_mode_purity.store(compute_renyi_state(pos_mode_prob).purity, std::memory_order_relaxed);
    status.ngc_coupling_residual.store(coupling_residual, std::memory_order_relaxed);
    status.rot_alpha_min.store(*s.rot_alpha_min, std::memory_order_relaxed);
    status.rot_alpha_max.store(*s.rot_alpha_max, std::memory_order_relaxed);
    status.rot_curve.store(*s.rot_curve, std::memory_order_relaxed);
    status.rot_deadzone.store(*s.rot_deadzone, std::memory_order_relaxed);
    status.pos_alpha_min.store(*s.pos_alpha_min, std::memory_order_relaxed);
    status.pos_alpha_max.store(*s.pos_alpha_max, std::memory_order_relaxed);
    status.pos_curve.store(*s.pos_curve, std::memory_order_relaxed);
    status.pos_deadzone.store(*s.pos_deadzone, std::memory_order_relaxed);
    status.anti_inertia_budget.store(anti_inertia_budget, std::memory_order_relaxed);
    status.anomaly_score.store(anomaly_score, std::memory_order_relaxed);
    status.outlier_quarantine_activity.store(
        outlier_quarantine_enabled ?
            0.5 * (joint_evidence_outlier(rot_chi_square_drive_avg, rot_pareto_drive_avg) +
                   joint_evidence_outlier(pos_chi_square_drive_avg, pos_pareto_drive_avg)) *
                outlier_quarantine_strength
            : 0.0,
        std::memory_order_relaxed);
    status.pos_predictive_translation_error.store(pos_predictive_translation_error_avg, std::memory_order_relaxed);
    status.invariant_correction_magnitude.store(invariant_correction_magnitude, std::memory_order_relaxed);
    status.anomaly_active.store(anomaly_active, std::memory_order_relaxed);
    const double budget_ratio = std::clamp(anti_inertia_budget / std::max(anti_inertia_budget_max, 1e-9), 0.0, 1.0);
    const bool quality_continuity = *s.quality_continuity;
    const bool quality_decisiveness = *s.quality_decisiveness;
    const bool quality_recovery = *s.quality_recovery_pace;
    const double bad_pair_drive = (quality_continuity && quality_decisiveness) ? 1.0 : 0.0;
    const double good_pair_relief = (quality_decisiveness && quality_recovery) ? 0.25 : 0.0;
    const double conflict_drive = std::clamp(bad_pair_drive - good_pair_relief, 0.0, 1.0);
    const double pathology_drive = std::clamp(0.35 * anomaly_score / std::max(anomaly_threshold, 1e-9) + (anomaly_active ? 0.4 : 0.0), 0.0, 1.0);
    for (int i = 0; i < mode_count; i++)
    {
        status.rot_bin_prob[i].store(rot_mode_prob[i], std::memory_order_relaxed);
        status.pos_bin_prob[i].store(pos_mode_prob[i], std::memory_order_relaxed);
        status.rot_bin_delta[i].store(std::fabs(rot_mode_prob[i] - last_rot_mode_prob_snapshot[i]), std::memory_order_relaxed);
        status.pos_bin_delta[i].store(std::fabs(pos_mode_prob[i] - last_pos_mode_prob_snapshot[i]), std::memory_order_relaxed);

        double conflict_weight = 0.05;
        if (i == 1) conflict_weight = 0.55;      // B2 plausibility gate
        if (i == 6) conflict_weight = 0.85;      // T3 decisive
        if (i == 7) conflict_weight = 0.95;      // T4 continuity
        if (i == 8) conflict_weight = 0.60;      // P1 pathology detect
        status.rot_bin_conflict[i].store(conflict_drive * conflict_weight * rot_mode_prob[i], std::memory_order_relaxed);
        status.pos_bin_conflict[i].store(conflict_drive * conflict_weight * pos_mode_prob[i], std::memory_order_relaxed);

        double pathology_weight = 0.05;
        if (i == 8) pathology_weight = 1.00;     // P1 pathology detect
        if (i == 9) pathology_weight = 0.75 * (1.0 - budget_ratio); // P2 quarantine
        if (i == 10) pathology_weight = anomaly_active ? 0.9 : 0.15; // P3 true-death
        if (i == 11) pathology_weight = (!anomaly_active) ? 0.7 * (1.0 - budget_ratio) : 0.10; // P4 re-entry
        if (i == 3) pathology_weight = 0.35;     // B4 recovery wall
        status.rot_bin_pathology[i].store(pathology_drive * pathology_weight, std::memory_order_relaxed);
        status.pos_bin_pathology[i].store(pathology_drive * pathology_weight, std::memory_order_relaxed);
    }
    last_rot_mode_prob_snapshot = rot_mode_prob;
    last_pos_mode_prob_snapshot = pos_mode_prob;
}

static const char* const alpha_diag_names[] = {
    "anomaly_score",
    "anti_inertia_budget",
    "anomaly_active",
    "predictive_translation_error",
    "invariant_correction_mag",
};
static constexpr int alpha_diag_count = (int)std::size(alpha_diag_names);

int alpha_spectrum::diagnostics_names(const char** buf, int maxn)
{
    const int n = std::min(alpha_diag_count, maxn);
    for (int i = 0; i < n; ++i)
        buf[i] = alpha_diag_names[i];
    return n;
}

int alpha_spectrum::diagnostics(double* buf, int maxn)
{
    if (maxn < alpha_diag_count)
        return 0;
    const auto& status = detail::alpha_spectrum::shared_calibration_status();
    buf[0] = status.anomaly_score.load(std::memory_order_relaxed);
    buf[1] = status.anti_inertia_budget.load(std::memory_order_relaxed);
    buf[2] = status.anomaly_active.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    buf[3] = status.pos_predictive_translation_error.load(std::memory_order_relaxed);
    buf[4] = status.invariant_correction_magnitude.load(std::memory_order_relaxed);
    return alpha_diag_count;
}

OPENTRACK_DECLARE_FILTER(alpha_spectrum, dialog_alpha_spectrum, alpha_spectrumDll)
