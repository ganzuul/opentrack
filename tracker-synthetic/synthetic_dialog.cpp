#include "synthetic.h"
#include "api/plugin-api.hpp"

synthetic_dialog::synthetic_dialog()
{
    ui.setupUi(this);

    connect(ui.buttonBox, SIGNAL(accepted()), this, SLOT(doOK()));
    connect(ui.buttonBox, SIGNAL(rejected()), this, SLOT(doCancel()));

    ui.preset->setCurrentIndex((int)s.preset);
    ui.output_mode->setCurrentIndex((int)s.output_mode);
    ui.sample_rate_hz->setValue((int)s.sample_rate_hz);
    ui.translation_amplitude->setValue((double)s.translation_amplitude);
    ui.rotation_amplitude->setValue((double)s.rotation_amplitude);
    ui.frequency_hz->setValue((double)s.frequency_hz);
    ui.orbit_tilt_deg->setValue((double)s.orbit_tilt_deg);
    ui.orbital_velocity->setValue((double)s.orbital_velocity);
    ui.gravity_enabled->setChecked((bool)s.gravity_enabled);
    ui.release_delay_s->setValue((double)s.release_delay_s);
    ui.post_release_stop_delay_s->setValue((double)s.post_release_stop_delay_s);
}

void synthetic_dialog::doOK()
{
    s.preset = ui.preset->currentIndex();
    s.output_mode = ui.output_mode->currentIndex();
    s.sample_rate_hz = ui.sample_rate_hz->value();
    s.translation_amplitude = ui.translation_amplitude->value();
    s.rotation_amplitude = ui.rotation_amplitude->value();
    s.frequency_hz = ui.frequency_hz->value();
    s.orbit_tilt_deg = ui.orbit_tilt_deg->value();
    s.orbital_velocity = ui.orbital_velocity->value();
    s.gravity_enabled = ui.gravity_enabled->isChecked();
    s.release_delay_s = ui.release_delay_s->value();
    s.post_release_stop_delay_s = ui.post_release_stop_delay_s->value();
    s.b->save();
    close();
}

void synthetic_dialog::doCancel()
{
    close();
}