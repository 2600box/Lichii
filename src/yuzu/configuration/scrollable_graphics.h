// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace ConfigurationShared {

// Keep the page itself in the tab widget (and in the per-game tab group), but
// let its contents scroll instead of squeezing native controls below their
// minimum height on small/Retina screens. Used by global and per-game pages.
inline void MakeGraphicsScrollable(QWidget* page) {
    auto* contents = new QWidget;
    contents->setLayout(page->layout());
    contents->layout()->setSizeConstraint(QLayout::SetMinAndMaxSize);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(contents);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

} // namespace ConfigurationShared
