#include "ftnoir_filter_alpha_spectrum.h"

#include <QCheckBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
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

namespace {
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
                    event->accept();
                    return;
                }
                QGraphicsProxyWidget::mouseReleaseEvent(event);
            }

        private:
            bool dragging = false;
            QPointF drag_offset;
        };

        explicit advanced_node_canvas(settings_alpha_spectrum* settings, QWidget* parent) :
            QWidget(parent), s(settings)
        {
            auto* root = new QVBoxLayout(this);
            auto* title = new QLabel(tr("Advanced Node Editor (Qt-native scaffold)"), this);
            auto* hint = new QLabel(tr("This advanced surface is native Qt and uses the same settings API.\n"
                                       "Nodes are draggable Qt widgets; links reflect architectural flow."), this);
            hint->setWordWrap(true);
            hint->setFrameShape(QFrame::StyledPanel);

            scene = new QGraphicsScene(this);
            view = new QGraphicsView(scene, this);
            view->setRenderHint(QPainter::Antialiasing, true);
            view->setDragMode(QGraphicsView::RubberBandDrag);
            view->setMinimumHeight(460);
            view->setSceneRect(0, 0, 1500, 520);

            auto add_checkbox_binding = [this](QVBoxLayout* layout, const QString& text,
                                               std::function<bool()> getter,
                                               std::function<void(bool)> setter) {
                auto* box = new QCheckBox(text);
                layout->addWidget(box);
                connect(box, &QCheckBox::toggled, this, [setter](bool v) { setter(v); });
                checks.push_back({box, std::move(getter), std::move(setter)});
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
                sliders.push_back({slider, value, min_v, max_v, std::move(getter), std::move(setter), std::move(formatter)});
            };

            auto* core_group = new QGroupBox(tr("Core EMA Geometry"));
            auto* core_layout = new QGridLayout(core_group);
            add_slider_binding(core_layout, 0, tr("Rot Min"), 0.005, 0.4,
                               [this] { return (double)*s->rot_alpha_min; },
                               [this](double v) { s->rot_alpha_min = options::slider_value{v, 0.005, 0.4}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(core_layout, 1, tr("Rot Max"), 0.02, 1.0,
                               [this] { return (double)*s->rot_alpha_max; },
                               [this](double v) { s->rot_alpha_max = options::slider_value{v, 0.02, 1.0}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(core_layout, 2, tr("Rot Curve"), 0.2, 8.0,
                               [this] { return (double)*s->rot_curve; },
                               [this](double v) { s->rot_curve = options::slider_value{v, 0.2, 8.0}; },
                               [](double v) { return QStringLiteral("%1").arg(v, 0, 'f', 2); });
            add_slider_binding(core_layout, 3, tr("Rot DZ"), 0.0, 0.3,
                               [this] { return (double)*s->rot_deadzone; },
                               [this](double v) { s->rot_deadzone = options::slider_value{v, 0.0, 0.3}; },
                               [](double v) { return QStringLiteral("%1°").arg(v, 0, 'f', 3); });
            add_slider_binding(core_layout, 4, tr("Pos Min"), 0.005, 0.4,
                               [this] { return (double)*s->pos_alpha_min; },
                               [this](double v) { s->pos_alpha_min = options::slider_value{v, 0.005, 0.4}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(core_layout, 5, tr("Pos Max"), 0.02, 1.0,
                               [this] { return (double)*s->pos_alpha_max; },
                               [this](double v) { s->pos_alpha_max = options::slider_value{v, 0.02, 1.0}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(core_layout, 6, tr("Pos Curve"), 0.2, 8.0,
                               [this] { return (double)*s->pos_curve; },
                               [this](double v) { s->pos_curve = options::slider_value{v, 0.2, 8.0}; },
                               [](double v) { return QStringLiteral("%1").arg(v, 0, 'f', 2); });
            add_slider_binding(core_layout, 7, tr("Pos DZ"), 0.0, 2.0,
                               [this] { return (double)*s->pos_deadzone; },
                               [this](double v) { s->pos_deadzone = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1mm").arg(v, 0, 'f', 3); });

            auto* ema_group = new QGroupBox(tr("Head: EMA"));
            auto* ema_layout = new QVBoxLayout(ema_group);
            ema_check = add_checkbox_binding(ema_layout, tr("Enabled"),
                                             [this] { return *s->ema_enabled; },
                                             [this](bool v) { s->ema_enabled = v; });

            auto* brownian_group = new QGroupBox(tr("Head: Brownian"));
            auto* brownian_layout = new QGridLayout(brownian_group);
            brownian_check = new QCheckBox(tr("Enabled"), brownian_group);
            brownian_layout->addWidget(brownian_check, 0, 0, 1, 3);
            connect(brownian_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->brownian_enabled = v; });
            checks.push_back({brownian_check, [this] { return *s->brownian_enabled; }, [this](bool v) { s->brownian_enabled = v; }});
            add_slider_binding(brownian_layout, 1, tr("Gain"), 0.0, 2.0,
                               [this] { return (double)*s->brownian_head_gain; },
                               [this](double v) { s->brownian_head_gain = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1x").arg(v, 0, 'f', 2); });

            auto* adaptive_group = new QGroupBox(tr("Head: Adaptive"));
            auto* adaptive_layout = new QGridLayout(adaptive_group);
            adaptive_check = new QCheckBox(tr("Enabled"), adaptive_group);
            adaptive_layout->addWidget(adaptive_check, 0, 0, 1, 3);
            connect(adaptive_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->adaptive_mode = v; });
            checks.push_back({adaptive_check, [this] { return *s->adaptive_mode; }, [this](bool v) { s->adaptive_mode = v; }});
            add_slider_binding(adaptive_layout, 1, tr("Lift"), 0.0, 0.6,
                               [this] { return (double)*s->adaptive_threshold_lift; },
                               [this](double v) { s->adaptive_threshold_lift = options::slider_value{v, 0.0, 0.6}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });

            auto* predictive_group = new QGroupBox(tr("Head: Predictive"));
            auto* predictive_layout = new QGridLayout(predictive_group);
            predictive_check = new QCheckBox(tr("Enabled"), predictive_group);
            predictive_layout->addWidget(predictive_check, 0, 0, 1, 3);
            connect(predictive_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->predictive_enabled = v; });
            checks.push_back({predictive_check, [this] { return *s->predictive_enabled; }, [this](bool v) { s->predictive_enabled = v; }});
            add_slider_binding(predictive_layout, 1, tr("Rot Gain"), 0.0, 2.0,
                               [this] { return (double)*s->predictive_head_gain; },
                               [this](double v) { s->predictive_head_gain = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1x").arg(v, 0, 'f', 2); });
            add_slider_binding(predictive_layout, 2, tr("Trans Gain"), 0.0, 2.0,
                               [this] { return (double)*s->predictive_translation_gain; },
                               [this](double v) { s->predictive_translation_gain = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1x").arg(v, 0, 'f', 2); });

            auto* entropy_group = new QGroupBox(tr("Entropy / MTM"));
            auto* entropy_layout = new QGridLayout(entropy_group);
            mtm_check = new QCheckBox(tr("MTM enabled"), entropy_group);
            entropy_layout->addWidget(mtm_check, 0, 0, 1, 3);
            connect(mtm_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->mtm_enabled = v; });
            checks.push_back({mtm_check, [this] { return *s->mtm_enabled; }, [this](bool v) { s->mtm_enabled = v; }});
            add_slider_binding(entropy_layout, 1, tr("Shoulder"), 0.0, 1.0,
                               [this] { return (double)*s->mtm_shoulder_base; },
                               [this](double v) { s->mtm_shoulder_base = options::slider_value{v, 0.0, 1.0}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });
            add_slider_binding(entropy_layout, 2, tr("NGC Kappa"), 0.0, 0.3,
                               [this] { return (double)*s->ngc_kappa; },
                               [this](double v) { s->ngc_kappa = options::slider_value{v, 0.0, 0.3}; },
                               [](double v) { return QStringLiteral("%1").arg(v, 0, 'f', 3); });
            add_slider_binding(entropy_layout, 3, tr("NGC Nominal Z"), 0.3, 2.0,
                               [this] { return (double)*s->ngc_nominal_z; },
                               [this](double v) { s->ngc_nominal_z = options::slider_value{v, 0.3, 2.0}; },
                               [](double v) { return QStringLiteral("%1m").arg(v, 0, 'f', 2); });
            purity_value = new QLabel(entropy_group);
            entropy_layout->addWidget(new QLabel(tr("Purity"), entropy_group), 4, 0);
            entropy_layout->addWidget(purity_value, 4, 1, 1, 2);

            auto* robust_group = new QGroupBox(tr("Robustness"));
            auto* robust_layout = new QGridLayout(robust_group);
            chi_check = new QCheckBox(tr("Chi-square"), robust_group);
            pareto_check = new QCheckBox(tr("Pareto"), robust_group);
            quarantine_check = new QCheckBox(tr("Quarantine"), robust_group);
            robust_layout->addWidget(chi_check, 0, 0, 1, 3);
            robust_layout->addWidget(pareto_check, 1, 0, 1, 3);
            robust_layout->addWidget(quarantine_check, 2, 0, 1, 3);
            connect(chi_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->chi_square_enabled = v; });
            connect(pareto_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->pareto_enabled = v; });
            connect(quarantine_check, &QCheckBox::toggled, this, [this](bool v) { if (s) s->outlier_quarantine_enabled = v; });
            checks.push_back({chi_check, [this] { return *s->chi_square_enabled; }, [this](bool v) { s->chi_square_enabled = v; }});
            checks.push_back({pareto_check, [this] { return *s->pareto_enabled; }, [this](bool v) { s->pareto_enabled = v; }});
            checks.push_back({quarantine_check, [this] { return *s->outlier_quarantine_enabled; }, [this](bool v) { s->outlier_quarantine_enabled = v; }});
            add_slider_binding(robust_layout, 3, tr("Chi Gain"), 0.0, 2.0,
                               [this] { return (double)*s->chi_square_head_gain; },
                               [this](double v) { s->chi_square_head_gain = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1x").arg(v, 0, 'f', 2); });
            add_slider_binding(robust_layout, 4, tr("Pareto Gain"), 0.0, 2.0,
                               [this] { return (double)*s->pareto_head_gain; },
                               [this](double v) { s->pareto_head_gain = options::slider_value{v, 0.0, 2.0}; },
                               [](double v) { return QStringLiteral("%1x").arg(v, 0, 'f', 2); });
            add_slider_binding(robust_layout, 5, tr("Quarantine"), 0.0, 1.0,
                               [this] { return (double)*s->outlier_quarantine_strength; },
                               [this](double v) { s->outlier_quarantine_strength = options::slider_value{v, 0.0, 1.0}; },
                               [](double v) { return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 1); });

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

            core_proxy = new draggable_proxy();
            core_proxy->setWidget(core_group);
            scene->addItem(core_proxy);
            ema_proxy = new draggable_proxy();
            ema_proxy->setWidget(ema_group);
            scene->addItem(ema_proxy);
            brownian_proxy = new draggable_proxy();
            brownian_proxy->setWidget(brownian_group);
            scene->addItem(brownian_proxy);
            adaptive_proxy = new draggable_proxy();
            adaptive_proxy->setWidget(adaptive_group);
            scene->addItem(adaptive_proxy);
            predictive_proxy = new draggable_proxy();
            predictive_proxy->setWidget(predictive_group);
            scene->addItem(predictive_proxy);
            entropy_proxy = scene->addWidget(entropy_group);
            robust_proxy = scene->addWidget(robust_group);
            quality_proxy = scene->addWidget(quality_group);
            for (auto* proxy : {entropy_proxy, robust_proxy, quality_proxy})
            {
                proxy->setFlag(QGraphicsItem::ItemIsMovable, true);
                proxy->setFlag(QGraphicsItem::ItemIsSelectable, true);
                proxy->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
            }
            for (auto* proxy : {core_proxy, ema_proxy, brownian_proxy, adaptive_proxy, predictive_proxy})
            {
                proxy->setFlag(QGraphicsItem::ItemIsSelectable, true);
                proxy->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
            }
            core_proxy->setPos(20, 40);
            ema_proxy->setPos(340, 20);
            brownian_proxy->setPos(340, 120);
            adaptive_proxy->setPos(340, 240);
            predictive_proxy->setPos(340, 360);
            entropy_proxy->setPos(700, 30);
            robust_proxy->setPos(1080, 60);
            quality_proxy->setPos(720, 280);

            core_to_ema = scene->addPath(QPainterPath(), QPen(QColor(110, 140, 220), 2.0));
            core_to_brownian = scene->addPath(QPainterPath(), QPen(QColor(110, 140, 220), 2.0));
            core_to_adaptive = scene->addPath(QPainterPath(), QPen(QColor(110, 140, 220), 2.0));
            core_to_predictive = scene->addPath(QPainterPath(), QPen(QColor(110, 140, 220), 2.0));
            brownian_to_entropy = scene->addPath(QPainterPath(), QPen(QColor(80, 140, 220), 2.0));
            adaptive_to_entropy = scene->addPath(QPainterPath(), QPen(QColor(80, 140, 220), 2.0));
            predictive_to_entropy = scene->addPath(QPainterPath(), QPen(QColor(80, 140, 220), 2.0));
            entropy_to_robust = scene->addPath(QPainterPath(), QPen(QColor(220, 140, 80), 2.0));
            predictive_to_robust = scene->addPath(QPainterPath(), QPen(QColor(140, 180, 100), 1.5, Qt::DashLine));
            quality_to_robust = scene->addPath(QPainterPath(), QPen(QColor(170, 110, 210), 1.5, Qt::DashLine));

            edge_label_entropy = scene->addSimpleText(tr("Fusion -> Robustness"));
            edge_label_predictive = scene->addSimpleText(tr("Predictive shortcut"));
            edge_label_quality = scene->addSimpleText(tr("Quality overlay"));
            for (auto* label : {edge_label_entropy, edge_label_predictive, edge_label_quality})
                label->setBrush(QBrush(QColor(190, 190, 190)));

            root->addWidget(title);
            root->addWidget(hint);
            root->addWidget(view, 1);

            auto* frame_timer = new QTimer(this);
            frame_timer->setInterval(100);
            QObject::connect(frame_timer, &QTimer::timeout, this, [this] {
                if (!s)
                    return;
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
                                          .arg(detail::alpha_spectrum::shared_calibration_status().rot_mode_purity.load(std::memory_order_relaxed), 0, 'f', 3)
                                          .arg(detail::alpha_spectrum::shared_calibration_status().pos_mode_purity.load(std::memory_order_relaxed), 0, 'f', 3));
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

            if (core_to_ema)
                core_to_ema->setPath(make_path(core_proxy, ema_proxy));
            if (core_to_brownian)
                core_to_brownian->setPath(make_path(core_proxy, brownian_proxy));
            if (core_to_adaptive)
                core_to_adaptive->setPath(make_path(core_proxy, adaptive_proxy));
            if (core_to_predictive)
                core_to_predictive->setPath(make_path(core_proxy, predictive_proxy));
            if (brownian_to_entropy)
                brownian_to_entropy->setPath(make_path(brownian_proxy, entropy_proxy));
            if (adaptive_to_entropy)
                adaptive_to_entropy->setPath(make_path(adaptive_proxy, entropy_proxy));
            if (predictive_to_entropy)
                predictive_to_entropy->setPath(make_path(predictive_proxy, entropy_proxy));
            if (entropy_to_robust)
                entropy_to_robust->setPath(make_path(entropy_proxy, robust_proxy));
            if (predictive_to_robust)
                predictive_to_robust->setPath(make_path(predictive_proxy, robust_proxy));
            if (quality_to_robust)
                quality_to_robust->setPath(make_path(quality_proxy, robust_proxy));

            if (edge_label_entropy)
                edge_label_entropy->setPos((entropy_proxy->sceneBoundingRect().center() + robust_proxy->sceneBoundingRect().center()) * 0.5 + QPointF(-10, -18));
            if (edge_label_predictive)
                edge_label_predictive->setPos((predictive_proxy->sceneBoundingRect().center() + robust_proxy->sceneBoundingRect().center()) * 0.5 + QPointF(-10, 8));
            if (edge_label_quality)
                edge_label_quality->setPos((quality_proxy->sceneBoundingRect().center() + robust_proxy->sceneBoundingRect().center()) * 0.5 + QPointF(-10, 22));
        }

        struct checkbox_binding final
        {
            QCheckBox* box = nullptr;
            std::function<bool()> getter;
            std::function<void(bool)> setter;
        };

        struct slider_binding final
        {
            QSlider* slider = nullptr;
            QLabel* value = nullptr;
            double min_value = 0.0;
            double max_value = 1.0;
            std::function<double()> getter;
            std::function<void(double)> setter;
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
        QGraphicsProxyWidget* entropy_proxy = nullptr;
        QGraphicsProxyWidget* robust_proxy = nullptr;
        QGraphicsProxyWidget* quality_proxy = nullptr;
        QGraphicsPathItem* core_to_ema = nullptr;
        QGraphicsPathItem* core_to_brownian = nullptr;
        QGraphicsPathItem* core_to_adaptive = nullptr;
        QGraphicsPathItem* core_to_predictive = nullptr;
        QGraphicsPathItem* brownian_to_entropy = nullptr;
        QGraphicsPathItem* adaptive_to_entropy = nullptr;
        QGraphicsPathItem* predictive_to_entropy = nullptr;
        QGraphicsPathItem* entropy_to_robust = nullptr;
        QGraphicsPathItem* predictive_to_robust = nullptr;
        QGraphicsPathItem* quality_to_robust = nullptr;
        QGraphicsSimpleTextItem* edge_label_entropy = nullptr;
        QGraphicsSimpleTextItem* edge_label_predictive = nullptr;
        QGraphicsSimpleTextItem* edge_label_quality = nullptr;
        std::vector<checkbox_binding> checks;
        std::vector<slider_binding> sliders;
        QCheckBox* ema_check = nullptr;
        QCheckBox* brownian_check = nullptr;
        QCheckBox* adaptive_check = nullptr;
        QCheckBox* predictive_check = nullptr;
        QCheckBox* mtm_check = nullptr;
        QCheckBox* chi_check = nullptr;
        QCheckBox* pareto_check = nullptr;
        QCheckBox* quarantine_check = nullptr;
        QSlider* shoulder_slider = nullptr;
        QLabel* shoulder_value = nullptr;
        QLabel* purity_value = nullptr;
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

    static QString top_bin_summary(const std::array<double, detail::alpha_spectrum::calibration_status::bin_count>& bins)
    {
        static const char* names[] = {
            "B1", "B2", "B3", "B4", "T1", "T2", "T3", "T4", "P1", "P2", "P3", "P4"
        };

        int first = 0;
        int second = 1;
        for (int i = 0; i < static_cast<int>(bins.size()); i++)
        {
            if (bins[i] > bins[first])
            {
                second = first;
                first = i;
            }
            else if (i != first && bins[i] > bins[second])
                second = i;
        }
        return QStringLiteral("%1:%2  %3:%4")
            .arg(QString::fromLatin1(names[first]))
            .arg(bins[first], 0, 'f', 2)
            .arg(QString::fromLatin1(names[second]))
            .arg(bins[second], 0, 'f', 2);
    }

    static QString heatmap_cell_style(double value, int mode)
    {
        const double t = std::clamp(value, 0.0, 1.0);
        int r = 240, g = 240, b = 240;
        switch (mode)
        {
            case 0: // occupancy
                r = int(240 - 100 * t); g = int(245 - 15 * t); b = int(240 - 150 * t);
                break;
            case 1: // delta
                r = 255; g = int(248 - 120 * t); b = int(230 - 190 * t);
                break;
            case 2: // conflict
                r = int(245 - 40 * t); g = int(238 - 170 * t); b = int(245 - 20 * t);
                break;
            default: // pathology
                r = int(248 - 20 * t); g = int(238 - 190 * t); b = int(238 - 190 * t);
                break;
        }
        return QStringLiteral("QLabel { background-color: rgb(%1,%2,%3); border: 1px solid rgb(120,120,120); }")
            .arg(r).arg(g).arg(b);
    }
}

dialog_alpha_spectrum::dialog_alpha_spectrum()
{
    ui.setupUi(this);

    detail::alpha_spectrum::shared_calibration_status().ui_open.store(true, std::memory_order_relaxed);

    connect(ui.buttonBox, SIGNAL(accepted()), this, SLOT(doOK()));
    connect(ui.buttonBox, SIGNAL(rejected()), this, SLOT(doCancel()));

    if (ui.verticalLayout)
    {
        auto* mode_layout = new QHBoxLayout();
        auto* mode_label = new QLabel(tr("UI Mode"), this);
        ui_mode_combo = new QComboBox(this);
        ui_mode_combo->addItem(tr("Basic"), static_cast<int>(ui_complexity_mode::basic));
        ui_mode_combo->addItem(tr("Simplified"), static_cast<int>(ui_complexity_mode::simplified));
        ui_mode_combo->addItem(tr("Advanced (Node Editor)"), static_cast<int>(ui_complexity_mode::advanced));
        mode_layout->addWidget(mode_label);
        mode_layout->addWidget(ui_mode_combo, 1);
        ui.verticalLayout->insertLayout(0, mode_layout);
    }

    {
        auto* placeholder_frame = new QFrame(this);
        placeholder_frame->setFrameShape(QFrame::StyledPanel);
        auto* placeholder_layout = new QVBoxLayout(placeholder_frame);
        auto* placeholder_title = new QLabel(tr("Curated Configurations"), placeholder_frame);
        auto* placeholder_text = new QLabel(
            tr("No curated simplified configurations are available yet.\n"
               "Use Advanced mode to develop and refine manual configurations;\n"
               "approved configurations will appear here once promoted."),
            placeholder_frame);
        auto* placeholder_combo = new QComboBox(placeholder_frame);
        placeholder_combo->addItem(tr("No configurations available"));
        placeholder_combo->setEnabled(false);
        placeholder_text->setWordWrap(true);
        placeholder_layout->addWidget(placeholder_title);
        placeholder_layout->addWidget(placeholder_combo);
        placeholder_layout->addWidget(placeholder_text);
        simplified_placeholder = placeholder_frame;
        if (ui.verticalLayout)
            ui.verticalLayout->insertWidget(2, simplified_placeholder);
        simplified_placeholder->setVisible(false);
    }

    advanced_canvas = new advanced_node_canvas(&s, this);
    if (advanced_canvas)
    {
        advanced_canvas->setMinimumHeight(180);
        if (ui.verticalLayout)
            ui.verticalLayout->insertWidget(3, advanced_canvas);
        advanced_canvas->setVisible(false);
    }

    tie_setting(s.adaptive_mode, ui.adaptive_mode_check);
    tie_setting(s.advanced_mode_ui, ui.advanced_mode_check);
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
    tie_setting(s.mtm_enabled, ui.mtm_enabled_check);

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
                [](double x) { return tr("%1°").arg(x, 0, 'f', 3); });
    tie_setting(s.pos_deadzone, ui.pos_deadzone_label,
                [](double x) { return tr("%1mm").arg(x, 0, 'f', 3); });
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

    ui.advanced_mode_check->setVisible(false);

    auto apply_simple_alpha = [this](int slider_value) {
        const double t = std::clamp(static_cast<double>(slider_value) / ui.simple_alpha_slider->maximum(), 0.0, 1.0);
        const double min_value = lerp(0.005, 0.4, t);
        const double max_value = lerp(0.02, 1.0, t);

        const auto rot_min_cfg = *s.rot_alpha_min;
        const auto rot_max_cfg = *s.rot_alpha_max;
        const auto pos_min_cfg = *s.pos_alpha_min;
        const auto pos_max_cfg = *s.pos_alpha_max;

        s.rot_alpha_min = options::slider_value{min_value, rot_min_cfg.min(), rot_min_cfg.max()};
        s.rot_alpha_max = options::slider_value{max_value, rot_max_cfg.min(), rot_max_cfg.max()};
        s.pos_alpha_min = options::slider_value{min_value, pos_min_cfg.min(), pos_min_cfg.max()};
        s.pos_alpha_max = options::slider_value{max_value, pos_max_cfg.min(), pos_max_cfg.max()};

        ui.simple_alpha_label->setText(QStringLiteral("Min %1% / Max %2%")
                                           .arg(min_value * 100.0, 0, 'f', 1)
                                           .arg(max_value * 100.0, 0, 'f', 1));
    };

    auto apply_simple_shape = [this](int slider_value) {
        const double t = std::clamp(static_cast<double>(slider_value) / ui.simple_shape_slider->maximum(), 0.0, 1.0);
        const double curve_value = lerp(0.2, 8.0, t);
        const double rot_deadzone_value = lerp(0.0, 0.3, t);
        const double pos_deadzone_value = lerp(0.0, 2.0, t);

        const auto rot_curve_cfg = *s.rot_curve;
        const auto pos_curve_cfg = *s.pos_curve;
        const auto rot_deadzone_cfg = *s.rot_deadzone;
        const auto pos_deadzone_cfg = *s.pos_deadzone;

        s.rot_curve = options::slider_value{curve_value, rot_curve_cfg.min(), rot_curve_cfg.max()};
        s.pos_curve = options::slider_value{curve_value, pos_curve_cfg.min(), pos_curve_cfg.max()};
        s.rot_deadzone = options::slider_value{rot_deadzone_value, rot_deadzone_cfg.min(), rot_deadzone_cfg.max()};
        s.pos_deadzone = options::slider_value{pos_deadzone_value, pos_deadzone_cfg.min(), pos_deadzone_cfg.max()};

        ui.simple_shape_label->setText(QStringLiteral("Curve %1 / RotDZ %2° / PosDZ %3mm")
                                           .arg(curve_value, 0, 'f', 2)
                                           .arg(rot_deadzone_value, 0, 'f', 3)
                                           .arg(pos_deadzone_value, 0, 'f', 3));
    };

    connect(ui.simple_alpha_slider, &QSlider::valueChanged, this, apply_simple_alpha);
    connect(ui.simple_shape_slider, &QSlider::valueChanged, this, apply_simple_shape);

    auto apply_attitude_projection = [this] {
        auto& overlay = detail::alpha_spectrum::shared_quality_overlay_state();
        if (!*s.qualities_mode_ui)
        {
            overlay.active.store(false, std::memory_order_relaxed);
            for (int j = 0; j < detail::alpha_spectrum::quality_overlay_state::value_count; j++)
                overlay.delta[j].store(0.0, std::memory_order_relaxed);
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
            overlay.delta[j].store(delta, std::memory_order_relaxed);
        }
        overlay.active.store(true, std::memory_order_relaxed);
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

    if (ui.qualities_layout)
    {
        auto* bins_group = new QGroupBox(tr("Entropy Heat Map"), ui.qualities_frame);
        auto* bins_grid = new QGridLayout(bins_group);
        heatmap_mode_combo = new QComboBox(bins_group);
        heatmap_mode_combo->addItem(tr("Occupancy"));
        heatmap_mode_combo->addItem(tr("Delta"));
        heatmap_mode_combo->addItem(tr("Conflict"));
        heatmap_mode_combo->addItem(tr("Pathology"));
        bins_grid->addWidget(new QLabel(tr("View"), bins_group), 0, 0);
        bins_grid->addWidget(heatmap_mode_combo, 0, 1, 1, 4);

        static const char* bin_names[] = {
            "B1", "B2", "B3", "B4", "T1", "T2", "T3", "T4", "P1", "P2", "P3", "P4"
        };

        bins_grid->addWidget(new QLabel(tr("Row"), bins_group), 1, 0);
        for (int i = 0; i < static_cast<int>(rot_bin_cells.size()); i++)
            bins_grid->addWidget(new QLabel(QString::fromLatin1(bin_names[i]), bins_group), 1, i + 1);

        bins_grid->addWidget(new QLabel(tr("Rot"), bins_group), 2, 0);
        bins_grid->addWidget(new QLabel(tr("Pos"), bins_group), 3, 0);

        for (int i = 0; i < static_cast<int>(rot_bin_cells.size()); i++)
        {
            auto* rot_cell = new QLabel(bins_group);
            auto* pos_cell = new QLabel(bins_group);
            rot_cell->setMinimumSize(18, 18);
            pos_cell->setMinimumSize(18, 18);
            rot_cell->setToolTip(QString::fromLatin1(bin_names[i]));
            pos_cell->setToolTip(QString::fromLatin1(bin_names[i]));
            bins_grid->addWidget(rot_cell, 2, i + 1);
            bins_grid->addWidget(pos_cell, 3, i + 1);
            rot_bin_cells[i] = rot_cell;
            pos_bin_cells[i] = pos_cell;
        }
        ui.qualities_layout->addWidget(bins_group);
    }

    auto sync_simple_from_advanced = [this, apply_simple_alpha, apply_simple_shape] {
        const double alpha_t = 0.5 * (norm(*s.rot_alpha_min, 0.005, 0.4) + norm(*s.rot_alpha_max, 0.02, 1.0));
        const double shape_t = 0.25 * (
            norm(*s.rot_curve, 0.2, 8.0) +
            norm(*s.pos_curve, 0.2, 8.0) +
            norm(*s.rot_deadzone, 0.0, 0.3) +
            norm(*s.pos_deadzone, 0.0, 2.0));

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
        ui.mtm_enabled_check->setEnabled(adv);
    };

    auto apply_ui_mode = [this, update_advanced_visibility](int mode_value) {
        int mode = sanitize_ui_mode(mode_value);
        s.ui_complexity_mode = mode;

        const bool is_basic = mode == static_cast<int>(ui_complexity_mode::basic);
        const bool is_simplified = mode == static_cast<int>(ui_complexity_mode::simplified);
        const bool is_advanced = mode == static_cast<int>(ui_complexity_mode::advanced);

        s.advanced_mode_ui = is_advanced;

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
        ui.mtm_enabled_check->setVisible(false);
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
    overlay.active.store(false, std::memory_order_relaxed);
    for (int j = 0; j < detail::alpha_spectrum::quality_overlay_state::value_count; j++)
        overlay.delta[j].store(0.0, std::memory_order_relaxed);
    detail::alpha_spectrum::shared_calibration_status().ui_open.store(false, std::memory_order_relaxed);
}

void dialog_alpha_spectrum::pull_status_into_ui(bool commit_to_settings)
{
    const auto& status = detail::alpha_spectrum::shared_calibration_status();

    if (!status.ui_open.load(std::memory_order_relaxed) && !status.active.load(std::memory_order_relaxed) && !commit_to_settings)
        return;

    const double rot_min = status.rot_alpha_min.load(std::memory_order_relaxed);
    const double rot_max = status.rot_alpha_max.load(std::memory_order_relaxed);
    const double rot_curve = status.rot_curve.load(std::memory_order_relaxed);
    const double rot_deadzone = status.rot_deadzone.load(std::memory_order_relaxed);
    const double pos_min = status.pos_alpha_min.load(std::memory_order_relaxed);
    const double pos_max = status.pos_alpha_max.load(std::memory_order_relaxed);
    const double pos_curve = status.pos_curve.load(std::memory_order_relaxed);
    const double pos_deadzone = status.pos_deadzone.load(std::memory_order_relaxed);
    const double rot_jitter = status.rot_jitter.load(std::memory_order_relaxed);
    const double pos_jitter = status.pos_jitter.load(std::memory_order_relaxed);
    const double rot_objective = status.rot_objective.load(std::memory_order_relaxed);
    const double pos_objective = status.pos_objective.load(std::memory_order_relaxed);
    const double rot_brownian_raw = status.rot_brownian_raw.load(std::memory_order_relaxed);
    const double rot_brownian_filtered = status.rot_brownian_filtered.load(std::memory_order_relaxed);
    const double rot_brownian_delta = status.rot_brownian_delta.load(std::memory_order_relaxed);
    const double rot_brownian_damped = status.rot_brownian_damped.load(std::memory_order_relaxed);
    const double rot_predictive_error = status.rot_predictive_error.load(std::memory_order_relaxed);
    const double pos_brownian_raw = status.pos_brownian_raw.load(std::memory_order_relaxed);
    const double pos_brownian_filtered = status.pos_brownian_filtered.load(std::memory_order_relaxed);
    const double pos_brownian_delta = status.pos_brownian_delta.load(std::memory_order_relaxed);
    const double pos_brownian_damped = status.pos_brownian_damped.load(std::memory_order_relaxed);
    const double pos_predictive_error = status.pos_predictive_error.load(std::memory_order_relaxed);
    const double rot_ema_drive = status.rot_ema_drive.load(std::memory_order_relaxed);
    const double rot_brownian_drive = status.rot_brownian_drive.load(std::memory_order_relaxed);
    const double rot_adaptive_drive = status.rot_adaptive_drive.load(std::memory_order_relaxed);
    const double rot_predictive_drive = status.rot_predictive_drive.load(std::memory_order_relaxed);
    const double rot_mtm_drive = status.rot_mtm_drive.load(std::memory_order_relaxed);
    const double pos_ema_drive = status.pos_ema_drive.load(std::memory_order_relaxed);
    const double pos_brownian_drive = status.pos_brownian_drive.load(std::memory_order_relaxed);
    const double pos_adaptive_drive = status.pos_adaptive_drive.load(std::memory_order_relaxed);
    const double pos_predictive_drive = status.pos_predictive_drive.load(std::memory_order_relaxed);
    const double pos_mtm_drive = status.pos_mtm_drive.load(std::memory_order_relaxed);
    const double rot_mode_expectation = status.rot_mode_expectation.load(std::memory_order_relaxed);
    const double pos_mode_expectation = status.pos_mode_expectation.load(std::memory_order_relaxed);
    const double rot_mode_peak = status.rot_mode_peak.load(std::memory_order_relaxed);
    const double pos_mode_peak = status.pos_mode_peak.load(std::memory_order_relaxed);
    const double rot_mode_purity = status.rot_mode_purity.load(std::memory_order_relaxed);
    const double pos_mode_purity = status.pos_mode_purity.load(std::memory_order_relaxed);
    const double ngc_coupling_residual = status.ngc_coupling_residual.load(std::memory_order_relaxed);
    const double outlier_quarantine_activity = status.outlier_quarantine_activity.load(std::memory_order_relaxed);
    std::array<double, detail::alpha_spectrum::calibration_status::bin_count> rot_bins {};
    std::array<double, detail::alpha_spectrum::calibration_status::bin_count> pos_bins {};
    std::array<double, detail::alpha_spectrum::calibration_status::bin_count> rot_delta_bins {};
    std::array<double, detail::alpha_spectrum::calibration_status::bin_count> pos_delta_bins {};
    std::array<double, detail::alpha_spectrum::calibration_status::bin_count> rot_conflict_bins {};
    std::array<double, detail::alpha_spectrum::calibration_status::bin_count> pos_conflict_bins {};
    std::array<double, detail::alpha_spectrum::calibration_status::bin_count> rot_pathology_bins {};
    std::array<double, detail::alpha_spectrum::calibration_status::bin_count> pos_pathology_bins {};
    for (int i = 0; i < static_cast<int>(rot_bins.size()); i++)
    {
        rot_bins[i] = status.rot_bin_prob[i].load(std::memory_order_relaxed);
        pos_bins[i] = status.pos_bin_prob[i].load(std::memory_order_relaxed);
        rot_delta_bins[i] = status.rot_bin_delta[i].load(std::memory_order_relaxed);
        pos_delta_bins[i] = status.pos_bin_delta[i].load(std::memory_order_relaxed);
        rot_conflict_bins[i] = status.rot_bin_conflict[i].load(std::memory_order_relaxed);
        pos_conflict_bins[i] = status.pos_bin_conflict[i].load(std::memory_order_relaxed);
        rot_pathology_bins[i] = status.rot_bin_pathology[i].load(std::memory_order_relaxed);
        pos_pathology_bins[i] = status.pos_bin_pathology[i].load(std::memory_order_relaxed);
    }
    const bool active = status.active.load(std::memory_order_relaxed);

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
    ui.rot_deadzone_label->setText(tr("%1°").arg(rot_deadzone, 0, 'f', 3));
    ui.pos_min_label->setText(QStringLiteral("%1%").arg(pos_min * 100.0, 0, 'f', 1));
    ui.pos_max_label->setText(QStringLiteral("%1%").arg(pos_max * 100.0, 0, 'f', 1));
    ui.pos_curve_label->setText(QStringLiteral("%1").arg(pos_curve, 0, 'f', 2));
    ui.pos_deadzone_label->setText(tr("%1mm").arg(pos_deadzone, 0, 'f', 3));

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
            .arg(status.rot_chi_square_drive.load(std::memory_order_relaxed), 0, 'f', 3)
            .arg(status.rot_pareto_drive.load(std::memory_order_relaxed), 0, 'f', 3)
            .arg(rot_mtm_drive, 0, 'f', 3));
    ui.info_pos_contrib_value->setText(
        QStringLiteral("EMA:%1 Br:%2 Ad:%3 Pr:%4 Cs:%5 Pa:%6 MTM:%7")
            .arg(pos_ema_drive, 0, 'f', 3)
            .arg(pos_brownian_drive, 0, 'f', 3)
            .arg(pos_adaptive_drive, 0, 'f', 3)
            .arg(pos_predictive_drive, 0, 'f', 3)
            .arg(status.pos_chi_square_drive.load(std::memory_order_relaxed), 0, 'f', 3)
            .arg(status.pos_pareto_drive.load(std::memory_order_relaxed), 0, 'f', 3)
            .arg(pos_mtm_drive, 0, 'f', 3));
    ui.status_value->setText(
        active ?
            QStringLiteral("Mon|E%1 B%2 A%3 P%4 M%5|rE%6 rP%7 pE%8 pP%9 rQ%10 pQ%11 k%12")
                .arg(*s.ema_enabled ? 1 : 0)
                .arg(*s.brownian_enabled ? 1 : 0)
                .arg(*s.adaptive_mode ? 1 : 0)
                .arg(*s.predictive_enabled ? 1 : 0)
                .arg(*s.mtm_enabled ? 1 : 0)
                .arg(rot_mode_expectation, 5, 'f', 3)
                .arg(rot_mode_peak, 5, 'f', 3)
                .arg(pos_mode_expectation, 5, 'f', 3)
                .arg(pos_mode_peak, 5, 'f', 3)
                .arg(rot_mode_purity, 5, 'f', 3)
                .arg(pos_mode_purity, 5, 'f', 3)
                .arg(ngc_coupling_residual, 5, 'f', 3)
            : tr("Idle"));

    const int heatmap_mode = heatmap_mode_combo ? heatmap_mode_combo->currentIndex() : 0;
    const auto& rot_heat = heatmap_mode == 0 ? rot_bins : heatmap_mode == 1 ? rot_delta_bins : heatmap_mode == 2 ? rot_conflict_bins : rot_pathology_bins;
    const auto& pos_heat = heatmap_mode == 0 ? pos_bins : heatmap_mode == 1 ? pos_delta_bins : heatmap_mode == 2 ? pos_conflict_bins : pos_pathology_bins;
    for (int i = 0; i < static_cast<int>(rot_bin_cells.size()); i++)
    {
        if (rot_bin_cells[i])
        {
            rot_bin_cells[i]->setStyleSheet(heatmap_cell_style(rot_heat[i], heatmap_mode));
            rot_bin_cells[i]->setToolTip(QStringLiteral("rot %1 = %2").arg(i).arg(rot_heat[i], 0, 'f', 4));
        }
        if (pos_bin_cells[i])
        {
            pos_bin_cells[i]->setStyleSheet(heatmap_cell_style(pos_heat[i], heatmap_mode));
            pos_bin_cells[i]->setToolTip(QStringLiteral("pos %1 = %2").arg(i).arg(pos_heat[i], 0, 'f', 4));
        }
    }

    QStringList active_qualities;
    if (*s.quality_stillness) active_qualities << tr("Stillness");
    if (*s.quality_continuity) active_qualities << tr("Continuity");
    if (*s.quality_robustness) active_qualities << tr("Robustness");
    if (*s.quality_decisiveness) active_qualities << tr("Decisiveness");
    if (*s.quality_pathology_defense) active_qualities << tr("Pathology Defense");
    if (*s.quality_recovery_pace) active_qualities << tr("Recovery Pace");

    const QString quality_text = active_qualities.isEmpty() ? tr("none") : active_qualities.join(QStringLiteral(", "));
    ui.qualities_state_label->setText(
        tr("Alchemy state: [%1] | rot bins %2 | pos bins %3 | quarantine %4%")
            .arg(quality_text)
            .arg(top_bin_summary(rot_bins))
            .arg(top_bin_summary(pos_bins))
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
    s.mtm_enabled.set_to_default();
    s.advanced_mode_ui.set_to_default();

    auto& overlay = detail::alpha_spectrum::shared_quality_overlay_state();
    overlay.active.store(false, std::memory_order_relaxed);
    for (int j = 0; j < detail::alpha_spectrum::quality_overlay_state::value_count; j++)
        overlay.delta[j].store(0.0, std::memory_order_relaxed);

    s.b->save();
    s.b->reload();

    const double alpha_t = 0.5 * (norm(*s.rot_alpha_min, 0.005, 0.4) + norm(*s.rot_alpha_max, 0.02, 1.0));
    const double shape_t = 0.25 * (
        norm(*s.rot_curve, 0.2, 8.0) +
        norm(*s.pos_curve, 0.2, 8.0) +
        norm(*s.rot_deadzone, 0.0, 0.3) +
        norm(*s.pos_deadzone, 0.0, 2.0));

    {
        const QSignalBlocker b1(ui.simple_alpha_slider);
        const QSignalBlocker b2(ui.simple_shape_slider);
        ui.simple_alpha_slider->setValue(static_cast<int>(alpha_t * ui.simple_alpha_slider->maximum()));
        ui.simple_shape_slider->setValue(static_cast<int>(shape_t * ui.simple_shape_slider->maximum()));
    }

    ui.simple_alpha_label->setText(QStringLiteral("Min %1% / Max %2%")
                                       .arg((*s.rot_alpha_min) * 100.0, 0, 'f', 1)
                                       .arg((*s.rot_alpha_max) * 100.0, 0, 'f', 1));
    ui.simple_shape_label->setText(QStringLiteral("Curve %1 / RotDZ %2° / PosDZ %3mm")
                                       .arg(static_cast<double>(*s.rot_curve), 0, 'f', 2)
                                       .arg(static_cast<double>(*s.rot_deadzone), 0, 'f', 3)
                                       .arg(static_cast<double>(*s.pos_deadzone), 0, 'f', 3));

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
    ui.mtm_enabled_check->setEnabled(reset_adv_controls);
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
