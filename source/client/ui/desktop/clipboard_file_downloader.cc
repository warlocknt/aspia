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

#include "client/ui/desktop/clipboard_file_downloader.h"

#include "base/logging.h"
#include "base/gui_application.h"
#include "client/client_file_transfer.h"
#include "client/client_session_state.h"
#include "client/file_transfer.h"
#include "client/ui/file_transfer/file_transfer_dialog.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace client {

//--------------------------------------------------------------------------------------------------
ClipboardFileDownloader::ClipboardFileDownloader(
    const Config& config, QWidget* parent_widget, QObject* parent)
    : QObject(parent),
      config_(config),
      parent_widget_(parent_widget)
{
    LOG(INFO) << "Ctor";
}

//--------------------------------------------------------------------------------------------------
ClipboardFileDownloader::~ClipboardFileDownloader()
{
    LOG(INFO) << "Dtor";

    emit sig_stopClient();

    // The downloaded files are deliberately NOT removed here. Handing the paths to the clipboard only
    // starts the paste: the pasting application copies from them afterwards, on its own schedule, and
    // deleting them now would empty the paste. They are cleared by the next paste instead - see
    // removeStalePastes().
}

//--------------------------------------------------------------------------------------------------
// static
void ClipboardFileDownloader::removeStalePastes()
{
    QDir root(QDir::tempPath() + u"/aspia_clipboard");
    if (!root.exists())
        return;

    // Anything from an earlier paste has long since been copied by whoever pasted it. An hour is far
    // more than any paste needs and short enough that the files do not accumulate.
    static const qint64 kMaxAgeSecs = 60 * 60;

    const QDateTime now = QDateTime::currentDateTime();
    const QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QFileInfo& entry : entries)
    {
        if (entry.lastModified().secsTo(now) < kMaxAgeSecs)
            continue;

        QDir dir(entry.absoluteFilePath());
        if (!dir.removeRecursively())
            LOG(WARNING) << "Unable to remove a stale paste directory";
    }
}

//--------------------------------------------------------------------------------------------------
void ClipboardFileDownloader::start(const proto::desktop::ClipboardFileList& file_list)
{
    file_list_ = file_list;

    if (!file_list_.file_size())
    {
        LOG(WARNING) << "Nothing to download";
        finish(QStringList());
        return;
    }

    // Clear out what earlier pastes left behind. Done here, not on the way out, because the files
    // must outlive the paste that reads them.
    removeStalePastes();

    // A directory of its own per paste: two pastes in flight cannot overwrite each other, and the
    // whole tree can be removed later without looking at what is inside it.
    const QString unique =
        QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

    target_path_ = QDir::tempPath() + u"/aspia_clipboard/" + unique;

    if (!QDir().mkpath(target_path_))
    {
        LOG(ERROR) << "Unable to create the temporary paste directory";
        finish(QStringList());
        return;
    }

    LOG(INFO) << "Downloading" << file_list_.file_size() << "top-level entries for paste";

    // A second session to the same host, differing only in type. The credentials come from the
    // desktop session, so the paste does not stop to ask; the host applies its own permissions to
    // this session regardless, which is what keeps clipboard paste from bypassing them.
    Config config = config_;
    config.session_type = proto::peer::SESSION_TYPE_FILE_TRANSFER;

    ClientFileTransfer* client = new ClientFileTransfer();
    client_ = client;

    client->moveToThread(base::GuiApplication::ioThread());
    client->setSessionState(std::make_shared<SessionState>(config));

    // Emitted once the session is up and the host has answered - the point at which a transfer may
    // be handed over.
    connect(client, &ClientFileTransfer::sig_showSessionWindow,
            this, &ClipboardFileDownloader::onSessionReady,
            Qt::QueuedConnection);

    connect(client, &ClientFileTransfer::sig_errorOccurred, this,
            [this](proto::file_transfer::ErrorCode error_code)
    {
        // Includes being refused for lack of file-transfer rights, which is a legitimate answer
        // rather than a failure: the paste simply produces nothing.
        LOG(ERROR) << "Clipboard file download session error:" << error_code;
        finish(QStringList());
    }, Qt::QueuedConnection);

    connect(client, &Client::sig_statusChanged, this,
            [this](Client::Status status, const QVariant& /* data */)
    {
        LOG(INFO) << "Clipboard file download client status:" << status;

        if (status == Client::Status::STOPPED)
            finish(QStringList());
    }, Qt::QueuedConnection);

    connect(this, &ClipboardFileDownloader::sig_startClient, client, &Client::start,
            Qt::QueuedConnection);
    connect(this, &ClipboardFileDownloader::sig_stopClient, client, &Client::deleteLater,
            Qt::QueuedConnection);
    connect(this, &ClipboardFileDownloader::sig_transferRequest,
            client, &ClientFileTransfer::onTransferRequest,
            Qt::QueuedConnection);

    emit sig_startClient();
}

