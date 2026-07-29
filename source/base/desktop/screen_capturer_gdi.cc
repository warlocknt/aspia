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

#include "base/desktop/screen_capturer_gdi.h"

#include "base/logging.h"
#include "base/desktop/mouse_cursor.h"
#include "base/desktop/win/cursor.h"
#include "base/desktop/win/screen_capture_utils.h"
#include "base/desktop/frame_dib.h"
#include "base/desktop/differ.h"
#include "base/win/scoped_select_object.h"

#include <algorithm>

#include <dwmapi.h>

namespace base {

namespace {

//--------------------------------------------------------------------------------------------------
bool isSameCursorShape(const CURSORINFO& left, const CURSORINFO& right)
{
    // If the cursors are not showing, we do not care the hCursor handle.
    return left.flags == right.flags && (left.flags != CURSOR_SHOWING ||
                                         left.hCursor == right.hCursor);
}

//--------------------------------------------------------------------------------------------------
// Tells an all-black capture from a real (possibly static) screen. BitBlt reports success either
// way, so this is the only way to see in the log which of the two the operator is looking at.
// Walking a full frame would be ~8 MB per capture, so probe a sparse grid instead - a screen with
// any content at all lights up several probes.
bool isFrameBlank(const Frame* frame)
{
    static const int kProbesPerAxis = 64;

    const quint8* data = frame->frameData();
    if (!data)
        return false;

    const int width = frame->size().width();
    const int height = frame->size().height();

    if (width <= 0 || height <= 0)
        return false;

    const int step_x = std::max(1, width / kProbesPerAxis);
    const int step_y = std::max(1, height / kProbesPerAxis);

    for (int y = 0; y < height; y += step_y)
    {
        const quint8* row = data + (static_cast<ptrdiff_t>(y) * frame->stride());

        for (int x = 0; x < width; x += step_x)
        {
            const quint8* pixel = row + (static_cast<ptrdiff_t>(x) * 4);

            // BitBlt does not set the alpha channel, so only the color components are meaningful.
            if (pixel[0] || pixel[1] || pixel[2])
                return false;
        }
    }

    return true;
}

} // namespace

//--------------------------------------------------------------------------------------------------
ScreenCapturerGdi::ScreenCapturerGdi(QObject* parent)
    : ScreenCapturerWin(Type::WIN_GDI, parent)
{
    LOG(INFO) << "Ctor";

    memset(&curr_cursor_info_, 0, sizeof(curr_cursor_info_));
    memset(&prev_cursor_info_, 0, sizeof(prev_cursor_info_));

    dwmapi_dll_ = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dwmapi_dll_)
    {
        dwm_enable_composition_func_ = reinterpret_cast<DwmEnableCompositionFunc>(
            GetProcAddress(dwmapi_dll_, "DwmEnableComposition"));
        if (!dwm_enable_composition_func_)
        {
            PLOG(ERROR) << "Unable to load DwmEnableComposition function";
        }

        dwm_is_composition_enabled_func_ = reinterpret_cast<DwmIsCompositionEnabledFunc>(
            GetProcAddress(dwmapi_dll_, "DwmIsCompositionEnabled"));
        if (!dwm_is_composition_enabled_func_)
        {
            PLOG(ERROR) << "Unable to load DwmIsCompositionEnabled function";
        }
    }
    else
    {
        PLOG(ERROR) << "Unable to load dwmapi.dll";
    }
}

//--------------------------------------------------------------------------------------------------
ScreenCapturerGdi::~ScreenCapturerGdi()
{
    LOG(INFO) << "Dtor";

    if (composition_changed_ && dwm_enable_composition_func_)
        dwm_enable_composition_func_(DWM_EC_ENABLECOMPOSITION);

    if (dwmapi_dll_)
        FreeLibrary(dwmapi_dll_);
}

//--------------------------------------------------------------------------------------------------
int ScreenCapturerGdi::screenCount()
{
    return ScreenCaptureUtils::screenCount();
}

//--------------------------------------------------------------------------------------------------
bool ScreenCapturerGdi::screenList(ScreenList* screens)
{
    return ScreenCaptureUtils::screenList(screens);
}

//--------------------------------------------------------------------------------------------------
bool ScreenCapturerGdi::selectScreen(ScreenId screen_id)
{
    LOG(INFO) << "Select screen with ID:" << screen_id;

    if (!ScreenCaptureUtils::isScreenValid(screen_id, &current_device_key_))
    {
        LOG(ERROR) << "Invalid screen";
        return false;
    }

    // At next screen capture, the resources are recreated.
    desktop_dc_rect_ = Rect();

    current_screen_id_ = screen_id;
    return true;
}

//--------------------------------------------------------------------------------------------------
ScreenCapturer::ScreenId ScreenCapturerGdi::currentScreen() const
{
    return current_screen_id_;
}

