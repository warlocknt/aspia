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

#ifndef COMMON_FILE_DEPACKETIZER_H
#define COMMON_FILE_DEPACKETIZER_H

#include <QFile>

#include <memory>

#include "proto/file_transfer.h"

namespace common {

class FileDepacketizer
{
public:
    ~FileDepacketizer();

    static std::unique_ptr<FileDepacketizer> create(const QString& file_path, bool overwrite);

    // Reads the packet and writes its contents to a file.
    bool writeNextPacket(const proto::file_transfer::Packet& packet);

private:
    FileDepacketizer(const QString& file_path, std::unique_ptr<QFile> file);

    QString file_path_;
    std::unique_ptr<QFile> file_;

    quint64 file_size_ = 0;
    quint64 left_size_ = 0;

    // The receiving half of the compression measurement: what arrived against what was written.
    // Accumulated per file and reported once, when the last packet lands.
    quint64 wire_bytes_ = 0;
    quint64 written_bytes_ = 0;
    int compressed_chunks_ = 0;
    int raw_chunks_ = 0;

    Q_DISABLE_COPY(FileDepacketizer)
};

} // namespace common

#endif // COMMON_FILE_DEPACKETIZER_H