//--------------------------------------------------------------------------------------------------
void ClipboardFileDownloader::onSessionReady()
{
    if (finished_)
        return;

    LOG(INFO) << "File transfer session ready; starting the download";

    QList<FileTransfer::Item> items;
    items.reserve(file_list_.file_size());

    for (int i = 0; i < file_list_.file_size(); ++i)
    {
        const proto::desktop::ClipboardFileList::File& file = file_list_.file(i);

        // Directories are listed but not walked on the source side, so their size is unknown here;
        // the transfer expands them itself, exactly as it does for a folder dragged in the file
        // manager.
        items.push_back(FileTransfer::Item(QString::fromStdString(file.name()),
                                           static_cast<qint64>(file.size()),
                                           file.is_dir()));
    }

    transfer_dialog_ = new FileTransferDialog(parent_widget_);
    transfer_dialog_->setAttribute(Qt::WA_DeleteOnClose);

    FileTransfer* transfer = new FileTransfer(
        FileTransfer::Type::DOWNLOADER,
        QString::fromStdString(file_list_.base_path()),
        target_path_,
        items);

    transfer->moveToThread(base::GuiApplication::ioThread());

    // The same progress dialog the file manager uses, so a pasted copy looks like any other file
    // operation - progress, speed, current file, and a working cancel.
    connect(transfer, &FileTransfer::sig_started,
            transfer_dialog_, &FileTransferDialog::start, Qt::QueuedConnection);
    connect(transfer, &FileTransfer::sig_finished,
            transfer_dialog_, &FileTransferDialog::stop, Qt::QueuedConnection);
    connect(transfer, &FileTransfer::sig_errorOccurred,
            transfer_dialog_, &FileTransferDialog::errorOccurred, Qt::QueuedConnection);
    connect(transfer, &FileTransfer::sig_progressChanged,
            transfer_dialog_, &FileTransferDialog::setCurrentProgress, Qt::QueuedConnection);
    connect(transfer, &FileTransfer::sig_currentItemChanged,
            transfer_dialog_, &FileTransferDialog::setCurrentItem, Qt::QueuedConnection);
    connect(transfer, &FileTransfer::sig_currentSpeedChanged,
            transfer_dialog_, &FileTransferDialog::setCurrentSpeed, Qt::QueuedConnection);

    connect(transfer_dialog_, &FileTransferDialog::sig_action,
            transfer, &FileTransfer::setAction, Qt::QueuedConnection);
    connect(transfer_dialog_, &FileTransferDialog::sig_stop,
            transfer, &FileTransfer::stop, Qt::QueuedConnection);

    connect(transfer, &FileTransfer::sig_finished,
            this, &ClipboardFileDownloader::onTransferFinished, Qt::QueuedConnection);

    emit sig_transferRequest(transfer);
}

//--------------------------------------------------------------------------------------------------
void ClipboardFileDownloader::onTransferFinished()
{
    LOG(INFO) << "Download for paste finished";

    // Report what actually arrived rather than what was asked for: a cancelled or partly failed
    // transfer still pastes the files that made it, and nothing that did not.
    QStringList paths;

    for (int i = 0; i < file_list_.file_size(); ++i)
    {
        const QString name = QString::fromStdString(file_list_.file(i).name());
        const QString path = target_path_ + u'/' + name;

        if (QFileInfo::exists(path))
            paths.append(path);
        else
            LOG(WARNING) << "A pasted entry did not arrive; it is left out of the paste";
    }

    finish(paths);
}

//--------------------------------------------------------------------------------------------------
void ClipboardFileDownloader::finish(const QStringList& paths)
{
    // Every failure path lands here, and a session error can follow a completed transfer, so the
    // answer must be given exactly once - the paste is blocked until it arrives.
    if (finished_)
        return;

    finished_ = true;

    LOG(INFO) << "Paste download finished with" << paths.size() << "paths";

    emit sig_finished(paths);

    // Deleted later rather than now: this runs from its own signals, and the temporary directory
    // must outlive the paste that is about to read from it.
    deleteLater();
}

} // namespace client
