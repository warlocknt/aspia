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

#ifndef COMMON_CLIPBOARD_MONITOR_H
#define COMMON_CLIPBOARD_MONITOR_H

#include "base/thread.h"
#include "common/clipboard.h"

namespace common {

class ClipboardMonitor final : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardMonitor(QObject* parent = nullptr);
    ~ClipboardMonitor() final;

    void start();

    void injectClipboardEvent(const proto::desktop::ClipboardEvent& event);
    void injectClipboardFileList(const proto::desktop::ClipboardFileList& file_list);
    void clearClipboard();

    // A tear-free copy of the per-type tallies for the statistics view. Safe to call from another
    // thread: the counters are atomic and their storage outlives the clipboard object.
    ClipboardStats::Snapshot statsSnapshot() const { return stats_->snapshot(); }

signals:
    void sig_clipboardEvent(const proto::desktop::ClipboardEvent& event);
    void sig_clipboardFileList(const proto::desktop::ClipboardFileList& file_list);
    void sig_injectClipboardEventPrivate(const proto::desktop::ClipboardEvent& event);
    void sig_injectClipboardFileListPrivate(const proto::desktop::ClipboardFileList& file_list);
    void sig_clearClipboardPrivate();

private slots:
    void onBeforeThreadRunning();
    void onAfterThreadRunning();

private:
    base::Thread thread_;
    std::unique_ptr<common::Clipboard> clipboard_;

    // Owned here rather than by the clipboard so that the statistics view can read it from the UI
    // thread while the clipboard, on its own thread, keeps writing - and so it survives the
    // clipboard being torn down at session end.
    std::shared_ptr<ClipboardStats> stats_ = std::make_shared<ClipboardStats>();

    Q_DISABLE_COPY(ClipboardMonitor)
};

} // namespace common

#endif // COMMON_CLIPBOARD_MONITOR_H
