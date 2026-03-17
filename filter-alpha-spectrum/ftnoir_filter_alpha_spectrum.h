#pragma once

#include "api/plugin-api.hpp"
#include "compat/timer.hpp"
#include "alpha-spectrum-settings.hpp"
#include "alpha-spectrum-governance.hpp"
#include "ui_ftnoir_alpha_spectrum_filtercontrols.h"

#include <atomic>
#include <array>
#include <chrono>
#include <vector>

class QProgressBar;
class QLabel;
class QComboBox;
class QWidget;

namespace detail::alpha_spectrum {

static constexpr size_t transition_matrix_dim = static_cast<size_t>(tracking_head::head_count);
static constexpr size_t transition_matrix_size = transition_matrix_dim * transition_matrix_dim;

struct temporal_economy_state final
{
    std::array<std::atomic<double>, transition_matrix_size> pos_transition_debt {};
    std::array<std::atomic<double>, transition_matrix_size> rot_transition_debt {};
    std::atomic<double> max_capacity {10.0};

    static constexpr size_t idx(tracking_head from, tracking_head to)
    {
        return static_cast<size_t>(from) * transition_matrix_dim + static_cast<size_t>(to);
    }

    static void atomic_add(std::atomic<double>& cell, double delta)
    {
        double current = cell.load(std::memory_order_relaxed);
        while (!cell.compare_exchange_weak(current, current + delta,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed))
        {}
    }

    void add_pos_debt(tracking_head from, tracking_head to, double value)
    {
        atomic_add(pos_transition_debt[idx(from, to)], value);
    }

    void add_rot_debt(tracking_head from, tracking_head to, double value)
    {
        atomic_add(rot_transition_debt[idx(from, to)], value);
    }
};

// Atomic rot/pos pair for calibration_status fields.
struct ap final { std::atomic<double> rot {0.0}, pos {0.0}; };
// Plain rot/pos pair for filter() accumulation locals.
struct rp final { double rot = 0.0, pos = 0.0; };

// Shared diagnostics exposed to the settings dialog.
// Note: this is runtime status, not persistent profile state.
struct calibration_status final
{
    static constexpr int bin_count = 12;
    std::atomic<bool> ui_open {false};
    std::atomic<bool> active {false};
    ap objective, jitter;
    ap brownian_raw, brownian_filtered, brownian_delta, brownian_damped, predictive_error;
    std::array<std::atomic<double>, tracking_head_count> rot_drive {}, pos_drive {};
    ap mtm_drive;
    ap mode_expectation, mode_peak, mode_purity;
    std::atomic<double> ngc_coupling_residual {0.0};
    std::atomic<double> rot_alpha_min {.04};
    std::atomic<double> rot_alpha_max {.65};
    std::atomic<double> rot_curve {1.4};
    std::atomic<double> rot_deadzone {.03};
    std::atomic<double> pos_alpha_min {.05};
    std::atomic<double> pos_alpha_max {.75};
    std::atomic<double> pos_curve {1.2};
    std::atomic<double> pos_deadzone {.1};
    std::atomic<double> anti_inertia_budget {1.0};
    std::atomic<double> anomaly_score {0.0};
    std::atomic<double> outlier_quarantine_activity {0.0};
    std::atomic<double> pos_predictive_translation_error {0.0};
    std::atomic<double> invariant_correction_magnitude {0.0};
    ap neck_weight_error, neck_invalid_ratio;
    std::array<std::atomic<bool>, governance::canvas_edge_count> edge_valid {};
    std::atomic<bool> anomaly_active {false};
    std::array<std::atomic<double>, bin_count> rot_bin_prob {};
    std::array<std::atomic<double>, bin_count> pos_bin_prob {};
    std::array<std::atomic<double>, bin_count> rot_bin_delta {};
    std::array<std::atomic<double>, bin_count> pos_bin_delta {};
    std::array<std::atomic<double>, bin_count> rot_bin_conflict {};
    std::array<std::atomic<double>, bin_count> pos_bin_conflict {};
    std::array<std::atomic<double>, bin_count> rot_bin_pathology {};
    std::array<std::atomic<double>, bin_count> pos_bin_pathology {};

    calibration_status()
    {
        auto zero = [](auto& arr) {
            for (auto& x : arr) x.store(0.0, std::memory_order_relaxed);
        };
        zero(rot_bin_prob); zero(pos_bin_prob);
        zero(rot_bin_delta); zero(pos_bin_delta);
        zero(rot_bin_conflict); zero(pos_bin_conflict);
        zero(rot_bin_pathology); zero(pos_bin_pathology);
        zero(rot_drive); zero(pos_drive);
        for (auto& x : edge_valid) x.store(false, std::memory_order_relaxed);
    }
};

calibration_status& shared_calibration_status();

struct quality_overlay_state final
{
    static constexpr int value_count = 10;
    std::atomic<bool> active {false};
    std::array<std::atomic<double>, value_count> delta {};

