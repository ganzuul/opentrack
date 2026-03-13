#pragma once

#include "api/plugin-api.hpp"

#include <QFile>
#include <QStringList>

class experiment_csv : public TR, public IProtocol
{
    Q_OBJECT

    QFile file;
    QString run_id;
    QStringList diag_names;
    bool begun = false;
    bool ended = false;
    int flush_counter = 0;
    int actual_rows = 0;

    static QString phase_name(experiment_phase phase);
    void write_line(const QStringList& fields);
    bool ensure_open();

public:
    experiment_csv() = default;
    module_status initialize() override { return status_ok(); }
    void pose(const double*, const double*) override {}
    QString game_name() override;
    void experiment_begin(const experiment_status_sample& status,
                          const char* const* diag_names_, int diag_count) override;
    void experiment_frame(const experiment_frame_record& rec,
                          const double* diag_values, int diag_count) override;
    void experiment_end(const experiment_status_sample& status, const char* reason) override;
};

class experiment_csv_dialog : public IProtocolDialog
{
    Q_OBJECT
public:
    experiment_csv_dialog() = default;
    void register_protocol(IProtocol*) override {}
    void unregister_protocol() override {}
};

class experiment_csv_metadata : public Metadata
{
    Q_OBJECT

    QString name() override { return tr("experiment csv collector"); }
    QIcon icon() override { return QIcon(":/images/csv.png"); }
};
