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

#include "common/file_capabilities.h"

#include "common/file_packet.h"

namespace common {

namespace {

// The only compression algorithm this build produces and consumes. The per-packet COMPRESSED_ZSTD
// flag identifies it on the wire, so no algorithm name travels with each chunk.
const char kCompressionZstd[] = "zstd";

} // namespace

//--------------------------------------------------------------------------------------------------
void setLocalFileCapabilities(proto::file_transfer::FileCapabilities* out)
{
    // The largest packet this side accepts. Both peers advertise it and the sender uses the smaller
    // of the two; today they always match, but announcing it now means the value can change on one
    // side later without breaking the other.
    out->set_max_packet_size(static_cast<quint32>(kMaxFilePacketSize));

    // Per-chunk zstd compression. Listed rather than assumed so that a peer without it (an older
    // build, or a future one that dropped it) is simply sent raw packets.
    out->add_compression(kCompressionZstd);

    // Checksums, resume and the rest are added here as they land - a new name in the relevant list,
    // nothing else in the handshake plumbing.
}

//--------------------------------------------------------------------------------------------------
std::string negotiatedCompression(const proto::file_transfer::FileCapabilities& peer)
{
    // Only zstd exists on this side; use it if the peer listed it. When more algorithms appear this
    // becomes "first of our preference order that the peer also has".
    for (const std::string& algorithm : peer.compression())
    {
        if (algorithm == kCompressionZstd)
            return kCompressionZstd;
    }

    return std::string();
}

} // namespace common
