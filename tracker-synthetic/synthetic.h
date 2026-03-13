#pragma once

#include "api/plugin-api.hpp"
#include "options/options.hpp"
#include "ui_synthetic.h"

#include <QMutex>
#include <QThread>
#include <deque>

using namespace options;

enum synthetic_preset : int {
    preset_depth_sine = 0,
    preset_depth_ramp = 1,
    preset_yaw_sine = 2,
    preset_orbit_6dof = 3,
    preset_step_depth = 4,
    preset_step_yaw = 5,
    preset_orbit_release = 6,
};

enum synthetic_output_mode : int {
    output_full_6dof = 0,
    output_position_only = 1,
    output_rotation_only = 2,
};

struct synthetic_settings : opts {
    value<int> preset;
    value<int> output_mode;
    value<int> sample_rate_hz;
    value<double> translation_amplitude;
    value<double> rotation_amplitude;
    value<double> frequency_hz;
    value<double> orbit_tilt_deg;
    value<double> orbital_velocity;
    value<bool> gravity_enabled;
    value<double> release_delay_s;
    value<double> post_release_stop_delay_s;

#ifdef SYNTHETIC_IS_B
    static constexpr const char* settings_name = "synthetic-tracker-b";
#else
    static constexpr const char* settings_name = "synthetic-tracker";
#endif

    synthetic_settings() :
        opts(settings_name),
        preset(b, "preset", preset_depth_sine),
        output_mode(b, "output-mode", output_full_6dof),
        sample_rate_hz(b, "sample-rate-hz", 250),
        translation_amplitude(b, "translation-amplitude", 15.0),
        rotation_amplitude(b, "rotation-amplitude", 45.0),
        frequency_hz(b, "frequency-hz", 0.5),
        orbit_tilt_deg(b, "orbit-tilt-deg", 35.0),
        orbital_velocity(b, "orbital-velocity", 2.0),
        gravity_enabled(b, "gravity-enabled", true),
        release_delay_s(b, "release-delay-s", 4.0),
        post_release_stop_delay_s(b, "post-release-stop-delay-s", 3.0)
    {}
};

class synthetic_tracker : protected QThread, public ITracker, public IHighrateSource, public IExperimentSource
{
    Q_OBJECT
public:
    synthetic_tracker();
    ~synthetic_tracker() override;

    module_status start_tracker(QFrame* frame) override;
    void data(double* data) override;
    bool get_highrate_samples(std::vector<highrate_pose_sample>& out) override;
    bool get_experiment_status(experiment_status_sample& out) override;

protected:
    void run() override;

private:
    static void fill_pose(double t_seconds, synthetic_preset preset, double translation_amplitude,
                          double rotation_amplitude, double frequency_hz,
                          double orbit_tilt_deg, double orbital_velocity,
                          bool gravity_enabled, double release_delay_s,
                          double post_release_stop_delay_s, double* pose);
    static void apply_output_mode(double* pose, synthetic_output_mode mode);

    synthetic_settings s;
    QMutex pose_mutex;
    QMutex sample_mutex;
    QMutex experiment_mutex;
    double last_pose[6] {};
    experiment_status_sample experiment_status;
    std::deque<highrate_pose_sample> pending_samples;
    volatile bool should_quit = false;
};

class synthetic_dialog : public ITrackerDialog
{
    Q_OBJECT
public:
    synthetic_dialog();
    void register_tracker(ITracker*) override {}
    void unregister_tracker() override {}

private:
    Ui::synthetic_ui ui;
    synthetic_settings s;

private slots:
    void doOK();
    void doCancel();
};

class synthetic_metadata : public Metadata
{
    Q_OBJECT

    QString name() override
    {
#ifdef SYNTHETIC_IS_B
        return tr("Synthetic test tracker B");
#else
        return tr("Synthetic test tracker");
#endif
    }
    QIcon icon() override { return QIcon(":/images/opentrack.png"); }
};