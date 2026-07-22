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

#ifndef COMMON_FILE_PACKETIZER_H
#define COMMON_FILE_PACKETIZER_H

#include <QFile>

#include <memory>

#include "proto/file_transfer.h"

namespace common {

class FilePacketizer
{
public:
    ~FilePacketizer() = default;

    // Creates an instance of the class.
    // Parameter |file_path| contains the full path to the file.
    // If the specified file can not be opened for reading, then returns nullptr.
    static std::unique_ptr<FilePacketizer> create(const QString& file_path);

    // Creates a packet for transferring. When |compress| is true each chunk is zstd-compressed and
    // sent compressed only if that came out smaller than the raw bytes (adaptive, per chunk), with
    // the COMPRESSED_ZSTD flag set. When false, or when compression did not help, the raw bytes are
    // sent. |compress| must be false unless the peer negotiated the "zstd" capability.
    std::unique_ptr<proto::file_transfer::Packet> readNextPacket(
        const proto::file_transfer::PacketRequest& request, bool compress);

private:
    explicit FilePacketizer(std::unique_ptr<QFile> file);

    std::unique_ptr<QFile> file_;

    quint64 file_size_ = 0;
    quint64 left_size_ = 0;

    Q_DISABLE_COPY(FilePacketizer)
};

} // namespace common

#endif // COMMON_FILE_PACKETIZER_H
