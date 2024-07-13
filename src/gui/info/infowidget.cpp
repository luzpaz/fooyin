/*
 * Fooyin
 * Copyright © 2022, Luke Taylor <LukeT1@proton.me>
 *
 * Fooyin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Fooyin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Fooyin.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "infowidget.h"

#include "infodelegate.h"
#include "infomodel.h"
#include "infoview.h"
#include "internalguisettings.h"

#include <core/player/playercontroller.h>
#include <core/track.h>
#include <gui/trackselectioncontroller.h>
#include <utils/settings/settingsmanager.h>

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QMenu>
#include <QScrollBar>

namespace Fooyin {
InfoWidget::InfoWidget(PlayerController* playerController, TrackSelectionController* selectionController,
                       SettingsManager* settings, QWidget* parent)
    : PropertiesTabWidget{parent}
    , m_selectionController{selectionController}
    , m_playerController{playerController}
    , m_settings{settings}
    , m_view{new InfoView(this)}
    , m_model{new InfoModel(this)}
    , m_displayOption{static_cast<SelectionDisplay>(m_settings->value<Settings::Gui::Internal::InfoDisplayPrefer>())}
    , m_scrollPos{-1}
{
    setObjectName(InfoWidget::name());

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins({});

    m_view->setItemDelegate(new ItemDelegate(this));
    m_view->setModel(m_model);

    layout->addWidget(m_view);

    m_view->setHeaderHidden(!settings->value<Settings::Gui::Internal::InfoHeader>());
    m_view->setVerticalScrollBarPolicy(
        settings->value<Settings::Gui::Internal::InfoScrollBar>() ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    m_view->setAlternatingRowColors(settings->value<Settings::Gui::Internal::InfoAltColours>());

    QObject::connect(selectionController, &TrackSelectionController::selectionChanged, this,
                     [this]() { m_resetTimer.start(50, this); });
    QObject::connect(m_playerController, &PlayerController::currentTrackChanged, this,
                     [this]() { m_resetTimer.start(50, this); });
    QObject::connect(m_model, &QAbstractItemModel::modelReset, this, [this]() { resetView(); });

    using namespace Settings::Gui::Internal;

    m_settings->subscribe<InfoHeader>(this, [this](bool enabled) { m_view->setHeaderHidden(!enabled); });
    m_settings->subscribe<InfoScrollBar>(this, [this](bool enabled) {
        m_view->setVerticalScrollBarPolicy(enabled ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    });
    m_settings->subscribe<InfoAltColours>(this, [this](bool enabled) { m_view->setAlternatingRowColors(enabled); });
    m_settings->subscribe<Settings::Gui::Internal::InfoDisplayPrefer>(this, [this](const int option) {
        m_displayOption = static_cast<SelectionDisplay>(option);
        resetModel();
    });

    resetModel();
}

InfoWidget::~InfoWidget() = default;

QString InfoWidget::name() const
{
    return tr("Selection Info");
}

QString InfoWidget::layoutName() const
{
    return QStringLiteral("SelectionInfo");
}

void InfoWidget::saveLayoutData(QJsonObject& layout)
{
    layout[QStringLiteral("Options")] = static_cast<int>(m_model->options());
}

void InfoWidget::loadLayoutData(const QJsonObject& layout)
{
    if(layout.contains(QStringLiteral("Options"))) {
        const auto options = static_cast<InfoItem::Options>(layout.value(QStringLiteral("Options")).toInt());
        m_model->setOptions(options);
    }
}

void InfoWidget::contextMenuEvent(QContextMenuEvent* event)
{
    using namespace Settings::Gui::Internal;

    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto* showHeaders = new QAction(QStringLiteral("Show Header"), this);
    showHeaders->setCheckable(true);
    showHeaders->setChecked(!m_view->isHeaderHidden());
    QAction::connect(showHeaders, &QAction::triggered, this,
                     [this](bool checked) { m_settings->set<InfoHeader>(checked); });

    auto* showScrollBar = new QAction(QStringLiteral("Show Scrollbar"), menu);
    showScrollBar->setCheckable(true);
    showScrollBar->setChecked(m_view->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff);
    QAction::connect(showScrollBar, &QAction::triggered, this,
                     [this](bool checked) { m_settings->set<InfoScrollBar>(checked); });
    menu->addAction(showScrollBar);

    auto* altColours = new QAction(QStringLiteral("Alternating Row Colours"), this);
    altColours->setCheckable(true);
    altColours->setChecked(m_view->alternatingRowColors());
    QAction::connect(altColours, &QAction::triggered, this,
                     [this](bool checked) { m_settings->set<InfoAltColours>(checked); });

    const auto options = m_model->options();

    auto* showMetadata = new QAction(QStringLiteral("Metadata"), this);
    showMetadata->setCheckable(true);
    showMetadata->setChecked(options & InfoItem::Metadata);
    QAction::connect(showMetadata, &QAction::triggered, this, [this](bool checked) {
        m_model->setOption(InfoItem::Metadata, checked);
        resetModel();
    });

    auto* showLocation = new QAction(QStringLiteral("Location"), this);
    showLocation->setCheckable(true);
    showLocation->setChecked(options & InfoItem::Location);
    QAction::connect(showLocation, &QAction::triggered, this, [this](bool checked) {
        m_model->setOption(InfoItem::Location, checked);
        resetModel();
    });

    auto* showGeneral = new QAction(QStringLiteral("General"), this);
    showGeneral->setCheckable(true);
    showGeneral->setChecked(options & InfoItem::General);
    QAction::connect(showGeneral, &QAction::triggered, this, [this](bool checked) {
        m_model->setOption(InfoItem::General, checked);
        resetModel();
    });

    menu->addAction(showHeaders);
    menu->addAction(showScrollBar);
    menu->addAction(altColours);
    menu->addSeparator();
    menu->addAction(showMetadata);
    menu->addAction(showLocation);
    menu->addAction(showGeneral);

    menu->popup(event->globalPos());
}

void InfoWidget::timerEvent(QTimerEvent* event)
{
    if(event->timerId() == m_resetTimer.timerId()) {
        m_resetTimer.stop();
        resetModel();
    }

    PropertiesTabWidget::timerEvent(event);
}

void InfoWidget::resetModel()
{
    m_scrollPos = m_view->verticalScrollBar()->value();

    const Track currentTrack = m_playerController->currentTrack();

    if(m_displayOption == SelectionDisplay::PreferPlaying && currentTrack.isValid()) {
        m_model->resetModel({currentTrack});
    }
    else if(m_selectionController->hasTracks()) {
        m_model->resetModel(m_selectionController->selectedTracks());
    }
    else if(currentTrack.isValid()) {
        m_model->resetModel({currentTrack});
    }
    else {
        m_model->resetModel({});
    }
}

void InfoWidget::resetView()
{
    if(m_scrollPos >= 0) {
        m_view->verticalScrollBar()->setValue(std::exchange(m_scrollPos, -1));
    }
}
} // namespace Fooyin
