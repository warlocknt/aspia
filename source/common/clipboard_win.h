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

#include <memory>

#include <QHash>
#include <QStringList>

#include "common/clipboard.h"

class QEventLoop;

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
    void setFileList(const proto::desktop::ClipboardFileList& file_list) final;
    void onRenderedFileList(const QStringList& paths) final;
    QString unsupportedFormatsSummary() const final;

private:
    void onClipboardUpdate();

    // |data| is UTF-8. Windows wants UTF-16 on the clipboard, so it is converted here rather than
    // by the caller, which has no reason to know that.
    void setDataText(const QByteArray& data);

    // |data| is a UTF-8 HTML fragment. Windows carries HTML under a registered format wrapped in
    // its own "CF_HTML" header; that header is built here, and a plain-text version is placed
    // alongside so applications that read only plain text still get something.
    void setDataHtml(const QByteArray& data);

    // |data| is a PNG. Windows has no notion of that on the clipboard, so it is converted to a
    // device independent bitmap, which every Windows application understands.
    void setDataImage(const QByteArray& data);

    void onClipboardText();
    bool onClipboardHtml();
    void onClipboardImage();

    // Reads the CF_HDROP file list, turns it into a ClipboardFileList (top-level entries only; the
    // download expands directories later) and hands it up through onFileList().
    void onClipboardFiles();

    // Renders CF_HDROP on demand: Windows sends WM_RENDERFORMAT the moment another application
    // pastes the files we advertised, and WM_RENDERALLFORMATS when it takes the promise over as this
    // owner goes away. The paths pasted come from |pending_file_list_|.
    void onRenderFileList();

    // Records, by format name, what turned up on the clipboard but could not be carried, so the
    // end-of-session summary can report it. Counts only, never content.
    void recordUnsupported();

    // Handles messages received by |window_|.
    bool onMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

    // Used to subscribe to WM_CLIPBOARDUPDATE messages.
    std::unique_ptr<base::MessageWindow> window_;

    // The registered clipboard format id for "HTML Format". Resolved once; zero if it could not be
    // registered, in which case HTML is simply not offered.
    UINT html_format_ = 0;

    // How often each unsupported format name was seen this session, for the teardown summary.
    QHash<QString, int> unsupported_seen_;

    // The listing a peer copied, held so its files can be produced when the user pastes here. Empty
    // when nothing has been received or the local clipboard has since been taken over by another
    // application (WM_DESTROYCLIPBOARD).
    proto::desktop::ClipboardFileList pending_file_list_;
    bool have_pending_file_list_ = false;

    // Set while our own delayed-render CF_HDROP is being placed, so the WM_CLIPBOARDUPDATE it raises
    // is not read back and sent to the peer as if the user had copied files locally.
    bool own_file_list_pending_ = false;

    // A paste (WM_RENDERFORMAT) must hand back the files before it returns, but the download runs on
    // the GUI thread. onRenderFileList blocks in this nested loop until provideRenderedFileList
    // delivers the paths (or a timeout fires, so a paste can never hang indefinitely).
    QEventLoop* render_loop_ = nullptr;
    QStringList rendered_paths_;

    // True between entering onRenderFileList and leaving it. A second paste arriving while the first
    // is still waiting would otherwise nest another loop inside it and overwrite |render_loop_|, so
    // only the innermost would ever be woken.
    bool rendering_ = false;

    // Cleared when this object is destroyed. The nested render loop pumps messages, so the clipboard
    // thread can shut down and delete this object while a paste is still waiting inside it; the
    // waiting frame checks its own copy afterwards instead of touching freed members.
    std::shared_ptr<bool> alive_;

    Q_DISABLE_COPY(ClipboardWin)
};

} // namespace common

#endif // COMMON_CLIPBOARD_WIN_H
