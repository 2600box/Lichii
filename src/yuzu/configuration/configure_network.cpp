// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCoreApplication>
#include <QDesktopServices>
#include <QFutureWatcher>
#include <QUrl>
#include <QtConcurrent/QtConcurrent>
#include "common/nextendo.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/internal_network/network_interface.h"
#include "ui_configure_network.h"
#include "yuzu/configuration/configure_network.h"

ConfigureNetwork::ConfigureNetwork(const Core::System& system_, QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureNetwork>()), system{system_} {
    ui->setupUi(this);
    for (const auto& iface : Network::GetAvailableNetworkInterfaces())
        ui->network_interface->addItem(QString::fromStdString(iface.name));
    this->SetConfiguration();

    connect(ui->nextendo_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) {
            ui->airplane_mode->setChecked(false);
        }
    });
    connect(ui->nextendo_sign_in, &QPushButton::clicked, this, &ConfigureNetwork::SignInToNextendo);
    connect(ui->nextendo_sign_out, &QPushButton::clicked, this, [this] {
        Common::Nextendo::ClearAccount();
        UpdateNextendoStatus();
    });
}

ConfigureNetwork::~ConfigureNetwork() = default;

void ConfigureNetwork::ApplyConfiguration() {
    Settings::values.network_interface = ui->network_interface->currentText().toStdString();
    Settings::values.airplane_mode = ui->airplane_mode->isChecked();
    Settings::values.nextendo_enabled = ui->nextendo_enabled->isChecked();
    Settings::values.nextendo_client_id = ui->nextendo_client_id->text().trimmed().toStdString();
}

void ConfigureNetwork::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureNetwork::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureNetwork::SetConfiguration() {
    const bool runtime_lock = !system.IsPoweredOn();
    auto const network_interface = Settings::values.network_interface.GetValue();
    auto const airplane_mode = Settings::values.airplane_mode.GetValue();
    ui->network_interface->setCurrentText(QString::fromStdString(network_interface));
    ui->network_interface->setEnabled(runtime_lock);
    ui->airplane_mode->setChecked(airplane_mode);
    ui->nextendo_enabled->setChecked(Settings::values.nextendo_enabled.GetValue());
    ui->nextendo_client_id->setText(
        QString::fromStdString(Settings::values.nextendo_client_id.GetValue()));
    UpdateNextendoStatus();
}

void ConfigureNetwork::SignInToNextendo() {
    const std::string client_id = ui->nextendo_client_id->text().trimmed().toStdString();
    ui->nextendo_sign_in->setEnabled(false);
    ui->nextendo_status->setText(tr("Finish signing in to Nextendo in your browser."));

    auto* watcher = new QFutureWatcher<Common::Nextendo::SignInResult>(this);
    connect(watcher, &QFutureWatcher<Common::Nextendo::SignInResult>::finished, this,
            [this, watcher] {
                const auto result = watcher->result();
                watcher->deleteLater();
                ui->nextendo_sign_in->setEnabled(true);
                UpdateNextendoStatus(QString::fromStdString(result.ok ? result.game_token_error
                                                                      : result.error),
                                     result.ok);
            });
    watcher->setFuture(QtConcurrent::run([client_id] {
        return Common::Nextendo::SignIn(client_id, [](const std::string& url) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), [url] { QDesktopServices::openUrl(QUrl{QString::fromStdString(url)}); },
                Qt::QueuedConnection);
        });
    }));
}

void ConfigureNetwork::UpdateNextendoStatus(const QString& error, bool signed_in) {
    const auto account = Common::Nextendo::LoadAccount();
    ui->nextendo_sign_out->setEnabled(account.has_value());
    QString status;
    if (!error.isEmpty() && !signed_in) {
        status = tr("Sign-in failed: %1").arg(error);
    } else if (!account) {
        status = tr("Not signed in.");
    } else {
        status = account->username.empty()
                     ? tr("Signed in.")
                     : tr("Signed in as %1.").arg(QString::fromStdString(account->username));
        if (account->nex_token.empty()) {
            status += QLatin1Char(' ') +
                      tr("Nextendo did not issue an online play token, so games can't log in "
                         "to Nextendo yet.");
            if (!error.isEmpty()) {
                status += QLatin1Char(' ') + error;
            }
        }
    }
    ui->nextendo_status->setText(status);
}
