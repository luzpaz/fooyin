/*
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

#pragma once

#include "fycore_export.h"

#include <taglib/audioproperties.h>

#include <QString>

#include <memory>

class QPixmap;

namespace Fooyin {
class Track;

class FYCORE_EXPORT TagReader
{
public:
    enum class Quality
    {
        Fast     = TagLib::AudioProperties::Fast,
        Average  = TagLib::AudioProperties::Average,
        Accurate = TagLib::AudioProperties::Accurate,
    };

    TagReader();
    ~TagReader();

    bool readMetaData(Track& track, Quality quality = Quality::Average);
    QByteArray readCover(const Track& track);

private:
    struct Private;
    std::unique_ptr<Private> p;
};
} // namespace Fooyin
