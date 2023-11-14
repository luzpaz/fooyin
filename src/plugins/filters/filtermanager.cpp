/*
 * Fooyin
 * Copyright 2022-2023, Luke Taylor <LukeT1@proton.me>
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

#include "filtermanager.h"

#include "fieldregistry.h"
#include "filterstore.h"
#include "filterwidget.h"

#include <core/library/musiclibrary.h>
#include <core/library/tracksort.h>
#include <core/playlist/playlistmanager.h>
#include <gui/trackselectioncontroller.h>
#include <utils/async.h>

#include <QActionGroup>
#include <QMenu>

#include <QCoroCore>

#include <set>

namespace {
bool containsSearch(const QString& text, const QString& search)
{
    return text.contains(search, Qt::CaseInsensitive);
}

// TODO: Support user-defined tags
bool matchSearch(const Fy::Core::Track& track, const QString& search)
{
    if(search.isEmpty()) {
        return true;
    }

    return containsSearch(track.artist(), search) || containsSearch(track.title(), search)
        || containsSearch(track.album(), search) || containsSearch(track.albumArtist(), search);
}

Fy::Core::TrackList filterTracks(const Fy::Core::TrackList& tracks, const QString& search)
{
    return Fy::Utils::filter(tracks, [search](const Fy::Core::Track& track) { return matchSearch(track, search); });
}
} // namespace

namespace Fy::Filters {
struct FilterManager::Private
{
    FilterManager* self;

    Core::Library::MusicLibrary* library;
    Gui::TrackSelectionController* trackSelection;
    Utils::SettingsManager* settings;

    std::map<int, FilterWidget*> filterWidgets;

    Gui::TrackAction doubleClickAction;
    Gui::TrackAction middleClickAction;

    FieldRegistry fieldsRegistry;
    Core::TrackList filteredTracks;
    FilterStore filterStore;
    QString searchFilter;

    Private(FilterManager* self, Core::Library::MusicLibrary* library, Gui::TrackSelectionController* trackSelection,
            Utils::SettingsManager* settings)
        : self{self}
        , library{library}
        , trackSelection{trackSelection}
        , settings{settings}
        , doubleClickAction{static_cast<Gui::TrackAction>(settings->value<Settings::FilterDoubleClick>())}
        , middleClickAction{static_cast<Gui::TrackAction>(settings->value<Settings::FilterMiddleClick>())}
        , fieldsRegistry{settings}
    {
        fieldsRegistry.loadItems();
    }

    void handleAction(const Gui::TrackAction& action, const QString& playlistName) const
    {
        const bool autoSwitch = settings->value<Settings::FilterAutoSwitch>();
        trackSelection->executeAction(action, autoSwitch ? Gui::Switch : Gui::None, playlistName);
    }

    void resetFiltersAfterIndex(int resetIndex)
    {
        for(const auto& [index, filterWidget] : filterWidgets) {
            if(index > resetIndex) {
                filterWidget->reset(tracks());
            }
        }
    }

    void deleteFilter(const LibraryFilter& filter)
    {
        filterWidgets.erase(filter.index);
        filterStore.removeFilter(filter.index);
        getFilteredTracks();
        resetFiltersAfterIndex(filter.index - 1);
    }

    void fieldChanged(const Filters::FilterField& field)
    {
        const FilterList filters = filterStore.filters();
        std::set<int> filtersToReset;
        int resetIndex{-1};

        for(const LibraryFilter& filter : filters) {
            if(filter.field.id == field.id) {
                LibraryFilter updatedFilter{filter};
                updatedFilter.field = field;
                filterStore.updateFilter(updatedFilter);

                filtersToReset.emplace(filter.index);
                resetIndex = resetIndex >= 0 ? std::min(resetIndex, filter.index) : filter.index;
            }
        }

        for(const auto& [index, filterWidget] : filterWidgets) {
            if(filtersToReset.contains(index)) {
                const LibraryFilter filter = filterStore.filterByIndex(index);
                filterWidget->changeFilter(filter);
            }
            if(index >= resetIndex) {
                filterWidget->reset(tracks());
            }
        }
    }

    QCoro::Task<void> selectionChanged(LibraryFilter filter, QString playlistName)
    {
        LibraryFilter updatedFilter{filter};

        Core::TrackList sortedTracks = co_await Utils::asyncExec(
            [&updatedFilter]() { return Core::Library::Sorting::sortTracks(updatedFilter.tracks); });

        trackSelection->changeSelectedTracks(sortedTracks, playlistName);
        updatedFilter.tracks = sortedTracks;

        if(settings->value<Settings::FilterPlaylistEnabled>()) {
            const QString autoPlaylist = settings->value<Settings::FilterAutoPlaylist>();
            const bool autoSwitch      = settings->value<Settings::FilterAutoSwitch>();

            Gui::ActionOptions options = Gui::KeepActive;
            if(autoSwitch) {
                options |= Gui::Switch;
            }
            trackSelection->executeAction(Gui::TrackAction::SendNewPlaylist, options, autoPlaylist);
        }

        filterStore.updateFilter(updatedFilter);

        const int resetIndex = filter.index;
        filterStore.clearActiveFilters(resetIndex);
        getFilteredTracks();

        for(const auto& [index, filterWidget] : filterWidgets) {
            if(index > resetIndex) {
                filterWidget->reset(tracks());
            }
        }
    }

    void changeFilterField(const LibraryFilter& filter, const QString& field)
    {
        const FilterField filterField = fieldsRegistry.itemByName(field);

        LibraryFilter updatedFilter{filter};
        updatedFilter.field = filterField;
        filterStore.updateFilter(updatedFilter);

        const int resetIndex = filter.index - 1;

        filterStore.clearActiveFilters(resetIndex);
        getFilteredTracks();

        for(const auto& [index, filterWidget] : filterWidgets) {
            if(index == filter.index) {
                filterWidget->changeFilter(updatedFilter);
            }
            if(index > resetIndex) {
                filterWidget->reset(tracks());
            }
        }
    }

    void filterHeaderMenu(const LibraryFilter& filter, QPoint pos)
    {
        auto* menu = new QMenu();
        menu->setAttribute(Qt::WA_DeleteOnClose);

        auto* filterList = new QActionGroup{menu};

        for(const auto& [filterIndex, registryField] : fieldsRegistry.items()) {
            const QString name = registryField.name;
            auto* fieldAction  = new QAction(menu);
            fieldAction->setText(name);
            fieldAction->setData(name);
            fieldAction->setCheckable(true);
            fieldAction->setChecked(name == filter.field.name);
            menu->addAction(fieldAction);
            filterList->addAction(fieldAction);
        }

        menu->setDefaultAction(filterList->checkedAction());
        QObject::connect(filterList, &QActionGroup::triggered, self,
                         [this, &filter](QAction* action) { changeFilterField(filter, action->data().toString()); });

        menu->popup(pos);
    }

    void filterContextMenu(const LibraryFilter& /*filter*/, QPoint pos) const
    {
        auto* menu = new QMenu();
        menu->setAttribute(Qt::WA_DeleteOnClose);

        trackSelection->addTrackPlaylistContextMenu(menu);
        trackSelection->addTrackContextMenu(menu);

        menu->popup(pos);
    }

    [[nodiscard]] bool hasTracks() const
    {
        return !filteredTracks.empty() || !searchFilter.isEmpty() || filterStore.hasActiveFilters();
    }

    [[nodiscard]] Core::TrackList tracks() const
    {
        return hasTracks() ? filteredTracks : library->tracks();
    }

    void getFilteredTracks()
    {
        filteredTracks.clear();

        for(auto& filter : filterStore.activeFilters()) {
            if(filteredTracks.empty()) {
                std::ranges::copy(filter.tracks, std::back_inserter(filteredTracks));
            }
            else {
                filteredTracks
                    = Utils::intersection<Core::Track, Core::Track::TrackHash>(filter.tracks, filteredTracks);
            }
        }
    }
};

