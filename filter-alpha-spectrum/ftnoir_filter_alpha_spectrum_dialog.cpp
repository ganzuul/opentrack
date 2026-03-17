"Copyright (C) 2026 ganzuul. This file is part of Alpha Spectrum. Alpha Spectrum is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3..."

#include "ftnoir_filter_alpha_spectrum.h"
#include "options/globals.hpp"

constexpr auto mo = std::memory_order_relaxed;
namespace gov = detail::alpha_spectrum::governance;

#include <QCheckBox>
#include <QDirIterator>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QDir>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QSizePolicy>
#include <QSlider>
#include <QSignalBlocker>
#include <QWidget>
#include <QGroupBox>
#include <QGridLayout>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QVBoxLayout>
#include <QStringList>
#include <algorithm>
#include <array>
#include <functional>
#include <vector>

static void ensure_preset_resources() { Q_INIT_RESOURCE(alpha_spectrum_presets); }

namespace {

// ---------------------------------------------------------------------------
// Preset YAML parser — handles the exact schema defined in presets/*.yaml.
// Returns nullopt if the text lacks a `name:` entry.
// ---------------------------------------------------------------------------

static std::optional<detail::alpha_spectrum::preset_def>
parse_preset_yaml(const QString& text)
{
    using detail::alpha_spectrum::tracking_head;
    using detail::alpha_spectrum::preset_def;
    using detail::alpha_spectrum::index;

    preset_def def;

    enum class yaml_section { none, heads, params } section = yaml_section::none;

    for (const QString& raw_line : text.split(QLatin1Char('\n')))
    {
        const QString line = raw_line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        const bool is_indented = raw_line.startsWith(QStringLiteral("  "));

        const int colon_pos = line.indexOf(QLatin1Char(':'));
        if (colon_pos < 0)
            continue;

        const QString key = line.left(colon_pos).trimmed();
        QString val = line.mid(colon_pos + 1).trimmed();

        // Section header: indented=false, nothing after colon.
        if (!is_indented && val.isEmpty())
        {
            if      (key == QStringLiteral("heads"))  section = yaml_section::heads;
            else if (key == QStringLiteral("params")) section = yaml_section::params;
            else                                      section = yaml_section::none;
            continue;
        }

        // Strip optional quotes from string values.
        if (val.size() >= 2 && val.startsWith(QLatin1Char('"')) && val.endsWith(QLatin1Char('"')))
            val = val.mid(1, val.size() - 2);

        if (!is_indented)
        {
            if      (key == QStringLiteral("name"))         def.name = val.toStdString();
            else if (key == QStringLiteral("description"))  def.description = val.toStdString();
            else if (key == QStringLiteral("adaptive-mode")) def.adaptive_mode = (val == QStringLiteral("true"));
        }
        else if (section == yaml_section::heads)
        {
            const bool enabled = (val == QStringLiteral("true"));
            if      (key == QStringLiteral("ema"))        def.head_enabled[index(tracking_head::ema)]        = enabled;
            else if (key == QStringLiteral("brownian"))   def.head_enabled[index(tracking_head::brownian)]   = enabled;
            else if (key == QStringLiteral("adaptive"))   { def.head_enabled[index(tracking_head::adaptive)]  = enabled; def.adaptive_mode = enabled; }
            else if (key == QStringLiteral("predictive")) def.head_enabled[index(tracking_head::predictive)] = enabled;
            else if (key == QStringLiteral("chi-square")) def.head_enabled[index(tracking_head::chi_square)] = enabled;
            else if (key == QStringLiteral("pareto"))     def.head_enabled[index(tracking_head::pareto)]     = enabled;
        }
        else if (section == yaml_section::params)
        {
            bool ok = false;
            double v = val.toDouble(&ok);
            if (!ok) continue;
            if      (key == QStringLiteral("rot-alpha-min")) def.rot_alpha_min = v;
            else if (key == QStringLiteral("rot-alpha-max")) def.rot_alpha_max = v;
            else if (key == QStringLiteral("rot-curve"))     def.rot_curve     = v;
            else if (key == QStringLiteral("pos-alpha-min")) def.pos_alpha_min = v;
            else if (key == QStringLiteral("pos-alpha-max")) def.pos_alpha_max = v;
            else if (key == QStringLiteral("pos-curve"))     def.pos_curve     = v;
            else if (key == QStringLiteral("rot-deadzone"))  def.rot_deadzone  = v;
            else if (key == QStringLiteral("pos-deadzone"))  def.pos_deadzone  = v;
        }
    }

    if (def.name.empty()) return std::nullopt;
    return def;
}

static std::vector<detail::alpha_spectrum::preset_def> load_yaml_presets()
{
    ensure_preset_resources();
    std::vector<detail::alpha_spectrum::preset_def> result;

    auto scan_dir = [&](const QString& dir_path) {
        QDirIterator it(dir_path);
        while (it.hasNext())
        {
            const QString path = it.next();
            if (!path.endsWith(QStringLiteral(".yaml"), Qt::CaseInsensitive))
                continue;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            auto def = parse_preset_yaml(QString::fromUtf8(f.readAll()));
            if (def)
                result.push_back(std::move(*def));
        }
    };

    scan_dir(QStringLiteral(":/alpha-spectrum/presets"));
    scan_dir(options::globals::ini_directory() + QStringLiteral("/alpha-spectrum-presets"));

    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}

static void apply_preset(const detail::alpha_spectrum::preset_def& def,
                         settings_alpha_spectrum& s)
{
    using detail::alpha_spectrum::tracking_head;
    using detail::alpha_spectrum::index;

    s.adaptive_mode       = def.head_enabled[index(tracking_head::adaptive)] || def.adaptive_mode;
    s.ema_enabled         = def.head_enabled[index(tracking_head::ema)];
    s.brownian_enabled    = def.head_enabled[index(tracking_head::brownian)];
    s.predictive_enabled  = def.head_enabled[index(tracking_head::predictive)];
    s.chi_square_enabled  = def.head_enabled[index(tracking_head::chi_square)];
    s.pareto_enabled      = def.head_enabled[index(tracking_head::pareto)];

    if (def.rot_alpha_min >= 0) s.rot_alpha_min = options::slider_value{def.rot_alpha_min, 0.005, 0.4};
    if (def.rot_alpha_max >= 0) s.rot_alpha_max = options::slider_value{def.rot_alpha_max, 0.02,  1.0};
    if (def.rot_curve     >= 0) s.rot_curve     = options::slider_value{def.rot_curve,     0.2,   8.0};
    if (def.pos_alpha_min >= 0) s.pos_alpha_min = options::slider_value{def.pos_alpha_min, 0.005, 0.4};
    if (def.pos_alpha_max >= 0) s.pos_alpha_max = options::slider_value{def.pos_alpha_max, 0.02,  1.0};
    if (def.pos_curve     >= 0) s.pos_curve     = options::slider_value{def.pos_curve,     0.2,   8.0};
    if (def.rot_deadzone  >= 0) s.rot_deadzone  = options::slider_value{def.rot_deadzone,  0.0,   3.0};
    if (def.pos_deadzone  >= 0) s.pos_deadzone  = options::slider_value{def.pos_deadzone,  0.0,   3.0};

    s.b->save();
}

// ---------------------------------------------------------------------------

    enum class ui_complexity_mode : int
    {
        basic = 0,
        simplified = 1,
        advanced = 2,
    };

    static int sanitize_ui_mode(int mode)
    {
        if (mode < static_cast<int>(ui_complexity_mode::basic) ||
            mode > static_cast<int>(ui_complexity_mode::advanced))
            return static_cast<int>(ui_complexity_mode::simplified);
        return mode;
    }

    class advanced_node_canvas final : public QWidget
    {
    public:
        class draggable_proxy final : public QGraphicsProxyWidget
        {
        public:
            explicit draggable_proxy(QGraphicsItem* parent = nullptr) : QGraphicsProxyWidget(parent) {}

        protected:
            void mousePressEvent(QGraphicsSceneMouseEvent* event) override
            {
                if (event->button() == Qt::LeftButton && event->pos().y() <= 24.0)
                {
                    dragging = true;
                    drag_offset = event->pos();
                    event->accept();
                    return;
                }
                QGraphicsProxyWidget::mousePressEvent(event);
            }

            void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override
            {
                if (dragging)
                {
                    setPos(event->scenePos() - drag_offset);
                    event->accept();
                    return;
                }
                QGraphicsProxyWidget::mouseMoveEvent(event);
            }

            void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override
            {
                if (dragging && event->button() == Qt::LeftButton)
                {
                    dragging = false;
                    if (on_moved)
                        on_moved(pos());
                    event->accept();
                    return;
                }
                QGraphicsProxyWidget::mouseReleaseEvent(event);
            }

        public:
            std::function<void(QPointF)> on_moved;

        private:
            bool dragging = false;
            QPointF drag_offset;
        };

