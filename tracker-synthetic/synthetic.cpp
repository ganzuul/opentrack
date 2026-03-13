#include "synthetic.h"

#include <QDebug>
#include <algorithm>
#include <cmath>
#include <chrono>

namespace {
double triangle_wave(double phase)
{
    const double wrapped = phase - std::floor(phase);
    return 4.0 * std::abs(wrapped - 0.5) - 1.0;
}

double square_wave(double phase)
{
    return (phase - std::floor(phase)) < 0.5 ? 1.0 : -1.0;
}
}

synthetic_tracker::synthetic_tracker() = default;

synthetic_tracker::~synthetic_tracker()
{
    should_quit = true;
    requestInterruption();
    wait();
}

module_status synthetic_tracker::start_tracker(QFrame*)
{
    should_quit = false;
    {
        QMutexLocker pose_lock(&pose_mutex);
        std::fill(last_pose, last_pose + 6, 0.0);
    }
    {
        QMutexLocker sample_lock(&sample_mutex);
        pending_samples.clear();
    }
    {
        QMutexLocker experiment_lock(&experiment_mutex);
        experiment_status = {};
    }
    start();
    return status_ok();
}

void synthetic_tracker::data(double* data)
{
    QMutexLocker lock(&pose_mutex);
    for (int i = 0; i < 6; i++)
        data[i] = last_pose[i];
}

bool synthetic_tracker::get_highrate_samples(std::vector<highrate_pose_sample>& out)
{
    QMutexLocker lock(&sample_mutex);
    if (pending_samples.empty())
        return false;

    out.reserve(out.size() + pending_samples.size());
    while (!pending_samples.empty())
    {
        out.push_back(pending_samples.front());
        pending_samples.pop_front();
    }
    return true;
}

bool synthetic_tracker::get_experiment_status(experiment_status_sample& out)
{
    QMutexLocker lock(&experiment_mutex);
    out = experiment_status;
    return out.active || out.complete;
}

void synthetic_tracker::fill_pose(double t_seconds, synthetic_preset preset, double translation_amplitude,
                                  double rotation_amplitude, double frequency_hz,
                                  double orbit_tilt_deg, double orbital_velocity,
                                  bool gravity_enabled, double release_delay_s,
                                  double post_release_stop_delay_s, double* pose)
{
    std::fill(pose, pose + 6, 0.0);

    const double phase = t_seconds * frequency_hz;
    const double omega = 2.0 * M_PI * frequency_hz;
    const double s1 = std::sin(omega * t_seconds);
    const double c1 = std::cos(omega * t_seconds);
    const double tilt = orbit_tilt_deg * M_PI / 180.0;
    const double ctilt = std::cos(tilt);
    const double stilt = std::sin(tilt);

    switch (preset)
    {
        case preset_depth_sine:
            pose[TZ] = translation_amplitude * s1;
            break;
        case preset_depth_ramp:
            pose[TZ] = translation_amplitude * triangle_wave(phase);
            break;
        case preset_yaw_sine:
            pose[Yaw] = rotation_amplitude * s1;
            break;
        case preset_orbit_6dof:
            pose[TX] = translation_amplitude * 0.6 * s1;
            pose[TY] = translation_amplitude * 0.35 * std::sin(omega * t_seconds * 0.5);
            pose[TZ] = translation_amplitude * 0.8 * c1;
            pose[Yaw] = rotation_amplitude * s1;
            pose[Pitch] = rotation_amplitude * 0.5 * std::sin(omega * t_seconds * 0.7);
            pose[Roll] = rotation_amplitude * 0.3 * std::cos(omega * t_seconds * 1.3);
            break;
        case preset_step_depth:
            pose[TZ] = translation_amplitude * square_wave(phase);
            break;
        case preset_step_yaw:
            pose[Yaw] = rotation_amplitude * square_wave(phase);
            break;
        case preset_orbit_release:
        {
            const double radius = std::max(translation_amplitude, 0.01);
            const double w = std::max(orbital_velocity, 0.01);
            const double g = gravity_enabled ? 9.81 : 0.0;

            const double theta_release = w * std::max(release_delay_s, 0.0);
            const double x0 = radius * std::cos(theta_release);
            const double y0 = radius * std::sin(theta_release) * ctilt;
            const double z0 = radius * std::sin(theta_release) * stilt;
            const double vx0 = -radius * w * std::sin(theta_release);
            const double vy0 = radius * w * std::cos(theta_release) * ctilt;
            const double vz0 = radius * w * std::cos(theta_release) * stilt;

            if (t_seconds <= release_delay_s)
            {
                const double theta = w * t_seconds;
                pose[TX] = radius * std::cos(theta);
                pose[TY] = radius * std::sin(theta) * ctilt;
                pose[TZ] = radius * std::sin(theta) * stilt;
            }
            else
            {
                const double dt_rel = t_seconds - release_delay_s;
                const double dt_cap = std::max(post_release_stop_delay_s, 0.0);
                const double dt = std::min(dt_rel, dt_cap);
                pose[TX] = x0 + vx0 * dt;
                pose[TY] = y0 + vy0 * dt - 0.5 * g * dt * dt;
                pose[TZ] = z0 + vz0 * dt;
            }

            // Orient approximately along instantaneous tangential velocity before release.
            const double t_eval = t_seconds <= release_delay_s ? t_seconds : release_delay_s;
            const double vx = t_seconds <= release_delay_s ? (-radius * w * std::sin(w * t_eval)) : vx0;
            const double vz = t_seconds <= release_delay_s ? (radius * w * std::cos(w * t_eval) * stilt) : vz0;
            pose[Yaw] = rotation_amplitude * std::atan2(vx, std::max(std::fabs(vz), 1e-6)) / (M_PI * 0.5);
            break;
        }
        default:
            pose[TZ] = translation_amplitude * s1;
            break;
    }
}

