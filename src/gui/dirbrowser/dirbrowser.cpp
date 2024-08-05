/*
 * Fooyin
 * Copyright © 2024, Luke Taylor <LukeT1@proton.me>
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

#include "dirbrowser.h"

#include "dirdelegate.h"
#include "dirproxymodel.h"
#include "dirtree.h"
#include "internalguisettings.h"
#include "playlist/playlistinteractor.h"

#include <core/player/playercontroller.h>
#include <core/playlist/playlist.h>
#include <core/playlist/playlisthandler.h>
#include <core/track.h>
#include <gui/guiconstants.h>
#include <gui/trackselectioncontroller.h>
#include <gui/widgets/toolbutton.h>
#include <utils/fileutils.h>
#include <utils/settings/settingsmanager.h>
#include <utils/utils.h>

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTreeView>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

constexpr auto DirPlaylist = "␟DirBrowserPlaylist␟";

namespace {
class DirChange : public QUndoCommand
{
public:
    DirChange(Fooyin::DirBrowser* browser, QAbstractItemView* view, const QString& oldPath, const QString& newPath)
        : QUndoCommand{nullptr}
        , m_browser{browser}
        , m_view{view}
    {
        m_oldState.path      = oldPath;
        m_oldState.scrollPos = m_view->verticalScrollBar()->value();
        saveSelectedRow(m_oldState);

        m_newState.path = newPath;
    }

    [[nodiscard]] QString undoPath() const
    {
        return m_oldState.path;
    }

    void undo() override
    {
        m_newState.scrollPos = m_view->verticalScrollBar()->value();
        saveSelectedRow(m_newState);

        QObject::connect(
            m_browser, &Fooyin::DirBrowser::rootChanged, m_browser,
            [this]() {
                m_view->verticalScrollBar()->setValue(m_oldState.scrollPos);
                restoreSelectedRow(m_oldState);
            },
            static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::SingleShotConnection));

        m_view->setUpdatesEnabled(false);
        m_browser->updateDir(m_oldState.path);
    }

    void redo() override
    {
        if(m_newState.scrollPos >= 0) {
            QObject::connect(
                m_browser, &Fooyin::DirBrowser::rootChanged, m_browser,
                [this]() {
                    m_view->verticalScrollBar()->setValue(m_newState.scrollPos);
                    restoreSelectedRow(m_newState);
                },
                static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::SingleShotConnection));
        }

        m_view->setUpdatesEnabled(false);
        m_browser->updateDir(m_newState.path);
    }

private:
    struct State
    {
        QString path;
        int scrollPos{-1};
        int selectedRow{-1};
    };

    void saveSelectedRow(State& state) const
    {
        const auto selected = m_view->selectionModel()->selectedRows();

        if(!selected.empty()) {
            state.selectedRow = selected.front().row();
        }
    }

    void restoreSelectedRow(const State& state)
    {
        if(state.selectedRow >= 0) {
            const auto index = m_view->model()->index(state.selectedRow, 0, {});
            if(index.isValid()) {
                m_view->setCurrentIndex(index);
            }
        }
    }

    Fooyin::DirBrowser* m_browser;
    QAbstractItemView* m_view;

    State m_oldState;
    State m_newState;
};
} // namespace

namespace Fooyin {
class DirBrowserPrivate
{
public:
    DirBrowserPrivate(DirBrowser* self, const QStringList& supportedExtensions, PlaylistInteractor* playlistInteractor,
                      SettingsManager* settings);

    void checkIconProvider();

    void handleModelUpdated() const;

    [[nodiscard]] QueueTracks loadQueueTracks(const TrackList& tracks) const;

    void handleAction(TrackAction action, bool onlySelection);
    void handlePlayAction(const QList<QUrl>& files, const QString& startingFile);
    void handleDoubleClick(const QModelIndex& index);
    void handleMiddleClick();

    void changeRoot(const QString& root);
    void updateIndent(bool show) const;

    void setControlsEnabled(bool enabled);
    void setLocationEnabled(bool enabled);

    void changeMode(DirBrowser::Mode newMode);

    void startPlayback(const TrackList& tracks, int row);

    void updateControlState() const;
    void goUp();

    DirBrowser* m_self;

    QStringList m_supportedExtensions;
    PlaylistInteractor* m_playlistInteractor;
    PlaylistHandler* m_playlistHandler;
    SettingsManager* m_settings;

    std::unique_ptr<QFileIconProvider> m_iconProvider;

    QHBoxLayout* m_controlLayout;
    QPointer<QLineEdit> m_dirEdit;
    QPointer<ToolButton> m_backDir;
    QPointer<ToolButton> m_forwardDir;
    QPointer<ToolButton> m_upDir;

    DirBrowser::Mode m_mode{DirBrowser::Mode::List};
    DirTree* m_dirTree;
    QFileSystemModel* m_model;
    DirProxyModel* m_proxyModel;
    QUndoStack m_dirHistory;

    Playlist* m_playlist{nullptr};

    TrackAction m_doubleClickAction;
    TrackAction m_middleClickAction;
};

DirBrowserPrivate::DirBrowserPrivate(DirBrowser* self, const QStringList& supportedExtensions,
                                     PlaylistInteractor* playlistInteractor, SettingsManager* settings)
    : m_self{self}
    , m_supportedExtensions{Utils::extensionsToWildcards(supportedExtensions)}
    , m_playlistInteractor{playlistInteractor}
    , m_playlistHandler{m_playlistInteractor->handler()}
    , m_settings{settings}
    , m_controlLayout{new QHBoxLayout()}
    , m_dirTree{new DirTree(m_self)}
    , m_model{new QFileSystemModel(m_self)}
    , m_proxyModel{new DirProxyModel(m_self)}
    , m_doubleClickAction{static_cast<TrackAction>(m_settings->value<Settings::Gui::Internal::DirBrowserDoubleClick>())}
    , m_middleClickAction{static_cast<TrackAction>(m_settings->value<Settings::Gui::Internal::DirBrowserMiddleClick>())}
{
    auto* layout = new QVBoxLayout(m_self);
    layout->setContentsMargins(0, 0, 0, 0);

    layout->addLayout(m_controlLayout);
    layout->addWidget(m_dirTree);

    checkIconProvider();

    m_model->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    m_model->setNameFilters(m_supportedExtensions);
    m_model->setNameFilterDisables(false);
    m_model->setReadOnly(true);

    m_proxyModel->setSourceModel(m_model);
    m_proxyModel->setIconsEnabled(m_settings->value<Settings::Gui::Internal::DirBrowserIcons>());

    m_dirTree->setItemDelegate(new DirDelegate(m_self));
    m_dirTree->setModel(m_proxyModel);

    QString rootPath = m_settings->value<Settings::Gui::Internal::DirBrowserPath>();
    if(rootPath.isEmpty()) {
        rootPath = QDir::homePath();
    }
    m_dirTree->setRootIndex(m_proxyModel->mapFromSource(m_model->setRootPath(rootPath)));
    updateIndent(m_settings->value<Settings::Gui::Internal::DirBrowserListIndent>());
}

void DirBrowserPrivate::checkIconProvider()
{
    auto* modelProvider = m_model->iconProvider();
    if(!modelProvider || modelProvider->icon(QFileIconProvider::Folder).isNull()
       || modelProvider->icon(QFileIconProvider::File).isNull()) {
        m_iconProvider = std::make_unique<QFileIconProvider>();
        m_model->setIconProvider(m_iconProvider.get());
    }
}

void DirBrowserPrivate::handleModelUpdated() const
{
    if(m_mode == DirBrowser::Mode::List) {
        const QModelIndex root = m_model->setRootPath(m_model->rootPath());
        m_dirTree->setRootIndex(m_proxyModel->mapFromSource(root));
        m_proxyModel->reset(root);
    }

    updateControlState();
    m_dirTree->setUpdatesEnabled(true);
}

QueueTracks DirBrowserPrivate::loadQueueTracks(const TrackList& tracks) const
{
    QueueTracks queueTracks;

    UId playlistId;
    if(m_playlist) {
        playlistId = m_playlist->id();
    }

    for(const Track& track : tracks) {
        queueTracks.emplace_back(track, playlistId);
    }

    return queueTracks;
}

void DirBrowserPrivate::handleAction(TrackAction action, bool onlySelection)
{
    QModelIndexList selected = m_dirTree->selectionModel()->selectedRows();

    if(selected.empty()) {
        return;
    }

    QString firstPath;

    if(selected.size() == 1) {
        const QModelIndex index = selected.front();
        if(index.isValid()) {
            const QFileInfo filePath{index.data(QFileSystemModel::FilePathRole).toString()};
            if(!onlySelection && filePath.isFile()) {
                // Add all files in same directory
                selected  = {m_proxyModel->mapToSource(selected.front()).parent()};
                firstPath = filePath.absoluteFilePath();
            }
        }
    }

    QList<QUrl> files;

    for(const QModelIndex& index : std::as_const(selected)) {
        if(index.isValid()) {
            const QFileInfo filePath{index.data(QFileSystemModel::FilePathRole).toString()};
            if(filePath.isDir()) {
                if(!onlySelection) {
                    files.append(Utils::File::getUrlsInDir(filePath.absoluteFilePath(), m_supportedExtensions));
                }
                else {
                    files.append(
                        Utils::File::getUrlsInDirRecursive(filePath.absoluteFilePath(), m_supportedExtensions));
                }
            }
            else {
                files.append(QUrl::fromLocalFile(filePath.absoluteFilePath()));
            }
        }
    }

    if(files.empty()) {
        return;
    }

    if(firstPath.isEmpty()) {
        firstPath = files.front().path();
    }

    QDir parentDir{firstPath};
    parentDir.cdUp();
    const QString playlistName = parentDir.dirName();

    const bool startPlayback = m_settings->value<Settings::Gui::Internal::DirBrowserSendPlayback>();

    switch(action) {
        case(TrackAction::Play):
            handlePlayAction(files, firstPath);
            break;
        case(TrackAction::AddCurrentPlaylist):
            m_playlistInteractor->filesToCurrentPlaylist(files);
            break;
        case(TrackAction::SendCurrentPlaylist):
            m_playlistInteractor->filesToCurrentPlaylistReplace(files, startPlayback);
            break;
        case(TrackAction::SendNewPlaylist):
            m_playlistInteractor->filesToNewPlaylist(playlistName, files, startPlayback);
            break;
        case(TrackAction::AddActivePlaylist):
            m_playlistInteractor->filesToActivePlaylist(files);
            break;
        case(TrackAction::AddToQueue):
            m_playlistInteractor->filesToTracks(files, [this](const TrackList& tracks) {
                m_playlistInteractor->playerController()->queueTracks(loadQueueTracks(tracks));
            });
            break;
        case(TrackAction::SendToQueue):
            m_playlistInteractor->filesToTracks(files, [this](const TrackList& tracks) {
                m_playlistInteractor->playerController()->replaceTracks(loadQueueTracks(tracks));
            });
            break;
        case(TrackAction::None):
            break;
    }
}

void DirBrowserPrivate::handlePlayAction(const QList<QUrl>& files, const QString& startingFile)
{
    int playIndex{0};

    if(!startingFile.isEmpty()) {
        auto rowIt = std::ranges::find_if(std::as_const(files),
                                          [&startingFile](const QUrl& file) { return file.path() == startingFile; });
        if(rowIt != files.cend()) {
            playIndex = static_cast<int>(std::distance(files.cbegin(), rowIt));
        }
    }

    TrackList tracks;
    std::ranges::transform(files, std::back_inserter(tracks),
                           [](const QUrl& file) { return Track{file.toLocalFile()}; });

    startPlayback(tracks, playIndex);
}

void DirBrowserPrivate::handleDoubleClick(const QModelIndex& index)
{
    if(!index.isValid()) {
        return;
    }

    const QString path = index.data(QFileSystemModel::FilePathRole).toString();

    if(path.isEmpty() && m_mode == DirBrowser::Mode::List) {
        goUp();
        return;
    }

    const QFileInfo filePath{path};
    if(filePath.isDir()) {
        if(m_mode == DirBrowser::Mode::List) {
            changeRoot(filePath.absoluteFilePath());
        }
        else {
            if(m_dirTree->isExpanded(index)) {
                m_dirTree->collapse(index);
            }
            else {
                m_dirTree->expand(index);
            }
        }
        return;
    }

    handleAction(m_doubleClickAction, m_doubleClickAction != TrackAction::Play);
}

void DirBrowserPrivate::handleMiddleClick()
{
    handleAction(m_middleClickAction, true);
}

void DirBrowserPrivate::changeRoot(const QString& root)
{
    if(root.isEmpty() || !QFileInfo::exists(root)) {
        return;
    }

    if(QDir{root} == QDir{m_model->rootPath()}) {
        return;
    }

    auto* changeDir = new DirChange(m_self, m_dirTree, m_model->rootPath(), root);
    m_dirHistory.push(changeDir);
}

void DirBrowserPrivate::updateIndent(bool show) const
{
    if(show || m_mode == DirBrowser::Mode::Tree) {
        m_dirTree->resetIndentation();
    }
    else {
        m_dirTree->setIndentation(0);
    }
}

void DirBrowserPrivate::setControlsEnabled(bool enabled)
{
    if(enabled && !m_upDir && !m_backDir && !m_forwardDir) {
        m_upDir      = new ToolButton(m_self);
        m_backDir    = new ToolButton(m_self);
        m_forwardDir = new ToolButton(m_self);

        m_upDir->setDefaultAction(
            new QAction(Utils::iconFromTheme(Constants::Icons::Up), DirBrowser::tr("Go up"), m_upDir));
        m_backDir->setDefaultAction(
            new QAction(Utils::iconFromTheme(Constants::Icons::GoPrevious), DirBrowser::tr("Go back"), m_backDir));
        m_forwardDir->setDefaultAction(
            new QAction(Utils::iconFromTheme(Constants::Icons::GoNext), DirBrowser::tr("Go forwards"), m_forwardDir));

        QObject::connect(m_upDir, &QPushButton::pressed, m_self, [this]() { goUp(); });
        QObject::connect(m_backDir, &QPushButton::pressed, m_self, [this]() {
            if(m_dirHistory.canUndo()) {
                m_dirHistory.undo();
            }
        });
        QObject::connect(m_forwardDir, &QPushButton::pressed, m_self, [this]() {
            if(m_dirHistory.canRedo()) {
                m_dirHistory.redo();
            }
        });

        m_controlLayout->insertWidget(0, m_upDir);
        m_controlLayout->insertWidget(0, m_forwardDir);
        m_controlLayout->insertWidget(0, m_backDir);
    }
    else {
        if(m_backDir) {
            m_backDir->deleteLater();
        }
        if(m_forwardDir) {
            m_forwardDir->deleteLater();
        }
        if(m_upDir) {
            m_upDir->deleteLater();
        }
    }
}

void DirBrowserPrivate::setLocationEnabled(bool enabled)
{
    if(enabled && !m_dirEdit) {
        m_dirEdit = new QLineEdit(m_self);
        QObject::connect(m_dirEdit, &QLineEdit::textEdited, m_self, [this](const QString& dir) { changeRoot(dir); });
        m_controlLayout->addWidget(m_dirEdit, 1);
        m_dirEdit->setText(m_model->rootPath());
    }
    else {
        if(m_dirEdit) {
            m_dirEdit->deleteLater();
        }
    }
}

void DirBrowserPrivate::changeMode(DirBrowser::Mode newMode)
{
    m_mode = newMode;

    const QString rootPath = m_model->rootPath();

    m_proxyModel->setFlat(m_mode == DirBrowser::Mode::List);

    const QModelIndex root = m_model->setRootPath(rootPath);
    m_dirTree->setRootIndex(m_proxyModel->mapFromSource(root));

    updateIndent(m_settings->value<Settings::Gui::Internal::DirBrowserListIndent>());
}

void DirBrowserPrivate::startPlayback(const TrackList& tracks, int row)
{
    if(!m_playlist) {
        m_playlist = m_playlistHandler->createTempPlaylist(QString::fromLatin1(DirPlaylist));
    }

    if(!m_playlist) {
        return;
    }

    m_playlistHandler->replacePlaylistTracks(m_playlist->id(), tracks);

    m_playlist->changeCurrentIndex(row);
    m_playlistHandler->startPlayback(m_playlist);
}

void DirBrowserPrivate::updateControlState() const
{
    if(m_upDir) {
        m_upDir->setEnabled(m_proxyModel->canGoUp());
    }
    if(m_backDir) {
        m_backDir->setEnabled(m_dirHistory.canUndo());
    }
    if(m_forwardDir) {
        m_forwardDir->setEnabled(m_dirHistory.canRedo());
    }
}

void DirBrowserPrivate::goUp()
{
    QDir root{m_model->rootPath()};

    if(!root.cdUp()) {
        return;
    }

    const QString newPath = root.absolutePath();

    if(m_dirHistory.canUndo()) {
        if(const auto* prevDir = static_cast<const DirChange*>(m_dirHistory.command(m_dirHistory.index() - 1))) {
            if(prevDir->undoPath() == newPath) {
                m_dirHistory.undo();
                return;
            }
        }
    }

    auto* changeDir = new DirChange(m_self, m_dirTree, m_model->rootPath(), newPath);
    m_dirHistory.push(changeDir);
}

DirBrowser::DirBrowser(const QStringList& supportedExtensions, PlaylistInteractor* playlistInteractor,
                       SettingsManager* settings, QWidget* parent)
    : FyWidget{parent}
    , p{std::make_unique<DirBrowserPrivate>(this, supportedExtensions, playlistInteractor, settings)}
{
    QObject::connect(p->m_dirTree, &QTreeView::doubleClicked, this,
                     [this](const QModelIndex& index) { p->handleDoubleClick(index); });
    QObject::connect(p->m_dirTree, &DirTree::middleClicked, this, [this]() { p->handleMiddleClick(); });
    QObject::connect(p->m_dirTree, &DirTree::backClicked, this, [this]() {
        if(p->m_dirHistory.canUndo()) {
            p->m_dirHistory.undo();
        }
    });
    QObject::connect(p->m_dirTree, &DirTree::forwardClicked, this, [this]() {
        if(p->m_dirHistory.canRedo()) {
            p->m_dirHistory.redo();
        }
    });

    QObject::connect(p->m_model, &QAbstractItemModel::layoutChanged, this, [this]() { p->handleModelUpdated(); });
    QObject::connect(
        p->m_proxyModel, &QAbstractItemModel::modelReset, this,
        [this]() {
            emit rootChanged();
            p->m_dirTree->selectionModel()->setCurrentIndex(p->m_proxyModel->index(0, 0, {}),
                                                            QItemSelectionModel::NoUpdate);
        },
        Qt::QueuedConnection);

    settings->subscribe<Settings::Gui::Internal::DirBrowserDoubleClick>(
        this, [this](int action) { p->m_doubleClickAction = static_cast<TrackAction>(action); });
    settings->subscribe<Settings::Gui::Internal::DirBrowserMiddleClick>(
        this, [this](int action) { p->m_middleClickAction = static_cast<TrackAction>(action); });
    p->m_settings->subscribe<Settings::Gui::Internal::DirBrowserMode>(
        this, [this](int mode) { p->changeMode(static_cast<Mode>(mode)); });
    p->m_settings->subscribe<Settings::Gui::Internal::DirBrowserIcons>(
        this, [this](bool enabled) { p->m_proxyModel->setIconsEnabled(enabled); });
    p->m_settings->subscribe<Settings::Gui::Internal::DirBrowserListIndent>(
        this, [this](bool enabled) { p->updateIndent(enabled); });
    p->m_settings->subscribe<Settings::Gui::Internal::DirBrowserControls>(
        this, [this](bool enabled) { p->setControlsEnabled(enabled); });
    p->m_settings->subscribe<Settings::Gui::Internal::DirBrowserLocation>(
        this, [this](bool enabled) { p->setLocationEnabled(enabled); });

    p->changeMode(static_cast<Mode>(settings->value<Settings::Gui::Internal::DirBrowserMode>()));
    p->setControlsEnabled(settings->value<Settings::Gui::Internal::DirBrowserControls>());
    p->setLocationEnabled(settings->value<Settings::Gui::Internal::DirBrowserLocation>());
    p->updateControlState();
}

DirBrowser::~DirBrowser()
{
    p->m_settings->set<Settings::Gui::Internal::DirBrowserPath>(p->m_model->rootPath());
}

QString DirBrowser::name() const
{
    return tr("Directory Browser");
}

QString DirBrowser::layoutName() const
{
    return QStringLiteral("DirectoryBrowser");
}

void DirBrowser::updateDir(const QString& dir)
{
    const QModelIndex root = p->m_model->setRootPath(dir);
    p->m_dirTree->setRootIndex(p->m_proxyModel->mapFromSource(root));

    if(p->m_dirEdit) {
        p->m_dirEdit->setText(dir);
    }

    if(p->m_playlist) {
        p->m_proxyModel->setPlayingPath(p->m_playlist->currentTrack().filepath());
    }
}

void DirBrowser::playstateChanged(Player::PlayState state)
{
    p->m_proxyModel->setPlayState(state);
}

void DirBrowser::activePlaylistChanged(Playlist* playlist)
{
    if(!playlist || !p->m_playlist) {
        return;
    }

    if(playlist->id() != p->m_playlist->id()) {
        p->m_proxyModel->setPlayingPath({});
    }
}

void DirBrowser::playlistTrackChanged(const PlaylistTrack& track)
{
    if(p->m_playlist && p->m_playlist->id() == track.playlistId) {
        p->m_proxyModel->setPlayingPath(track.track.filepath());
    }
}

void DirBrowser::contextMenuEvent(QContextMenuEvent* event)
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto* playAction = new QAction(tr("Play"), menu);
    QObject::connect(playAction, &QAction::triggered, this, [this]() { p->handleAction(TrackAction::Play, false); });

    auto* addCurrent = new QAction(tr("Add to current playlist"), menu);
    QObject::connect(addCurrent, &QAction::triggered, this,
                     [this]() { p->handleAction(TrackAction::AddCurrentPlaylist, true); });

    auto* addActive = new QAction(tr("Add to active playlist"), menu);
    QObject::connect(addActive, &QAction::triggered, this,
                     [this]() { p->handleAction(TrackAction::AddActivePlaylist, true); });

    auto* sendCurrent = new QAction(tr("Send to current playlist"), menu);
    QObject::connect(sendCurrent, &QAction::triggered, this,
                     [this]() { p->handleAction(TrackAction::SendCurrentPlaylist, true); });

    auto* sendNew = new QAction(tr("Send to new playlist"), menu);
    QObject::connect(sendNew, &QAction::triggered, this,
                     [this]() { p->handleAction(TrackAction::SendNewPlaylist, true); });

    auto* addQueue = new QAction(tr("Add to playback queue"), menu);
    QObject::connect(addQueue, &QAction::triggered, this, [this]() { p->handleAction(TrackAction::AddToQueue, true); });

    auto* sendQueue = new QAction(tr("Send to playback queue"), menu);
    QObject::connect(sendQueue, &QAction::triggered, this,
                     [this]() { p->handleAction(TrackAction::SendToQueue, true); });

    menu->addAction(playAction);
    menu->addSeparator();
    menu->addAction(addCurrent);
    menu->addAction(addActive);
    menu->addAction(sendCurrent);
    menu->addAction(sendNew);
    menu->addSeparator();
    menu->addAction(addQueue);
    menu->addAction(sendQueue);
    menu->addSeparator();

    const QModelIndex index = p->m_dirTree->indexAt(p->m_dirTree->mapFromGlobal(event->globalPos()));

    if(index.isValid()) {
        const QFileInfo selectedPath{index.data(QFileSystemModel::FilePathRole).toString()};
        if(selectedPath.isDir()) {
            const QString dir = index.data(QFileSystemModel::FilePathRole).toString();
            auto* setRoot     = new QAction(tr("Set as root"), menu);
            QObject::connect(setRoot, &QAction::triggered, this, [this, dir]() { p->changeRoot(dir); });
            menu->addAction(setRoot);
        }
    }

    menu->popup(event->globalPos());
}

void DirBrowser::keyPressEvent(QKeyEvent* event)
{
    const auto key = event->key();

    if(key == Qt::Key_Enter || key == Qt::Key_Return) {
        const auto indexes = p->m_dirTree->selectionModel()->selectedRows();
        if(!indexes.empty()) {
            p->handleDoubleClick(indexes.front());
        }
    }
    else if(key == Qt::Key_Backspace) {
        p->goUp();
    }

    QWidget::keyPressEvent(event);
}
} // namespace Fooyin

#include "moc_dirbrowser.cpp"