//--------------------------------------------------------------------------------------------------
const Frame* ScreenCapturerGdi::captureFrame(Error* error)
{
    DCHECK(error);

    queue_.moveToNextFrame();
    *error = Error::TEMPORARY;

    // Reported here rather than at the end of the method so that the summary keeps coming out even
    // while every capture is failing - the silence would otherwise look the same as a stopped loop.
    reportCaptureDiagnostics();

    if (!prepareCaptureResources())
        return nullptr;

    screen_rect_ = ScreenCaptureUtils::screenRect(current_screen_id_, current_device_key_);
    if (screen_rect_.isEmpty())
    {
        // TEMPORARY, not PERMANENT. On a desktop/session switch (lock, the Winlogon/login secure
        // desktop) EnumDisplayDevices can briefly return failure with no error set - a transient the
        // display stack clears within about a second. Treating it as PERMANENT tore the capturer down
        // and it never recovered until the client reconnected (observed as a frozen picture after
        // locking a Windows 7 host). As TEMPORARY the capture loop simply retries the next frame and
        // resumes on its own once the enumeration succeeds again.
        // Throttled: the capture loop runs at the session FPS, so an unbroken failure streak would
        // otherwise write this line dozens of times per second.
        if (screen_rect_failures_ == 0)
        {
            LOG(ERROR) << "Failed to get screen rect (will retry, repeats are counted)";
        }

        ++screen_rect_failures_;

        *error = Error::TEMPORARY;
        return nullptr;
    }

    if (!queue_.currentFrame() || queue_.currentFrame()->size() != screen_rect_.size())
    {
        DCHECK(desktop_dc_);
        DCHECK(memory_dc_);

        std::unique_ptr<Frame> frame = FrameDib::create(
            screen_rect_.size(), PixelFormat::ARGB(), sharedMemoryFactory(), memory_dc_);
        if (!frame)
        {
            LOG(ERROR) << "Failed to create frame buffer";
            return nullptr;
        }

        frame->setCapturerType(static_cast<quint32>(type()));
        queue_.replaceCurrentFrame(std::move(frame));
    }

    Frame* current = queue_.currentFrame();
    Frame* previous = queue_.previousFrame();

    {
        ScopedSelectObject select_object(
            memory_dc_, static_cast<FrameDib*>(current)->bitmap());

        if (!BitBlt(memory_dc_,
                    0, 0,
                    screen_rect_.width(), screen_rect_.height(),
                    desktop_dc_,
                    screen_rect_.left(), screen_rect_.top(),
                    CAPTUREBLT | SRCCOPY))
        {
            static thread_local int count = 0;

            if (count == 0)
            {
                LOG(ERROR) << "BitBlt failed";
            }

            if (++count > 10)
                count = 0;

            ++bitblt_failures_;
            return nullptr;
        }
    }

    current->setTopLeft(screen_rect_.topLeft().subtract(desktop_dc_rect_.topLeft()));

    if (!previous || previous->size() != current->size())
    {
        differ_ = std::make_unique<Differ>(screen_rect_.size());
        current->updatedRegion()->addRect(Rect::makeSize(screen_rect_.size()));
    }
    else
    {
        differ_->calcDirtyRegion(previous->frameData(),
                                 current->frameData(),
                                 current->updatedRegion());
    }

    ++captured_frames_;

    if (current->updatedRegion()->isEmpty())
        ++unchanged_frames_;

    // The signature we are hunting: BitBlt succeeds, the frame is entirely black and nothing ever
    // changes in it, so no packet is sent and the operator stares at a frozen black screen while
    // the log claims everything is fine.
    const bool is_blank = isFrameBlank(current);
    if (is_blank)
        ++blank_frames_;

    if (!blank_state_known_ || is_blank != frame_is_blank_)
    {
        if (is_blank)
        {
            LOG(WARNING) << "Captured frame is entirely black (screen type:" << lastScreenType()
                         << "screen rect:" << screen_rect_ << ")";
        }
        else
        {
            LOG(INFO) << "Captured frame has visible content (screen type:" << lastScreenType()
                      << "screen rect:" << screen_rect_ << ")";
        }

        frame_is_blank_ = is_blank;
        blank_state_known_ = true;
    }

    *error = Error::SUCCEEDED;
    return current;
}

