// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QScrollBar>
#include <catch2/catch_test_macros.hpp>
#include "yuzu/configuration/scrollable_graphics.h"

TEST_CASE("Graphics settings scroll without compressing native controls", "[qt][configuration]") {
    int argc = 1;
    char name[] = "graphics-layout-test";
    char* argv[] = {name, nullptr};
    QApplication application(argc, argv);
    QWidget page;
    auto* contents_layout = new QVBoxLayout(&page);
    ConfigurationShared::MakeGraphicsScrollable(&page);

    std::vector<QComboBox*> controls;
    // Settings are dynamically populated after setupUi and after wrapping the page.
    for (int row = 0; row < 40; ++row) {
        auto* layout = new QHBoxLayout;
        layout->addWidget(new QLabel(QStringLiteral("Graphics setting %1").arg(row)));
        auto* control = new QComboBox;
        control->addItem(QStringLiteral("Default"));
        layout->addWidget(control);
        controls.push_back(control);
        contents_layout->addLayout(layout);
    }
    page.resize(500, 300);
    page.show();
    QApplication::processEvents();
    auto* scroll = page.findChild<QScrollArea*>();
    REQUIRE(scroll);
    REQUIRE(scroll->widget()->layout() == contents_layout);
    REQUIRE(page.layout() != contents_layout);
    REQUIRE(page.height() == 300);
    REQUIRE(scroll->verticalScrollBar()->maximum() > 0);
    for (const auto* control : controls) {
        REQUIRE(control->height() >= control->minimumSizeHint().height());
    }
    scroll->ensureWidgetVisible(controls.back());
    QApplication::processEvents();
    REQUIRE(scroll->verticalScrollBar()->value() > 0);
    REQUIRE(scroll->viewport()->rect().contains(
        controls.back()->mapTo(scroll->viewport(), controls.back()->rect().center())));
    page.resize(500, 2400);
    QApplication::processEvents();
    REQUIRE(scroll->verticalScrollBar()->maximum() == 0);
}