        explicit advanced_node_canvas(settings_alpha_spectrum* settings, QWidget* parent) :
            QWidget(parent), s(settings)
        {
            auto* root = new QVBoxLayout(this);
            auto* title = new QLabel(tr("Advanced Node Editor (Independent Heads)"), this);
            auto* hint = new QLabel(tr("This advanced surface is native Qt and uses the same settings API.\n"
                                       "Head nodes are independent biases over raw input; Renyi neck edges feed shoulder composition."), this);
            hint->setWordWrap(true);
            hint->setFrameShape(QFrame::StyledPanel);

            scene = new QGraphicsScene(this);
            view = new QGraphicsView(scene, this);
            view->setRenderHint(QPainter::Antialiasing, true);
            view->setDragMode(QGraphicsView::RubberBandDrag);
            view->setMinimumHeight(380);
            view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

            auto add_checkbox_binding = [this](QVBoxLayout* layout, const QString& text,
                                               std::function<bool()> getter,
                                               std::function<void(bool)> setter) {
                auto* box = new QCheckBox(text);
                layout->addWidget(box);
                connect(box, &QCheckBox::toggled, this, [setter](bool v) { setter(v); });
                checks.push_back({box, std::move(getter)});
                return box;
            };

            auto add_slider_binding = [this](QGridLayout* layout, int row, const QString& text,
                                             double min_v, double max_v,
                                             std::function<double()> getter,
                                             std::function<void(double)> setter,
                                             std::function<QString(double)> formatter) {
                auto* label = new QLabel(text);
                auto* slider = new QSlider(Qt::Horizontal);
                auto* value = new QLabel;
                slider->setRange(0, 1000);
                layout->addWidget(label, row, 0);
                layout->addWidget(slider, row, 1);
                layout->addWidget(value, row, 2);
                connect(slider, &QSlider::valueChanged, this, [min_v, max_v, setter](int pos) {
                    const double t = std::clamp(pos / 1000.0, 0.0, 1.0);
                    setter(min_v + (max_v - min_v) * t);
                });
                sliders.push_back({slider, value, min_v, max_v, std::move(getter), std::move(formatter)});
            };

            auto* ema_group = new QGroupBox(tr("Head: EMA"));
            auto* ema_layout = new QGridLayout(ema_group);
            ema_check = new QCheckBox(tr("Enabled"), ema_group);
            ema_layout->addWidget(ema_check, 0, 0, 1, 3);
            connect(ema_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->ema_enabled = v; });
            checks.push_back({ema_check, [this] { return *s->ema_enabled; }});
            add_slider_binding(ema_layout, 1, tr("Rot Min"), 0.005, 0.4,
                               [this] { return (double)*s->rot_alpha_min; },
                               [this](double v) { s->rot_alpha_min = options::slider_value{v, 0.005, 0.4}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(ema_layout, 2, tr("Rot Max"), 0.02, 1.0,
                               [this] { return (double)*s->rot_alpha_max; },
                               [this](double v) { s->rot_alpha_max = options::slider_value{v, 0.02, 1.0}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(ema_layout, 3, tr("Rot Curve"), 0.2, 8.0,
                               [this] { return (double)*s->rot_curve; },
                               [this](double v) { s->rot_curve = options::slider_value{v, 0.2, 8.0}; },
                               [](double v) { return QStringLiteral("%1").arg(v, 0, 'f', 2); });
            add_slider_binding(ema_layout, 4, tr("Pos Min"), 0.005, 0.4,
                               [this] { return (double)*s->pos_alpha_min; },
                               [this](double v) { s->pos_alpha_min = options::slider_value{v, 0.005, 0.4}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(ema_layout, 5, tr("Pos Max"), 0.02, 1.0,
                               [this] { return (double)*s->pos_alpha_max; },
                               [this](double v) { s->pos_alpha_max = options::slider_value{v, 0.02, 1.0}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(ema_layout, 6, tr("Pos Curve"), 0.2, 8.0,
                               [this] { return (double)*s->pos_curve; },
                               [this](double v) { s->pos_curve = options::slider_value{v, 0.2, 8.0}; },
                               [](double v) { return QStringLiteral("%1").arg(v, 0, 'f', 2); });

            auto* brownian_group = new QGroupBox(tr("Head: Brownian"));
            auto* brownian_layout = new QGridLayout(brownian_group);
            brownian_check = new QCheckBox(tr("Enabled"), brownian_group);
            brownian_layout->addWidget(brownian_check, 0, 0, 1, 3);
            connect(brownian_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->brownian_enabled = v; });
            checks.push_back({brownian_check, [this] { return *s->brownian_enabled; }});
            add_slider_binding(brownian_layout, 1, tr("Gain"), 0.0, 2.0,
                               [this] { return (double)*s->brownian_head_gain; },
                               [this](double v) { s->brownian_head_gain = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1x").arg(v, 0, 'f', 2); });

            auto* adaptive_group = new QGroupBox(tr("Head: Adaptive"));
            auto* adaptive_layout = new QGridLayout(adaptive_group);
            adaptive_check = new QCheckBox(tr("Enabled"), adaptive_group);
            adaptive_layout->addWidget(adaptive_check, 0, 0, 1, 3);
            connect(adaptive_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->adaptive_mode = v; });
            checks.push_back({adaptive_check, [this] { return *s->adaptive_mode; }});
            add_slider_binding(adaptive_layout, 1, tr("Lift"), 0.0, 0.6,
                               [this] { return (double)*s->adaptive_threshold_lift; },
                               [this](double v) { s->adaptive_threshold_lift = options::slider_value{v, 0.0, 0.6}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });

            auto* predictive_group = new QGroupBox(tr("Head: Predictive"));
            auto* predictive_layout = new QGridLayout(predictive_group);
            predictive_check = new QCheckBox(tr("Enabled"), predictive_group);
            predictive_layout->addWidget(predictive_check, 0, 0, 1, 3);
            connect(predictive_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->predictive_enabled = v; });
            checks.push_back({predictive_check, [this] { return *s->predictive_enabled; }});
            add_slider_binding(predictive_layout, 1, tr("Rot Gain"), 0.0, 2.0,
                               [this] { return (double)*s->predictive_head_gain; },
                               [this](double v) { s->predictive_head_gain = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1x").arg(v, 0, 'f', 2); });
            add_slider_binding(predictive_layout, 2, tr("Trans Gain"), 0.0, 2.0,
                               [this] { return (double)*s->predictive_translation_gain; },
                               [this](double v) { s->predictive_translation_gain = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1x").arg(v, 0, 'f', 2); });

            auto* entropy_group = new QGroupBox(tr("Renyi Neck / Shoulder"));
            auto* entropy_layout = new QGridLayout(entropy_group);
            add_slider_binding(entropy_layout, 0, tr("Shoulder"), 0.0, 1.0,
                               [this] { return (double)*s->mtm_shoulder_base; },
                               [this](double v) { s->mtm_shoulder_base = options::slider_value{v, 0.0, 1.0}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(entropy_layout, 1, tr("NGC Kappa"), 0.0, 0.3,
                               [this] { return (double)*s->ngc_kappa; },
                               [this](double v) { s->ngc_kappa = options::slider_value{v, 0.0, 0.3}; },
                               [](double v) { return QStringLiteral("%1").arg(v, 0, 'f', 3); });
            add_slider_binding(entropy_layout, 2, tr("NGC Nominal Z"), 0.3, 2.0,
                               [this] { return (double)*s->ngc_nominal_z; },
                               [this](double v) { s->ngc_nominal_z = options::slider_value{v, 0.3, 2.0}; },
                               [](double v) { return QStringLiteral("%1m").arg(v, 0, 'f', 2); });
            purity_value = new QLabel(entropy_group);
            entropy_layout->addWidget(new QLabel(tr("Purity"), entropy_group), 3, 0);
            entropy_layout->addWidget(purity_value, 3, 1, 1, 2);
            neck_health_value = new QLabel(entropy_group);
            entropy_layout->addWidget(new QLabel(tr("Neck Health"), entropy_group), 4, 0);
            entropy_layout->addWidget(neck_health_value, 4, 1, 1, 2);

            auto* chi_group = new QGroupBox(tr("Head: Chi-square"));
            auto* chi_layout = new QGridLayout(chi_group);
            chi_check = new QCheckBox(tr("Enabled (error gain)"), chi_group);
            chi_layout->addWidget(chi_check, 0, 0, 1, 3);
            connect(chi_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->chi_square_enabled = v; });
            checks.push_back({chi_check, [this] { return *s->chi_square_enabled; }});
            add_slider_binding(chi_layout, 1, tr("Gain"), 0.0, 2.0,
                               [this] { return (double)*s->chi_square_head_gain; },
                               [this](double v) { s->chi_square_head_gain = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1x").arg(v, 0, 'f', 2); });

            auto* pareto_group = new QGroupBox(tr("Head: Pareto"));
            auto* pareto_layout = new QGridLayout(pareto_group);
            pareto_check = new QCheckBox(tr("Enabled (error gain)"), pareto_group);
            pareto_layout->addWidget(pareto_check, 0, 0, 1, 3);
            connect(pareto_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->pareto_enabled = v; });
            checks.push_back({pareto_check, [this] { return *s->pareto_enabled; }});
            add_slider_binding(pareto_layout, 1, tr("Gain"), 0.0, 2.0,
                               [this] { return (double)*s->pareto_head_gain; },
                               [this](double v) { s->pareto_head_gain = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1x").arg(v, 0, 'f', 2); });

            auto* errorgate_group = new QGroupBox(tr("Error Gate / IMM Deadzone"));
            auto* errorgate_layout = new QGridLayout(errorgate_group);
            quarantine_check = new QCheckBox(tr("Quarantine outliers"), errorgate_group);
            errorgate_layout->addWidget(quarantine_check, 0, 0, 1, 3);
            connect(quarantine_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->outlier_quarantine_enabled = v; });
            checks.push_back({quarantine_check, [this] { return *s->outlier_quarantine_enabled; }});
            add_slider_binding(errorgate_layout, 1, tr("Strength"), 0.0, 1.0,
                               [this] { return (double)*s->outlier_quarantine_strength; },
                               [this](double v) { s->outlier_quarantine_strength = options::slider_value{v, 0.0, 1.0}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(errorgate_layout, 2, tr("Rot DZ"), 0.0, 3.0,
                               [this] { return (double)*s->rot_deadzone; },
                               [this](double v) { s->rot_deadzone = options::slider_value{v, 0.0, 3.0}; },
                               [](double v) { return QStringLiteral("%1σ").arg(v, 0, 'f', 2); });
            add_slider_binding(errorgate_layout, 3, tr("Pos DZ"), 0.0, 3.0,
                               [this] { return (double)*s->pos_deadzone; },
                               [this](double v) { s->pos_deadzone = options::slider_value{v, 0.0, 3.0}; },
                               [](double v) { return QStringLiteral("%1σ").arg(v, 0, 'f', 2); });

            auto* quality_group = new QGroupBox(tr("Quality Projection"));
            auto* quality_layout = new QVBoxLayout(quality_group);
            add_checkbox_binding(quality_layout, tr("Qualities mode"),
                                 [this] { return *s->qualities_mode_ui; },
                                 [this](bool v) { s->qualities_mode_ui = v; });
            add_checkbox_binding(quality_layout, tr("Stillness"),
                                 [this] { return *s->quality_stillness; },
                                 [this](bool v) { s->quality_stillness = v; });
            add_checkbox_binding(quality_layout, tr("Continuity"),
                                 [this] { return *s->quality_continuity; },
                                 [this](bool v) { s->quality_continuity = v; });
            add_checkbox_binding(quality_layout, tr("Robustness"),
                                 [this] { return *s->quality_robustness; },
                                 [this](bool v) { s->quality_robustness = v; });
            add_checkbox_binding(quality_layout, tr("Decisiveness"),
                                 [this] { return *s->quality_decisiveness; },
                                 [this](bool v) { s->quality_decisiveness = v; });
            add_checkbox_binding(quality_layout, tr("Pathology Defense"),
                                 [this] { return *s->quality_pathology_defense; },
                                 [this](bool v) { s->quality_pathology_defense = v; });
            add_checkbox_binding(quality_layout, tr("Recovery Pace"),
                                 [this] { return *s->quality_recovery_pace; },
                                 [this](bool v) { s->quality_recovery_pace = v; });

            auto make_draggable = [&](QWidget* w) {
                auto* p = new draggable_proxy();
                p->setWidget(w);
                scene->addItem(p);
                p->setFlag(QGraphicsItem::ItemIsSelectable, true);
                p->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
                return p;
            };
            ema_proxy        = make_draggable(ema_group);
            brownian_proxy   = make_draggable(brownian_group);
            adaptive_proxy   = make_draggable(adaptive_group);
            predictive_proxy = make_draggable(predictive_group);
            chi_proxy        = make_draggable(chi_group);
            pareto_proxy     = make_draggable(pareto_group);
            shoulder_proxy   = make_draggable(entropy_group);
            errorgate_proxy  = make_draggable(errorgate_group);
            quality_proxy    = make_draggable(quality_group);
            ema_proxy->setPos(*s->node_pos_ema_x,         *s->node_pos_ema_y);
            brownian_proxy->setPos(*s->node_pos_brownian_x,   *s->node_pos_brownian_y);
            adaptive_proxy->setPos(*s->node_pos_adaptive_x,   *s->node_pos_adaptive_y);
            predictive_proxy->setPos(*s->node_pos_predictive_x, *s->node_pos_predictive_y);
            chi_proxy->setPos(*s->node_pos_chi_x,         *s->node_pos_chi_y);
            pareto_proxy->setPos(*s->node_pos_pareto_x,     *s->node_pos_pareto_y);
            shoulder_proxy->setPos(*s->node_pos_shoulder_x,   *s->node_pos_shoulder_y);
            errorgate_proxy->setPos(*s->node_pos_errorgate_x,  *s->node_pos_errorgate_y);
            quality_proxy->setPos(*s->node_pos_quality_x,   *s->node_pos_quality_y);

            static_cast<draggable_proxy*>(ema_proxy)->on_moved        = [this](QPointF p) { s->node_pos_ema_x = p.x(); s->node_pos_ema_y = p.y(); s->b->save(); };
            static_cast<draggable_proxy*>(brownian_proxy)->on_moved   = [this](QPointF p) { s->node_pos_brownian_x = p.x(); s->node_pos_brownian_y = p.y(); s->b->save(); };
            static_cast<draggable_proxy*>(adaptive_proxy)->on_moved   = [this](QPointF p) { s->node_pos_adaptive_x = p.x(); s->node_pos_adaptive_y = p.y(); s->b->save(); };
            static_cast<draggable_proxy*>(predictive_proxy)->on_moved = [this](QPointF p) { s->node_pos_predictive_x = p.x(); s->node_pos_predictive_y = p.y(); s->b->save(); };
            static_cast<draggable_proxy*>(chi_proxy)->on_moved        = [this](QPointF p) { s->node_pos_chi_x = p.x(); s->node_pos_chi_y = p.y(); s->b->save(); };
            static_cast<draggable_proxy*>(pareto_proxy)->on_moved     = [this](QPointF p) { s->node_pos_pareto_x = p.x(); s->node_pos_pareto_y = p.y(); s->b->save(); };
            static_cast<draggable_proxy*>(shoulder_proxy)->on_moved   = [this](QPointF p) { s->node_pos_shoulder_x = p.x(); s->node_pos_shoulder_y = p.y(); s->b->save(); };
            static_cast<draggable_proxy*>(errorgate_proxy)->on_moved  = [this](QPointF p) { s->node_pos_errorgate_x = p.x(); s->node_pos_errorgate_y = p.y(); s->b->save(); };
            static_cast<draggable_proxy*>(quality_proxy)->on_moved    = [this](QPointF p) { s->node_pos_quality_x = p.x(); s->node_pos_quality_y = p.y(); s->b->save(); };

            node_proxies[gov::index(gov::canvas_node::ema)]        = ema_proxy;
            node_proxies[gov::index(gov::canvas_node::brownian)]   = brownian_proxy;
            node_proxies[gov::index(gov::canvas_node::adaptive)]   = adaptive_proxy;
            node_proxies[gov::index(gov::canvas_node::predictive)] = predictive_proxy;
            node_proxies[gov::index(gov::canvas_node::chi_square)] = chi_proxy;
            node_proxies[gov::index(gov::canvas_node::pareto)]     = pareto_proxy;
            node_proxies[gov::index(gov::canvas_node::shoulder)]   = shoulder_proxy;
            node_proxies[gov::index(gov::canvas_node::error_gate)] = errorgate_proxy;
            node_proxies[gov::index(gov::canvas_node::quality)]    = quality_proxy;
            for (size_t i = 0; i < gov::canvas_edge_count; ++i) {
                const auto& m = gov::canvas_edge_registry[i];
                const QColor c(m.active_color.r, m.active_color.g, m.active_color.b);
                edge_items[i] = scene->addPath(QPainterPath(),
                    QPen(c, m.dashed ? 1.5 : 2.0, m.dashed ? Qt::DashLine : Qt::SolidLine));
            }

            edge_label_entropy = scene->addSimpleText(tr("Shoulder -> Error Gate"));
            edge_label_predictive = scene->addSimpleText(tr("Head ε-gain -> Neck"));
            edge_label_quality = scene->addSimpleText(tr("Quality discriminant -> Shoulder"));
            for (auto* label : {edge_label_entropy, edge_label_predictive, edge_label_quality})
                label->setBrush(QBrush(QColor(190, 190, 190)));

            root->addWidget(title);
            root->addWidget(hint);

            auto* btn_bar = new QHBoxLayout;
            auto* save_btn = new QPushButton(tr("Save as Configuration…"), this);
            save_btn->setToolTip(tr("Export current settings as a named preset that will appear in the Simplified view's Configuration dropdown."));
            btn_bar->addStretch();
            btn_bar->addWidget(save_btn);
            root->addLayout(btn_bar);

            connect(save_btn, &QPushButton::clicked, this, [this] {
                bool ok = false;
                const QString name = QInputDialog::getText(
                    this, tr("Save as Configuration"),
                    tr("Configuration name:"),
                    QLineEdit::Normal, QString(), &ok);
                if (!ok || name.trimmed().isEmpty())
                    return;

                const QString preset_dir = options::globals::ini_directory()
                                           + QStringLiteral("/alpha-spectrum-presets");
                QDir().mkpath(preset_dir);

                QString filename = name.trimmed().toLower();
                filename.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]")),
                                 QStringLiteral("-"));
                const QString filepath = preset_dir + QStringLiteral("/") + filename
                                         + QStringLiteral(".yaml");

                QFile f(filepath);
                if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
                    return;

                QTextStream ts(&f);
                auto bstr = [](bool v) -> QLatin1String { return v ? QLatin1String("true") : QLatin1String("false"); };
                ts << "# Alpha Spectrum configuration saved from Advanced view\n";
                ts << "name: \"" << name.trimmed() << "\"\n";
                ts << "description: \"Saved from Advanced node canvas\"\n";
                ts << "adaptive-mode: " << bstr(*s->adaptive_mode) << "\n";
                ts << "heads:\n";
                ts << "  ema: "        << bstr(*s->ema_enabled)        << "\n";
                ts << "  brownian: "   << bstr(*s->brownian_enabled)   << "\n";
                ts << "  adaptive: "   << bstr(*s->adaptive_mode)      << "\n";
                ts << "  predictive: " << bstr(*s->predictive_enabled) << "\n";
                ts << "  chi-square: " << bstr(*s->chi_square_enabled) << "\n";
                ts << "  pareto: "     << bstr(*s->pareto_enabled)     << "\n";
                ts << "params:\n";
                ts << "  rot-alpha-min: " << QString::number((double)*s->rot_alpha_min, 'f', 4) << "\n";
                ts << "  rot-alpha-max: " << QString::number((double)*s->rot_alpha_max, 'f', 4) << "\n";
                ts << "  rot-curve: "     << QString::number((double)*s->rot_curve,     'f', 4) << "\n";
                ts << "  pos-alpha-min: " << QString::number((double)*s->pos_alpha_min, 'f', 4) << "\n";
                ts << "  pos-alpha-max: " << QString::number((double)*s->pos_alpha_max, 'f', 4) << "\n";
                ts << "  pos-curve: "     << QString::number((double)*s->pos_curve,     'f', 4) << "\n";
                ts << "  rot-deadzone: "  << QString::number((double)*s->rot_deadzone,  'f', 4) << "\n";
                ts << "  pos-deadzone: "  << QString::number((double)*s->pos_deadzone,  'f', 4) << "\n";
            });

            root->addWidget(view, 1);

            auto* frame_timer = new QTimer(this);
            frame_timer->setInterval(100);
            QObject::connect(frame_timer, &QTimer::timeout, this, [this] {
                if (!s)
                    return;
                const auto& status = detail::alpha_spectrum::shared_calibration_status();
                for (auto& binding : checks)
                {
                    const QSignalBlocker b(binding.box);
                    binding.box->setChecked(binding.getter());
                }
                for (auto& binding : sliders)
                {
                    const QSignalBlocker b(binding.slider);
                    const double x = std::clamp(binding.getter(), binding.min_value, binding.max_value);
                    const double t = (x - binding.min_value) / std::max(1e-9, binding.max_value - binding.min_value);
                    binding.slider->setValue(static_cast<int>(t * 1000.0));
                    binding.value->setText(binding.formatter(x));
                }
                purity_value->setText(QStringLiteral("rot %1 / pos %2")
                                          .arg(status.mode_purity.rot.load(mo), 0, 'f', 3)
                                          .arg(status.mode_purity.pos.load(mo), 0, 'f', 3));

                const double rot_werr = status.neck_weight_error.rot.load(mo);
                const double pos_werr = status.neck_weight_error.pos.load(mo);
                const double rot_inv  = status.neck_invalid_ratio.rot.load(mo);
                const double pos_inv  = status.neck_invalid_ratio.pos.load(mo);
                std::array<bool, gov::canvas_edge_count> edge_active;
                for (size_t i = 0; i < gov::canvas_edge_count; ++i)
                    edge_active[i] = status.edge_valid[i].load(mo);
                const bool shoulder_edge_valid =
                    edge_active[gov::index(gov::canvas_edge::shoulder_to_error_gate)];

                neck_health_value->setText(QStringLiteral("wErr r/p %1 / %2 | inv r/p %3 / %4")
                                               .arg(rot_werr, 0, 'g', 3)
                                               .arg(pos_werr, 0, 'g', 3)
                                               .arg(rot_inv, 0, 'f', 2)
                                               .arg(pos_inv, 0, 'f', 2));
                if (edge_label_entropy)
                {
                    edge_label_entropy->setText(QStringLiteral("Shoulder -> Error Gate | wErr %1/%2")
                                                    .arg(rot_werr, 0, 'g', 2)
                                                    .arg(pos_werr, 0, 'g', 2));
                }
                if (edge_label_predictive)
                {
                    edge_label_predictive->setText(QStringLiteral("Head ε-gain -> Neck | inv %1/%2")
                                                       .arg(rot_inv, 0, 'f', 2)
                                                       .arg(pos_inv, 0, 'f', 2));
                }

                const double worst_werr = std::max(rot_werr, pos_werr);
                const double worst_inv = std::max(rot_inv, pos_inv);
                QColor health_color(90, 190, 120);
                if (worst_werr > 1e-2 || worst_inv > 0.10)
                    health_color = QColor(220, 90, 90);
                else if (worst_werr > 1e-4 || worst_inv > 0.01)
                    health_color = QColor(220, 170, 80);

                const QColor inactive_color(110, 110, 110);
                for (size_t i = 0; i < gov::canvas_edge_count; ++i) {
                    if (!edge_items[i]) continue;
                    const auto& m = gov::canvas_edge_registry[i];
                    const QColor active_col = m.use_health_color
                        ? health_color
                        : QColor(m.active_color.r, m.active_color.g, m.active_color.b);
                    if (edge_active[i])
                        edge_items[i]->setPen(QPen(active_col, 2.4, Qt::SolidLine));
                    else
                        edge_items[i]->setPen(QPen(inactive_color, 1.5, Qt::DashLine));
                }

                if (edge_label_entropy)
                    edge_label_entropy->setBrush(QBrush(shoulder_edge_valid ? health_color : inactive_color));
                if (edge_label_predictive) {
                    const bool any_head_valid =
                        edge_active[gov::index(gov::canvas_edge::ema_to_shoulder)]
                        || edge_active[gov::index(gov::canvas_edge::brownian_to_shoulder)]
                        || edge_active[gov::index(gov::canvas_edge::adaptive_to_shoulder)]
                        || edge_active[gov::index(gov::canvas_edge::predictive_to_shoulder)]
                        || edge_active[gov::index(gov::canvas_edge::chi_square_to_shoulder)]
                        || edge_active[gov::index(gov::canvas_edge::pareto_to_shoulder)];
                    edge_label_predictive->setBrush(QBrush(any_head_valid ? health_color : inactive_color));
                }

                update_edges();
            });
            frame_timer->start();

            update_edges();
        }

    private:
        void update_edges()
        {
            auto make_path = [](QGraphicsProxyWidget* from, QGraphicsProxyWidget* to) {
                QPainterPath path;
                if (!from || !to)
                    return path;
                const QRectF a = from->sceneBoundingRect();
                const QRectF b = to->sceneBoundingRect();
                const QPointF start(a.right(), a.center().y());
                const QPointF end(b.left(), b.center().y());
                const qreal dx = std::max<qreal>(60.0, (end.x() - start.x()) * 0.45);
                path.moveTo(start);
                path.cubicTo(start + QPointF(dx, 0.0), end - QPointF(dx, 0.0), end);
                return path;
            };

            for (size_t i = 0; i < gov::canvas_edge_count; ++i) {
                if (!edge_items[i]) continue;
                const auto& m = gov::canvas_edge_registry[i];
                edge_items[i]->setPath(make_path(node_proxies[gov::index(m.from)],
                                                 node_proxies[gov::index(m.to)]));
            }

            if (edge_label_entropy)
                edge_label_entropy->setPos((shoulder_proxy->sceneBoundingRect().center() + errorgate_proxy->sceneBoundingRect().center()) * 0.5 + QPointF(-10, -22));
            if (edge_label_predictive)
                edge_label_predictive->setPos((brownian_proxy->sceneBoundingRect().center() + shoulder_proxy->sceneBoundingRect().center()) * 0.5 + QPointF(-10, -20));
            if (edge_label_quality)
                edge_label_quality->setPos((quality_proxy->sceneBoundingRect().center() + shoulder_proxy->sceneBoundingRect().center()) * 0.5 + QPointF(-10, 16));
        }

        struct checkbox_binding final
        {
            QCheckBox* box = nullptr;
            std::function<bool()> getter;
        };

        struct slider_binding final
        {
            QSlider* slider = nullptr;
            QLabel* value = nullptr;
            double min_value = 0.0;
            double max_value = 1.0;
            std::function<double()> getter;
            std::function<QString(double)> formatter;
        };

        settings_alpha_spectrum* s = nullptr;
        QGraphicsScene* scene = nullptr;
        QGraphicsView* view = nullptr;
        QGraphicsProxyWidget* core_proxy = nullptr;
        QGraphicsProxyWidget* ema_proxy = nullptr;
        QGraphicsProxyWidget* brownian_proxy = nullptr;
        QGraphicsProxyWidget* adaptive_proxy = nullptr;
        QGraphicsProxyWidget* predictive_proxy = nullptr;
        QGraphicsProxyWidget* chi_proxy = nullptr;
        QGraphicsProxyWidget* pareto_proxy = nullptr;
        QGraphicsProxyWidget* shoulder_proxy = nullptr;
        QGraphicsProxyWidget* errorgate_proxy = nullptr;
        QGraphicsProxyWidget* quality_proxy = nullptr;
        std::array<QGraphicsPathItem*, gov::canvas_edge_count> edge_items {};
        std::array<QGraphicsProxyWidget*, gov::canvas_node_count> node_proxies {};
        QGraphicsSimpleTextItem* edge_label_entropy = nullptr;
        QGraphicsSimpleTextItem* edge_label_predictive = nullptr;
        QGraphicsSimpleTextItem* edge_label_quality = nullptr;
        std::vector<checkbox_binding> checks;
        std::vector<slider_binding> sliders;
        QCheckBox* ema_check = nullptr;
        QCheckBox* brownian_check = nullptr;
        QCheckBox* adaptive_check = nullptr;
        QCheckBox* predictive_check = nullptr;
        QCheckBox* chi_check = nullptr;
        QCheckBox* pareto_check = nullptr;
        QCheckBox* quarantine_check = nullptr;
        QSlider* shoulder_slider = nullptr;
        QLabel* shoulder_value = nullptr;
        QLabel* purity_value = nullptr;
        QLabel* neck_health_value = nullptr;
        QSlider* quarantine_slider = nullptr;
        QLabel* quarantine_value = nullptr;
    };

    static double lerp(double min, double max, double t)
    {
        return min + (max - min) * t;
    }

    static double norm(double value, double min, double max)
    {
        if (max <= min)
            return 0.0;
        return std::clamp((value - min) / (max - min), 0.0, 1.0);
    }

}

dialog_alpha_spectrum::dialog_alpha_spectrum()
{
    ui.setupUi(this);

    detail::alpha_spectrum::shared_calibration_status().ui_open.store(true, mo);

    connect(ui.buttonBox, SIGNAL(accepted()), this, SLOT(doOK()));
    connect(ui.buttonBox, SIGNAL(rejected()), this, SLOT(doCancel()));

    if (ui.verticalLayout)
    {
        auto* mode_layout = new QHBoxLayout();
        auto* mode_label = new QLabel(tr("UI Mode"), this);
        ui_mode_combo = new QComboBox(this);
        ui_mode_combo->setObjectName(QStringLiteral("ui_mode_combo"));
        ui_mode_combo->addItem(tr("Basic"), static_cast<int>(ui_complexity_mode::basic));
        ui_mode_combo->addItem(tr("Simplified"), static_cast<int>(ui_complexity_mode::simplified));
        ui_mode_combo->addItem(tr("Advanced (Node Editor)"), static_cast<int>(ui_complexity_mode::advanced));
        mode_layout->addWidget(mode_label);
        mode_layout->addWidget(ui_mode_combo, 1);
        ui.verticalLayout->insertLayout(0, mode_layout);
    }

    QComboBox* behavior_combo_ptr = nullptr;
    QLabel*    behavior_desc_ptr  = nullptr;
    {
        loaded_presets = load_yaml_presets();

        auto* simplified_frame  = new QFrame(this);
        auto* simplified_layout = new QVBoxLayout(simplified_frame);
        simplified_layout->setContentsMargins(0, 0, 0, 0);

        // --- Behavior profiles ---
        auto* profile_group  = new QGroupBox(tr("Behavior Profile"), simplified_frame);
        auto* profile_layout = new QVBoxLayout(profile_group);
        auto* behavior_combo = new QComboBox(profile_group);
        behavior_combo->setObjectName(QStringLiteral("behavior_combo"));
        profile_layout->addWidget(behavior_combo);
        auto* profile_desc = new QLabel(profile_group);
        profile_desc->setWordWrap(true);
        profile_desc->setMinimumHeight(48);
        profile_desc->setFrameShape(QFrame::StyledPanel);
        profile_layout->addWidget(profile_desc);
        behavior_combo_ptr = behavior_combo;
        behavior_desc_ptr  = profile_desc;
        simplified_layout->addWidget(profile_group);

        // --- Curated Configurations ---
        auto* preset_frame = new QFrame(simplified_frame);
        preset_frame->setFrameShape(QFrame::StyledPanel);
        auto* preset_layout = new QVBoxLayout(preset_frame);

        auto* preset_header = new QHBoxLayout();
        auto* preset_label  = new QLabel(tr("Curated Configuration"), preset_frame);
        preset_combo        = new QComboBox(preset_frame);
        preset_combo->setObjectName(QStringLiteral("preset_combo"));

        if (loaded_presets.empty())
        {
            preset_combo->addItem(tr("No configurations available"));
            preset_combo->setEnabled(false);
        }
        else
        {
            for (int i = 0; i < (int)loaded_presets.size(); ++i)
                preset_combo->addItem(QString::fromStdString(loaded_presets[i].name), i);
        }

        preset_header->addWidget(preset_label);
        preset_header->addWidget(preset_combo, 1);
        preset_layout->addLayout(preset_header);

        preset_desc = new QLabel(preset_frame);
        preset_desc->setWordWrap(true);
        preset_desc->setFrameShape(QFrame::StyledPanel);
        if (!loaded_presets.empty())
            preset_desc->setText(QString::fromStdString(loaded_presets[0].description));
        preset_layout->addWidget(preset_desc);

        connect(preset_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int idx)
                {
                    if (!preset_combo || idx < 0) return;
                    const int preset_idx = preset_combo->itemData(idx).toInt();
                    if (preset_idx < 0 || preset_idx >= (int)loaded_presets.size()) return;
                    const auto& def = loaded_presets[preset_idx];
                    preset_desc->setText(QString::fromStdString(def.description));
                    apply_preset(def, s);
                });

        simplified_layout->addWidget(preset_frame);

        simplified_placeholder = simplified_frame;
        if (ui.verticalLayout)
            ui.verticalLayout->insertWidget(2, simplified_placeholder);
        simplified_placeholder->setVisible(false);
    }

    advanced_canvas = new advanced_node_canvas(&s, this);
    if (advanced_canvas)
    {
        if (ui.verticalLayout)
            ui.verticalLayout->insertWidget(3, advanced_canvas);
        advanced_canvas->setVisible(false);
    }

    tie_setting(s.adaptive_mode, ui.adaptive_mode_check);
    tie_setting(s.qualities_mode_ui, ui.qualities_mode_check);
    tie_setting(s.quality_stillness, ui.quality_stillness_check);
    tie_setting(s.quality_continuity, ui.quality_continuity_check);
    tie_setting(s.quality_robustness, ui.quality_robustness_check);
    tie_setting(s.quality_decisiveness, ui.quality_decisiveness_check);
    tie_setting(s.quality_pathology_defense, ui.quality_pathology_defense_check);
    tie_setting(s.quality_recovery_pace, ui.quality_recovery_pace_check);
    tie_setting(s.ema_enabled, ui.ema_enabled_check);
    tie_setting(s.brownian_enabled, ui.brownian_enabled_check);
    tie_setting(s.predictive_enabled, ui.predictive_enabled_check);
    tie_setting(s.chi_square_enabled, ui.chi_square_enabled_check);
    tie_setting(s.pareto_enabled, ui.pareto_enabled_check);
    tie_setting(s.outlier_quarantine_enabled, ui.outlier_quarantine_enabled_check);

    tie_setting(s.rot_alpha_min, ui.rot_min_slider);
    tie_setting(s.rot_alpha_max, ui.rot_max_slider);
    tie_setting(s.rot_curve, ui.rot_curve_slider);
    tie_setting(s.pos_alpha_min, ui.pos_min_slider);
    tie_setting(s.pos_alpha_max, ui.pos_max_slider);
    tie_setting(s.pos_curve, ui.pos_curve_slider);
    tie_setting(s.rot_deadzone, ui.rot_deadzone_slider);
    tie_setting(s.pos_deadzone, ui.pos_deadzone_slider);
    tie_setting(s.brownian_head_gain, ui.brownian_gain_slider);
    tie_setting(s.adaptive_threshold_lift, ui.adaptive_threshold_slider);
    tie_setting(s.predictive_head_gain, ui.predictive_gain_slider);
    tie_setting(s.chi_square_head_gain, ui.chi_square_gain_slider);
    tie_setting(s.pareto_head_gain, ui.pareto_gain_slider);
    tie_setting(s.outlier_quarantine_strength, ui.outlier_quarantine_strength_slider);
    tie_setting(s.mtm_shoulder_base, ui.mtm_shoulder_slider);
    tie_setting(s.ngc_kappa, ui.ngc_kappa_slider);
    tie_setting(s.ngc_nominal_z, ui.ngc_nominal_z_slider);

    tie_setting(s.rot_alpha_min, ui.rot_min_label,
                [](double x) { return QStringLiteral("%1%").arg(x * 100.0, 0, 'f', 1); });
    tie_setting(s.rot_alpha_max, ui.rot_max_label,
                [](double x) { return QStringLiteral("%1%").arg(x * 100.0, 0, 'f', 1); });
    tie_setting(s.rot_curve, ui.rot_curve_label,
                [](double x) { return QStringLiteral("%1").arg(x, 0, 'f', 2); });
    tie_setting(s.pos_alpha_min, ui.pos_min_label,
                [](double x) { return QStringLiteral("%1%").arg(x * 100.0, 0, 'f', 1); });
    tie_setting(s.pos_alpha_max, ui.pos_max_label,
                [](double x) { return QStringLiteral("%1%").arg(x * 100.0, 0, 'f', 1); });
    tie_setting(s.pos_curve, ui.pos_curve_label,
                [](double x) { return QStringLiteral("%1").arg(x, 0, 'f', 2); });
    tie_setting(s.rot_deadzone, ui.rot_deadzone_label,
                [](double x) { return tr("%1σ").arg(x, 0, 'f', 2); });
    tie_setting(s.pos_deadzone, ui.pos_deadzone_label,
                [](double x) { return tr("%1σ").arg(x, 0, 'f', 2); });
    tie_setting(s.brownian_head_gain, ui.brownian_gain_label,
                [](double x) { return QStringLiteral("%1x").arg(x, 0, 'f', 2); });
    tie_setting(s.adaptive_threshold_lift, ui.adaptive_threshold_label,
                [](double x) { return QStringLiteral("%1%").arg(x * 100.0, 0, 'f', 1); });
    tie_setting(s.predictive_head_gain, ui.predictive_gain_label,
                [](double x) { return QStringLiteral("%1x").arg(x, 0, 'f', 2); });
    tie_setting(s.chi_square_head_gain, ui.chi_square_gain_label,
                [](double x) { return QStringLiteral("%1x").arg(x, 0, 'f', 2); });
    tie_setting(s.pareto_head_gain, ui.pareto_gain_label,
                [](double x) { return QStringLiteral("%1x").arg(x, 0, 'f', 2); });
    tie_setting(s.outlier_quarantine_strength, ui.outlier_quarantine_strength_label,
                [](double x) { return QStringLiteral("%1%").arg(x * 100.0, 0, 'f', 1); });
    tie_setting(s.mtm_shoulder_base, ui.mtm_shoulder_label,
                [](double x) { return QStringLiteral("%1%").arg(x * 100.0, 0, 'f', 1); });
    tie_setting(s.ngc_kappa, ui.ngc_kappa_label,
                [](double x) { return QStringLiteral("%1").arg(x, 0, 'f', 3); });
    tie_setting(s.ngc_nominal_z, ui.ngc_nominal_z_label,
                [](double x) { return QStringLiteral("%1m").arg(x, 0, 'f', 2); });

    connect(ui.rot_min_slider, &QSlider::valueChanged, this,
            [&](int v) { if (ui.rot_max_slider->value() < v) ui.rot_max_slider->setValue(v); });
    connect(ui.rot_max_slider, &QSlider::valueChanged, this,
            [&](int v) { if (ui.rot_min_slider->value() > v) ui.rot_min_slider->setValue(v); });

    connect(ui.pos_min_slider, &QSlider::valueChanged, this,
            [&](int v) { if (ui.pos_max_slider->value() < v) ui.pos_max_slider->setValue(v); });
    connect(ui.pos_max_slider, &QSlider::valueChanged, this,
            [&](int v) { if (ui.pos_min_slider->value() > v) ui.pos_min_slider->setValue(v); });

    connect(ui.reset_defaults_button, &QPushButton::clicked, this,
            [this] { reset_to_defaults(); });

    auto apply_simple_alpha = [this](int slider_value) {
        const double t = std::clamp(static_cast<double>(slider_value) / ui.simple_alpha_slider->maximum(), 0.0, 1.0);
        // t=1 (right) = most EMA smoothing = minimum alpha values
        const double min_value = lerp(0.4, 0.005, t);
        const double max_value = lerp(1.0, 0.02,  t);

        const auto rot_min_cfg = *s.rot_alpha_min;
        const auto rot_max_cfg = *s.rot_alpha_max;
        const auto pos_min_cfg = *s.pos_alpha_min;
        const auto pos_max_cfg = *s.pos_alpha_max;

        s.rot_alpha_min = options::slider_value{min_value, rot_min_cfg.min(), rot_min_cfg.max()};
        s.rot_alpha_max = options::slider_value{max_value, rot_max_cfg.min(), rot_max_cfg.max()};
        s.pos_alpha_min = options::slider_value{min_value, pos_min_cfg.min(), pos_min_cfg.max()};
        s.pos_alpha_max = options::slider_value{max_value, pos_max_cfg.min(), pos_max_cfg.max()};

        ui.simple_alpha_label->setText(QStringLiteral("EMA α: [%1%, %2%]")
                                           .arg(min_value * 100.0, 0, 'f', 1)
                                           .arg(max_value * 100.0, 0, 'f', 1));
    };

    auto apply_simple_shape = [this](int slider_value) {
        const double t = std::clamp(static_cast<double>(slider_value) / ui.simple_shape_slider->maximum(), 0.0, 1.0);
        const double curve_value = lerp(0.2, 8.0, t);
        const double rot_deadzone_value = lerp(0.0, 3.0, t);
        const double pos_deadzone_value = lerp(0.0, 3.0, t);

        const auto rot_curve_cfg = *s.rot_curve;
        const auto pos_curve_cfg = *s.pos_curve;
        const auto rot_deadzone_cfg = *s.rot_deadzone;
        const auto pos_deadzone_cfg = *s.pos_deadzone;

        s.rot_curve = options::slider_value{curve_value, rot_curve_cfg.min(), rot_curve_cfg.max()};
        s.pos_curve = options::slider_value{curve_value, pos_curve_cfg.min(), pos_curve_cfg.max()};
        s.rot_deadzone = options::slider_value{rot_deadzone_value, rot_deadzone_cfg.min(), rot_deadzone_cfg.max()};
        s.pos_deadzone = options::slider_value{pos_deadzone_value, pos_deadzone_cfg.min(), pos_deadzone_cfg.max()};

        ui.simple_shape_label->setText(QStringLiteral("Curve %1 / RotDZ %2σ / PosDZ %3σ")
                                           .arg(curve_value, 0, 'f', 2)
                                           .arg(rot_deadzone_value, 0, 'f', 2)
                                           .arg(pos_deadzone_value, 0, 'f', 2));
    };

    connect(ui.simple_alpha_slider, &QSlider::valueChanged, this, apply_simple_alpha);
    connect(ui.simple_shape_slider, &QSlider::valueChanged, this, apply_simple_shape);

    auto apply_attitude_projection = [this] {
        auto& overlay = detail::alpha_spectrum::shared_quality_overlay_state();
        if (!*s.qualities_mode_ui)
        {
            overlay.active.store(false, mo);
            for (int j = 0; j < detail::alpha_spectrum::quality_overlay_state::value_count; j++)
                overlay.delta[j].store(0.0, mo);
            return;
        }

        const std::array<double, 6> q {{
            *s.quality_stillness ? 1.0 : 0.0,
            *s.quality_continuity ? 1.0 : 0.0,
            *s.quality_robustness ? 1.0 : 0.0,
            *s.quality_decisiveness ? 1.0 : 0.0,
            *s.quality_pathology_defense ? 1.0 : 0.0,
            *s.quality_recovery_pace ? 1.0 : 0.0,
        }};

        // quality -> variable delta matrix (6x10), first implementation pass.
        static constexpr double w[6][10] = {
            {+0.20, -0.05, -0.35, -0.35, +0.10, -0.015, +0.20, +0.05, +0.60, +0.20}, // stillness
            {-0.10, +0.02, +0.45, +0.45, +0.05, +0.015, -0.10, -0.02, -0.30, -0.05}, // continuity
            {+0.35, +0.05, -0.15, -0.10, +0.15, +0.010, +0.45, +0.08, +0.50, +0.30}, // robustness
            {-0.10, +0.20, +0.20, +0.10, +0.30, +0.020, -0.15, -0.03, -0.45, +0.10}, // decisiveness
            {+0.10, +0.05, -0.10, -0.05, +0.20, +0.015, +0.50, +0.10, -0.80, +0.55}, // pathology defense
            {+0.00, +0.00, -0.05, -0.05, +0.10, +0.000, +0.30, +0.35, +0.10, +0.05}, // recovery pace
        };

        for (int j = 0; j < detail::alpha_spectrum::quality_overlay_state::value_count; j++)
        {
            double delta = 0.0;
            for (int i = 0; i < static_cast<int>(q.size()); i++)
                delta += q[i] * w[i][j];
            overlay.delta[j].store(delta, mo);
        }
        overlay.active.store(true, mo);
    };

    connect(ui.qualities_mode_check, &QCheckBox::toggled, this, [this, apply_attitude_projection](bool) {
        ui.quality_stillness_check->setEnabled(*s.qualities_mode_ui);
        ui.quality_continuity_check->setEnabled(*s.qualities_mode_ui);
        ui.quality_robustness_check->setEnabled(*s.qualities_mode_ui);
        ui.quality_decisiveness_check->setEnabled(*s.qualities_mode_ui);
        ui.quality_pathology_defense_check->setEnabled(*s.qualities_mode_ui);
        ui.quality_recovery_pace_check->setEnabled(*s.qualities_mode_ui);
        apply_attitude_projection();
    });
    connect(ui.quality_stillness_check, &QCheckBox::toggled, this, [apply_attitude_projection](bool) { apply_attitude_projection(); });
    connect(ui.quality_continuity_check, &QCheckBox::toggled, this, [apply_attitude_projection](bool) { apply_attitude_projection(); });
    connect(ui.quality_robustness_check, &QCheckBox::toggled, this, [apply_attitude_projection](bool) { apply_attitude_projection(); });
    connect(ui.quality_decisiveness_check, &QCheckBox::toggled, this, [apply_attitude_projection](bool) { apply_attitude_projection(); });
    connect(ui.quality_pathology_defense_check, &QCheckBox::toggled, this, [apply_attitude_projection](bool) { apply_attitude_projection(); });
    connect(ui.quality_recovery_pace_check, &QCheckBox::toggled, this, [apply_attitude_projection](bool) { apply_attitude_projection(); });

    if (behavior_combo_ptr && behavior_desc_ptr)
    {
        struct simplified_profile final {
            const char* name;
            const char* description;
            bool  qualities_mode;
            bool  stillness, continuity, robustness, decisiveness, pathology_def, recovery;
            double alpha_smooth; // 1.0=max smoothing (min alpha), 0.0=max responsive (max alpha)
            double dz;           // σ
        };
        static constexpr simplified_profile profiles[] = {
            { "Raw (neutral)",
              "Pure IMM output. No quality projection \xe2\x80\x94 heads compete on measured evidence alone. Use as a diagnostic baseline.",
              false, false,false,false,false,false,false, 0.50, 0.00 },
            { "Smooth Cruise",
              "Heavy EMA smoothing with noise resistance. Best for slow deliberate head movement and flight-on-rails.",
              true, true,false,true,false,false,false, 0.90, 0.40 },
            { "Nimble",
              "Fast, low-latency response with minimal smoothing. Suited for FPS and competitive play.",
              true, false,true,false,true,false,false, 0.15, 0.00 },
            { "Cockpit Precision",
              "Tight deadzone and pathology protection for instrument flying. Holds steady; suppresses oscillation.",
              true, true,false,true,false,true,false, 0.70, 0.60 },
            { "Recovery Mode",
              "Snaps back quickly after occlusion or large repositioning. Good for webcam and clip-on trackers.",
              true, false,true,false,false,false,true, 0.50, 0.10 },
            { "All Qualities",
              "Activates every quality adaptation simultaneously. The filter self-tunes continuously \xe2\x80\x94 good all-purpose setting.",
              true, true,true,true,true,true,true, 0.45, 0.25 },
        };

        for (const auto& p : profiles)
            behavior_combo_ptr->addItem(tr(p.name));
        behavior_desc_ptr->setText(tr(profiles[0].description));

        connect(behavior_combo_ptr, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, apply_attitude_projection, bd = behavior_desc_ptr](int idx) {
                    if (idx < 0 || idx >= static_cast<int>(std::size(profiles))) return;
                    const auto& p = profiles[idx];
                    bd->setText(tr(p.description));
                    s.qualities_mode_ui         = p.qualities_mode;
                    s.quality_stillness         = p.stillness;
                    s.quality_continuity        = p.continuity;
                    s.quality_robustness        = p.robustness;
                    s.quality_decisiveness      = p.decisiveness;
                    s.quality_pathology_defense = p.pathology_def;
                    s.quality_recovery_pace     = p.recovery;
                    const double alpha_min = lerp(0.4,  0.005, p.alpha_smooth);
                    const double alpha_max = lerp(1.0,  0.02,  p.alpha_smooth);
                    s.rot_alpha_min = options::slider_value{alpha_min, 0.005, 0.4};
                    s.rot_alpha_max = options::slider_value{alpha_max, 0.02,  1.0};
                    s.pos_alpha_min = options::slider_value{alpha_min, 0.005, 0.4};
                    s.pos_alpha_max = options::slider_value{alpha_max, 0.02,  1.0};
                    s.rot_deadzone  = options::slider_value{p.dz, 0.0, 3.0};
                    s.pos_deadzone  = options::slider_value{p.dz, 0.0, 3.0};
                    apply_attitude_projection();
                    s.b->save();
                });
    }

    auto sync_simple_from_advanced = [this, apply_simple_alpha, apply_simple_shape] {
        const double alpha_t = 1.0 - 0.5 * (norm(*s.rot_alpha_min, 0.005, 0.4) + norm(*s.rot_alpha_max, 0.02, 1.0));
        const double shape_t = 0.25 * (
            norm(*s.rot_curve, 0.2, 8.0) +
            norm(*s.pos_curve, 0.2, 8.0) +
            norm(*s.rot_deadzone, 0.0, 3.0) +
            norm(*s.pos_deadzone, 0.0, 3.0));

        {
            const QSignalBlocker b1(ui.simple_alpha_slider);
            const QSignalBlocker b2(ui.simple_shape_slider);
            ui.simple_alpha_slider->setValue(static_cast<int>(alpha_t * ui.simple_alpha_slider->maximum()));
            ui.simple_shape_slider->setValue(static_cast<int>(shape_t * ui.simple_shape_slider->maximum()));
        }
        apply_simple_alpha(ui.simple_alpha_slider->value());
        apply_simple_shape(ui.simple_shape_slider->value());
    };

    auto current_ui_mode_value = [this]() {
        if (!ui_mode_combo)
            return sanitize_ui_mode(*s.ui_complexity_mode);
        const int idx = ui_mode_combo->currentIndex();
        if (idx < 0)
            return sanitize_ui_mode(*s.ui_complexity_mode);
        return sanitize_ui_mode(ui_mode_combo->itemData(idx).toInt());
    };

    auto update_advanced_visibility = [this, current_ui_mode_value] {
        const int mode = current_ui_mode_value();
        const bool adv = mode != static_cast<int>(ui_complexity_mode::basic);
        ui.controls_frame->setEnabled(adv);
        ui.ema_enabled_check->setEnabled(adv);
        ui.brownian_enabled_check->setEnabled(adv);
        ui.predictive_enabled_check->setEnabled(adv);
        ui.chi_square_enabled_check->setEnabled(adv);
        ui.pareto_enabled_check->setEnabled(adv);
        ui.outlier_quarantine_enabled_check->setEnabled(adv);
    };

    auto apply_ui_mode = [this, update_advanced_visibility](int mode_value) {
        int mode = sanitize_ui_mode(mode_value);
        s.ui_complexity_mode = mode;

        const bool is_basic = mode == static_cast<int>(ui_complexity_mode::basic);
        const bool is_simplified = mode == static_cast<int>(ui_complexity_mode::simplified);
        const bool is_advanced = mode == static_cast<int>(ui_complexity_mode::advanced);

        ui.simple_controls_frame->setVisible(is_basic);
        if (simplified_placeholder)
            simplified_placeholder->setVisible(is_simplified);
        ui.controls_frame->setVisible(false);
        ui.qualities_frame->setVisible(false);
        ui.adaptive_mode_check->setVisible(false);
        ui.ema_enabled_check->setVisible(false);
        ui.brownian_enabled_check->setVisible(false);
        ui.predictive_enabled_check->setVisible(false);
        ui.chi_square_enabled_check->setVisible(false);
        ui.pareto_enabled_check->setVisible(false);
        ui.outlier_quarantine_enabled_check->setVisible(false);
        ui.status_frame->setVisible(!is_basic);
        if (advanced_canvas)
            advanced_canvas->setVisible(is_advanced);

        update_advanced_visibility();
    };

    if (ui_mode_combo)
    {
        connect(ui_mode_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, apply_ui_mode](int idx) {
                    if (!ui_mode_combo)
                        return;
                    apply_ui_mode(ui_mode_combo->itemData(idx).toInt());
                    s.b->save();
                });
    }

    sync_simple_from_advanced();
    update_advanced_visibility();
    ui.quality_stillness_check->setEnabled(*s.qualities_mode_ui);
    ui.quality_continuity_check->setEnabled(*s.qualities_mode_ui);
    ui.quality_robustness_check->setEnabled(*s.qualities_mode_ui);
    ui.quality_decisiveness_check->setEnabled(*s.qualities_mode_ui);
    ui.quality_pathology_defense_check->setEnabled(*s.qualities_mode_ui);
    ui.quality_recovery_pace_check->setEnabled(*s.qualities_mode_ui);
    apply_attitude_projection();

    if (ui_mode_combo)
    {
        const int stored_mode = sanitize_ui_mode(*s.ui_complexity_mode);
        int select_idx = ui_mode_combo->findData(stored_mode);
        if (select_idx < 0)
            select_idx = ui_mode_combo->findData(static_cast<int>(ui_complexity_mode::simplified));
        if (select_idx >= 0)
            ui_mode_combo->setCurrentIndex(select_idx);
        apply_ui_mode(stored_mode);
    }

    ui.status_text->setVisible(false);
    ui.status_value->setVisible(false);
    ui.info_rot_text->setVisible(false);
    ui.info_rot_value->setVisible(false);
    ui.info_pos_text->setVisible(false);
    ui.info_pos_value->setVisible(false);
    ui.info_rot_brownian_text->setVisible(false);
    ui.info_rot_brownian_value->setVisible(false);
    ui.info_pos_brownian_text->setVisible(false);
    ui.info_pos_brownian_value->setVisible(false);
    ui.info_predictive_error_text->setVisible(false);
    ui.info_predictive_error_value->setVisible(false);

    const QFont fixed_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    ui.status_value->setFont(fixed_font);
    ui.info_rot_value->setFont(fixed_font);
    ui.info_pos_value->setFont(fixed_font);
    ui.info_rot_brownian_value->setFont(fixed_font);
    ui.info_pos_brownian_value->setFont(fixed_font);
    ui.info_rot_contrib_value->setFont(fixed_font);
    ui.info_pos_contrib_value->setFont(fixed_font);
    ui.info_predictive_error_value->setFont(fixed_font);

    const QFontMetrics fm(fixed_font);
    const QString status_template = QStringLiteral("Mon|E1 B1 A1 P1 M1|rE0.000 rP0.000 pE0.000 pP0.000 k0.000");
    ui.status_value->setMinimumWidth(fm.horizontalAdvance(status_template));
    ui.status_value->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* calib_timer = new QTimer(this);
    calib_timer->setInterval(100);
    connect(calib_timer, &QTimer::timeout, this, [this] {
        pull_status_into_ui(false);
    });
    calib_timer->start();
}

dialog_alpha_spectrum::~dialog_alpha_spectrum()
{
    auto& overlay = detail::alpha_spectrum::shared_quality_overlay_state();
    overlay.active.store(false, mo);
    for (int j = 0; j < detail::alpha_spectrum::quality_overlay_state::value_count; j++)
        overlay.delta[j].store(0.0, mo);
    detail::alpha_spectrum::shared_calibration_status().ui_open.store(false, mo);
}

void dialog_alpha_spectrum::pull_status_into_ui(bool commit_to_settings)
{
    const auto& status = detail::alpha_spectrum::shared_calibration_status();

    if (!status.ui_open.load(mo) && !status.active.load(mo) && !commit_to_settings)
        return;

    const double rot_min = status.rot_alpha_min.load(mo);
    const double rot_max = status.rot_alpha_max.load(mo);
    const double rot_curve = status.rot_curve.load(mo);
    const double rot_deadzone = status.rot_deadzone.load(mo);
    const double pos_min = status.pos_alpha_min.load(mo);
    const double pos_max = status.pos_alpha_max.load(mo);
    const double pos_curve = status.pos_curve.load(mo);
    const double pos_deadzone = status.pos_deadzone.load(mo);
    const double rot_jitter            = status.jitter.rot.load(mo);
    const double pos_jitter            = status.jitter.pos.load(mo);
    const double rot_objective         = status.objective.rot.load(mo);
    const double pos_objective         = status.objective.pos.load(mo);
    const double rot_brownian_raw      = status.brownian_raw.rot.load(mo);
    const double rot_brownian_filtered = status.brownian_filtered.rot.load(mo);
    const double rot_brownian_delta    = status.brownian_delta.rot.load(mo);
    const double rot_brownian_damped   = status.brownian_damped.rot.load(mo);
    const double rot_predictive_error  = status.predictive_error.rot.load(mo);
    const double pos_brownian_raw      = status.brownian_raw.pos.load(mo);
    const double pos_brownian_filtered = status.brownian_filtered.pos.load(mo);
    const double pos_brownian_delta    = status.brownian_delta.pos.load(mo);
    const double pos_brownian_damped   = status.brownian_damped.pos.load(mo);
    const double pos_predictive_error  = status.predictive_error.pos.load(mo);
    namespace as = detail::alpha_spectrum;
    const double rot_ema_drive        = status.rot_drive[as::index(as::tracking_head::ema)].load(mo);
    const double rot_brownian_drive   = status.rot_drive[as::index(as::tracking_head::brownian)].load(mo);
    const double rot_adaptive_drive   = status.rot_drive[as::index(as::tracking_head::adaptive)].load(mo);
    const double rot_predictive_drive = status.rot_drive[as::index(as::tracking_head::predictive)].load(mo);
    const double rot_mtm_drive        = status.mtm_drive.rot.load(mo);
    const double pos_ema_drive        = status.pos_drive[as::index(as::tracking_head::ema)].load(mo);
    const double pos_brownian_drive   = status.pos_drive[as::index(as::tracking_head::brownian)].load(mo);
    const double pos_adaptive_drive   = status.pos_drive[as::index(as::tracking_head::adaptive)].load(mo);
    const double pos_predictive_drive = status.pos_drive[as::index(as::tracking_head::predictive)].load(mo);
    const double pos_mtm_drive        = status.mtm_drive.pos.load(mo);
    const double rot_mode_expectation = status.mode_expectation.rot.load(mo);
    const double pos_mode_expectation = status.mode_expectation.pos.load(mo);
    const double rot_mode_peak        = status.mode_peak.rot.load(mo);
    const double pos_mode_peak        = status.mode_peak.pos.load(mo);
    const double rot_mode_purity      = status.mode_purity.rot.load(mo);
    const double pos_mode_purity      = status.mode_purity.pos.load(mo);
    const double ngc_coupling_residual = status.ngc_coupling_residual.load(mo);
    const double outlier_quarantine_activity = status.outlier_quarantine_activity.load(mo);
    const bool active = status.active.load(mo);

    {
        const QSignalBlocker b1(ui.rot_min_slider);
        const QSignalBlocker b2(ui.rot_max_slider);
        const QSignalBlocker b3(ui.rot_curve_slider);
        const QSignalBlocker b4(ui.rot_deadzone_slider);
        const QSignalBlocker b5(ui.pos_min_slider);
        const QSignalBlocker b6(ui.pos_max_slider);
        const QSignalBlocker b7(ui.pos_curve_slider);
        const QSignalBlocker b8(ui.pos_deadzone_slider);

        const auto rot_min_cfg = *s.rot_alpha_min;
        const auto rot_max_cfg = *s.rot_alpha_max;
        const auto rot_curve_cfg = *s.rot_curve;
        const auto rot_deadzone_cfg = *s.rot_deadzone;
        const auto pos_min_cfg = *s.pos_alpha_min;
        const auto pos_max_cfg = *s.pos_alpha_max;
        const auto pos_curve_cfg = *s.pos_curve;
        const auto pos_deadzone_cfg = *s.pos_deadzone;

        ui.rot_min_slider->setValue(options::slider_value{rot_min, rot_min_cfg.min(), rot_min_cfg.max()}.to_slider_pos(ui.rot_min_slider->minimum(), ui.rot_min_slider->maximum()));
        ui.rot_max_slider->setValue(options::slider_value{rot_max, rot_max_cfg.min(), rot_max_cfg.max()}.to_slider_pos(ui.rot_max_slider->minimum(), ui.rot_max_slider->maximum()));
        ui.rot_curve_slider->setValue(options::slider_value{rot_curve, rot_curve_cfg.min(), rot_curve_cfg.max()}.to_slider_pos(ui.rot_curve_slider->minimum(), ui.rot_curve_slider->maximum()));
        ui.rot_deadzone_slider->setValue(options::slider_value{rot_deadzone, rot_deadzone_cfg.min(), rot_deadzone_cfg.max()}.to_slider_pos(ui.rot_deadzone_slider->minimum(), ui.rot_deadzone_slider->maximum()));
        ui.pos_min_slider->setValue(options::slider_value{pos_min, pos_min_cfg.min(), pos_min_cfg.max()}.to_slider_pos(ui.pos_min_slider->minimum(), ui.pos_min_slider->maximum()));
        ui.pos_max_slider->setValue(options::slider_value{pos_max, pos_max_cfg.min(), pos_max_cfg.max()}.to_slider_pos(ui.pos_max_slider->minimum(), ui.pos_max_slider->maximum()));
        ui.pos_curve_slider->setValue(options::slider_value{pos_curve, pos_curve_cfg.min(), pos_curve_cfg.max()}.to_slider_pos(ui.pos_curve_slider->minimum(), ui.pos_curve_slider->maximum()));
        ui.pos_deadzone_slider->setValue(options::slider_value{pos_deadzone, pos_deadzone_cfg.min(), pos_deadzone_cfg.max()}.to_slider_pos(ui.pos_deadzone_slider->minimum(), ui.pos_deadzone_slider->maximum()));
    }

    ui.rot_min_label->setText(QStringLiteral("%1%").arg(rot_min * 100.0, 0, 'f', 1));
    ui.rot_max_label->setText(QStringLiteral("%1%").arg(rot_max * 100.0, 0, 'f', 1));
    ui.rot_curve_label->setText(QStringLiteral("%1").arg(rot_curve, 0, 'f', 2));
    ui.rot_deadzone_label->setText(tr("%1σ").arg(rot_deadzone, 0, 'f', 2));
    ui.pos_min_label->setText(QStringLiteral("%1%").arg(pos_min * 100.0, 0, 'f', 1));
    ui.pos_max_label->setText(QStringLiteral("%1%").arg(pos_max * 100.0, 0, 'f', 1));
    ui.pos_curve_label->setText(QStringLiteral("%1").arg(pos_curve, 0, 'f', 2));
    ui.pos_deadzone_label->setText(tr("%1σ").arg(pos_deadzone, 0, 'f', 2));

    ui.info_rot_value->setText(QStringLiteral("%1 / %2")
                                        .arg(rot_jitter, 0, 'f', 4)
                                        .arg(rot_objective, 0, 'f', 4));
    ui.info_pos_value->setText(QStringLiteral("%1 / %2")
                                        .arg(pos_jitter, 0, 'f', 4)
                                        .arg(pos_objective, 0, 'f', 4));
    ui.info_rot_brownian_value->setText(
        QStringLiteral("%1 / %2 / Δ%3 / %4%")
            .arg(rot_brownian_raw, 0, 'f', 4)
            .arg(rot_brownian_filtered, 0, 'f', 4)
            .arg(rot_brownian_delta, 0, 'f', 4)
            .arg(rot_brownian_damped * 100.0, 0, 'f', 1));
    ui.info_pos_brownian_value->setText(
        QStringLiteral("%1 / %2 / Δ%3 / %4%")
            .arg(pos_brownian_raw, 0, 'f', 4)
            .arg(pos_brownian_filtered, 0, 'f', 4)
            .arg(pos_brownian_delta, 0, 'f', 4)
            .arg(pos_brownian_damped * 100.0, 0, 'f', 1));
    ui.info_predictive_error_value->setText(
        QStringLiteral("%1 / %2")
            .arg(rot_predictive_error, 0, 'f', 4)
            .arg(pos_predictive_error, 0, 'f', 4));
    ui.info_rot_contrib_value->setText(
        QStringLiteral("EMA:%1 Br:%2 Ad:%3 Pr:%4 Cs:%5 Pa:%6 MTM:%7")
            .arg(rot_ema_drive, 0, 'f', 3)
            .arg(rot_brownian_drive, 0, 'f', 3)
            .arg(rot_adaptive_drive, 0, 'f', 3)
            .arg(rot_predictive_drive, 0, 'f', 3)
            .arg(status.rot_drive[as::index(as::tracking_head::chi_square)].load(mo), 0, 'f', 3)
            .arg(status.rot_drive[as::index(as::tracking_head::pareto)].load(mo), 0, 'f', 3)
            .arg(rot_mtm_drive, 0, 'f', 3));
    ui.info_pos_contrib_value->setText(
        QStringLiteral("EMA:%1 Br:%2 Ad:%3 Pr:%4 Cs:%5 Pa:%6 MTM:%7")
            .arg(pos_ema_drive, 0, 'f', 3)
            .arg(pos_brownian_drive, 0, 'f', 3)
            .arg(pos_adaptive_drive, 0, 'f', 3)
            .arg(pos_predictive_drive, 0, 'f', 3)
            .arg(status.pos_drive[as::index(as::tracking_head::chi_square)].load(mo), 0, 'f', 3)
            .arg(status.pos_drive[as::index(as::tracking_head::pareto)].load(mo), 0, 'f', 3)
            .arg(pos_mtm_drive, 0, 'f', 3));
    ui.status_value->setText(
        active ?
            QStringLiteral("Mon|E%1 B%2 A%3 P%4|rE%5 rP%6 pE%7 pP%8 rQ%9 pQ%10 k%11")
                .arg(*s.ema_enabled ? 1 : 0)
                .arg(*s.brownian_enabled ? 1 : 0)
                .arg(*s.adaptive_mode ? 1 : 0)
                .arg(*s.predictive_enabled ? 1 : 0)
                .arg(rot_mode_expectation, 5, 'f', 3)
                .arg(rot_mode_peak, 5, 'f', 3)
                .arg(pos_mode_expectation, 5, 'f', 3)
                .arg(pos_mode_peak, 5, 'f', 3)
                .arg(rot_mode_purity, 5, 'f', 3)
                .arg(pos_mode_purity, 5, 'f', 3)
                .arg(ngc_coupling_residual, 5, 'f', 3)
            : tr("Idle"));

    QStringList active_qualities;
    if (*s.quality_stillness) active_qualities << tr("Stillness");
    if (*s.quality_continuity) active_qualities << tr("Continuity");
    if (*s.quality_robustness) active_qualities << tr("Robustness");
    if (*s.quality_decisiveness) active_qualities << tr("Decisiveness");
    if (*s.quality_pathology_defense) active_qualities << tr("Pathology Defense");
    if (*s.quality_recovery_pace) active_qualities << tr("Recovery Pace");

    const QString quality_text = active_qualities.isEmpty() ? tr("none") : active_qualities.join(QStringLiteral(", "));
    ui.qualities_state_label->setText(
        tr("Alchemy state: [%1] | rot purity %2 | pos purity %3 | quarantine %4%")
            .arg(quality_text)
            .arg(rot_mode_purity, 0, 'f', 3)
            .arg(pos_mode_purity, 0, 'f', 3)
            .arg(outlier_quarantine_activity * 100.0, 0, 'f', 1));

    if (commit_to_settings)
    {
        const auto rot_min_cfg = *s.rot_alpha_min;
        const auto rot_max_cfg = *s.rot_alpha_max;
        const auto rot_curve_cfg = *s.rot_curve;
        const auto rot_deadzone_cfg = *s.rot_deadzone;
        const auto pos_min_cfg = *s.pos_alpha_min;
        const auto pos_max_cfg = *s.pos_alpha_max;
        const auto pos_curve_cfg = *s.pos_curve;
        const auto pos_deadzone_cfg = *s.pos_deadzone;

        s.rot_alpha_min = options::slider_value{rot_min, rot_min_cfg.min(), rot_min_cfg.max()};
        s.rot_alpha_max = options::slider_value{rot_max, rot_max_cfg.min(), rot_max_cfg.max()};
        s.rot_curve = options::slider_value{rot_curve, rot_curve_cfg.min(), rot_curve_cfg.max()};
        s.rot_deadzone = options::slider_value{rot_deadzone, rot_deadzone_cfg.min(), rot_deadzone_cfg.max()};
        s.pos_alpha_min = options::slider_value{pos_min, pos_min_cfg.min(), pos_min_cfg.max()};
        s.pos_alpha_max = options::slider_value{pos_max, pos_max_cfg.min(), pos_max_cfg.max()};
        s.pos_curve = options::slider_value{pos_curve, pos_curve_cfg.min(), pos_curve_cfg.max()};
        s.pos_deadzone = options::slider_value{pos_deadzone, pos_deadzone_cfg.min(), pos_deadzone_cfg.max()};
    }
}

void dialog_alpha_spectrum::reset_to_defaults()
{
    s.rot_alpha_min.set_to_default();
    s.rot_alpha_max.set_to_default();
    s.rot_curve.set_to_default();
    s.rot_deadzone.set_to_default();
    s.pos_alpha_min.set_to_default();
    s.pos_alpha_max.set_to_default();
    s.pos_curve.set_to_default();
    s.pos_deadzone.set_to_default();
    s.brownian_head_gain.set_to_default();
    s.adaptive_threshold_lift.set_to_default();
    s.predictive_head_gain.set_to_default();
    s.chi_square_head_gain.set_to_default();
    s.pareto_head_gain.set_to_default();
    s.outlier_quarantine_strength.set_to_default();
    s.mtm_shoulder_base.set_to_default();
    s.ngc_kappa.set_to_default();
    s.ngc_nominal_z.set_to_default();
    s.adaptive_mode.set_to_default();
    s.ema_enabled.set_to_default();
    s.brownian_enabled.set_to_default();
    s.predictive_enabled.set_to_default();
    s.chi_square_enabled.set_to_default();
    s.pareto_enabled.set_to_default();
    s.outlier_quarantine_enabled.set_to_default();

    auto& overlay = detail::alpha_spectrum::shared_quality_overlay_state();
    overlay.active.store(false, mo);
    for (int j = 0; j < detail::alpha_spectrum::quality_overlay_state::value_count; j++)
        overlay.delta[j].store(0.0, mo);

    s.b->save();
    s.b->reload();

    const double alpha_t = 1.0 - 0.5 * (norm(*s.rot_alpha_min, 0.005, 0.4) + norm(*s.rot_alpha_max, 0.02, 1.0));
    const double shape_t = 0.25 * (
        norm(*s.rot_curve, 0.2, 8.0) +
        norm(*s.pos_curve, 0.2, 8.0) +
        norm(*s.rot_deadzone, 0.0, 3.0) +
        norm(*s.pos_deadzone, 0.0, 3.0));

    {
        const QSignalBlocker b1(ui.simple_alpha_slider);
        const QSignalBlocker b2(ui.simple_shape_slider);
        ui.simple_alpha_slider->setValue(static_cast<int>(alpha_t * ui.simple_alpha_slider->maximum()));
        ui.simple_shape_slider->setValue(static_cast<int>(shape_t * ui.simple_shape_slider->maximum()));
    }

    ui.simple_alpha_label->setText(QStringLiteral("EMA α: [%1%, %2%]")
                                       .arg((*s.rot_alpha_min) * 100.0, 0, 'f', 1)
                                       .arg((*s.rot_alpha_max) * 100.0, 0, 'f', 1));
    ui.simple_shape_label->setText(QStringLiteral("Curve %1 / RotDZ %2σ / PosDZ %3σ")
                                       .arg(static_cast<double>(*s.rot_curve), 0, 'f', 2)
                                       .arg(static_cast<double>(*s.rot_deadzone), 0, 'f', 2)
                                       .arg(static_cast<double>(*s.pos_deadzone), 0, 'f', 2));

    const int reset_mode = sanitize_ui_mode(*s.ui_complexity_mode);
    if (ui_mode_combo)
    {
        const int mode = sanitize_ui_mode(*s.ui_complexity_mode);
        int idx = ui_mode_combo->findData(mode);
        if (idx < 0)
            idx = ui_mode_combo->findData(static_cast<int>(ui_complexity_mode::simplified));
        if (idx >= 0)
            ui_mode_combo->setCurrentIndex(idx);
    }
    const bool reset_adv_controls = reset_mode != static_cast<int>(ui_complexity_mode::basic);
    ui.controls_frame->setEnabled(reset_adv_controls);
    ui.ema_enabled_check->setEnabled(reset_adv_controls);
    ui.brownian_enabled_check->setEnabled(reset_adv_controls);
    ui.predictive_enabled_check->setEnabled(reset_adv_controls);
    ui.chi_square_enabled_check->setEnabled(reset_adv_controls);
    ui.pareto_enabled_check->setEnabled(reset_adv_controls);
    ui.outlier_quarantine_enabled_check->setEnabled(reset_adv_controls);
}

void dialog_alpha_spectrum::doOK()
{
    save();
    close();
}

void dialog_alpha_spectrum::doCancel()
{
    close();
}

void dialog_alpha_spectrum::save()
{
    s.b->save();
}

void dialog_alpha_spectrum::reload()
{
    s.b->reload();
}

void dialog_alpha_spectrum::set_buttons_visible(bool x)
{
    ui.buttonBox->setVisible(x);
}
