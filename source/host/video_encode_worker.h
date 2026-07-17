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

#ifndef HOST_VIDEO_ENCODE_WORKER_H
#define HOST_VIDEO_ENCODE_WORKER_H

#include <QObject>

#include <memory>

#include "base/serialization.h"
#include "proto/desktop.h"

namespace base {
class CursorEncoder;
class Frame;
class MouseCursor;
class ScaleReducer;
class VideoEncoder;
} // namespace base

namespace host {

// Performs the CPU-heavy screen/cursor encoding for a single client on its own thread, so that it
// no longer competes with network I/O, IPC and input injection on the host service main thread.
//
// Threading contract:
//  - The object is created on the main thread and then moved to a dedicated QThread.
//  - configure()/requestKeyFrame()/encode() must be invoked on the worker thread. Callers on the
//    main thread must marshal the call, e.g. via QMetaObject::invokeMethod(worker, ..., QueuedConnection).
//  - The frame passed to encode() must be a private copy (not the shared-memory capture buffer),
//    because the capture buffer may be reused by the agent as soon as the capture is acknowledged.
//  - sig_encoded() is emitted from the worker thread and delivered to the owner via a queued
//    connection (QByteArray/bool are registered meta types, so no extra registration is required).
class VideoEncodeWorker final : public QObject
{
    Q_OBJECT

public:
    VideoEncodeWorker();
    ~VideoEncodeWorker() final;

    // (Re)creates the video/cursor encoders from |config|. Runs on the worker thread.
    void configure(const proto::desktop::Config& config);

    // Requests that the next encoded frame be a key frame. Runs on the worker thread.
    void requestKeyFrame();

    // Encodes |frame| (may be null for a cursor-only update) scaled to |target_width|x|target_height|
    // together with |cursor| (may be null). Emits sig_encoded() with the serialized HostToClient
    // message. Runs on the worker thread.
    void encode(std::shared_ptr<base::Frame> frame,
                std::shared_ptr<base::MouseCursor> cursor,
                int target_width,
                int target_height);

signals:
    // |serialized| is a ready-to-send HostToClient message. |has_video_packet| is true when the
    // message carries a video packet (used by the owner for statistics).
    void sig_encoded(const QByteArray& serialized, bool has_video_packet);

private:
    std::unique_ptr<base::ScaleReducer> scale_reducer_;
    std::unique_ptr<base::VideoEncoder> video_encoder_;
    std::unique_ptr<base::CursorEncoder> cursor_encoder_;

    base::Serializer<proto::desktop::HostToClient> outgoing_message_;

    Q_DISABLE_COPY(VideoEncodeWorker)
};

} // namespace host

#endif // HOST_VIDEO_ENCODE_WORKER_H
