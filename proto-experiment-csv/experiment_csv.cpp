#include "experiment_csv.hpp"

#include "compat/base-path.hpp"

#include <QDateTime>
#include <QDir>
#include <QDebug>
#include <QTextStream>

namespace {
QString number(double value)
{
    return QString::number(value, 'g', 12);
}

QString integer(int value)
{
    return QString::number(value);
}
}

QString experiment_csv::phase_name(experiment_phase phase)
{
    switch (phase)
    {
        case experiment_phase::orbiting: return QStringLiteral("orbiting");
        case experiment_phase::released: return QStringLiteral("released");
        case experiment_phase::complete: return QStringLiteral("complete");
        case experiment_phase::inactive:
        default: return QStringLiteral("inactive");
    }
}

void experiment_csv::write_line(const QStringList& fields)
{
    QTextStream out(&file);
    out << fields.join(',') << '\n';
    if (++flush_counter >= 32)
    {
        file.flush();
        flush_counter = 0;
    }
}

bool experiment_csv::ensure_open()
{
    if (file.isOpen())
        return true;

    const QDir dir(application_base_path() + QStringLiteral("/experiment-runs"));
    if (!QDir().mkpath(dir.path()))
        return false;

    const QString timestamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz"));
    run_id = timestamp;
    file.setFileName(dir.filePath(QStringLiteral("experiment-") + timestamp + QStringLiteral(".csv")));
    const bool ok = file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
    if (ok)
        qDebug() << "proto-experiment-csv: writing" << file.fileName();
    else
        qDebug() << "proto-experiment-csv: failed to open" << file.fileName();
    return ok;
}

QString experiment_csv::game_name()
{
    return tr("Experiment CSV Collector");
}

void experiment_csv::experiment_begin(const experiment_status_sample& status,
                                      const char* const* diag_names_, int diag_count)
{
    if (begun)
        return;
    if (!ensure_open())
        return;

    diag_names.clear();
    for (int i = 0; i < diag_count; ++i)
        diag_names.push_back(QString::fromUtf8(diag_names_[i] ? diag_names_[i] : ""));

    QStringList header {
        QStringLiteral("record_type"),
        QStringLiteral("run_id"),
        QStringLiteral("t_cumulative"),
        QStringLiteral("row_index"),
        QStringLiteral("expected_rows"),
        QStringLiteral("expected_duration_s"),
        QStringLiteral("experiment_active"),
        QStringLiteral("experiment_complete"),
        QStringLiteral("experiment_phase"),
        QStringLiteral("dt"),
        QStringLiteral("rawTX"), QStringLiteral("rawTY"), QStringLiteral("rawTZ"),
        QStringLiteral("rawYaw"), QStringLiteral("rawPitch"), QStringLiteral("rawRoll"),
        QStringLiteral("correctedTX"), QStringLiteral("correctedTY"), QStringLiteral("correctedTZ"),
        QStringLiteral("correctedYaw"), QStringLiteral("correctedPitch"), QStringLiteral("correctedRoll"),
        QStringLiteral("filteredTX"), QStringLiteral("filteredTY"), QStringLiteral("filteredTZ"),
        QStringLiteral("filteredYaw"), QStringLiteral("filteredPitch"), QStringLiteral("filteredRoll"),
        QStringLiteral("mappedTX"), QStringLiteral("mappedTY"), QStringLiteral("mappedTZ"),
        QStringLiteral("mappedYaw"), QStringLiteral("mappedPitch"), QStringLiteral("mappedRoll")
    };
    for (const QString& name : diag_names)
        header.push_back(name);
    header.push_back(QStringLiteral("completion_reason"));
    write_line(header);

    QStringList start {
        QStringLiteral("start"),
        run_id,
        QStringLiteral("0"),
        QStringLiteral("0"),
        integer(status.expected_rows),
        number(status.expected_duration_seconds),
        status.active ? QStringLiteral("1") : QStringLiteral("0"),
        status.complete ? QStringLiteral("1") : QStringLiteral("0"),
        phase_name(status.phase),
        QStringLiteral("0")
    };
    for (int i = 0; i < 24 + diag_count; ++i)
        start.push_back(QStringLiteral("0"));
    start.push_back(QStringLiteral("begin"));
    write_line(start);
    actual_rows = 0;
    begun = true;
    qDebug() << "proto-experiment-csv: begin run" << run_id;
}

void experiment_csv::experiment_frame(const experiment_frame_record& rec,
                                      const double* diag_values, int diag_count)
{
    if (!begun && !ended)
        experiment_begin(rec.experiment, nullptr, 0);
    if (!file.isOpen() || ended)
        return;

    QStringList fields {
        QStringLiteral("data"),
        run_id,
        number(rec.t_cumulative_seconds),
        integer(rec.row_index),
        integer(rec.experiment.expected_rows),
        number(rec.experiment.expected_duration_seconds),
        rec.experiment.active ? QStringLiteral("1") : QStringLiteral("0"),
        rec.experiment.complete ? QStringLiteral("1") : QStringLiteral("0"),
        phase_name(rec.experiment.phase),
        number(rec.dt_seconds)
    };

    const double* poses[] = {
        rec.raw_pose,
        rec.corrected_pose,
        rec.filtered_pose,
        rec.mapped_pose,
    };
    for (const double* pose : poses)
        for (int i = 0; i < Axis_COUNT; ++i)
            fields.push_back(number(pose[i]));

    for (int i = 0; i < diag_count; ++i)
        fields.push_back(number(diag_values[i]));

    fields.push_back(QString());
    write_line(fields);
    actual_rows++;
}

void experiment_csv::experiment_end(const experiment_status_sample& status, const char* reason)
{
    if (ended)
        return;
    if (!begun)
        experiment_begin(status, nullptr, 0);
    if (!file.isOpen())
        return;

    QStringList fields {
        QStringLiteral("end"),
        run_id,
        number(status.elapsed_seconds),
        integer(actual_rows),
        integer(status.expected_rows),
        number(status.expected_duration_seconds),
        status.active ? QStringLiteral("1") : QStringLiteral("0"),
        status.complete ? QStringLiteral("1") : QStringLiteral("0"),
        phase_name(status.phase),
        QStringLiteral("0")
    };
    for (int i = 0; i < 24 + diag_names.size(); ++i)
        fields.push_back(QStringLiteral("0"));
    fields.push_back(QString::fromUtf8(reason ? reason : "ended"));
    write_line(fields);
    file.flush();
    file.close();
    ended = true;
    qDebug() << "proto-experiment-csv: end run" << run_id << "reason" << (reason ? reason : "ended")
             << "rows" << actual_rows;
}

OPENTRACK_DECLARE_PROTOCOL(experiment_csv, experiment_csv_dialog, experiment_csv_metadata)