FilterManager::FilterManager(Core::Library::MusicLibrary* library, Gui::TrackSelectionController* trackSelection,
                             Utils::SettingsManager* settings, QObject* parent)
    : QObject{parent}
    , p{std::make_unique<Private>(this, library, trackSelection, settings)}
{
    QObject::connect(p->library, &Core::Library::MusicLibrary::tracksAdded, this, &FilterManager::tracksAdded);
    QObject::connect(p->library, &Core::Library::MusicLibrary::tracksUpdated, this, &FilterManager::tracksUpdated);
    QObject::connect(p->library, &Core::Library::MusicLibrary::tracksDeleted, this, &FilterManager::tracksRemoved);

    const auto tracksChanged = [this]() {
        p->getFilteredTracks();
        p->resetFiltersAfterIndex(-1);
    };

    QObject::connect(p->library, &Core::Library::MusicLibrary::tracksLoaded, this, tracksChanged);
    QObject::connect(p->library, &Core::Library::MusicLibrary::tracksSorted, this, tracksChanged);
    QObject::connect(p->library, &Core::Library::MusicLibrary::libraryChanged, this, tracksChanged);
    QObject::connect(p->library, &Core::Library::MusicLibrary::libraryRemoved, this, tracksChanged);

    QObject::connect(&p->fieldsRegistry, &FieldRegistry::fieldChanged, this,
                     [this](const Filters::FilterField& field) { p->fieldChanged(field); });

    settings->subscribe<Settings::FilterDoubleClick>(
        this, [this](int action) { p->doubleClickAction = static_cast<Gui::TrackAction>(action); });
    settings->subscribe<Settings::FilterMiddleClick>(
        this, [this](int action) { p->middleClickAction = static_cast<Gui::TrackAction>(action); });
}

