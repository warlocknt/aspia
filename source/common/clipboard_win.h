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

#ifndef COMMON_CLIPBOARD_WIN_H
#define COMMON_CLIPBOARD_WIN_H

#include <qt_windows.h>

#include "common/clipboard.h"

namespace base {
class MessageWindow;
} // namespace base

namespace common {

class ClipboardWin final : public Clipboard
{
    Q_OBJECT

public:
    explicit ClipboardWin(QObject* parent = nullptr);
    ~ClipboardWin() final;

protected:
    // Clipboard implementation.
    void init() final;
    void setData(const QString& mime_type, const QByteArray& data) final;

private:
    void onClipboardUpdate();

    // |data| is UTF-8. Windows wants UTF-16 on the clipboard, so it is converted here rather than
    // by the caller, which has no reason to know that.
    void setDataText(const QByteArray& data);

    // |data| is a PNG. Windows has no notion of that on the clipboard, so it is converted to a
    // device independent bitmap, which every Windows application understands.
    void setDataImage(const QByteArray& data);

    void onClipboardText();
    void onClipboardImage();

    // Handles messages received by |window_|.
    bool onMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

    // Used to subscribe to WM_CLIPBOARDUPDATE messages.
    std::unique_ptr<base::MessageWindow> window_;

    Q_DISABLE_COPY(ClipboardWin)
};

} // namespace common

#endif // COMMON_CLIPBOARD_WIN_H
