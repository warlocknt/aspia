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

#include "common/file_packetizer.h"

#include "base/logging.h"
#include "base/codec/zstd_compress.h"
#include "common/file_packet.h"

namespace common {

namespace {

// zstd level for per-chunk compression. Level 1 is the fastest: chunks are read and compressed on
// the network IO thread, and the point is to spend less on the wire, not to squeeze every last byte.
const int kCompressionLevel = 1;

} // namespace

//--------------------------------------------------------------------------------------------------
FilePacketizer::FilePacketizer(std::unique_ptr<QFile> file)
    : file_(std::move(file))
{
    file_size_ = file_->size();
    left_size_ = file_size_;
}

//--------------------------------------------------------------------------------------------------
std::unique_ptr<FilePacketizer> FilePacketizer::create(const QString& file_path)
{
    std::unique_ptr<QFile> file = std::make_unique<QFile>(file_path);

    if (!file->open(QFile::ReadOnly))
        return nullptr;

    return std::unique_ptr<FilePacketizer>(new FilePacketizer(std::move(file)));
}

//--------------------------------------------------------------------------------------------------
std::unique_ptr<proto::file_transfer::Packet> FilePacketizer::readNextPacket(
    const proto::file_transfer::PacketRequest& request, bool compress)
{
    DCHECK(file_->isOpen());

    // Create a new file packet.
    std::unique_ptr<proto::file_transfer::Packet> packet =
        std::make_unique<proto::file_transfer::Packet>();

    if (request.flags() & proto::file_transfer::PacketRequest::CANCEL)
    {
        packet->set_flags(proto::file_transfer::Packet::LAST_PACKET);
        return packet;
    }

    size_t packet_buffer_size = kMaxFilePacketSize;

    if (left_size_ < kMaxFilePacketSize)
        packet_buffer_size = static_cast<size_t>(left_size_);

    // Read the raw chunk into a temporary buffer: it may go on the wire either as-is or compressed,
    // and the choice is made only after seeing which is smaller.
    std::string chunk;
    chunk.resize(packet_buffer_size);

    // Moving to a new position in file.
    if (!file_->seek(file_size_ - left_size_))
    {
        LOG(ERROR) << "Unable to seek file";
        return nullptr;
    }

    if (file_->read(chunk.data(), packet_buffer_size) == -1)
    {
        LOG(ERROR) << "Unable to read file";
        return nullptr;
    }

    // Adaptive, per chunk: compress and keep the result only when it is actually smaller, so an
    // already-compressed region (a zip, a jpeg) costs a compression attempt but is still sent raw.
    bool sent_compressed = false;
    if (compress && packet_buffer_size)
    {
        std::string compressed = base::ZstdCompress::compress(chunk, kCompressionLevel);
        if (!compressed.empty() && compressed.size() < chunk.size())
        {
            *packet->mutable_data() = std::move(compressed);
            packet->set_flags(packet->flags() | proto::file_transfer::Packet::COMPRESSED_ZSTD);
            sent_compressed = true;
        }
    }

    if (!sent_compressed)
        *packet->mutable_data() = std::move(chunk);

    if (left_size_ == file_size_)
    {
        packet->set_flags(packet->flags() | proto::file_transfer::Packet::FIRST_PACKET);

        // Set file path and size in first packet.
        packet->set_file_size(file_size_);
    }

    // Progress is tracked in uncompressed bytes - the amount actually read from the file - not by
    // the size of |data|, which may now be the compressed length.
    left_size_ -= packet_buffer_size;

    if (!left_size_)
    {
        file_size_ = 0;
        file_->close();

        packet->set_flags(packet->flags() | proto::file_transfer::Packet::LAST_PACKET);
    }

    return packet;
}

} // namespace common
