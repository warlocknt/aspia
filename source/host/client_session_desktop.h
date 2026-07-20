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

#ifndef HOST_CLIENT_SESSION_DESKTOP_H
#define HOST_CLIENT_SESSION_DESKTOP_H

#include <QPointer>
#include <QTimer>

#include "base/serialization.h"
#include "base/desktop/geometry.h"
#include "host/client_session.h"
#include "host/desktop_session.h"
#include "host/stat_counter.h"

#if defined(Q_OS_WINDOWS)
#include "host/task_manager.h"
#endif // defined(Q_OS_WINDOWS)

class QThread;

namespace base {
class AudioEncoder;
class Frame;
class MouseCursor;
} // namespace base

namespace host {

class VideoEncodeWorker;

class ClientSessionDesktop final : public ClientSession
{
    Q_OBJECT

public:
    ClientSessionDesktop(
        proto::peer::SessionType session_type, base::TcpChannel* channel, QObject* parent);
    ~ClientSessionDesktop() final;

    void encodeScreen(const base::Frame* frame, const base::MouseCursor* cursor);
    void encodeAudio(const proto::desktop::AudioPacket& audio_packet);
    void setVideoErrorCode(proto::desktop::VideoErrorCode error_code);
    void setCursorPosition(const proto::desktop::CursorPosition& cursor_position);
    void setScreenList(const proto::desktop::ScreenList& list);
    void setScreenType(const proto::desktop::ScreenType& type);
    void injectClipboardEvent(const proto::desktop::ClipboardEvent& event);

    const DesktopSession::Config& desktopSessionConfig() const { return desktop_session_config_; }

signals:
    void sig_control(proto::internal::DesktopControl::Action action);
    void sig_selectScreen(const proto::desktop::Screen& screen);
    void sig_captureScreen();
    void sig_captureFpsChanged(int fps);
    void sig_injectKeyEvent(const proto::desktop::KeyEvent& event);
    void sig_injectTextEvent(const proto::desktop::TextEvent& event);
    void sig_injectMouseEvent(const proto::desktop::MouseEvent& event);
    void sig_injectTouchEvent(const proto::desktop::TouchEvent& event);
    void sig_injectClipboardEvent(const proto::desktop::ClipboardEvent& event);

protected:
    // ClientSession implementation.
    void onStarted() final;
    void onReceived(const QByteArray& buffer) final;

private slots:
#if defined(Q_OS_WINDOWS)
    void onTaskManagerMessage(const proto::task_manager::HostToClient& message);
#endif // defined(Q_OS_WINDOWS)

private:
    void onEncoded(const QByteArray& serialized, bool has_video_packet);
    void requestKeyFrame();
    void readExtension(const proto::desktop::Extension& extension);
    void readConfig(const proto::desktop::Config& config);
    void readSelectScreenExtension(const std::string& data);
    void readPreferredSizeExtension(const std::string& data);
    void readVideoPauseExtension(const std::string& data);
    void readAudioPauseExtension(const std::string& data);
    void readPowerControlExtension(const std::string& data);
    void readRemoteUpdateExtension(const std::string& data);
    void readSystemInfoExtension(const std::string& data);
    void readVideoRecordingExtension(const std::string& data);
    void readTaskManagerExtension(const std::string& data);
    void onOverflowDetectionTimer();
    void downStepOverflow();
    void upStepOverflow();

    std::unique_ptr<base::AudioEncoder> audio_encoder_;

    // Video/cursor encoding runs on |encode_worker_|, which lives on |encode_thread_|, so that the
    // CPU-heavy encode no longer serializes with network I/O, IPC and input on the main thread.
    QThread* encode_thread_ = nullptr;
    VideoEncodeWorker* encode_worker_ = nullptr;
    bool has_video_encoder_ = false;

    // True while the worker is encoding a frame. Capture is no longer gated by encoding, so without
    // this guard frames would pile up in the worker's queue (each holding a full frame copy) faster
    // than they can be encoded. Real-time semantics: drop frames instead of queueing them, the next
    // capture carries fresher content anyway.
    bool encode_in_flight_ = false;
    DesktopSession::Config desktop_session_config_;
    base::Size source_size_;
    base::Size preferred_size_;
    base::Size forced_size_;

    // Source->target scale factors (percent), kept on the main thread for translating input/cursor
    // coordinates. Mirror what the worker's ScaleReducer computes (target * 100 / source).
    double scale_x_ = 100.0;
    double scale_y_ = 100.0;

    bool is_video_paused_ = false;
    bool is_audio_paused_ = false;

    // Set when the client announces the clipboard_image extension. Without it clipboard images
    // are not forwarded to this client - it would drop them silently anyway.
    bool client_supports_clipboard_image_ = false;

    QPointer<QTimer> overflow_detection_timer_;
    size_t write_overflow_count_ = 0;
    size_t write_normal_count_ = 1;
    size_t last_pending_count_ = 0;
    bool critical_overflow_ = false;
    int max_fps_ = 0;

#if defined(Q_OS_WINDOWS)
    QPointer<TaskManager> task_manager_;
#endif // defined(Q_OS_WINDOWS)

    base::Parser<proto::desktop::ClientToHost> incoming_message_;
    base::Serializer<proto::desktop::HostToClient> outgoing_message_;

    StatCounter stat_counter_;

    Q_DISABLE_COPY(ClientSessionDesktop)
};

} // namespace host

#endif // HOST_CLIENT_SESSION_DESKTOP_H
