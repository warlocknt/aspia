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

#ifndef CLIENT_CLIENT_FILE_TRANSFER_H
#define CLIENT_CLIENT_FILE_TRANSFER_H

#include "base/serialization.h"
#include "client/client.h"
#include "client/file_remover.h"
#include "client/file_transfer.h"
#include "common/file_task_factory.h"
#include "common/file_worker.h"

namespace client {

class ClientFileTransfer final : public Client
{
    Q_OBJECT

public:
    explicit ClientFileTransfer(QObject* parent = nullptr);
    ~ClientFileTransfer() final;

public slots:
    void onTask(const common::FileTask& task);
    void onDriveListRequest(common::FileTask::Target target);
    void onFileListRequest(common::FileTask::Target target, const QString& path);
    void onCreateDirectoryRequest(common::FileTask::Target target, const QString& path);
    void onRenameRequest(common::FileTask::Target target,
                         const QString& old_path,
                         const QString& new_path);
    void onRemoveRequest(client::FileRemover* remover);
    void onTransferRequest(client::FileTransfer* transfer);

signals:
    void sig_errorOccurred(proto::file_transfer::ErrorCode error_code);
    void sig_driveListReply(common::FileTask::Target target,
                            proto::file_transfer::ErrorCode error_code,
                            const proto::file_transfer::DriveList& drive_list);
    void sig_fileListReply(common::FileTask::Target target,
                      proto::file_transfer::ErrorCode error_code,
                      const proto::file_transfer::List& file_list);
    void sig_createDirectoryReply(common::FileTask::Target target, proto::file_transfer::ErrorCode error_code);
    void sig_renameReply(common::FileTask::Target target, proto::file_transfer::ErrorCode error_code);

protected:
    // Client implementation.
    void onSessionStarted() final;
    void onSessionMessageReceived(const QByteArray& buffer) final;
    void onSessionMessageWritten(size_t pending) final;

private slots:
    void onTaskDone(const common::FileTask& task);

private:
    void pumpRemoteTasks();

    common::FileTaskFactory* taskFactory(common::FileTask::Target target);

    // How many queued requests may be on the wire at once. One at a time made every packet cost
    // a full network round trip, which capped transfers at packet_size / RTT regardless of
    // bandwidth. The host processes messages in order and TCP preserves it, so replies still
    // match requests by queue position. Sized above the producer's packet window so this layer
    // is never the constraint.
    static constexpr int kMaxInFlightRemoteTasks = 12;

    // The first |remote_tasks_in_flight_| entries of |remote_task_queue_| have been sent and
    // await replies; the rest have not been sent yet.
    int remote_tasks_in_flight_ = 0;

    QPointer<common::FileTaskFactory> local_task_factory_;
    QPointer<common::FileTaskFactory> remote_task_factory_;

    QQueue<common::FileTask> remote_task_queue_;
    common::FileWorker* local_worker_ = nullptr;

    QPointer<FileRemover> remover_;
    QPointer<FileTransfer> transfer_;

    base::SerializerImpl serializer_;

    Q_DISABLE_COPY(ClientFileTransfer)
};

} // namespace client

#endif // CLIENT_CLIENT_FILE_TRANSFER_H
