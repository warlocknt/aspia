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

#include "common/clipboard_win.h"

#include "base/logging.h"
#include "base/win/message_window.h"
#include "base/win/scoped_clipboard.h"
#include "base/win/scoped_hglobal.h"

#include <QStringList>

namespace common {

namespace {

//--------------------------------------------------------------------------------------------------
// GetClipboardFormatName() only knows about registered formats and fails for the predefined ones,
// so those are named here. Anything not listed is reported by number, which is still enough to
// recognize it later.
QString standardFormatName(UINT format)
{
    switch (format)
    {
        case CF_TEXT:          return QStringLiteral("CF_TEXT");
        case CF_BITMAP:        return QStringLiteral("CF_BITMAP");
        case CF_METAFILEPICT:  return QStringLiteral("CF_METAFILEPICT");
        case CF_SYLK:          return QStringLiteral("CF_SYLK");
        case CF_DIF:           return QStringLiteral("CF_DIF");
        case CF_TIFF:          return QStringLiteral("CF_TIFF");
        case CF_OEMTEXT:       return QStringLiteral("CF_OEMTEXT");
        case CF_DIB:           return QStringLiteral("CF_DIB");
        case CF_PALETTE:       return QStringLiteral("CF_PALETTE");
        case CF_RIFF:          return QStringLiteral("CF_RIFF");
        case CF_WAVE:          return QStringLiteral("CF_WAVE");
        case CF_UNICODETEXT:   return QStringLiteral("CF_UNICODETEXT");
        case CF_ENHMETAFILE:   return QStringLiteral("CF_ENHMETAFILE");
        case CF_HDROP:         return QStringLiteral("CF_HDROP");
        case CF_LOCALE:        return QStringLiteral("CF_LOCALE");
        case CF_DIBV5:         return QStringLiteral("CF_DIBV5");
        default:               return QStringLiteral("unknown(%1)").arg(format);
    }
}

//--------------------------------------------------------------------------------------------------
// Names of every format currently on the clipboard, as one comma-separated string. The clipboard
// must already be open: EnumClipboardFormats() requires it, unlike IsClipboardFormatAvailable().
//
// Only the names are collected, never the contents. Clipboards routinely carry passwords and other
// secrets, and logs get gathered up and shipped elsewhere for analysis, so nothing that passes
// through here may end up in one.
QString availableFormats()
{
    QStringList formats;

    UINT format = 0;
    while ((format = EnumClipboardFormats(format)) != 0)
    {
        wchar_t name[128] = { 0 };

        if (GetClipboardFormatNameW(format, name, static_cast<int>(std::size(name))) > 0)
            formats << QStringLiteral("%1(%2)").arg(QString::fromWCharArray(name)).arg(format);
        else
            formats << standardFormatName(format);
    }

    return formats.join(QStringLiteral(", "));
}

//--------------------------------------------------------------------------------------------------
// True when the clipboard holds a picture or a file list. Both are lost when only the text next to
// them is taken, so they are worth noticing even on the path that succeeds.
bool hasUnsupportedRichContent()
{
    return IsClipboardFormatAvailable(CF_DIB) ||
           IsClipboardFormatAvailable(CF_BITMAP) ||
           IsClipboardFormatAvailable(CF_HDROP);
}

//--------------------------------------------------------------------------------------------------
// Records what the clipboard holds whenever it changes to something this class cannot pass on, so
// that the formats worth implementing can be chosen from what actually turns up on working machines
// rather than from guesswork.
void logUnsupportedFormats(HWND owner)
{
    base::ScopedClipboard clipboard;
    if (!clipboard.init(owner))
        return;

    const QString formats = availableFormats();
    if (formats.isEmpty())
        return;

    LOG(INFO) << "Clipboard changed to content that is not supported. Available formats:"
              << formats;
}

} // namespace

//--------------------------------------------------------------------------------------------------
ClipboardWin::ClipboardWin(QObject* parent)
    : Clipboard(parent)
{
    LOG(INFO) << "Ctor";
}

//--------------------------------------------------------------------------------------------------
ClipboardWin::~ClipboardWin()
{
    LOG(INFO) << "Dtor";

    if (!window_)
    {
        LOG(ERROR) << "Window not created";
        return;
    }

    RemoveClipboardFormatListener(window_->hwnd());
    window_.reset();
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::init()
{
    if (window_)
    {
        LOG(ERROR) << "Window already created";
        return;
    }

    window_ = std::make_unique<base::MessageWindow>();

    if (!window_->create(std::bind(&ClipboardWin::onMessage,
                                   this,
                                   std::placeholders::_1, std::placeholders::_2,
                                   std::placeholders::_3, std::placeholders::_4)))
    {
        LOG(ERROR) << "Couldn't create clipboard window";
        return;
    }

    if (!AddClipboardFormatListener(window_->hwnd()))
    {
        PLOG(ERROR) << "AddClipboardFormatListener failed";
        return;
    }
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::setData(const QString& data)
{
    if (!window_)
    {
        LOG(ERROR) << "Window not created";
        return;
    }

    QString text = data;
    text.replace("\n", "\r\n");

    base::ScopedClipboard clipboard;
    if (!clipboard.init(window_->hwnd()))
    {
        PLOG(ERROR) << "Couldn't open the clipboard";
        return;
    }

    clipboard.empty();

    if (text.isEmpty())
        return;

    HGLOBAL text_global = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!text_global)
    {
        PLOG(ERROR) << "GlobalAlloc failed";
        return;
    }

    LPWSTR text_global_locked = reinterpret_cast<LPWSTR>(GlobalLock(text_global));
    if (!text_global_locked)
    {
        PLOG(ERROR) << "GlobalLock failed";
        GlobalFree(text_global);
        return;
    }

    memcpy(text_global_locked, text.utf16(), text.size() * sizeof(wchar_t));
    text_global_locked[text.size()] = 0;

    GlobalUnlock(text_global);

    clipboard.setData(CF_UNICODETEXT, text_global);
}

//--------------------------------------------------------------------------------------------------
bool ClipboardWin::onMessage(UINT message, WPARAM /* wParam */, LPARAM /* lParam */, LRESULT& result)
{
    switch (message)
    {
        case WM_CLIPBOARDUPDATE:
            onClipboardUpdate();
            break;

        default:
            return false;
    }

    result = 0;
    return true;
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::onClipboardUpdate()
{
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
    {
        logUnsupportedFormats(window_->hwnd());
        return;
    }

    QString data;

    // Add a scope, so that we keep the clipboard open for as short a time as possible.
    {
        base::ScopedClipboard clipboard;

        if (!clipboard.init(window_->hwnd()))
        {
            PLOG(ERROR) << "Couldn't open the clipboard";
            return;
        }

        HGLOBAL text_global = clipboard.data(CF_UNICODETEXT);
        if (!text_global)
        {
            PLOG(ERROR) << "Couldn't get data from the clipboard";
            return;
        }

        {
            base::ScopedHGLOBAL<wchar_t> text_lock(text_global);
            if (!text_lock.get())
            {
                PLOG(ERROR) << "Couldn't lock clipboard data";
                return;
            }

            data = QString::fromWCharArray(text_lock.get());
        }

        // The text was taken, but a picture or a file list was sitting next to it and is being
        // dropped. Logged separately from the case above: this is where content is silently
        // degraded rather than lost outright, and it is the more common of the two - applications
        // usually offer text alongside whatever else they put on the clipboard.
        //
        // Done while the clipboard is still open, since enumerating the formats requires it.
        if (hasUnsupportedRichContent())
        {
            LOG(INFO) << "Clipboard text taken, but unsupported rich content was present too. "
                         "Available formats:" << availableFormats();
        }
    }

    if (!data.isEmpty())
    {
        data.replace("\r\n", "\n");
        onData(data);
    }
}

} // namespace common
