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

#include "host/video_encode_worker.h"

#include "base/logging.h"
#include "base/desktop/frame.h"
#include "base/desktop/mouse_cursor.h"
#include "base/desktop/pixel_format.h"
#include "base/codec/cursor_encoder.h"
#include "base/codec/scale_reducer.h"
#include "base/codec/video_encoder_vpx.h"
#include "base/codec/video_encoder_zstd.h"

namespace host {

//--------------------------------------------------------------------------------------------------
VideoEncodeWorker::VideoEncodeWorker()
{
    LOG(INFO) << "Ctor";
}

//--------------------------------------------------------------------------------------------------
VideoEncodeWorker::~VideoEncodeWorker()
{
    LOG(INFO) << "Dtor";
}

//--------------------------------------------------------------------------------------------------
void VideoEncodeWorker::configure(const proto::desktop::Config& config)
{
    switch (config.video_encoding())
    {
        case proto::desktop::VIDEO_ENCODING_VP8:
            video_encoder_ = base::VideoEncoderVPX::createVP8();
            break;

        case proto::desktop::VIDEO_ENCODING_VP9:
            video_encoder_ = base::VideoEncoderVPX::createVP9();
            break;

        case proto::desktop::VIDEO_ENCODING_ZSTD:
            video_encoder_ = base::VideoEncoderZstd::create(
                base::PixelFormat::fromProto(config.pixel_format()),
                static_cast<int>(config.compress_ratio()));
            break;

        default:
            LOG(ERROR) << "Unsupported video encoding:" << config.video_encoding();
            video_encoder_.reset();
            break;
    }

    if (!video_encoder_)
        LOG(ERROR) << "Video encoder not initialized!";

    cursor_encoder_.reset();
    if (config.flags() & proto::desktop::ENABLE_CURSOR_SHAPE)
    {
        LOG(INFO) << "Cursor shape enabled. Init cursor encoder";
        cursor_encoder_ = std::make_unique<base::CursorEncoder>();
    }

    scale_reducer_ = std::make_unique<base::ScaleReducer>();
}

//--------------------------------------------------------------------------------------------------
void VideoEncodeWorker::requestKeyFrame()
{
    if (video_encoder_)
        video_encoder_->setKeyFrameRequired(true);
}

//--------------------------------------------------------------------------------------------------
void VideoEncodeWorker::encode(std::shared_ptr<base::Frame> frame,
                               std::shared_ptr<base::MouseCursor> cursor,
                               int target_width,
                               int target_height)
{
    proto::desktop::HostToClient& message = outgoing_message_.newMessage();

    if (frame && video_encoder_ && scale_reducer_)
    {
        base::Size target_size(target_width, target_height);
        if (target_size.isEmpty())
            target_size = frame->size();

        const base::Frame* scaled_frame = scale_reducer_->scaleFrame(frame.get(), target_size);
        if (!scaled_frame)
        {
            LOG(ERROR) << "No scaled frame";
        }
        else
        {
            proto::desktop::VideoPacket* packet = message.mutable_video_packet();

            // Encode the frame into a video packet.
            if (!video_encoder_->encode(scaled_frame, packet))
            {
                LOG(ERROR) << "Unable to encode video packet";
                message.clear_video_packet();
            }
            else if (packet->has_format())
            {
                proto::desktop::VideoPacketFormat* format = packet->mutable_format();

                // In video packets that contain the format, we pass the screen capture type.
                format->set_capturer_type(frame->capturerType());

                // Real screen size.
                proto::desktop::Size* screen_size = format->mutable_screen_size();
                screen_size->set_width(frame->size().width());
                screen_size->set_height(frame->size().height());
            }
        }
    }

    if (cursor && cursor_encoder_)
    {
        if (!cursor_encoder_->encode(*cursor, message.mutable_cursor_shape()))
            message.clear_cursor_shape();
    }

    const bool has_video_packet = message.has_video_packet();

    if (!has_video_packet && !message.has_cursor_shape())
    {
        // Nothing to send, but the owner must still learn that we are free again, otherwise its
        // in-flight guard would never be cleared and no further frames would ever be encoded.
        emit sig_encoded(QByteArray(), false);
        return;
    }

    QByteArray serialized = outgoing_message_.serialize();

    if (has_video_packet)
    {
        // Return the encode buffer for reuse on the next frame (avoids reallocation).
        video_encoder_->setEncodeBuffer(
            std::move(*message.mutable_video_packet()->mutable_data()));
    }

    emit sig_encoded(serialized, has_video_packet);
}

} // namespace host
