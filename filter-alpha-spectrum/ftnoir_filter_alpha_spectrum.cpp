#include "ftnoir_filter_alpha_spectrum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>
#include <QDebug>

namespace as  = detail::alpha_spectrum;
namespace gov = detail::alpha_spectrum::governance;
constexpr auto mo = std::memory_order_relaxed;
using as::rp;

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
static constexpr int hydra_head_capacity_local = 6;
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
    const as::temporal_economy_state& state,
    bool rotation)
{
    const auto& debt = rotation ? state.rot_transition_debt : state.pos_transition_debt;
    double total = 0.0;
    for (size_t i = 0; i < as::transition_matrix_size; i++)
        total += std::max(0.0, debt[i].load(mo));
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

static as::tracking_head dominant_head_from_shares(const std::array<double, as::tracking_head_count>& shares)
{
    size_t best = 0;
    for (size_t i = 1; i < shares.size(); i++)
        if (shares[i] > shares[best]) best = i;
    return static_cast<as::tracking_head>(best);
}

static void decay_transition_debt(as::temporal_economy_state& state, double rate)
{
    const double clamped = std::clamp(rate, 0.0, 1.0);
    for (size_t i = 0; i < as::transition_matrix_size; i++)
    {
        const double pos = state.pos_transition_debt[i].load(mo);
        const double rot = state.rot_transition_debt[i].load(mo);
        state.pos_transition_debt[i].store(pos * clamped, mo);
        state.rot_transition_debt[i].store(rot * clamped, mo);
    }
}

// Per-head Q scale factors (process noise relative to sigma2).
// Larger Q = responds faster to motion; smaller Q = more stable at rest.
static constexpr std::array<double, as::tracking_head_count> head_Q_scale = {
    0.005,  // ema       — very stable
    0.10,   // brownian  — follows motion energy bursts
    0.020,  // adaptive  — moderate
    0.030,  // predictive — moderate (gyro-assisted prior)
    0.010,  // chi_square — conservative (statistical gate)
    0.15,   // pareto    — fast / heavy-tail saccade catch
};

// IMM transition matrix: imm_mtm[from][to].  Each row sums to 1.0.
// Pareto column receives extra probability mass as the saccade-reset lane.
static constexpr std::array<std::array<double, as::tracking_head_count>, as::tracking_head_count> imm_mtm = {{
    {0.70, 0.05, 0.05, 0.05, 0.05, 0.10},  // from ema
    {0.05, 0.70, 0.05, 0.05, 0.05, 0.10},  // from brownian
    {0.05, 0.05, 0.70, 0.05, 0.05, 0.10},  // from adaptive
    {0.05, 0.05, 0.05, 0.70, 0.05, 0.10},  // from predictive
    {0.05, 0.05, 0.05, 0.05, 0.70, 0.10},  // from chi_square
    {0.04, 0.04, 0.04, 0.04, 0.04, 0.80},  // from pareto (saccade reset)
}};

static as::imm_head_state imm_head_predict(
    const as::imm_head_state& s, double dt, double Q_pos, double Q_vel)
{
    as::imm_head_state p;
    p.x    = s.x + s.v * dt;
    p.v    = s.v;
    p.P_xx = s.P_xx + dt * (2.0 * s.P_xv + dt * s.P_vv) + Q_pos;
    p.P_xv = s.P_xv + dt * s.P_vv;
    p.P_vv = s.P_vv + Q_vel;
    return p;
}

static as::imm_head_state imm_head_update(
    const as::imm_head_state& s, double measurement, double R)
{
    const double S = s.P_xx + R;
    if (S < 1e-15) return s;
    const double K_x   = s.P_xx / S;
    const double K_v   = s.P_xv / S;
    const double innov = measurement - s.x;
    as::imm_head_state u;
    u.x    = s.x + K_x * innov;
    u.v    = s.v + K_v * innov;
    u.P_xx = (1.0 - K_x) * s.P_xx;
    u.P_xv = (1.0 - K_x) * s.P_xv;
    u.P_vv = s.P_vv - K_v * s.P_xv;
    return u;
}

struct neck_head_candidate final
{
    bool enabled = false;
    as::tracking_head kind = as::tracking_head::ema;
    double sample = 0.0;
    double evidence = 0.0;
    double alpha = 1.0;  // per-head Rényi α for likelihood geometry
};

struct renyi_neck_edge final
{
    as::tracking_head head = as::tracking_head::ema;
    double sample = 0.0;
    double residual = 0.0;
    double mahalanobis_sq = 0.0;
    double likelihood = 0.0;
    double normalized_weight = 0.0;
    double evidence = 0.0;
    uint32_t test_mask = 0u;
    bool tests_pass = false;
};

struct neck_compose_result final
{
    double hydra_sample = 0.0;
    std::array<double, as::tracking_head_count> shares {};
    double weight_sum_error = 0.0;
    double invalid_edge_ratio = 0.0;
    std::array<renyi_neck_edge, hydra_head_capacity_local> edges {};
};

static uint32_t evaluate_neck_edge_tests(const renyi_neck_edge& e, double sigma2)
{
    using gov::neck_edge_test;

    uint32_t mask_bits = 0u;
    const double safe_sigma2 = std::max(sigma2, 1e-9);
    if (std::isfinite(e.sample))
        mask_bits |= gov::mask(neck_edge_test::finite_sample);
    if (std::isfinite(e.evidence))
        mask_bits |= gov::mask(neck_edge_test::finite_evidence);
    if (e.evidence >= 0.0 && e.evidence <= 1.0)
        mask_bits |= gov::mask(neck_edge_test::evidence_range);
    if (std::isfinite(e.likelihood))
        mask_bits |= gov::mask(neck_edge_test::finite_likelihood);
    if (e.likelihood >= 0.0)
        mask_bits |= gov::mask(neck_edge_test::nonnegative_likelihood);
    if (std::isfinite(e.normalized_weight))
        mask_bits |= gov::mask(neck_edge_test::finite_weight);
    if (e.normalized_weight >= 0.0 && e.normalized_weight <= 1.0)
        mask_bits |= gov::mask(neck_edge_test::weight_range);

    const double expected_mahalanobis = (e.residual * e.residual) / safe_sigma2;
    const double denom = std::max(1.0, std::fabs(expected_mahalanobis));
    const double err = std::fabs(e.mahalanobis_sq - expected_mahalanobis) / denom;
    if (std::isfinite(e.mahalanobis_sq) && err < 1e-6)
        mask_bits |= gov::mask(neck_edge_test::renyi_channel_consistency);

    return mask_bits;
}

static neck_compose_result compose_renyi_neck(
    const std::array<neck_head_candidate, hydra_head_capacity_local>& heads,
    int head_count,
    double input_value,
    double sigma2)
{
    neck_compose_result out;
    if (head_count <= 0)
        return out;

    double sum = 0.0;
    int invalid_edges = 0;
    for (int h = 0; h < head_count; h++)
    {
        const double sample = std::isfinite(heads[h].sample) ? heads[h].sample : input_value;
        const double residual = input_value - sample;
        const double mahalanobis_sq = (residual * residual) / std::max(sigma2, 1e-9);
        double likelihood = renyi_tsallis_likelihood(mahalanobis_sq, heads[h].alpha);
        if (!std::isfinite(likelihood) || likelihood < 0.0)
        {
            likelihood = 1e-9;
            invalid_edges++;
        }
        if (!std::isfinite(heads[h].sample))
            invalid_edges++;
        out.edges[h] = {heads[h].kind, sample, residual, mahalanobis_sq, likelihood, 0.0, heads[h].evidence, 0u, false};
        // Neck weight is likelihood gated by head evidence.
        // Raw likelihood is preserved in the edge for IMM weight update (uses pure Rényi L).
        sum += likelihood * std::max(heads[h].evidence, 1e-9);
    }

    if (sum <= 1e-15)
    {
        const double uniform_w = 1.0 / static_cast<double>(head_count);
        for (int h = 0; h < head_count; h++)
            out.edges[h].normalized_weight = uniform_w;
    }
    else
    {
        for (int h = 0; h < head_count; h++)
            out.edges[h].normalized_weight =
                (out.edges[h].likelihood * std::max(out.edges[h].evidence, 1e-9)) / sum;
    }

    for (int h = 0; h < head_count; h++)
    {
        const double w = out.edges[h].normalized_weight;
        out.edges[h].test_mask = evaluate_neck_edge_tests(out.edges[h], sigma2);
        out.edges[h].tests_pass =
            (out.edges[h].test_mask & gov::neck_edge_required_mask) ==
            gov::neck_edge_required_mask;
        out.hydra_sample += w * out.edges[h].sample;
        out.shares[size_t(out.edges[h].head)] += w;
    }

    if (!std::isfinite(out.hydra_sample))
    {
        out.hydra_sample = input_value;
        invalid_edges++;
    }

    double wsum = 0.0;
    for (int h = 0; h < head_count; h++)
        wsum += out.edges[h].normalized_weight;
    out.weight_sum_error = std::fabs(1.0 - wsum);
    out.invalid_edge_ratio = std::clamp(static_cast<double>(invalid_edges) / std::max(1, head_count), 0.0, 1.0);

    return out;
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
    std::array<double, as::quality_overlay_state::value_count> delta {};
};

static quality_overlay_snapshot capture_quality_overlay_snapshot()
{
    const auto& overlay = as::shared_quality_overlay_state();
    quality_overlay_snapshot q;
    q.active = overlay.active.load(mo);
    for (int i = 0; i < static_cast<int>(q.delta.size()); i++)
        q.delta[i] = overlay.delta[i].load(mo);
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

as::calibration_status& as::shared_calibration_status()
{
    static calibration_status status;
    return status;
}

as::quality_overlay_state& as::shared_quality_overlay_state()
{
    static quality_overlay_state state;
    return state;
}

namespace {

struct filter_frame_status {
    as::rp objective, jitter;
    as::rp brownian_raw, brownian_filtered, brownian_delta, brownian_damped, predictive_error;
    std::array<double, as::tracking_head_count> rot_drive {}, pos_drive {};
    as::rp mtm_drive;
    as::rp mode_expectation, mode_peak, mode_purity;
    double ngc_coupling_residual = 0.0;
    double rot_alpha_min = 0.0, rot_alpha_max = 0.0, rot_curve = 0.0, rot_deadzone = 0.0;
    double pos_alpha_min = 0.0, pos_alpha_max = 0.0, pos_curve = 0.0, pos_deadzone = 0.0;
    double anti_inertia_budget = 0.0;
    double anomaly_score = 0.0;
    double outlier_quarantine_activity = 0.0;
    double pos_predictive_translation_error = 0.0;
    double invariant_correction_magnitude = 0.0;
    as::rp neck_weight_error, neck_invalid_ratio;
    std::array<bool, gov::canvas_edge_count> edge_valid {};
    bool anomaly_active = false;
    std::array<double, as::calibration_status::bin_count> rot_bin_prob {}, pos_bin_prob {};
    std::array<double, as::calibration_status::bin_count> rot_bin_delta {}, pos_bin_delta {};
    std::array<double, as::calibration_status::bin_count> rot_bin_conflict {}, pos_bin_conflict {};
    std::array<double, as::calibration_status::bin_count> rot_bin_pathology {}, pos_bin_pathology {};
};

static void publish_status(as::calibration_status& st, const filter_frame_status& fs)
{
    st.objective.rot.store(fs.objective.rot, mo);
    st.objective.pos.store(fs.objective.pos, mo);
    st.jitter.rot.store(fs.jitter.rot, mo);
    st.jitter.pos.store(fs.jitter.pos, mo);
    st.brownian_raw.rot.store(fs.brownian_raw.rot, mo);
    st.brownian_raw.pos.store(fs.brownian_raw.pos, mo);
    st.brownian_filtered.rot.store(fs.brownian_filtered.rot, mo);
    st.brownian_filtered.pos.store(fs.brownian_filtered.pos, mo);
    st.brownian_delta.rot.store(fs.brownian_delta.rot, mo);
    st.brownian_delta.pos.store(fs.brownian_delta.pos, mo);
    st.brownian_damped.rot.store(fs.brownian_damped.rot, mo);
    st.brownian_damped.pos.store(fs.brownian_damped.pos, mo);
    st.predictive_error.rot.store(fs.predictive_error.rot, mo);
    st.predictive_error.pos.store(fs.predictive_error.pos, mo);
    for (size_t h = 0; h < as::tracking_head_count; ++h)
    {
        st.rot_drive[h].store(fs.rot_drive[h], mo);
        st.pos_drive[h].store(fs.pos_drive[h], mo);
    }
    st.mtm_drive.rot.store(fs.mtm_drive.rot, mo);
    st.mtm_drive.pos.store(fs.mtm_drive.pos, mo);
    st.mode_expectation.rot.store(fs.mode_expectation.rot, mo);
    st.mode_expectation.pos.store(fs.mode_expectation.pos, mo);
    st.mode_peak.rot.store(fs.mode_peak.rot, mo);
    st.mode_peak.pos.store(fs.mode_peak.pos, mo);
    st.mode_purity.rot.store(fs.mode_purity.rot, mo);
    st.mode_purity.pos.store(fs.mode_purity.pos, mo);
    st.ngc_coupling_residual.store(fs.ngc_coupling_residual, mo);
    st.rot_alpha_min.store(fs.rot_alpha_min, mo);
    st.rot_alpha_max.store(fs.rot_alpha_max, mo);
    st.rot_curve.store(fs.rot_curve, mo);
    st.rot_deadzone.store(fs.rot_deadzone, mo);
    st.pos_alpha_min.store(fs.pos_alpha_min, mo);
    st.pos_alpha_max.store(fs.pos_alpha_max, mo);
    st.pos_curve.store(fs.pos_curve, mo);
    st.pos_deadzone.store(fs.pos_deadzone, mo);
    st.anti_inertia_budget.store(fs.anti_inertia_budget, mo);
    st.anomaly_score.store(fs.anomaly_score, mo);
    st.outlier_quarantine_activity.store(fs.outlier_quarantine_activity, mo);
    st.pos_predictive_translation_error.store(fs.pos_predictive_translation_error, mo);
    st.invariant_correction_magnitude.store(fs.invariant_correction_magnitude, mo);
    st.neck_weight_error.rot.store(fs.neck_weight_error.rot, mo);
    st.neck_weight_error.pos.store(fs.neck_weight_error.pos, mo);
    st.neck_invalid_ratio.rot.store(fs.neck_invalid_ratio.rot, mo);
    st.neck_invalid_ratio.pos.store(fs.neck_invalid_ratio.pos, mo);
    for (size_t i = 0; i < fs.edge_valid.size(); ++i)
        st.edge_valid[i].store(fs.edge_valid[i], mo);
    st.anomaly_active.store(fs.anomaly_active, mo);
    for (size_t i = 0; i < fs.rot_bin_prob.size(); ++i)
    {
        st.rot_bin_prob[i].store(fs.rot_bin_prob[i], mo);
        st.pos_bin_prob[i].store(fs.pos_bin_prob[i], mo);
        st.rot_bin_delta[i].store(fs.rot_bin_delta[i], mo);
        st.pos_bin_delta[i].store(fs.pos_bin_delta[i], mo);
        st.rot_bin_conflict[i].store(fs.rot_bin_conflict[i], mo);
        st.pos_bin_conflict[i].store(fs.pos_bin_conflict[i], mo);
        st.rot_bin_pathology[i].store(fs.rot_bin_pathology[i], mo);
        st.pos_bin_pathology[i].store(fs.pos_bin_pathology[i], mo);
    }
}

} // namespace

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
        initialize_uniform(rot_mode_prob);
        initialize_uniform(pos_mode_prob);
        last_rot_mode_prob_snapshot = rot_mode_prob;
        last_pos_mode_prob_snapshot = pos_mode_prob;
        for (auto& x : temporal_state.pos_transition_debt)
            x.store(0.0, mo);
        for (auto& x : temporal_state.rot_transition_debt)
            x.store(0.0, mo);
        temporal_state.max_capacity.store(10.0, mo);
        last_dominant_head.fill(as::tracking_head::ema);
        for (int j = 0; j < axis_count; j++)
        {
            for (auto& hs : head_states[j])
                hs = {input[j], 0.0, 1.0, 0.0, 1.0};
            imm_weights[j].fill(1.0 / static_cast<double>(head_states[j].size()));
        }
        std::copy(input, input + axis_count, stillness_anchor);
        std::fill(escape_level, escape_level + axis_count, 1.0);  // Start fully escaped — no initial freeze.
        std::copy(last_output, last_output + axis_count, output);
        return;
    }

    const double dt = timer.elapsed_seconds();
    timer.start();
    const double safe_dt = std::min(dt_max, std::max(dt_min, dt));

    noise_rc = std::min(noise_rc + safe_dt, noise_rc_max);
    const double delta_alpha = safe_dt / (safe_dt + delta_rc);
    const double noise_alpha = safe_dt / (safe_dt + noise_rc);
    const double brownian_alpha = safe_dt / (safe_dt + brownian_rc);

    const runtime_filter_controls ctl = resolve_runtime_controls(s, capture_quality_overlay_snapshot());
    const bool adaptive_mode = ctl.adaptive_mode;
    const bool ema_enabled = ctl.ema_enabled;
    const bool brownian_enabled = ctl.brownian_enabled;
    const bool predictive_enabled = ctl.predictive_enabled;
    const bool chi_square_enabled = ctl.chi_square_enabled;
    const bool pareto_enabled = ctl.pareto_enabled;
    const bool outlier_quarantine_enabled = ctl.outlier_quarantine_enabled;
    auto& status = as::shared_calibration_status();
    status.active.store(true, mo);

    static bool logged_independent_pipeline = false;
    if (!logged_independent_pipeline)
    {
        qDebug() << "alpha-spectrum: pipeline boundary active (heads -> renyi neck -> shoulder)";
        logged_independent_pipeline = true;
    }

    double rot_mode_e_prior = 0.0;
    double pos_mode_e_prior = 0.0;
    renyi_bin_state rot_rs_prior;
    renyi_bin_state pos_rs_prior;
    diffuse_modes(rot_mode_prob, safe_dt);
    diffuse_modes(pos_mode_prob, safe_dt);
    rot_mode_e_prior = mode_expectation(rot_mode_prob);
    pos_mode_e_prior = mode_expectation(pos_mode_prob);
    rot_rs_prior = compute_renyi_state(rot_mode_prob);
    pos_rs_prior = compute_renyi_state(pos_mode_prob);

    const double brownian_head_gain = ctl.brownian_head_gain;
    const double rot_deadzone = *s.rot_deadzone;
    const double pos_deadzone = *s.pos_deadzone;
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

    double invariant_correction_magnitude_sum = 0.0;
    double pos_predictive_translation_error_sum = 0.0;

    // Stage 1: per-axis measurement processing.
    // - wrap handling for rotational continuity
    // - deadzone suppression
    // - innovation/noise estimation for adaptive alpha
    rp jitter_sum {}, objective_sum {};
    rp brownian_raw_sum {}, brownian_filtered_sum {}, predictive_error_sum {};
    std::array<double, as::tracking_head_count> rot_drive_sum {}, pos_drive_sum {};
    double rot_mtm_drive_sum = 0.0, pos_mtm_drive_sum = 0.0;
    rp neck_weight_error_sum {}, neck_invalid_ratio_sum {};
    std::array<bool, as::tracking_head_count> head_edge_seen {};
    std::array<bool, as::tracking_head_count> head_edge_valid {};
    head_edge_valid.fill(true);
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

        last_delta[i] += delta_alpha * (delta - last_delta[i]);

        const double innovation = last_delta[i] * last_delta[i];
        last_noise[i] = noise_alpha * innovation + (1.0 - noise_alpha) * last_noise[i];
        const double norm_innovation =
            last_noise[i] < 1e-12 ? 0.0 : std::min(innovation / (9.0 * last_noise[i]), 1.0);

        (is_rotation ? jitter_sum.rot    : jitter_sum.pos)    += std::sqrt(std::max(last_noise[i], 0.0));
        (is_rotation ? objective_sum.rot : objective_sum.pos) += norm_innovation;

        // Stage 2: per-head scalar Kalman predict + update.
        // Each head maintains its own (x, v, P) state; deadzone is not applied here.

        const double raw_brownian      = std::sqrt(std::max(raw_brownian_energy[i], 0.0));
        const double filtered_brownian = std::sqrt(std::max(filtered_brownian_energy[i], 0.0));
        // Brownian evidence = filtered/raw energy ratio.
        // HIGH when filter tracks genuine motion (energies match).
        // LOW when filter suppresses jitter (filtered << raw) — prevents noise amplification.
        const double tuned_brownian_drive =
            brownian_enabled && raw_brownian > 1e-12
                ? clamp01((filtered_brownian / raw_brownian) * brownian_head_gain)
                : 0.0;

        const double sigma2 = std::max(last_noise[i], 1e-8);
        const double R      = sigma2;  // measurement noise = running noise estimate

        // Evidence computed against filtered_prev as a fast approximation (pre-Kalman-predict).
        constexpr double chi_sq_thresh = 3.841458820694124;
        const double chi_sq_innov  = (input_value - filtered_prev) * (input_value - filtered_prev) /
                                     std::max(sigma2, 1e-9);
        const double chi_confidence = clamp01((chi_sq_thresh - chi_sq_innov) / chi_sq_thresh);
        const double pareto_scaled  = std::fabs(input_value - filtered_prev) /
                                      (std::sqrt(std::max(sigma2, 1e-9)) + 1e-9);
        // Pareto evidence: threshold-based saccade gate.
        // 0 below 1.5σ (normal motion), ramps to 1.0 at 3.5σ (large saccade).
        const double pareto_weight  = clamp01((pareto_scaled - 1.5) / 2.0);
        const double gate           = remap_with_threshold(norm_innovation, adaptive_threshold_lift);

        std::array<neck_head_candidate, hydra_head_capacity_local> heads {};
        int head_count = 0;

        // Predictive head is processed first (uses external kinematic prior injection).
        if (predictive_enabled && head_count < hydra_head_capacity_local)
        {
            const size_t hi = as::index(as::tracking_head::predictive);
            auto& hs = head_states[i][hi];
            const double blend = predictive_head_gain * std::min(safe_dt * 8.0, 1.0);
            hs.x += blend * (predicted_next_output[i] - hs.x);
            const double Q_pos = head_Q_scale[hi] * sigma2;
            hs = imm_head_predict(hs, safe_dt, Q_pos, 0.1 * Q_pos);
            hs = imm_head_update(hs, input_value, R);
            const double pred_conf = clamp01(1.0 - std::fabs(input_value - hs.x) /
                                             (3.0 * std::sqrt(sigma2)));
            heads[head_count++] = neck_head_candidate{true, as::tracking_head::predictive, hs.x,
                                   pred_conf, as::head_alpha_table[hi]};
        }

        const double predictive_error = head_count > 0
            ? std::fabs(input_value - heads[0].sample)
            : std::fabs(input_value - predicted_next_output[i]);
        (is_rotation ? predictive_error_sum.rot : predictive_error_sum.pos) += predictive_error;

        auto add_imm_head = [&](as::tracking_head kind, bool enabled, double evidence_val) {
            if (!enabled || head_count >= hydra_head_capacity_local) return;
            const size_t hi = as::index(kind);
            auto& hs = head_states[i][hi];
            const double Q_pos = head_Q_scale[hi] * sigma2;
            hs = imm_head_predict(hs, safe_dt, Q_pos, 0.1 * Q_pos);
            hs = imm_head_update(hs, input_value, R);
            heads[head_count++] = neck_head_candidate{true, kind, hs.x, clamp01(evidence_val),
                                   as::head_alpha_table[hi]};
        };

        add_imm_head(as::tracking_head::ema,        ema_enabled,         1.0);
        add_imm_head(as::tracking_head::brownian,   brownian_enabled,    tuned_brownian_drive);
        add_imm_head(as::tracking_head::adaptive,   adaptive_mode,       gate);
        add_imm_head(as::tracking_head::chi_square, chi_square_enabled,  chi_confidence * chi_square_head_gain);
        add_imm_head(as::tracking_head::pareto,     pareto_enabled,      pareto_weight * pareto_head_gain);

        std::array<double, as::tracking_head_count> shares {};
        const renyi_bin_state& rs_prior = is_rotation ? rot_rs_prior : pos_rs_prior;
        const double shoulder_gain = ctl.effective_shoulder(rs_prior);
        double composed_output = input_value;

        if (head_count == 0)
        {
            composed_output = input_value;
        }
        else
        {
            const neck_compose_result neck = compose_renyi_neck(heads, head_count, input_value, sigma2);

            // IMM weight update: μ_new[j] = L[j] * Σ_i(MTM[i][j] * μ_prev[i]), normalized.
            std::array<double, as::tracking_head_count> L {};
            for (int h = 0; h < head_count; h++)
                L[as::index(neck.edges[h].head)] = neck.edges[h].likelihood;
            double Lsum = 0.0;
            for (const double l : L) Lsum += l;
            if (Lsum < 1e-15) Lsum = 1.0;
            for (double& l : L) l /= Lsum;

            for (size_t j = 0; j < as::tracking_head_count; j++)
            {
                double mu_j = 0.0;
                for (size_t ii = 0; ii < as::tracking_head_count; ii++)
                    mu_j += imm_mtm[ii][j] * imm_weights[i][ii];
                imm_weights[i][j] = L[j] * mu_j;
            }
            double mu_sum = 0.0;
            for (const double m : imm_weights[i]) mu_sum += m;
            if (mu_sum < 1e-15)
                imm_weights[i].fill(1.0 / static_cast<double>(as::tracking_head_count));
            else
                for (double& m : imm_weights[i]) m /= mu_sum;
            shares = imm_weights[i];

            (is_rotation ? neck_weight_error_sum.rot : neck_weight_error_sum.pos) += neck.weight_sum_error;
            (is_rotation ? neck_invalid_ratio_sum.rot : neck_invalid_ratio_sum.pos) += neck.invalid_edge_ratio;

            for (int h = 0; h < head_count; h++)
            {
                const auto idx = static_cast<size_t>(neck.edges[h].head);
                if (idx >= head_edge_seen.size())
                    continue;
                head_edge_seen[idx] = true;
                head_edge_valid[idx] = head_edge_valid[idx] && neck.edges[h].tests_pass;
            }

            composed_output = filtered_prev + shoulder_gain * (neck.hydra_sample - filtered_prev);
        }

        // Divot sticktion — last-resort jitter catch.
        // Holds output absolutely still at stillness_anchor until evidence channels confirm
        // genuine intentional motion, then smoothly anti-inertia releases toward composed_output.
        // No binary threshold: escape_level is continuous → no stepping grid artifact.
        // dz=0 → disabled; dz>0 → sticktion strength (evidence required to escape the divot).
        {
            const double dz = is_rotation ? rot_deadzone : pos_deadzone;
            if (dz > 0.0)
            {
                // Composite motion confidence from all evidence channels (inclusive OR).
                // pareto_weight       : large saccade (>1.5σ) — escape immediately
                // gate                : above-noise adaptive innovation
                // tuned_brownian_drive: energy ratio HIGH when filter tracks genuine motion
                const double motion_conf = std::max({pareto_weight, gate, tuned_brownian_drive});

                // Stiction threshold scales with dz: dz=1→0.25, dz=2→0.50, dz=3→0.75.
                // Only motion confidence above this level contributes to escape.
                const double stiction_thresh = std::min(dz * 0.25, 0.85);
                const double mc_above        = clamp01(
                    (motion_conf - stiction_thresh) / std::max(1.0 - stiction_thresh, 1e-6));

                // Fast attack: strong evidence → release quickly.
                // Fast settle: motion ends → snap to divot at new position with no jump.
                const double alpha_attack = clamp01(1.0 - std::exp(-mc_above * 50.0 * safe_dt));
                const double alpha_settle = clamp01(1.0 - std::exp(-12.0 * safe_dt));
                const double alpha        = (mc_above > escape_level[i]) ? alpha_attack : alpha_settle;
                escape_level[i]           = clamp01(escape_level[i] + alpha * (mc_above - escape_level[i]));

                // Anchor tracks composed_output only when escaping (quadratic gate).
                // Freezes sharply at the divot bottom — zero drift when still.
                // When fully escaped anchor ≈ composed_output so re-settling lands at
                // the current true position with no discontinuity.
                const double eff_escape  = (escape_level[i] > 0.05) ? escape_level[i] : 0.0;
                stillness_anchor[i]     += eff_escape * eff_escape * (composed_output - stillness_anchor[i]);

                // Blend: escape_level=0 → frozen at anchor; escape_level=1 → full pass-through.
                // The gradual escape ramp provides the anti-inertia glide with no drag once free.
                composed_output = stillness_anchor[i] + escape_level[i] * (composed_output - stillness_anchor[i]);
            }
        }

        const auto current_head = dominant_head_from_shares(shares);
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

        const double innovation_debt = 0.05 * normalized_residual;
        if (is_rotation)
            temporal_state.add_rot_debt(previous_head, current_head, innovation_debt);
        else
            temporal_state.add_pos_debt(previous_head, current_head, innovation_debt);

        const double total_debt = evaluate_total_topological_debt(temporal_state, is_rotation);
        const double max_capacity = temporal_state.max_capacity.load(mo);
        double coupling_constant = calculate_coupling_constant(total_debt, max_capacity);

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

            // Coupling policy is applied strictly after neck/shoulder composition.
            const double axis_coupling_weight = (i == TZ) ? 1.0 : 0.5;
            const double coupling_post = invariant_correction_gain * coupling_residual * axis_coupling_weight;
            corrected_composed_output += std::copysign(coupling_post, residual);
            invariant_correction_magnitude_sum += std::fabs(coupling_post);
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
        
        if (is_position)
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

        (is_rotation ? brownian_raw_sum.rot      : brownian_raw_sum.pos)      += std::sqrt(std::max(raw_brownian_energy[i], 0.0));
        (is_rotation ? brownian_filtered_sum.rot : brownian_filtered_sum.pos) += std::sqrt(std::max(filtered_brownian_energy[i], 0.0));
        {
            auto& ds = is_rotation ? rot_drive_sum : pos_drive_sum;
            for (size_t h = 0; h < as::tracking_head_count; ++h) ds[h] += shares[h];
            (is_rotation ? rot_mtm_drive_sum : pos_mtm_drive_sum) += shoulder_gain;
        }

        last_input[i] = raw_input_value;
    }

    const auto avg3 = [](const rp& s) noexcept -> rp { return {s.rot / 3.0, s.pos / 3.0}; };
    const rp brownian_raw_avg      = avg3(brownian_raw_sum);
    const rp brownian_filtered_avg = avg3(brownian_filtered_sum);
    const rp predictive_error_avg  = avg3(predictive_error_sum);
    const rp brownian_delta_avg    = {brownian_raw_avg.rot - brownian_filtered_avg.rot,
                                      brownian_raw_avg.pos - brownian_filtered_avg.pos};
    std::array<double, as::tracking_head_count> rot_drive_avg, pos_drive_avg;
    for (size_t h = 0; h < as::tracking_head_count; ++h)
    {
        rot_drive_avg[h] = rot_drive_sum[h] / 3.0;
        pos_drive_avg[h] = pos_drive_sum[h] / 3.0;
    }
    const double rot_mtm_drive_avg = rot_mtm_drive_sum / 3.0;
    const double pos_mtm_drive_avg = pos_mtm_drive_sum / 3.0;
    const double rot_chi_square_drive_avg = rot_drive_avg[as::index(as::tracking_head::chi_square)];
    const double rot_pareto_drive_avg     = rot_drive_avg[as::index(as::tracking_head::pareto)];
    const double pos_chi_square_drive_avg = pos_drive_avg[as::index(as::tracking_head::chi_square)];
    const double pos_pareto_drive_avg     = pos_drive_avg[as::index(as::tracking_head::pareto)];
    const rp neck_weight_error_avg = avg3(neck_weight_error_sum);
    const rp neck_invalid_ratio_avg = avg3(neck_invalid_ratio_sum);
    const double pos_predictive_translation_error_avg = pos_predictive_translation_error_sum / 3.0;
    const double invariant_correction_magnitude = invariant_correction_magnitude_sum / 3.0;

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
        brownian_raw_avg.rot > 1e-12 ?
            std::clamp(1.0 - brownian_filtered_avg.rot / brownian_raw_avg.rot, -1.0, 1.0) : 0.0;
    const double pos_brownian_damped =
        brownian_raw_avg.pos > 1e-12 ?
            std::clamp(1.0 - brownian_filtered_avg.pos / brownian_raw_avg.pos, -1.0, 1.0) : 0.0;

    double rot_mode_e = 0.0;
    double pos_mode_e = 0.0;
    double rot_mode_peak = 0.0;
    double pos_mode_peak = 0.0;
    const rp objective_avg = avg3(objective_sum);
    // Entropy-native coupling: chi-square and pareto heads contribute evidence
    // into the Renyi diffusion update, not only the hydra output blend.
    const double rot_entropy_stable = joint_evidence_stable(rot_chi_square_drive_avg, rot_pareto_drive_avg);
    const double rot_entropy_outlier = joint_evidence_outlier(rot_chi_square_drive_avg, rot_pareto_drive_avg);
    const double pos_entropy_stable = joint_evidence_stable(pos_chi_square_drive_avg, pos_pareto_drive_avg);
    const double pos_entropy_outlier = joint_evidence_outlier(pos_chi_square_drive_avg, pos_pareto_drive_avg);

    double rot_measurement = brownian_enabled ?
        clamp01(0.55 * objective_avg.rot +
            0.20 * std::max(rot_brownian_damped, 0.0) +
            0.25 * rot_entropy_stable) :
        clamp01(0.75 * objective_avg.rot +
            0.25 * rot_entropy_stable);
    double pos_measurement = brownian_enabled ?
        clamp01(0.55 * objective_avg.pos +
            0.20 * std::max(pos_brownian_damped, 0.0) +
            0.25 * pos_entropy_stable) :
        clamp01(0.75 * objective_avg.pos +
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

    // Stage 4: collate frame status and publish to shared calibration_status.
    {
        const double rot_mode_purity   = compute_renyi_state(rot_mode_prob).purity;
        const double pos_mode_purity   = compute_renyi_state(pos_mode_prob).purity;
        const double outlier_quarantine_activity =
            outlier_quarantine_enabled
                ? 0.5 * (joint_evidence_outlier(rot_chi_square_drive_avg, rot_pareto_drive_avg) +
                         joint_evidence_outlier(pos_chi_square_drive_avg, pos_pareto_drive_avg)) *
                      outlier_quarantine_strength
                : 0.0;
        const bool quality_continuity   = *s.quality_continuity;
        const bool quality_decisiveness = *s.quality_decisiveness;
        const bool quality_recovery     = *s.quality_recovery_pace;
        const double bad_pair_drive   = (quality_continuity && quality_decisiveness) ? 1.0 : 0.0;
        const double good_pair_relief = (quality_decisiveness && quality_recovery)   ? 0.25 : 0.0;
        const double conflict_drive   = std::clamp(bad_pair_drive - good_pair_relief, 0.0, 1.0);
        const double budget_ratio     = std::clamp(anti_inertia_budget / std::max(anti_inertia_budget_max, 1e-9), 0.0, 1.0);
        const double pathology_drive  = std::clamp(0.35 * anomaly_score / std::max(anomaly_threshold, 1e-9) + (anomaly_active ? 0.4 : 0.0), 0.0, 1.0);

        filter_frame_status fs;
        fs.objective         = {rot_mode_e, pos_mode_e};
        fs.jitter            = {jitter_sum.rot / 3.0, jitter_sum.pos / 3.0};
        fs.brownian_raw      = brownian_raw_avg;
        fs.brownian_filtered = brownian_filtered_avg;
        fs.brownian_delta    = brownian_delta_avg;
        fs.brownian_damped   = {rot_brownian_damped, pos_brownian_damped};
        fs.predictive_error  = predictive_error_avg;
        fs.rot_drive         = rot_drive_avg;
        fs.pos_drive         = pos_drive_avg;
        fs.mtm_drive         = {rot_mtm_drive_avg, pos_mtm_drive_avg};
        fs.mode_expectation  = {rot_mode_e, pos_mode_e};
        fs.mode_peak         = {rot_mode_peak, pos_mode_peak};
        fs.mode_purity       = {rot_mode_purity, pos_mode_purity};
        fs.ngc_coupling_residual            = coupling_residual;
        fs.rot_alpha_min = *s.rot_alpha_min; fs.rot_alpha_max = *s.rot_alpha_max;
        fs.rot_curve     = *s.rot_curve;     fs.rot_deadzone  = *s.rot_deadzone;
        fs.pos_alpha_min = *s.pos_alpha_min; fs.pos_alpha_max = *s.pos_alpha_max;
        fs.pos_curve     = *s.pos_curve;     fs.pos_deadzone  = *s.pos_deadzone;
        fs.anti_inertia_budget              = anti_inertia_budget;
        fs.anomaly_score                    = anomaly_score;
        fs.outlier_quarantine_activity      = outlier_quarantine_activity;
        fs.pos_predictive_translation_error = pos_predictive_translation_error_avg;
        fs.invariant_correction_magnitude   = invariant_correction_magnitude;
        fs.neck_weight_error                = neck_weight_error_avg;
        fs.neck_invalid_ratio               = neck_invalid_ratio_avg;
        fs.anomaly_active                   = anomaly_active;

        const std::array<bool, as::tracking_head_count> head_enabled_arr = {
            ema_enabled, brownian_enabled, adaptive_mode,
            predictive_enabled, chi_square_enabled, pareto_enabled,
        };
        for (size_t h = 0; h < as::tracking_head_count; ++h) {
            const auto th = static_cast<as::tracking_head>(h);
            // head->shoulder edge: earned by the filter
            fs.edge_valid[gov::index(gov::edge_for_head(th))] =
                head_enabled_arr[h] && head_edge_seen[h] && head_edge_valid[h];
            // core->head edge: live whenever the head is enabled
            fs.edge_valid[gov::index(gov::core_edge_for_head(th))] = head_enabled_arr[h];
        }
        fs.edge_valid[gov::index(gov::canvas_edge::shoulder_to_error_gate)] =
            std::isfinite(neck_weight_error_avg.rot) && std::isfinite(neck_weight_error_avg.pos) &&
            std::isfinite(neck_invalid_ratio_avg.rot) && std::isfinite(neck_invalid_ratio_avg.pos) &&
            neck_weight_error_avg.rot < 5e-2 && neck_weight_error_avg.pos < 5e-2 &&
            neck_invalid_ratio_avg.rot < 0.25 && neck_invalid_ratio_avg.pos < 0.25;
        // quality_to_shoulder: visual edge driven by UI display setting
        fs.edge_valid[gov::index(gov::canvas_edge::quality_to_shoulder)] = *s.qualities_mode_ui;

        for (size_t i = 0; i < fs.rot_bin_prob.size(); ++i)
        {
            fs.rot_bin_prob[i]     = rot_mode_prob[i];
            fs.pos_bin_prob[i]     = pos_mode_prob[i];
            fs.rot_bin_delta[i]    = std::fabs(rot_mode_prob[i] - last_rot_mode_prob_snapshot[i]);
            fs.pos_bin_delta[i]    = std::fabs(pos_mode_prob[i] - last_pos_mode_prob_snapshot[i]);
            const double cw        = gov::bin_conflict_weights[i];
            fs.rot_bin_conflict[i] = conflict_drive * cw * rot_mode_prob[i];
            fs.pos_bin_conflict[i] = conflict_drive * cw * pos_mode_prob[i];
            const double pw = [&]() -> double {
                switch (i) {
                case  3: return 0.35;                                            // B4 recovery wall
                case  8: return 1.00;                                            // P1 pathology detect
                case  9: return 0.75 * (1.0 - budget_ratio);                    // P2 quarantine (dynamic)
                case 10: return anomaly_active ? 0.9 : 0.15;                    // P3 true-death (dynamic)
                case 11: return !anomaly_active ? 0.7 * (1.0 - budget_ratio) : 0.10; // P4 re-entry (dynamic)
                default: return 0.05;
                }
            }();
            fs.rot_bin_pathology[i] = pathology_drive * pw;
            fs.pos_bin_pathology[i] = pathology_drive * pw;
        }

        publish_status(status, fs);
    }
    last_rot_mode_prob_snapshot = rot_mode_prob;
    last_pos_mode_prob_snapshot = pos_mode_prob;
}

OPENTRACK_DECLARE_FILTER(alpha_spectrum, dialog_alpha_spectrum, alpha_spectrumDll)
