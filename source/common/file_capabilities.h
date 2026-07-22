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

#ifndef COMMON_FILE_CAPABILITIES_H
#define COMMON_FILE_CAPABILITIES_H

#include "proto/file_transfer.h"

namespace common {

// Fills |out| with the optional file-transfer features this build supports. Both peers call it - the
// client to announce itself on the first request, the host to answer on the reply - so the set is
// defined in exactly one place. Feature-adding tasks (compression, checksums, resume, ...) extend
// this and nothing else in the handshake plumbing.
void setLocalFileCapabilities(proto::file_transfer::FileCapabilities* out);

} // namespace common

#endif // COMMON_FILE_CAPABILITIES_H
