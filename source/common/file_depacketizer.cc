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

#include "common/file_depacketizer.h"

#include "base/logging.h"
#include "base/codec/zstd_compress.h"

namespace common {

namespace {

// Matches the sender's threshold: below this the ratio says nothing useful, and a folder of small
// files would produce a line each.
const quint64 kSizeLogThreshold = 1024 * 1024; // 1 MB

} // namespace

//--------------------------------------------------------------------------------------------------
FileDepacketizer::FileDepacketizer(const QString& file_path, std::unique_ptr<QFile> file)
    : file_path_(file_path),
      file_(std::move(file))
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
FileDepacketizer::~FileDepacketizer()
{
    // If the file is opened, it was not completely written.
    if (file_->isOpen())
    {
        file_->close();

        // The transfer of files was canceled. Delete the file.
        QFile::remove(file_path_);
    }
}

//--------------------------------------------------------------------------------------------------
// static
std::unique_ptr<FileDepacketizer> FileDepacketizer::create(const QString& file_path, bool overwrite)
{
    QFile::OpenMode mode = QFile::WriteOnly;

    if (overwrite)
        mode |= QFile::Truncate;

    std::unique_ptr<QFile> file = std::make_unique<QFile>(file_path);
    if (!file->open(mode))
        return nullptr;

    return std::unique_ptr<FileDepacketizer>(new FileDepacketizer(file_path, std::move(file)));
}

//--------------------------------------------------------------------------------------------------
bool FileDepacketizer::writeNextPacket(const proto::file_transfer::Packet& packet)
{
    DCHECK(file_->isOpen());

    // The empty-packet sentinels (zero-length file, cancel) are judged by the wire size: they carry
    // no data and are never compressed.
    if (!packet.data().size())
    {
        if (packet.flags() & proto::file_transfer::Packet::LAST_PACKET)
        {
            if (packet.flags() & proto::file_transfer::Packet::FIRST_PACKET)
            {
                // Zero-length file received.
                file_size_ = 0;
                file_->close();
            }
            else
            {
                // If an empty data packet with the last packet flag set is received, the transfer
                // is canceled.
            }

            return true;
        }

        LOG(ERROR) << "Wrong packet size";
        return false;
    }

    // A compressed chunk is expanded back to its original bytes here; the write and the progress
    // accounting below both work on the uncompressed payload, so |packet_size| is the real length.
    std::string decompressed;
    const std::string* payload = &packet.data();

    if (packet.flags() & proto::file_transfer::Packet::COMPRESSED_ZSTD)
    {
        decompressed = base::ZstdCompress::decompress(packet.data());
        if (decompressed.empty())
        {
            LOG(ERROR) << "Unable to decompress packet of" << packet.data().size() << "bytes";
            return false;
        }

        payload = &decompressed;
    }

    const size_t packet_size = payload->size();

    // The first packet must have the full file size.
    if (packet.flags() & proto::file_transfer::Packet::FIRST_PACKET)
    {
        file_size_ = packet.file_size();
        left_size_ = file_size_;
    }

    if (!file_->seek(file_size_ - left_size_))
    {
        LOG(ERROR) << "seek failed";
        return false;
    }

    if (file_->write(payload->data(), packet_size) == -1)
    {
        LOG(ERROR) << "Unable to write file";
        return false;
    }

    left_size_ -= packet_size;

    wire_bytes_ += packet.data().size();
    written_bytes_ += packet_size;

    if (packet.flags() & proto::file_transfer::Packet::COMPRESSED_ZSTD)
        ++compressed_chunks_;
    else
        ++raw_chunks_;

    if (packet.flags() & proto::file_transfer::Packet::LAST_PACKET)
    {
        if (written_bytes_ >= kSizeLogThreshold)
        {
            LOG(INFO) << "File received." << "Size:" << wire_bytes_ << "->" << written_bytes_
                      << "bytes" << "(saved" << (100 - (100 * wire_bytes_ / written_bytes_))
                      << "%, chunks:" << compressed_chunks_ << "compressed," << raw_chunks_
                      << "raw)";
        }

        file_size_ = 0;
        file_->close();
    }

    return true;
}

} // namespace common
