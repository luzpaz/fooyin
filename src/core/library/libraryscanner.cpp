﻿/*
 * Fooyin
 * Copyright © 2023, Luke Taylor <LukeT1@proton.me>
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

#include "libraryscanner.h"

#include "database/trackdatabase.h"
#include "internalcoresettings.h"
#include "librarywatcher.h"
#include "playlist/playlistloader.h"

#include <core/coresettings.h>
#include <core/library/libraryinfo.h>
#include <core/playlist/playlist.h>
#include <core/playlist/playlistparser.h>
#include <core/track.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionpool.h>
#include <utils/fileutils.h>
#include <utils/timer.h>
#include <utils/utils.h>

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFileSystemWatcher>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <ranges>

Q_LOGGING_CATEGORY(LIB_SCANNER, "LibraryScanner", QtInfoMsg)

constexpr auto BatchSize   = 250;
constexpr auto ArchivePath = R"(unpack://%1|%2|file://%3!)";

namespace {
void sortFiles(QFileInfoList& files)
{
    std::sort(files.begin(), files.end(),
              [](const QFileInfo& a, const QFileInfo& b) { return a.filePath() < b.filePath(); });

    std::stable_sort(files.begin(), files.end(), [](const QFileInfo& a, const QFileInfo& b) {
        const bool aIsCue = a.suffix().compare(u"cue", Qt::CaseInsensitive) == 0;
        const bool bIsCue = b.suffix().compare(u"cue", Qt::CaseInsensitive) == 0;
        if(aIsCue && !bIsCue) {
            return true;
        }
        if(!aIsCue && bIsCue) {
            return false;
        }
        return false;
    });
}

QFileInfoList getFiles(const QList<QUrl>& urls, const QStringList& restrictExtensions,
                       const QStringList& excludeExtensions, const QStringList& playlistExtensions)
{
    QFileInfoList files;

    QStringList nameFilters{restrictExtensions};
    for(const auto& ext : excludeExtensions) {
        nameFilters.removeAll(ext);
    }

    for(const QUrl& url : urls) {
        if(!url.isLocalFile()) {
            continue;
        }

        const QFileInfo file{url.toLocalFile()};

        if(file.isDir()) {
            QDirIterator dirIt{file.absoluteFilePath(), Fooyin::Utils::extensionsToWildcards(nameFilters), QDir::Files,
                               QDirIterator::Subdirectories};
            while(dirIt.hasNext()) {
                dirIt.next();
                const QFileInfo info = dirIt.fileInfo();
                if(info.size() > 0) {
                    files.append(info);
                }
            }
        }
        else {
            if(playlistExtensions.contains(file.suffix())) {
                files.emplace_back(file.absoluteFilePath());
            }
        }
    }

    sortFiles(files);

    return files;
}

QFileInfoList getFiles(const QString& baseDirectory, const QStringList& restrictExtensions,
                       const QStringList& excludeExtensions)
{
    return getFiles({QUrl::fromLocalFile(baseDirectory)}, restrictExtensions, excludeExtensions, {});
}
} // namespace

namespace Fooyin {
class LibraryScannerPrivate
{
public:
    LibraryScannerPrivate(LibraryScanner* self, DbConnectionPoolPtr dbPool,
                          std::shared_ptr<PlaylistLoader> playlistLoader, std::shared_ptr<AudioLoader> audioLoader)
        : m_self{self}
        , m_dbPool{std::move(dbPool)}
        , m_playlistLoader{std::move(playlistLoader)}
        , m_audioLoader{std::move(audioLoader)}
    { }

    void finishScan();
    void cleanupScan();

    void addWatcher(const Fooyin::LibraryInfo& library);

    void reportProgress() const;
    void fileScanned(const QString& file);

    Track matchMissingTrack(const Track& track);

    void checkBatchFinished();
    void readFileProperties(Track& track);

    [[nodiscard]] TrackList readTracks(const QString& filepath) const;
    [[nodiscard]] TrackList readArchiveTracks(const QString& filepath) const;
    [[nodiscard]] TrackList readPlaylist(const QString& filepath) const;
    [[nodiscard]] TrackList readPlaylistTracks(const QString& filepath, bool addMissing = false) const;
    [[nodiscard]] TrackList readEmbeddedPlaylistTracks(const Track& track) const;

    void updateExistingCueTracks(const TrackList& tracks, const QString& cue);
    void addNewCueTracks(const QString& cue, const QString& filename);
    void readCue(const QString& cue, bool onlyModified);

    void setTrackProps(Track& track);
    void setTrackProps(Track& track, const QString& file);

    void updateExistingTrack(Track& track, const QString& file);
    void readNewTrack(const QString& file);

    void readFile(const QString& file, bool onlyModified);
    void populateExistingTracks(const TrackList& tracks, bool includeMissing = true);
    bool getAndSaveAllTracks(const QString& path, const TrackList& tracks, bool onlyModified);

    void changeLibraryStatus(LibraryInfo::Status status);

    LibraryScanner* m_self;

    FySettings m_settings;
    DbConnectionPoolPtr m_dbPool;
    std::shared_ptr<PlaylistLoader> m_playlistLoader;
    std::shared_ptr<AudioLoader> m_audioLoader;

    std::unique_ptr<DbConnectionHandler> m_dbHandler;

    bool m_monitor{false};
    LibraryInfo m_currentLibrary;
    TrackDatabase m_trackDatabase;

    TrackList m_tracksToStore;
    TrackList m_tracksToUpdate;

    std::unordered_map<QString, TrackList> m_trackPaths;
    std::unordered_map<QString, TrackList> m_existingArchives;
    std::unordered_map<QString, Track> m_missingFiles;
    std::unordered_map<QString, Track> m_missingHashes;
    std::unordered_map<QString, TrackList> m_existingCueTracks;
    std::unordered_map<QString, TrackList> m_missingCueTracks;
    std::set<QString> m_cueFilesScanned;

    std::set<QString> m_filesScanned;
    size_t m_totalFiles{0};

    std::unordered_map<int, LibraryWatcher> m_watchers;
};

void LibraryScannerPrivate::finishScan()
{
    if(m_self->state() != LibraryScanner::Paused) {
        m_self->setState(LibraryScanner::Idle);
        reportProgress();
        cleanupScan();
        emit m_self->finished();
    }
}

void LibraryScannerPrivate::cleanupScan()
{
    m_audioLoader->destroyThreadInstance();
    m_filesScanned.clear();
    m_totalFiles = 0;
    m_tracksToStore.clear();
    m_tracksToUpdate.clear();
    m_trackPaths.clear();
    m_existingArchives.clear();
    m_missingFiles.clear();
    m_missingHashes.clear();
    m_existingCueTracks.clear();
    m_missingCueTracks.clear();
    m_cueFilesScanned.clear();
}

void LibraryScannerPrivate::addWatcher(const LibraryInfo& library)
{
    auto watchPaths = [this, library](const QString& path) {
        QStringList dirs = Utils::File::getAllSubdirectories(path);
        dirs.append(path);
        m_watchers[library.id].addPaths(dirs);
    };

    watchPaths(library.path);

    QObject::connect(&m_watchers.at(library.id), &LibraryWatcher::libraryDirChanged, m_self,
                     [this, watchPaths, library](const QString& dir) {
                         watchPaths(dir);
                         emit m_self->directoryChanged(library, dir);
                     });
}

void LibraryScannerPrivate::reportProgress() const
{
    emit m_self->progressChanged(static_cast<int>(m_filesScanned.size()), static_cast<int>(m_totalFiles));
}

void LibraryScannerPrivate::fileScanned(const QString& file)
{
    m_filesScanned.emplace(file);
    reportProgress();
}

Track LibraryScannerPrivate::matchMissingTrack(const Track& track)
{
    const QString filename = track.filename();
    const QString hash     = track.hash();

    if(m_missingFiles.contains(filename) && m_missingFiles.at(filename).duration() == track.duration()) {
        return m_missingFiles.at(filename);
    }

    if(m_missingHashes.contains(hash) && m_missingHashes.at(hash).duration() == track.duration()) {
        return m_missingHashes.at(hash);
    }

    return {};
}

void LibraryScannerPrivate::checkBatchFinished()
{
    if(m_tracksToStore.size() >= BatchSize || m_tracksToUpdate.size() > BatchSize) {
        if(m_tracksToStore.size() >= BatchSize) {
            m_trackDatabase.storeTracks(m_tracksToStore);
        }
        if(m_tracksToUpdate.size() >= BatchSize) {
            m_trackDatabase.updateTracks(m_tracksToUpdate);
        }
        emit m_self->scanUpdate({.addedTracks = m_tracksToStore, .updatedTracks = {}});
        m_tracksToStore.clear();
        m_tracksToUpdate.clear();
    }
}

void LibraryScannerPrivate::readFileProperties(Track& track)
{
    const QFileInfo fileInfo{track.filepath()};

    if(track.addedTime() == 0) {
        track.setAddedTime(QDateTime::currentMSecsSinceEpoch());
    }
    if(track.modifiedTime() == 0) {
        const QDateTime modifiedTime = fileInfo.lastModified();
        track.setModifiedTime(modifiedTime.isValid() ? modifiedTime.toMSecsSinceEpoch() : 0);
    }
    if(track.fileSize() == 0) {
        track.setFileSize(fileInfo.size());
    }
}

TrackList LibraryScannerPrivate::readTracks(const QString& filepath) const
{
    if(m_audioLoader->isArchive(filepath)) {
        return readArchiveTracks(filepath);
    }

    auto* tagReader = m_audioLoader->readerForFile(filepath);
    if(!tagReader) {
        return {};
    }

    QFile file{filepath};
    if(!file.open(QIODevice::ReadOnly)) {
        qCWarning(LIB_SCANNER) << "Failed to open file:" << filepath;
        return {};
    }
    const AudioSource source{filepath, &file, nullptr};

    if(!tagReader->init(source)) {
        qCInfo(LIB_SCANNER) << "Unsupported file:" << filepath;
        return {};
    }

    TrackList tracks;
    const int subsongCount = tagReader->subsongCount();

    for(int subIndex{0}; subIndex < subsongCount; ++subIndex) {
        Track subTrack{filepath, subIndex};
        subTrack.setFileSize(file.size());

        source.device->seek(0);
        if(tagReader->readTrack(source, subTrack)) {
            subTrack.generateHash();
            tracks.push_back(subTrack);
        }
    }

    return tracks;
}

TrackList LibraryScannerPrivate::readArchiveTracks(const QString& filepath) const
{
    auto* archiveReader = m_audioLoader->archiveReaderForFile(filepath);
    if(!archiveReader) {
        return {};
    }

    if(!archiveReader->init(filepath)) {
        return {};
    }

    TrackList tracks;
    const QString type        = archiveReader->type();
    const QString archivePath = QLatin1String{ArchivePath}.arg(type).arg(filepath.size()).arg(filepath);
    const QFileInfo archiveInfo{filepath};
    const QDateTime modifiedTime = archiveInfo.lastModified();

    auto readEntry = [&](const QString& entry, QIODevice* device) {
        if(!device->open(QIODevice::ReadOnly)) {
            qCInfo(LIB_SCANNER) << "Failed to open file:" << entry;
            return;
        }

        auto* fileReader = m_audioLoader->readerForFile(entry);
        if(!fileReader) {
            qCInfo(LIB_SCANNER) << "Unsupported file:" << entry;
            return;
        }

        AudioSource source;
        source.filepath      = filepath;
        source.device        = device;
        source.archiveReader = archiveReader;

        if(!fileReader->init(source)) {
            qCInfo(LIB_SCANNER) << "Unsupported file:" << entry;
            return;
        }

        const int subsongCount = fileReader->subsongCount();
        for(int subIndex{0}; subIndex < subsongCount; ++subIndex) {
            Track subTrack{archivePath + entry, subIndex};
            subTrack.setFileSize(device->size());
            subTrack.setModifiedTime(modifiedTime.isValid() ? modifiedTime.toMSecsSinceEpoch() : 0);
            source.filepath = subTrack.filepath();

            device->seek(0);
            if(fileReader->readTrack(source, subTrack)) {
                subTrack.generateHash();
                tracks.push_back(subTrack);
            }
        }
    };

    if(archiveReader->readTracks(readEntry)) {
        qCDebug(LIB_SCANNER) << "Indexed" << tracks.size() << "tracks in" << filepath;
        return tracks;
    }

    return {};
}

TrackList LibraryScannerPrivate::readPlaylist(const QString& filepath) const
{
    TrackList tracks;

    const TrackList playlistTracks = readPlaylistTracks(filepath, true);
    for(const Track& playlistTrack : playlistTracks) {
        const auto trackKey = playlistTrack.filepath();

        if(m_trackPaths.contains(trackKey)) {
            const auto existingTracks = m_trackPaths.at(trackKey);
            for(const Track& track : existingTracks) {
                if(track.uniqueFilepath() == playlistTrack.uniqueFilepath()) {
                    tracks.push_back(track);
                    break;
                }
            }
        }
        else {
            Track track{playlistTrack};
            track.generateHash();
            tracks.push_back(track);
        }
    }

    return tracks;
}

TrackList LibraryScannerPrivate::readPlaylistTracks(const QString& path, bool addMissing) const
{
    if(path.isEmpty()) {
        return {};
    }

    QFile playlistFile{path};
    if(!playlistFile.open(QIODevice::ReadOnly)) {
        qCWarning(LIB_SCANNER) << "Could not open file" << path << "for reading:" << playlistFile.errorString();
        return {};
    }

    const QFileInfo info{playlistFile};
    QDir dir{path};
    dir.cdUp();

    if(auto* parser = m_playlistLoader->parserForExtension(info.suffix())) {
        return parser->readPlaylist(&playlistFile, path, dir, !addMissing);
    }

    return {};
}

TrackList LibraryScannerPrivate::readEmbeddedPlaylistTracks(const Track& track) const
{
    const auto cues = track.extraTag(QStringLiteral("CUESHEET"));
    QByteArray bytes{cues.front().toUtf8()};
    QBuffer buffer(&bytes);
    if(!buffer.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(LIB_SCANNER) << "Can't open buffer for reading:" << buffer.errorString();
        return {};
    }

    if(auto* parser = m_playlistLoader->parserForExtension(QStringLiteral("cue"))) {
        TrackList tracks = parser->readPlaylist(&buffer, track.filepath(), {}, false);
        for(auto& plTrack : tracks) {
            plTrack.generateHash();
        }
        return tracks;
    }

    return {};
}

void LibraryScannerPrivate::updateExistingCueTracks(const TrackList& tracks, const QString& cue)
{
    std::unordered_map<QString, Track> existingTrackPaths;
    for(const Track& track : tracks) {
        existingTrackPaths.emplace(track.uniqueFilepath(), track);
    }

    const TrackList cueTracks = readPlaylistTracks(cue);
    for(const Track& cueTrack : cueTracks) {
        Track track{cueTrack};
        if(existingTrackPaths.contains(track.uniqueFilepath())) {
            track.setId(existingTrackPaths.at(track.uniqueFilepath()).id());
        }
        setTrackProps(track);
        m_tracksToUpdate.push_back(track);
        m_cueFilesScanned.emplace(track.filepath());
    }
}

void LibraryScannerPrivate::addNewCueTracks(const QString& cue, const QString& filename)
{
    if(m_missingCueTracks.contains(filename)) {
        TrackList refoundCueTracks = m_missingCueTracks.at(cue);
        for(Track& track : refoundCueTracks) {
            track.setCuePath(cue);
            m_tracksToUpdate.push_back(track);
        }
    }
    else {
        const TrackList cueTracks = readPlaylistTracks(cue);
        for(const Track& cueTrack : cueTracks) {
            Track track{cueTrack};
            setTrackProps(track);
            m_tracksToStore.push_back(track);
            m_cueFilesScanned.emplace(track.filepath());
        }
    }
}

void LibraryScannerPrivate::readCue(const QString& cue, bool onlyModified)
{
    const QFileInfo info{cue};
    const QDateTime lastModifiedTime{info.lastModified()};
    uint64_t lastModified{0};

    if(lastModifiedTime.isValid()) {
        lastModified = static_cast<uint64_t>(lastModifiedTime.toMSecsSinceEpoch());
    }

    if(m_existingCueTracks.contains(cue)) {
        const auto& tracks = m_existingCueTracks.at(cue);
        if(tracks.front().modifiedTime() < lastModified || !onlyModified) {
            updateExistingCueTracks(tracks, cue);
        }
        else {
            for(const Track& track : tracks) {
                m_cueFilesScanned.emplace(track.filepath());
            }
        }
    }
    else {
        addNewCueTracks(cue, info.fileName());
    }
}

void LibraryScannerPrivate::setTrackProps(Track& track)
{
    setTrackProps(track, track.filepath());
}

void LibraryScannerPrivate::setTrackProps(Track& track, const QString& file)
{
    readFileProperties(track);
    track.setFilePath(file);

    if(m_currentLibrary.id >= 0) {
        track.setLibraryId(m_currentLibrary.id);
    }
    track.generateHash();
    track.setIsEnabled(true);
}

void LibraryScannerPrivate::updateExistingTrack(Track& track, const QString& file)
{
    setTrackProps(track, file);
    m_missingFiles.erase(track.filename());

    if(track.id() < 0) {
        const int id = m_trackDatabase.idForTrack(track);
        if(id < 0) {
            qCWarning(LIB_SCANNER) << "Attempting to update track not in database:" << file;
        }
        else {
            track.setId(id);
        }
    }

    if(track.hasExtraTag(QStringLiteral("CUESHEET"))) {
        std::unordered_map<QString, Track> existingTrackPaths;
        if(m_existingCueTracks.contains(track.filepath())) {
            const auto& tracks = m_existingCueTracks.at(track.filepath());
            for(const Track& existingTrack : tracks) {
                existingTrackPaths.emplace(existingTrack.uniqueFilepath(), existingTrack);
            }
        }

        TrackList cueTracks = readEmbeddedPlaylistTracks(track);
        for(Track& cueTrack : cueTracks) {
            if(existingTrackPaths.contains(cueTrack.uniqueFilepath())) {
                cueTrack.setId(existingTrackPaths.at(cueTrack.uniqueFilepath()).id());
            }
            setTrackProps(cueTrack, file);
            m_tracksToUpdate.push_back(cueTrack);
            m_missingHashes.erase(cueTrack.hash());
        }
    }
    else {
        m_tracksToUpdate.push_back(track);
        m_missingHashes.erase(track.hash());
    }
}

void LibraryScannerPrivate::readNewTrack(const QString& file)
{
    TrackList tracks = readTracks(file);
    if(tracks.empty()) {
        return;
    }

    for(Track& track : tracks) {
        Track refoundTrack = matchMissingTrack(track);
        if(refoundTrack.isInLibrary() || refoundTrack.isInDatabase()) {
            m_missingHashes.erase(refoundTrack.hash());
            m_missingFiles.erase(refoundTrack.filename());

            setTrackProps(refoundTrack, file);
            m_tracksToUpdate.push_back(refoundTrack);
        }
        else {
            setTrackProps(track);
            track.setAddedTime(QDateTime::currentMSecsSinceEpoch());

            if(track.hasExtraTag(QStringLiteral("CUESHEET"))) {
                TrackList cueTracks = readEmbeddedPlaylistTracks(track);
                for(Track& cueTrack : cueTracks) {
                    setTrackProps(cueTrack, file);
                    m_tracksToStore.push_back(cueTrack);
                }
            }
            else {
                m_tracksToStore.push_back(track);
            }
        }
    }
}

void LibraryScannerPrivate::readFile(const QString& file, bool onlyModified)
{
    if(!m_self->mayRun()) {
        return;
    }

    if(m_cueFilesScanned.contains(file)) {
        return;
    }

    const QFileInfo info{file};
    const QDateTime lastModifiedTime{info.lastModified()};
    uint64_t lastModified{0};

    if(lastModifiedTime.isValid()) {
        lastModified = static_cast<uint64_t>(lastModifiedTime.toMSecsSinceEpoch());
    }

    if(m_trackPaths.contains(file)) {
        const Track& libraryTrack = m_trackPaths.at(file).front();

        if(!libraryTrack.isEnabled() || libraryTrack.libraryId() != m_currentLibrary.id
           || libraryTrack.modifiedTime() < lastModified || !onlyModified) {
            Track changedTrack{libraryTrack};
            if(!m_audioLoader->readTrackMetadata(changedTrack)) {
                return;
            }

            if(lastModifiedTime.isValid()) {
                changedTrack.setModifiedTime(lastModified);
            }

            updateExistingTrack(changedTrack, file);
        }
    }
    else if(m_existingArchives.contains(file)) {
        const Track& libraryTrack = m_existingArchives.at(file).front();

        if(!libraryTrack.isEnabled() || libraryTrack.libraryId() != m_currentLibrary.id
           || libraryTrack.modifiedTime() < lastModified || !onlyModified) {
            TrackList tracks = readArchiveTracks(file);
            for(Track& track : tracks) {
                updateExistingTrack(track, track.filepath());
            }
        }
    }
    else {
        readNewTrack(file);
    }
}

void LibraryScannerPrivate::populateExistingTracks(const TrackList& tracks, bool includeMissing)
{
    for(const Track& track : tracks) {
        m_trackPaths[track.filepath()].push_back(track);
        if(track.isInArchive()) {
            m_existingArchives[track.archivePath()].push_back(track);
        }

        if(includeMissing) {
            if(track.hasCue()) {
                const auto cuePath = track.cuePath() == u"Embedded" ? track.filepath() : track.cuePath();
                m_existingCueTracks[cuePath].emplace_back(track);
                if(!QFileInfo::exists(cuePath)) {
                    m_missingCueTracks[cuePath].emplace_back(track);
                }
            }

            if(!track.isInArchive()) {
                if(!QFileInfo::exists(track.filepath())) {
                    m_missingFiles.emplace(track.filename(), track);
                    m_missingHashes.emplace(track.hash(), track);
                }
            }
            else {
                if(!QFileInfo::exists(track.archivePath())) {
                    m_missingFiles.emplace(track.filename(), track);
                    m_missingHashes.emplace(track.hash(), track);
                }
            }
        }
    }
}

bool LibraryScannerPrivate::getAndSaveAllTracks(const QString& path, const TrackList& tracks, bool onlyModified)
{
    populateExistingTracks(tracks);

    using namespace Settings::Core::Internal;

    QStringList restrictExtensions = m_settings.value(QLatin1String{LibraryRestrictTypes}).toStringList();
    const QStringList excludeExtensions
        = m_settings.value(QLatin1String{LibraryExcludeTypes}, QStringList{QStringLiteral("cue")}).toStringList();

    if(restrictExtensions.empty()) {
        restrictExtensions = m_audioLoader->supportedFileExtensions();
        restrictExtensions.append(QStringLiteral("cue"));
    }

    const auto files = getFiles(path, restrictExtensions, excludeExtensions);

    m_totalFiles = files.size();
    reportProgress();

    for(const auto& file : files) {
        if(!m_self->mayRun()) {
            return false;
        }

        const QString filepath = file.absoluteFilePath();

        if(file.suffix() == u"cue") {
            readCue(filepath, onlyModified);
        }
        else {
            readFile(filepath, onlyModified);
        }

        fileScanned(filepath);
        checkBatchFinished();
    }

    for(auto& track : m_missingFiles | std::views::values) {
        if(track.isInLibrary() || track.isEnabled()) {
            track.setLibraryId(-1);
            track.setIsEnabled(false);
            m_tracksToUpdate.push_back(track);
        }
    }

    m_trackDatabase.storeTracks(m_tracksToStore);
    m_trackDatabase.updateTracks(m_tracksToUpdate);

    if(!m_tracksToStore.empty() || !m_tracksToUpdate.empty()) {
        emit m_self->scanUpdate({m_tracksToStore, m_tracksToUpdate});
    }

    return true;
}

void LibraryScannerPrivate::changeLibraryStatus(LibraryInfo::Status status)
{
    m_currentLibrary.status = status;
    emit m_self->statusChanged(m_currentLibrary);
}

LibraryScanner::LibraryScanner(DbConnectionPoolPtr dbPool, std::shared_ptr<PlaylistLoader> playlistLoader,
                               std::shared_ptr<AudioLoader> audioLoader, QObject* parent)
    : Worker{parent}
    , p{std::make_unique<LibraryScannerPrivate>(this, std::move(dbPool), std::move(playlistLoader),
                                                std::move(audioLoader))}
{ }

LibraryScanner::~LibraryScanner() = default;

void LibraryScanner::initialiseThread()
{
    Worker::initialiseThread();

    p->m_dbHandler = std::make_unique<DbConnectionHandler>(p->m_dbPool);
    p->m_trackDatabase.initialise(DbConnectionProvider{p->m_dbPool});
}

void LibraryScanner::stopThread()
{
    if(state() == Running) {
        QMetaObject::invokeMethod(
            this,
            [this]() { emit progressChanged(static_cast<int>(p->m_totalFiles), static_cast<int>(p->m_totalFiles)); },
            Qt::QueuedConnection);
    }

    setState(Idle);
}

void LibraryScanner::setMonitorLibraries(bool enabled)
{
    p->m_monitor = enabled;
}

void LibraryScanner::setupWatchers(const LibraryInfoMap& libraries, bool enabled)
{
    for(const auto& library : libraries | std::views::values) {
        if(!enabled) {
            if(library.status == LibraryInfo::Status::Monitoring) {
                LibraryInfo updatedLibrary{library};
                updatedLibrary.status = LibraryInfo::Status::Idle;
                emit statusChanged(updatedLibrary);
            }
        }
        else if(!p->m_watchers.contains(library.id)) {
            p->addWatcher(library);
            LibraryInfo updatedLibrary{library};
            updatedLibrary.status = LibraryInfo::Status::Monitoring;
            emit statusChanged(updatedLibrary);
        }
    }

    if(!enabled) {
        p->m_watchers.clear();
    }
}

void LibraryScanner::scanLibrary(const LibraryInfo& library, const TrackList& tracks, bool onlyModified)
{
    setState(Running);

    p->m_currentLibrary = library;
    p->changeLibraryStatus(LibraryInfo::Status::Scanning);

    const Timer timer;

    if(p->m_currentLibrary.id >= 0 && QFileInfo::exists(p->m_currentLibrary.path)) {
        if(p->m_monitor && !p->m_watchers.contains(library.id)) {
            p->addWatcher(library);
        }
        p->getAndSaveAllTracks(library.path, tracks, onlyModified);
        p->cleanupScan();
    }

    qCInfo(LIB_SCANNER) << "Scan of" << library.name << "took" << timer.elapsedFormatted();

    if(state() == Paused) {
        p->changeLibraryStatus(LibraryInfo::Status::Pending);
    }
    else {
        p->changeLibraryStatus(p->m_monitor ? LibraryInfo::Status::Monitoring : LibraryInfo::Status::Idle);
        setState(Idle);
        emit finished();
    }
}

void LibraryScanner::scanLibraryDirectory(const LibraryInfo& library, const QString& dir, const TrackList& tracks)
{
    setState(Running);

    p->m_currentLibrary = library;
    p->changeLibraryStatus(LibraryInfo::Status::Scanning);

    p->getAndSaveAllTracks(dir, tracks, true);
    p->cleanupScan();

    if(state() == Paused) {
        p->changeLibraryStatus(LibraryInfo::Status::Pending);
    }
    else {
        p->changeLibraryStatus(p->m_monitor ? LibraryInfo::Status::Monitoring : LibraryInfo::Status::Idle);
        setState(Idle);
        emit finished();
    }
}

void LibraryScanner::scanTracks(const TrackList& /*libraryTracks*/, const TrackList& tracks)
{
    setState(Running);

    const Timer timer;

    p->m_totalFiles = tracks.size();

    TrackList tracksToUpdate;

    for(const Track& track : tracks) {
        if(!mayRun()) {
            p->finishScan();
            return;
        }

        if(track.hasCue()) {
            continue;
        }

        Track updatedTrack{track.filepath()};

        if(p->m_audioLoader->readTrackMetadata(updatedTrack)) {
            updatedTrack.setId(track.id());
            updatedTrack.setLibraryId(track.libraryId());
            updatedTrack.setAddedTime(track.addedTime());
            p->readFileProperties(updatedTrack);
            updatedTrack.generateHash();

            tracksToUpdate.push_back(updatedTrack);
        }

        p->fileScanned(track.filepath());
    }

    if(!tracksToUpdate.empty()) {
        p->m_trackDatabase.updateTracks(tracksToUpdate);
        p->m_trackDatabase.updateTrackStats(tracksToUpdate);

        emit scanUpdate({{}, tracksToUpdate});
    }

    qCInfo(LIB_SCANNER) << "Scan of" << p->m_totalFiles << "tracks took" << timer.elapsedFormatted();

    p->finishScan();
}

