//
// Aspia Project
// Copyright (C) 2016-2025 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#ifndef COMMON_DESKTOP_CONFIG_LOGGING_H
#define COMMON_DESKTOP_CONFIG_LOGGING_H

#include <QString>

namespace proto {
namespace desktop {
class Config;
} // namespace desktop
} // namespace proto

namespace common {

// Formats a desktop session Config into one human-readable line: every flag decoded on/off, plus the
// video/audio encodings and the compression ratio. Logged at session start on both the client and the
// host so a session's actual parameters - most usefully whether the clipboard is even enabled - can be
// read straight out of either log instead of being inferred. Content-free by construction.
QString desktopConfigToString(const proto::desktop::Config& config);

} // namespace common

#endif // COMMON_DESKTOP_CONFIG_LOGGING_H
