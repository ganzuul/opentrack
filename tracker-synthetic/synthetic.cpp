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

void synthetic_tracker::fill_pose(double t_seconds, synthetic_preset preset, double translation_amplitude,
                                  double rotation_amplitude, double frequency_hz, double* pose)
{
    std::fill(pose, pose + 6, 0.0);

    const double phase = t_seconds * frequency_hz;
    const double omega = 2.0 * M_PI * frequency_hz;
    const double s1 = std::sin(omega * t_seconds);
    const double c1 = std::cos(omega * t_seconds);

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
                  pose);
        apply_output_mode(pose, (synthetic_output_mode)(int)s.output_mode);

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