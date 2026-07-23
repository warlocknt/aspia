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

#include "base/desktop/screen_capturer_wrapper.h"

#include "base/logging.h"
#include "base/desktop/desktop_environment.h"
#include "base/desktop/desktop_resizer.h"
#include "base/desktop/mouse_cursor.h"
#include "base/desktop/power_save_blocker.h"
#include "base/ipc/shared_memory_factory.h"

#if defined(Q_OS_WINDOWS)
#include "base/desktop/screen_capturer_win.h"
#include "base/win/windows_version.h"
#elif defined(Q_OS_LINUX)
#include "base/desktop/screen_capturer_x11.h"
#elif defined(Q_OS_MACOS)
// TODO
#else
#error Platform support not implemented
#endif

namespace base {

//--------------------------------------------------------------------------------------------------
ScreenCapturerWrapper::ScreenCapturerWrapper(ScreenCapturer::Type preferred_type, QObject* parent)
    : QObject(parent),
      preferred_type_(preferred_type),
      power_save_blocker_(std::make_unique<PowerSaveBlocker>()),
      environment_(DesktopEnvironment::create(this))
{
    LOG(INFO) << "Ctor";
    selectCapturer(ScreenCapturer::Error::SUCCEEDED);
}

//--------------------------------------------------------------------------------------------------
ScreenCapturerWrapper::~ScreenCapturerWrapper()
{
    LOG(INFO) << "Dtor";
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerWrapper::selectScreen(ScreenCapturer::ScreenId screen_id, const Size& resolution)
{
    if (!screen_capturer_)
    {
        LOG(ERROR) << "Screen capturer not initialized";
        return;
    }

    if (screen_id == screen_capturer_->currentScreen())
    {
        if (resolution.isEmpty())
        {
            LOG(ERROR) << "Empty resolution";
        }
        else
        {
            if (!resizer_)
            {
                LOG(ERROR) << "No desktop resizer";
            }
            else
            {
                LOG(INFO) << "Change resolution for screen" << screen_id << "to:" << resolution;
                if (!resizer_->setResolution(screen_id, resolution))
                {
                    LOG(ERROR) << "setResolution failed";
                    return;
                }
            }
        }
    }
    else
    {
        LOG(INFO) << "Try to select screen:" << screen_id;

        if (!screen_capturer_->selectScreen(screen_id))
        {
            LOG(ERROR) << "ScreenCapturer::selectScreen failed";
        }
        else
        {
            LOG(INFO) << "Screen" << screen_id << "selected";
            last_screen_id_ = screen_id;
        }
    }

    ScreenCapturer::ScreenList screen_list;
    if (screen_capturer_->screenList(&screen_list))
    {
        LOG(INFO) << "Received an updated list of screens";

        if (resizer_)
        {
            screen_list.resolutions = resizer_->supportedResolutions(screen_id);
            if (screen_list.resolutions.empty())
            {
                LOG(INFO) << "No supported resolutions";
            }

            for (const auto& resolition : std::as_const(screen_list.resolutions))
            {
                LOG(INFO) << "Supported resolution:" << resolition;
            }
        }
        else
        {
            LOG(INFO) << "No desktop resizer";
        }

        for (const auto& screen : std::as_const(screen_list.screens))
        {
            LOG(INFO) << "Screen #" << screen.id << "(position:" << screen.position
                      << "resolution:" << screen.resolution << "DPI:" << screen.dpi << ")";
        }

        emit sig_screenListChanged(screen_list, screen_id);
    }
    else
    {
        LOG(ERROR) << "ScreenCapturer::screenList failed";
    }
}

//--------------------------------------------------------------------------------------------------
ScreenCapturer::Error ScreenCapturerWrapper::captureFrame(
    const Frame** frame, const MouseCursor** mouse_cursor)
{
    if (!screen_capturer_)
    {
        LOG(ERROR) << "Screen capturer NOT initialized";
        return ScreenCapturer::Error::TEMPORARY;
    }

#if defined(Q_OS_WINDOWS)
    if (dxgi_fallback_)
    {
        // The capturer is chosen once, so a session that fell back to GDI would otherwise stay
        // slow until the client reconnects - even though DXGI typically becomes available again
        // seconds later (once the machine finishes waking up or the login desktop appears).
        //
        // The wake case is the common one and it recovers fast: a laptop host resumes, the display
        // and D3D device come back within a second or two, and we want the operator back on DXGI the
        // moment that happens - not up to 5 s later. So for a short window right after the fallback
        // starts, retry every second. If DXGI still has not come back after that (a host where GDI is
        // the lasting state - no D3D at all, or a genuinely long resume), back off to 5 s so we are
        // not rebuilding the DXGI stack every second forever. Each attempt resets last_capturer_retry_.
        const auto now = std::chrono::steady_clock::now();
        const auto kFastRetryWindow = std::chrono::seconds(15);
        const auto retry_interval = (now - fallback_since_ < kFastRetryWindow)
            ? std::chrono::seconds(1) : std::chrono::seconds(5);

        if (now - last_capturer_retry_ >= retry_interval)
        {
            LOG(INFO) << "Session is on the GDI fallback, retrying the preferred capturer";
            selectCapturer(ScreenCapturer::Error::SUCCEEDED);
        }
    }
#endif // defined(Q_OS_WINDOWS)

    screen_capturer_->switchToInputDesktop();

    int count = screen_capturer_->screenCount();
    if (screen_count_ != count)
    {
        LOG(INFO) << "Screen count changed:" << count << "(old:" << screen_count_ << ")";

        resizer_.reset();
        resizer_ = DesktopResizer::create();

        screen_count_ = count;
        selectScreen(defaultScreen(), Size());
    }

    ScreenCapturer::Error error;
    *frame = screen_capturer_->captureFrame(&error);
    if (!*frame)
    {
        switch (error)
        {
            case ScreenCapturer::Error::TEMPORARY:
                return error;

            case ScreenCapturer::Error::PERMANENT:
                selectCapturer(ScreenCapturer::Error::PERMANENT);
                return error;

            default:
                NOTREACHED();
                break;
        }
    }

    *mouse_cursor = screen_capturer_->captureCursor();

    if (enable_cursor_position_)
    {
        Point cursor_pos = screen_capturer_->cursorPosition();

        qint32 delta_x = std::abs(cursor_pos.x() - last_cursor_pos_.x());
        qint32 delta_y = std::abs(cursor_pos.y() - last_cursor_pos_.y());

        if (delta_x > 1 || delta_y > 1)
        {
            emit sig_cursorPositionChanged(cursor_pos);
            last_cursor_pos_ = cursor_pos;
        }
    }

    return ScreenCapturer::Error::SUCCEEDED;
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerWrapper::setSharedMemoryFactory(SharedMemoryFactory* shared_memory_factory)
{
    shared_memory_factory_ = shared_memory_factory;

    if (screen_capturer_)
        screen_capturer_->setSharedMemoryFactory(shared_memory_factory);
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerWrapper::enableWallpaper(bool enable)
{
    if (!environment_)
    {
        LOG(ERROR) << "Desktop environment not initialized";
        return;
    }

    environment_->setWallpaper(enable);
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerWrapper::enableEffects(bool enable)
{
    if (!environment_)
    {
        LOG(ERROR) << "Desktop environment not initialized";
        return;
    }

    environment_->setEffects(enable);
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerWrapper::enableFontSmoothing(bool enable)
{
    if (!environment_)
    {
        LOG(ERROR) << "Desktop environment not initialized";
        return;
    }

    environment_->setFontSmoothing(enable);
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerWrapper::enableCursorPosition(bool enable)
{
    enable_cursor_position_ = enable;
}

//--------------------------------------------------------------------------------------------------
ScreenCapturer::ScreenId ScreenCapturerWrapper::defaultScreen()
{
    if (!screen_capturer_)
    {
        LOG(ERROR) << "Screen capturer not initialized";
        return ScreenCapturer::kInvalidScreenId;
    }

    ScreenCapturer::ScreenList screen_list;
    if (screen_capturer_->screenList(&screen_list))
    {
        for (const auto& screen : std::as_const(screen_list.screens))
        {
            if (screen.is_primary)
            {
                LOG(INFO) << "Primary screen found:" << screen.id;
                return screen.id;
            }
        }
    }
    else
    {
        LOG(ERROR) << "ScreenCapturer::screenList failed";
    }

    LOG(INFO) << "Primary screen NOT found";
    return ScreenCapturer::kFullDesktopScreenId;
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerWrapper::selectCapturer(ScreenCapturer::Error last_error)
{
    LOG(INFO) << "Selecting screen capturer. Preferred capturer:" << preferred_type_;

    delete screen_capturer_;

#if defined(Q_OS_WINDOWS)
    screen_capturer_ = ScreenCapturerWin::create(preferred_type_, last_error, this);
#elif defined(Q_OS_LINUX)
    screen_capturer_ = ScreenCapturerX11::create();
    if (!screen_capturer_)
    {
        LOG(LS_ERROR) << "Unable to create X11 screen capturer";
        return;
    }
#elif defined(Q_OS_MACOS)
    NOTIMPLEMENTED();
#else
    NOTIMPLEMENTED();
#endif

    if (!screen_capturer_)
    {
        LOG(ERROR) << "Unable to create screen capturer";
        return;
    }

    LOG(INFO) << "Selected screen capturer:" << screen_capturer_->type();

#if defined(Q_OS_WINDOWS)
    // Landing on GDI while DXGI was expected means the fallback fired. GDI works but captures with
    // a CPU copy, which the operator experiences as a jerky session. Remember that this is a
    // fallback, so captureFrame() can periodically try DXGI again and return the operator to the
    // fast capturer on its own.
    //
    // This deliberately includes the case where the DXGI capturer hit a PERMANENT error - which is
    // exactly what a sleeping laptop host produces: the D3D device is lost on resume and takes a
    // while to come back. That path used to be excluded to avoid "looping" between DXGI and GDI,
    // but the field cost of the exclusion was the operator stuck on GDI for minutes until some
    // unrelated event happened to re-select the capturer. The retry is at most one DXGI rebuild per
    // interval with GDI serving frames in between, and it stops the instant a DXGI frame succeeds,
    // so that "loop" is the recovery we want and it self-terminates. A session that deliberately
    // uses GDI is still never retried: its preferred_type_ is WIN_GDI, excluded just below.
    const bool was_fallback = dxgi_fallback_;
    dxgi_fallback_ =
        (preferred_type_ == ScreenCapturer::Type::DEFAULT ||
         preferred_type_ == ScreenCapturer::Type::WIN_DXGI) &&
        windowsVersion() >= VERSION_WIN8 &&
        screen_capturer_->type() == ScreenCapturer::Type::WIN_GDI;
    last_capturer_retry_ = std::chrono::steady_clock::now();

    // Mark the start of the fallback streak only on the transition into it, not on the repeated
    // retries while already fallen back - captureFrame() uses this to retry fast at first, then slow.
    if (dxgi_fallback_ && !was_fallback)
        fallback_since_ = last_capturer_retry_;

    if (dxgi_fallback_)
    {
        LOG(WARNING) << "GDI capturer is a fallback, DXGI was expected but is unavailable. "
                        "Will retry DXGI periodically";
    }
#endif // defined(Q_OS_WINDOWS)

    connect(screen_capturer_, &ScreenCapturer::sig_screenTypeChanged,
            this, &ScreenCapturerWrapper::sig_screenTypeChanged);

    connect(screen_capturer_, &ScreenCapturer::sig_desktopChanged, this, [this]()
    {
        if (!environment_)
        {
            LOG(ERROR) << "Desktop environment not initialized";
            return;
        }

        environment_->onDesktopChanged();
    });

    screen_capturer_->setSharedMemoryFactory(shared_memory_factory_);
    if (last_screen_id_ != ScreenCapturer::kInvalidScreenId)
    {
        LOG(INFO) << "Restore selected screen:" << last_screen_id_;
        selectScreen(last_screen_id_, Size());
    }
}

} // namespace base