void synthetic_tracker::apply_output_mode(double* pose, synthetic_output_mode mode)
{
    switch (mode)
    {
        case output_position_only:
            pose[Yaw] = 0.0;
            pose[Pitch] = 0.0;
            pose[Roll] = 0.0;
            break;
        case output_rotation_only:
            pose[TX] = 0.0;
            pose[TY] = 0.0;
            pose[TZ] = 0.0;
            break;
        case output_full_6dof:
        default:
            break;
    }
}

void synthetic_tracker::run()
{
    using clock = std::chrono::steady_clock;

    const auto start_time = clock::now();
    auto previous_time = start_time;

    const int sample_rate = std::max(1, (int)s.sample_rate_hz);
    const unsigned sleep_us = (unsigned)std::max(1, 1000000 / sample_rate);

    qDebug() << "synthetic-tracker: started with preset" << (int)s.preset << "at" << sample_rate << "Hz";

    while (!should_quit && !isInterruptionRequested())
    {
        const auto now = clock::now();
        const double t_seconds = std::chrono::duration<double>(now - start_time).count();
        const double dt_seconds = std::clamp(std::chrono::duration<double>(now - previous_time).count(), 1e-5, 0.1);
        previous_time = now;

        double pose[6] {};
        fill_pose(t_seconds,
                  (synthetic_preset)(int)s.preset,
                  (double)s.translation_amplitude,
                  (double)s.rotation_amplitude,
                  (double)s.frequency_hz,
                  (double)s.orbit_tilt_deg,
                  (double)s.orbital_velocity,
                  (bool)s.gravity_enabled,
                  (double)s.release_delay_s,
                  (double)s.post_release_stop_delay_s,
                  pose);
        apply_output_mode(pose, (synthetic_output_mode)(int)s.output_mode);

        {
            QMutexLocker experiment_lock(&experiment_mutex);
            experiment_status = {};
            if ((synthetic_preset)(int)s.preset == preset_orbit_release)
            {
                const double expected_duration =
                    std::max(0.0, (double)s.release_delay_s) + std::max(0.0, (double)s.post_release_stop_delay_s);
                experiment_status.active = true;
                experiment_status.elapsed_seconds = t_seconds;
                experiment_status.expected_duration_seconds = expected_duration;
                experiment_status.expected_rows = int(std::llround(expected_duration * 250.0));
                if (t_seconds < (double)s.release_delay_s)
                    experiment_status.phase = experiment_phase::orbiting;
                else if (t_seconds < expected_duration)
                    experiment_status.phase = experiment_phase::released;
                else
                {
                    experiment_status.phase = experiment_phase::complete;
                    experiment_status.complete = true;
                }
            }
        }

        {
            QMutexLocker pose_lock(&pose_mutex);
            std::copy(pose, pose + 6, last_pose);
        }

        {
            highrate_pose_sample sample;
            sample.dt_seconds = dt_seconds;
            std::copy(pose, pose + 6, sample.pose);
            sample.source_id = 1;

            QMutexLocker sample_lock(&sample_mutex);
            pending_samples.push_back(sample);
            const size_t max_samples = (size_t)std::max(32, sample_rate / 2);
            while (pending_samples.size() > max_samples)
                pending_samples.pop_front();
        }

        QThread::usleep(sleep_us);
    }

    qDebug() << "synthetic-tracker: stopped";
}

OPENTRACK_DECLARE_TRACKER(synthetic_tracker, synthetic_dialog, synthetic_metadata)