void LibraryScanner::scanFiles(const TrackList& libraryTracks, const QList<QUrl>& urls)
{
    setState(Running);

    const Timer timer;

    TrackList tracksScanned;

    p->populateExistingTracks(libraryTracks, false);

    using namespace Settings::Core::Internal;

    const QStringList playlistExtensions = Playlist::supportedPlaylistExtensions();
    QStringList restrictExtensions       = p->m_settings.value(QLatin1String{ExternalRestrictTypes}).toStringList();
    const QStringList excludeExtensions  = p->m_settings.value(QLatin1String{ExternalExcludeTypes}).toStringList();

    if(restrictExtensions.empty()) {
        restrictExtensions = p->m_audioLoader->supportedFileExtensions();
        restrictExtensions.append(QStringLiteral("cue"));
    }

    const auto files = getFiles(urls, restrictExtensions, excludeExtensions, playlistExtensions);

    p->m_totalFiles = files.size();

    p->reportProgress();

    for(const auto& file : files) {
        if(!mayRun()) {
            p->finishScan();
            return;
        }

        const QString filepath = file.absoluteFilePath();

        if(playlistExtensions.contains(file.suffix())) {
            const TrackList playlistTracks = p->readPlaylist(filepath);
            p->m_totalFiles += playlistTracks.size();

            for(const Track& track : playlistTracks) {
                p->fileScanned(track.filepath());
                tracksScanned.emplace_back(track);
            }
            p->fileScanned(filepath);
        }
        else {
            if(!p->m_filesScanned.contains(filepath)) {
                if(p->m_trackPaths.contains(filepath)) {
                    const auto existingTracks = p->m_trackPaths.at(filepath);
                    for(const Track& track : existingTracks) {
                        tracksScanned.push_back(track);
                    }
                }
                else if(p->m_existingArchives.contains(filepath)) {
                    const auto existingTracks = p->m_existingArchives.at(filepath);
                    for(const Track& track : existingTracks) {
                        tracksScanned.push_back(track);
                    }
                }
                else {
                    TrackList tracks = p->readTracks(filepath);
                    if(tracks.empty()) {
                        continue;
                    }
                    for(Track& track : tracks) {
                        p->readFileProperties(track);
                        track.setAddedTime(QDateTime::currentMSecsSinceEpoch());

                        if(track.hasExtraTag(QStringLiteral("CUESHEET"))) {
                            const TrackList cueTracks = p->readEmbeddedPlaylistTracks(track);
                            std::ranges::copy(cueTracks, std::back_inserter(tracksScanned));
                        }
                        else {
                            tracksScanned.push_back(track);
                        }
                    }
                }
            }

            p->fileScanned(filepath);
        }
    }

    if(!tracksScanned.empty()) {
        p->m_trackDatabase.storeTracks(tracksScanned);
        emit scannedTracks(tracksScanned);
    }

    qCInfo(LIB_SCANNER) << "Scan of" << p->m_totalFiles << "files took" << timer.elapsedFormatted();

    p->finishScan();
}

void LibraryScanner::scanPlaylist(const TrackList& libraryTracks, const QList<QUrl>& urls)
{
    setState(Running);

    const Timer timer;

    TrackList tracksScanned;

    p->populateExistingTracks(libraryTracks, false);
    p->reportProgress();

    if(!mayRun()) {
        p->finishScan();
        return;
    }

    for(const auto& url : urls) {
        const TrackList playlistTracks = p->readPlaylist(url.toLocalFile());
        for(const Track& track : playlistTracks) {
            p->m_filesScanned.emplace(track.filepath());
            tracksScanned.push_back(track);
        }
    }

    if(!tracksScanned.empty()) {
        p->m_trackDatabase.storeTracks(tracksScanned);
        emit playlistLoaded(tracksScanned);
    }

    qCInfo(LIB_SCANNER) << "Scan of playlist took" << timer.elapsedFormatted();

    p->finishScan();
}
} // namespace Fooyin

#include "moc_libraryscanner.cpp"