    quality_overlay_state()
    {
        for (auto& x : delta)
            x.store(0.0, std::memory_order_relaxed);
    }
};

quality_overlay_state& shared_quality_overlay_state();

// Per-head scalar Kalman state: position estimate, velocity, and 2×2 covariance (upper triangle).
struct imm_head_state final { double x = 0, v = 0, P_xx = 1, P_xv = 0, P_vv = 1; };

} // ns detail::alpha_spectrum

struct alpha_spectrum : IFilter
{
    alpha_spectrum();
    void filter(const double* input, double* output) override;
    void set_tracker(ITracker* tracker) override;
    void center() override { first_run = true; }
    module_status initialize() override { return status_ok(); }
    int diagnostics_names(const char** buf, int maxn) override;
    int diagnostics(double* buf, int maxn) override;

private:
    // Current implementation scope:
    // - Per-axis adaptive EMA with alpha-spectrum mode probability state
    // - MTM-style probability diffusion + measurement likelihood update
    // - Brownian status for raw vs filtered noise-energy contribution
    static constexpr int axis_count = 6;
    static constexpr int mode_count = 12;
    static constexpr int hydra_head_capacity = 6;
    static constexpr double delta_rc = 1. / 90.;
    static constexpr double brownian_rc = .6;
    static constexpr double mode_diffusion_rc = .75;
    static constexpr double noise_rc_max = 30.;
    static constexpr double dt_min = 1e-4;
    static constexpr double dt_max = .25;

    settings_alpha_spectrum s;
    Timer timer;
    double last_input[axis_count] {};
    double last_output[axis_count] {};
    double last_delta[axis_count] {};
    double last_noise[axis_count] {};
    double raw_brownian_energy[axis_count] {};
    double filtered_brownian_energy[axis_count] {};
    double predicted_next_output[axis_count] {};
    // Per-axis divot sticktion state.
    double stillness_anchor[axis_count] {};
    double escape_level[axis_count] {};
    double noise_rc = 0.;
    double last_Z = 0.0;
    double coupling_residual = 0.0;
    double last_coupling_residual = 0.0;
    double anti_inertia_budget = 1.0;
    double anomaly_score = 0.0;
    int anomaly_cooldown_frames = 0;
    bool anomaly_active = false;

    struct translational_predictive_state final
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        double vx = 0.0;
        double vy = 0.0;
        double vz = 0.0;
    };
    translational_predictive_state translation_state;
    std::array<double, mode_count> rot_mode_prob {};
    std::array<double, mode_count> pos_mode_prob {};
    std::array<double, mode_count> last_rot_mode_prob_snapshot {};
    std::array<double, mode_count> last_pos_mode_prob_snapshot {};
    detail::alpha_spectrum::temporal_economy_state temporal_state;
    std::array<detail::alpha_spectrum::tracking_head, axis_count> last_dominant_head {
        detail::alpha_spectrum::tracking_head::ema,
        detail::alpha_spectrum::tracking_head::ema,
        detail::alpha_spectrum::tracking_head::ema,
        detail::alpha_spectrum::tracking_head::ema,
        detail::alpha_spectrum::tracking_head::ema,
        detail::alpha_spectrum::tracking_head::ema
    };
    bool first_run = true;
    
    // High-rate gyro integration for Predictive head
    IHighrateSource* highrate_source = nullptr;
    double gyro_integrated_rotation[3] {}; // Per-frame integrated delta for Yaw/Pitch/Roll
    double last_highrate_pose[3] {};
    bool last_highrate_pose_valid = false;
    bool has_highrate_source = false;

    // IMM per-head Kalman state and mode weights, both indexed [axis][head].
    static constexpr size_t imm_head_count = detail::alpha_spectrum::tracking_head_count;
    using imm_head_state = detail::alpha_spectrum::imm_head_state;
    std::array<std::array<imm_head_state, imm_head_count>, axis_count> head_states {};
    std::array<std::array<double,         imm_head_count>, axis_count> imm_weights {};

    void integrate_highrate_samples();
};

class dialog_alpha_spectrum : public IFilterDialog
{
    Q_OBJECT
public:
    dialog_alpha_spectrum();
    ~dialog_alpha_spectrum() override;
    void register_filter(IFilter*) override {}
    void unregister_filter() override {}
    void save() override;
    void reload() override;
    bool embeddable() noexcept override { return true; }
    void set_buttons_visible(bool x) override;

private:
    Ui::UICdialog_alpha_spectrum ui;
    settings_alpha_spectrum s;
    QComboBox* ui_mode_combo = nullptr;
    QComboBox* preset_combo = nullptr;
    QLabel*    preset_desc = nullptr;
    QWidget* simplified_placeholder = nullptr;
    QWidget* advanced_canvas = nullptr;
    std::vector<detail::alpha_spectrum::preset_def> loaded_presets;
    void pull_status_into_ui(bool commit_to_settings);
    void reset_to_defaults();

private slots:
    void doOK();
    void doCancel();
};

class alpha_spectrumDll : public Metadata
{
    Q_OBJECT

    QString name() override { return tr("Alpha Spectrum"); }
    QIcon icon() override { return QIcon(":/images/filter-16.png"); }
};
