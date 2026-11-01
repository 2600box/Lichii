// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>
#include <QCheckBox>
#include <QLabel>
#include <QSlider>
#include <qnamespace.h>
#include "common/settings.h"
#include "core/core.h"
#include "qt_common/config/shared_translation.h"
#include "ui_configure_graphics_extensions.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics_extensions.h"
#include "yuzu/configuration/scrollable_graphics.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureGraphicsExtensions::ConfigureGraphicsExtensions(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureGraphicsExtensions>()},
      system{system_} {

    ui->setupUi(this);
    ConfigurationShared::MakeGraphicsScrollable(this);

    Setup(builder);

    SetConfiguration();
}

ConfigureGraphicsExtensions::~ConfigureGraphicsExtensions() = default;

void ConfigureGraphicsExtensions::SetConfiguration() {}

void ConfigureGraphicsExtensions::Setup(const ConfigurationShared::Builder& builder) {
    auto& layout = *ui->populate_target->layout();

    std::map<u32, QWidget*> hold{}; // A map will sort the data for us

    // Feature availability is checked by the selected Vulkan device. macOS
    // defaults EDS to disabled for MoltenVK, but other ICDs can support it.
    for (auto setting :
         Settings::values.linkage.by_category[Settings::Category::RendererExtensions]) {
        ConfigurationShared::Widget* widget = [&]() {
            if (setting->Id() == Settings::values.sample_shading.Id()) {
                // TODO(crueter): should support this natively perhaps?
                return builder.BuildWidget(
                    setting, apply_funcs, ConfigurationShared::RequestType::Slider, true, 1.0f,
                    nullptr, tr("%", "Sample Shading percentage (e.g. 50%)"));
            } else {
                return builder.BuildWidget(setting, apply_funcs);
            }
        }();

        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }

        hold.emplace(setting->Id(), widget);
    }

    for (const auto& [id, widget] : hold) {
        layout.addWidget(widget);
    }

    auto& hacks = *ui->hacks_target->layout();
    std::map<u32, QWidget*> hacks_hold{}; // A map will sort the data for us

    for (auto setting : Settings::values.linkage.by_category[Settings::Category::RendererHacks]) {
        auto* widget = builder.BuildWidget(setting, apply_funcs);

        if (widget == nullptr) {
            continue;
        }

        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }

        hacks_hold.emplace(setting->Id(), widget);
    }

    for (const auto& [id, widget] : hacks_hold) {
        hacks.addWidget(widget);
    }
}

void ConfigureGraphicsExtensions::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigureGraphicsExtensions::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphicsExtensions::RetranslateUI() {
    ui->retranslateUi(this);
}
