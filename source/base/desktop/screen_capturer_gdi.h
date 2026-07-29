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

#ifndef BASE_DESKTOP_SCREEN_CAPTURER_GDI_H
#define BASE_DESKTOP_SCREEN_CAPTURER_GDI_H

#include "base/desktop/screen_capturer_win.h"
#include "base/win/scoped_hdc.h"

#include <chrono>

namespace base {

class Differ;
class SharedMemoryFactory;

class ScreenCapturerGdi final : public ScreenCapturerWin
{
    Q_OBJECT

public:
    explicit ScreenCapturerGdi(QObject* parent = nullptr);
    ~ScreenCapturerGdi() final;

    // ScreenCapturer implementation.
    int screenCount() final;
    bool screenList(ScreenList* screens) final;
    bool selectScreen(ScreenId screen_id) final;
    ScreenId currentScreen() const final;
    const Frame* captureFrame(Error* error) final;
    const MouseCursor* captureCursor() final;
    Point cursorPosition() final;

protected:
    // ScreenCapturer implementation.
    void reset() final;

private:
    bool prepareCaptureResources();
    void reportCaptureDiagnostics();

    typedef HRESULT(STDAPICALLTYPE* DwmEnableCompositionFunc) (UINT uCompositionAction);
    typedef HRESULT(STDAPICALLTYPE* DwmIsCompositionEnabledFunc) (BOOL* pfEnabled);

    HMODULE dwmapi_dll_ = nullptr;
    DwmEnableCompositionFunc dwm_enable_composition_func_ = nullptr;
    DwmIsCompositionEnabledFunc dwm_is_composition_enabled_func_ = nullptr;

    bool composition_changed_ = false;

    ScreenId current_screen_id_ = kFullDesktopScreenId;
    std::wstring current_device_key_;

    Rect desktop_dc_rect_;
    Rect screen_rect_;

    std::unique_ptr<Differ> differ_;
    ScopedGetDC desktop_dc_;
    ScopedCreateDC memory_dc_;

    FrameQueue<Frame> queue_;

    std::unique_ptr<MouseCursor> mouse_cursor_;
    CURSORINFO curr_cursor_info_;
    CURSORINFO prev_cursor_info_;

    // GetCursorInfo fails persistently on the secure desktop; captureCursor runs per frame, so
    // only the start of a failure streak and the recovery are logged.
    bool cursor_info_failure_reported_ = false;

    // Lock-screen diagnostics. After locking a Windows 7 host the picture goes black while the
    // capture loop keeps reporting success, so nothing shows up in the log - a black screen and a
    // static one are both "no error, no update". These record what the capture actually produced:
    // the blank state is reported on every change, the rest as a periodic summary.
    bool frame_is_blank_ = false;
    bool blank_state_known_ = false;
    int captured_frames_ = 0;
    int blank_frames_ = 0;
    int unchanged_frames_ = 0;
    int bitblt_failures_ = 0;
    int screen_rect_failures_ = 0;
    std::chrono::steady_clock::time_point last_diagnostics_report_;

    Q_DISABLE_COPY(ScreenCapturerGdi)
};

} // namespace base

#endif // BASE_DESKTOP_SCREEN_CAPTURER_GDI_H
