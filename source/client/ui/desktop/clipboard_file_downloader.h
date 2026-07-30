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

#ifndef CLIENT_UI_DESKTOP_CLIPBOARD_FILE_DOWNLOADER_H
#define CLIENT_UI_DESKTOP_CLIPBOARD_FILE_DOWNLOADER_H

#include <QObject>
#include <QPointer>
#include <QStringList>

#include "client/client_config.h"
#include "proto/desktop.h"

class QWidget;

namespace client {

class ClientFileTransfer;
class FileTransfer;
class FileTransferDialog;

// Fetches the files a peer copied to its clipboard, when the user pastes them here.
//
// The listing arrives over the desktop session, but the content is deliberately not: pulling
// megabytes for a copy that may never be pasted would be wasteful, and streaming it down the desktop
// channel would put the video behind it. Instead this opens a second, ordinary file-transfer session
// on demand and reuses the normal transfer machinery, progress dialog and all - so a pasted copy
// behaves exactly like dragging the same files in the file manager.
//
// The credentials of the desktop session are reused, so the paste does not stop to ask for them. The
// host still enforces its own permissions on that session: an operator without file-transfer rights
// is refused there, which is the point - the clipboard must not become a way around it.
//
// One instance serves one paste and deletes itself when done.
class ClipboardFileDownloader : public QObject
{
    Q_OBJECT

public:
    // |parent_widget| owns the progress dialog. |config| is the desktop session's own configuration;
    // only the session type is changed.
    ClipboardFileDownloader(const Config& config, QWidget* parent_widget, QObject* parent = nullptr);
    ~ClipboardFileDownloader() final;

    // Connects and downloads |file_list| into a temporary directory. sig_finished always follows,
    // with the local paths of what arrived, or empty if the download failed or was cancelled.
    void start(const proto::desktop::ClipboardFileList& file_list);

signals:
    void sig_finished(const QStringList& paths);
    void sig_startClient();
    void sig_stopClient();
    void sig_transferRequest(client::FileTransfer* transfer);

private slots:
    void onSessionReady();
    void onTransferFinished();

private:
    // Reports |paths| once and schedules deletion. Every exit path goes through here, so a paste is
    // never left waiting on a downloader that quietly gave up.
    void finish(const QStringList& paths);

    // Removes what earlier pastes downloaded. Called when a new paste starts rather than when one
    // ends: handing the paths over only begins the paste, and the pasting application reads the
    // files afterwards, so they must outlive the downloader that fetched them.
    static void removeStalePastes();

    const Config config_;
    QPointer<QWidget> parent_widget_;
    proto::desktop::ClipboardFileList file_list_;

    // Where the downloaded files are put. Named per paste so two pastes cannot collide, and removed
    // when this object dies - by which point the pasting application has copied what it wanted.
    QString target_path_;

    QPointer<ClientFileTransfer> client_;
    QPointer<FileTransferDialog> transfer_dialog_;
    bool finished_ = false;

    Q_DISABLE_COPY(ClipboardFileDownloader)
};

} // namespace client

#endif // CLIENT_UI_DESKTOP_CLIPBOARD_FILE_DOWNLOADER_H
