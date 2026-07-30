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

#ifndef COMMON_CLIPBOARD_H
#define COMMON_CLIPBOARD_H

#include <atomic>
#include <memory>

#include <QObject>
#include <QStringList>

#include "proto/desktop.h"

namespace common {

// Per-session tally of what the clipboard carried, by type. Lives in a shared_ptr owned by
// ClipboardMonitor so that the monitor thread can increment it while another thread reads a
// snapshot for the statistics view - hence std::atomic rather than plain ints. Content never
// enters here: only how many times each type moved, and how many were dropped. See the note in
// clipboard_win.cc on why clipboard contents must never reach a counter or a log.
struct ClipboardStats
{
    // Successfully read from this side's clipboard and handed to the peer.
    std::atomic<int> text_sent { 0 };
    std::atomic<int> html_sent { 0 };
    std::atomic<int> image_sent { 0 };

    // Received from the peer and put on this side's clipboard.
    std::atomic<int> text_received { 0 };
    std::atomic<int> html_received { 0 };
    std::atomic<int> image_received { 0 };

    // The local clipboard changed to something with no format this side can carry (a file list, a
    // format we have not implemented). Nothing was sent.
    std::atomic<int> unsupported_out { 0 };

    // Text was taken, but a picture or file list sitting next to it had to be left behind.
    std::atomic<int> degraded_out { 0 };

    // The peer sent a mime type this side does not accept. Negotiation should keep this at zero;
    // a non-zero value means a peer built from different sources ignored it.
    std::atomic<int> unsupported_in { 0 };

    // A clipboard change too large to send, or received above the cap. Dropped rather than risk
    // the session (an oversized message is a protocol violation that disconnects).
    std::atomic<int> oversized { 0 };

    // A plain-int copy for the reader, taken without tearing (each field is read once).
    struct Snapshot
    {
        int text_sent, html_sent, image_sent;
        int text_received, html_received, image_received;
        int unsupported_out, degraded_out, unsupported_in, oversized;
    };

    Snapshot snapshot() const
    {
        return { text_sent, html_sent, image_sent,
                 text_received, html_received, image_received,
                 unsupported_out, degraded_out, unsupported_in, oversized };
    }
};

// Zstd-compresses a clipboard event's payload in place, adaptively: |data| (and |text_fallback|, if
// present) are packed together and the compressed flag set only when compression actually shrank the
// data - an already-compressed PNG is left as-is. Called at the network boundary only toward a peer
// that negotiated the clipboard_zstd extension. The receiving side decompresses in
// Clipboard::injectClipboardEvent by the flag alone. No-op if the event is already compressed.
void compressClipboardEvent(proto::desktop::ClipboardEvent* event);

class Clipboard : public QObject
{
    Q_OBJECT

public:
    explicit Clipboard(QObject* parent);
    virtual ~Clipboard() = default;

    // The content types that can travel through the clipboard. Anything else is dropped, and what
    // was dropped is recorded by the platform implementation so that the list can grow from what
    // actually turns up rather than from guesswork.
    static const QString kMimeTypeTextUtf8;
    static const QString kMimeTypeTextHtml;
    static const QString kMimeTypeImagePng;

    // Where the per-type tallies accumulate. Set once, before start(), by the monitor that owns
    // the shared counter. Without it counting is simply skipped.
    void setStats(std::shared_ptr<ClipboardStats> stats);

    // Writes a single content-free line summarizing the session's clipboard activity by type. Call
    // while the object is fully alive (not from a destructor): it dispatches to the platform's
    // unsupportedFormatsSummary(), which a destructor's static type would hide.
    void logSessionSummary();

public slots:
    void start();
    void injectClipboardEvent(const proto::desktop::ClipboardEvent& event);
    void injectClipboardFileList(const proto::desktop::ClipboardFileList& file_list);
    void clearClipboard();

    // The result of a download requested by sig_renderFileList: the local (temporary) paths the
    // pasted files now live at, or empty if the download failed or was cancelled. Runs on the
    // clipboard thread; the platform layer uses it to satisfy the pending paste.
    void provideRenderedFileList(const QStringList& paths);

signals:
    void sig_clipboardEvent(const proto::desktop::ClipboardEvent& event);

    // The local clipboard now holds a file listing (the user copied files). Only the listing, never
    // the bytes: the content is fetched on paste, on the far side, over a file-transfer session.
    void sig_clipboardFileList(const proto::desktop::ClipboardFileList& file_list);

    // The user pasted the advertised files, so their content is needed now. The owner downloads the
    // listing over a file-transfer session and returns the local paths through
    // provideRenderedFileList. Emitted on the clipboard thread; handled on the GUI thread.
    void sig_renderFileList(const proto::desktop::ClipboardFileList& file_list);

protected:
    virtual void init() = 0;

    // Content is carried as a mime type and raw bytes rather than as text: an image is not a
    // string, and neither is a file list. For text the bytes are UTF-8; for HTML, a UTF-8 fragment.
    virtual void setData(const QString& mime_type, const QByteArray& data) = 0;

    // Puts a file listing on the local clipboard. Only Windows (delayed-render CF_HDROP) implements
    // it; elsewhere copying files through the clipboard is not offered, so the default does nothing.
    virtual void setFileList(const proto::desktop::ClipboardFileList& /* file_list */) {}

    // Delivers the downloaded paths to the platform layer waiting on a pending paste. Windows quits
    // the loop that WM_RENDERFORMAT is blocked on; other platforms have no pending paste.
    virtual void onRenderedFileList(const QStringList& /* paths */) {}

    // Emits sig_renderFileList. Called by the platform layer from a pending paste to ask the owner
    // to download the advertised files.
    void requestRenderFileList(const proto::desktop::ClipboardFileList& file_list);

    // |text_fallback| is a plain-text rendering carried alongside formatted (HTML) content, so a
    // peer that cannot take HTML can be downgraded to text at the network boundary. Empty for text
    // and images.
    void onData(const QString& mime_type, const QByteArray& data,
                const QByteArray& text_fallback = QByteArray());

    // A one-line, content-free description of the formats that turned up but could not be carried,
    // for the end-of-session summary. Empty unless a platform records them.
    virtual QString unsupportedFormatsSummary() const { return QString(); }

    // Emits sig_clipboardFileList. Called by the platform implementation when the local clipboard
    // turns out to hold files rather than text or an image.
    void onFileList(const proto::desktop::ClipboardFileList& file_list);

    // Increment a tally if one is attached. Called by the platform implementation as content moves.
    void countUnsupportedOut();
    void countDegradedOut();

    // The plain-text rendering that rode along with the event currently being injected (HTML only).
    // The platform implementation uses it as the plain-text side of a formatted paste instead of
    // recomputing it - recomputing meant a QtGui rich-text parse on this worker thread, which is
    // not thread-safe and crashed on embedded images.
    const QByteArray& injectedTextFallback() const { return injected_text_fallback_; }

    std::shared_ptr<ClipboardStats> stats_;

private:
    // What was last injected from the other side. The platform implementation reports that as a
    // clipboard change of its own, and without remembering it the content would be sent straight
    // back where it came from.
    QString last_mime_type_;
    QByteArray last_data_;

    // Plain-text carried alongside the formatted content of the event being injected. Empty for
    // text and images. Set before setData() is called, read by the platform implementation.
    QByteArray injected_text_fallback_;
};

} // namespace common

#endif // COMMON_CLIPBOARD_H