FilterManager::~FilterManager() = default;

FilterWidget* FilterManager::createFilter()
{
    auto* filter = new FilterWidget(p->settings);

    const FilterField filterField = p->fieldsRegistry.itemByName(QStringLiteral(""));
    const LibraryFilter libFilter = p->filterStore.addFilter(filterField);

    filter->changeFilter(libFilter);

    p->filterWidgets.emplace(libFilter.index, filter);

    QObject::connect(filter, &FilterWidget::doubleClicked, this,
                     [this](const QString& playlistName) { p->handleAction(p->doubleClickAction, playlistName); });
    QObject::connect(filter, &FilterWidget::middleClicked, this,
                     [this](const QString& playlistName) { p->handleAction(p->middleClickAction, playlistName); });
    QObject::connect(
        filter, &FilterWidget::requestFieldChange, this,
        [this](const LibraryFilter& filter, const QString& field) { p->changeFilterField(filter, field); });
    QObject::connect(filter, &FilterWidget::requestHeaderMenu, this,
                     [this](const LibraryFilter& filter, QPoint pos) { p->filterHeaderMenu(filter, pos); });
    QObject::connect(filter, &FilterWidget::requestContextMenu, this,
                     [this](const LibraryFilter& filter, QPoint pos) { p->filterContextMenu(filter, pos); });
    QObject::connect(filter, &FilterWidget::selectionChanged, this,
                     [this](const LibraryFilter& filter, const QString& playlistName) {
                         p->selectionChanged(filter, playlistName);
                     });
    QObject::connect(filter, &FilterWidget::filterDeleted, this,
                     [this](const LibraryFilter& filter) { p->deleteFilter(filter); });

    QObject::connect(this, &FilterManager::tracksAdded, filter, &FilterWidget::tracksAdded);
    QObject::connect(this, &FilterManager::tracksUpdated, filter, &FilterWidget::tracksUpdated);
    QObject::connect(this, &FilterManager::tracksRemoved, filter, &FilterWidget::tracksRemoved);

    filter->reset(p->tracks());

    return filter;
}

void FilterManager::shutdown()
{
    p->fieldsRegistry.saveItems();
};

FieldRegistry* FilterManager::fieldRegistry() const
{
    return &p->fieldsRegistry;
}

QCoro::Task<void> FilterManager::searchChanged(QString search)
{
    const bool reset = p->searchFilter.length() > search.length();
    p->searchFilter  = search;

    Core::TrackList tracksToFilter{!reset && !p->filteredTracks.empty() ? p->filteredTracks : p->library->tracks()};

    p->filteredTracks
        = co_await Utils::asyncExec([&search, &tracksToFilter]() { return filterTracks(tracksToFilter, search); });

    p->resetFiltersAfterIndex(-1);
}
} // namespace Fy::Filters

#include "moc_filtermanager.cpp"