//--------------------------------------------------------------------------------------------------
const MouseCursor* ScreenCapturerGdi::captureCursor()
{
    if (!desktop_dc_.isValid())
        return nullptr;

    memset(&curr_cursor_info_, 0, sizeof(curr_cursor_info_));

    // Note: cursor_info.hCursor does not need to be freed.
    curr_cursor_info_.cbSize = sizeof(curr_cursor_info_);
    if (GetCursorInfo(&curr_cursor_info_))
    {
        if (cursor_info_failure_reported_)
        {
            LOG(INFO) << "GetCursorInfo recovered";
            cursor_info_failure_reported_ = false;
        }

        if (!isSameCursorShape(curr_cursor_info_, prev_cursor_info_))
        {
            if (curr_cursor_info_.flags == 0)
            {
                LOG(INFO) << "No hardware cursor attached. Using default mouse cursor";

                // Host machine does not have a hardware mouse attached, we will send a default one
                // instead. Note, Windows automatically caches cursor resource, so we do not need
                // to cache the result of LoadCursor.
                curr_cursor_info_.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                if (!curr_cursor_info_.hCursor)
                {
                    PLOG(ERROR) << "LoadCursorW failed";
                    return nullptr;
                }
            }

            mouse_cursor_.reset(mouseCursorFromHCursor(desktop_dc_, curr_cursor_info_.hCursor));
            if (mouse_cursor_)
            {
                prev_cursor_info_ = curr_cursor_info_;

                int dpi_x = GetDeviceCaps(desktop_dc_, LOGPIXELSX);
                int dpi_y = GetDeviceCaps(desktop_dc_, LOGPIXELSY);

                mouse_cursor_->dpi() = Point(dpi_x, dpi_y);
                return mouse_cursor_.get();
            }
        }
    }
    else
    {
        // Fails persistently on the secure desktop (access denied) and runs per captured frame,
        // so report the start of the failure and the recovery instead of every occurrence.
        if (!cursor_info_failure_reported_)
        {
            PLOG(ERROR) << "GetCursorInfo failed (repeats are not logged until it recovers)";
            cursor_info_failure_reported_ = true;
        }
    }

    return nullptr;
}

//--------------------------------------------------------------------------------------------------
Point ScreenCapturerGdi::cursorPosition()
{
    Point cursor_pos(curr_cursor_info_.ptScreenPos.x, curr_cursor_info_.ptScreenPos.y);

    if (current_screen_id_ == kFullDesktopScreenId)
        cursor_pos = cursor_pos.subtract(desktop_dc_rect_.topLeft());
    else
        cursor_pos = cursor_pos.subtract(screen_rect_.topLeft());

    return cursor_pos;
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerGdi::reset()
{
    // Release GDI resources otherwise SetThreadDesktop will fail.
    desktop_dc_.close();
    memory_dc_.reset();

    // A reset means the capture is about to come from somewhere else (a desktop switch), so the
    // blank state is reported again for the new source instead of being carried over.
    blank_state_known_ = false;
}

//--------------------------------------------------------------------------------------------------
bool ScreenCapturerGdi::prepareCaptureResources()
{
    Rect desktop_rect = ScreenCaptureUtils::fullScreenRect();

    // If the display bounds have changed then recreate GDI resources.
    if (desktop_rect != desktop_dc_rect_)
    {
        LOG(INFO) << "Desktop rect changed from" << desktop_dc_rect_ << "to" << desktop_rect;

        desktop_dc_.close();
        memory_dc_.reset();

        desktop_dc_rect_ = Rect();
    }

    if (!desktop_dc_)
    {
        DCHECK(!memory_dc_);

        LOG(INFO) << "Creating GDI capture resources (desktop rect:" << desktop_rect << ")";

        if (dwm_enable_composition_func_ && dwm_is_composition_enabled_func_)
        {
            BOOL enabled;
            HRESULT hr = dwm_is_composition_enabled_func_(&enabled);

            LOG(INFO) << "DWM composition enabled:" << (SUCCEEDED(hr) ? (enabled ? "yes" : "no")
                                                                      : "unknown");

            if (SUCCEEDED(hr) && enabled)
            {
                // Vote to disable Aero composited desktop effects while capturing.
                // Windows will restore Aero automatically if the process exits.
                // This has no effect under Windows 8 or higher.
                dwm_enable_composition_func_(DWM_EC_DISABLECOMPOSITION);
                composition_changed_ = true;
            }
        }

        // Create GDI device contexts to capture from the desktop into memory.
        desktop_dc_.getDC(nullptr);
        memory_dc_.reset(CreateCompatibleDC(desktop_dc_));
        if (!memory_dc_)
        {
            LOG(ERROR) << "CreateCompatibleDC failed";
            return false;
        }

        desktop_dc_rect_ = desktop_rect;

        // Make sure the frame buffers will be reallocated.
        queue_.reset();
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerGdi::reportCaptureDiagnostics()
{
    static const std::chrono::seconds kReportInterval(30);

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    if (last_diagnostics_report_ == std::chrono::steady_clock::time_point())
    {
        last_diagnostics_report_ = now;
        return;
    }

    if (now - last_diagnostics_report_ < kReportInterval)
        return;

    // Everything is per interval, not cumulative: what matters is whether the loop is still
    // producing frames right now, and what those frames look like.
    LOG(INFO) << "Capture diagnostics (30s). Frames:" << captured_frames_
              << "blank:" << blank_frames_
              << "unchanged:" << unchanged_frames_
              << "bitblt failures:" << bitblt_failures_
              << "screen rect failures:" << screen_rect_failures_
              << "screen type:" << lastScreenType();

    captured_frames_ = 0;
    blank_frames_ = 0;
    unchanged_frames_ = 0;
    bitblt_failures_ = 0;
    screen_rect_failures_ = 0;
    last_diagnostics_report_ = now;
}

} // namespace base
