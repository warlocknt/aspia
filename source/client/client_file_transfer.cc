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

#include "client/client_file_transfer.h"

#include "base/logging.h"
#include "common/file_capabilities.h"
#include "common/file_task_factory.h"

namespace client {

//--------------------------------------------------------------------------------------------------
ClientFileTransfer::ClientFileTransfer(QObject* parent)
    : Client(parent),
      local_worker_(new common::FileWorker(this))
{
    LOG(INFO) << "Ctor";
    qRegisterMetaType<common::FileTask>();
}

//--------------------------------------------------------------------------------------------------
ClientFileTransfer::~ClientFileTransfer()
{
    LOG(INFO) << "Dtor";
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onSessionStarted()
{
    LOG(INFO) << "File transfer session started";

    local_task_factory_ = new common::FileTaskFactory(common::FileTask::Target::LOCAL, this);

    connect(local_task_factory_, &common::FileTaskFactory::sig_taskDone,
            this, &ClientFileTransfer::onTaskDone);

    remote_task_factory_ = new common::FileTaskFactory(common::FileTask::Target::REMOTE, this);

    connect(remote_task_factory_, &common::FileTaskFactory::sig_taskDone,
            this, &ClientFileTransfer::onTaskDone);
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onSessionMessageReceived(const QByteArray& buffer)
{
    proto::file_transfer::Reply reply;

    if (!base::parse(buffer, &reply))
    {
        // Replies are matched to requests by their order in the queue, so an unreadable reply
        // leaves the queue with a request that will never be answered. Nothing else drains the
        // queue, and the queue is only drained by replies, so every later operation
        // would be silently queued forever. Report the failure instead of stalling.
        LOG(ERROR) << "Invalid message from host";

        if (!remote_task_queue_.isEmpty())
        {
            remote_task_queue_.pop_front();
            if (remote_tasks_in_flight_ > 0)
                --remote_tasks_in_flight_;
        }

        emit sig_errorOccurred(proto::file_transfer::ERROR_CODE_UNKNOWN);
        return;
    }

    // The host echoes its capabilities on the reply to our first request. Their presence is what
    // tells us the host understands the mechanism; an older host never sets the field.
    if (reply.has_capabilities() && !host_supports_file_caps_)
    {
        host_supports_file_caps_ = true;
        host_file_caps_ = reply.capabilities();
        LOG(INFO) << "Host supports file capabilities (max_packet_size="
                  << host_file_caps_.max_packet_size() << ")";
    }

    if (reply.error_code() == proto::file_transfer::ERROR_CODE_NO_LOGGED_ON_USER ||
        reply.error_code() == proto::file_transfer::ERROR_CODE_USER_SESSION_LOCKED)
    {
        LOG(INFO) << "File transfer is not available on host side:" << reply.error_code();
        emit sig_errorOccurred(reply.error_code());
    }
    else if (!remote_task_queue_.isEmpty())
    {
        // Remove the request from the queue BEFORE delivering the reply. Delivering it can
        // synchronously enqueue follow-up tasks, and those must land behind the requests that
        // are already on the wire, with the bookkeeping already consistent.
        common::FileTask task = remote_task_queue_.front();
        remote_task_queue_.pop_front();
        if (remote_tasks_in_flight_ > 0)
            --remote_tasks_in_flight_;

        // Move the reply to the request and notify the sender.
        task.onReply(std::move(reply));

        // Send more requests if the window has room.
        pumpRemoteTasks();
    }
    else
    {
        emit sig_errorOccurred(proto::file_transfer::ERROR_CODE_UNKNOWN);
    }
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onSessionMessageWritten(size_t /* pending */)
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onTaskDone(const common::FileTask& task)
{
    const proto::file_transfer::Request& request = task.request();
    const proto::file_transfer::Reply& reply = task.reply();

    if (request.has_drive_list_request())
    {
        emit sig_driveListReply(task.target(), reply.error_code(), reply.drive_list());
    }
    else if (request.has_file_list_request())
    {
        emit sig_fileListReply(task.target(), reply.error_code(), reply.file_list());
    }
    else if (request.has_create_directory_request())
    {
        emit sig_createDirectoryReply(task.target(), reply.error_code());
    }
    else if (request.has_rename_request())
    {
        emit sig_renameReply(task.target(), reply.error_code());
    }
    else
    {
        NOTREACHED();
    }
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onTask(const common::FileTask& task)
{
    if (task.target() == common::FileTask::Target::LOCAL)
    {
        local_worker_->doRequest(task);
    }
    else
    {
        // Add the request to the queue.
        remote_task_queue_.push_back(task);

        // Send it right away if the window has room.
        pumpRemoteTasks();
    }
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onDriveListRequest(common::FileTask::Target target)
{
    onTask(taskFactory(target)->driveList());
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onFileListRequest(common::FileTask::Target target, const QString& path)
{
    onTask(taskFactory(target)->fileList(path));
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onCreateDirectoryRequest(common::FileTask::Target target, const QString& path)
{
    onTask(taskFactory(target)->createDirectory(path));
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onRenameRequest(common::FileTask::Target target,
                                         const QString& old_path,
                                         const QString& new_path)
{
    onTask(taskFactory(target)->rename(old_path, new_path));
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onRemoveRequest(FileRemover* remover)
{
    DCHECK(!remover_);
    remover_ = remover;

    connect(remover_, &FileRemover::sig_doTask, this, &ClientFileTransfer::onTask);
    connect(remover_, &FileRemover::sig_finished, remover_, &FileRemover::deleteLater);

    remover_->start();
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::onTransferRequest(FileTransfer* transfer)
{
    DCHECK(!transfer_);
    transfer_ = transfer;

    connect(transfer_, &FileTransfer::sig_doTask, this, &ClientFileTransfer::onTask);
    connect(transfer_, &FileTransfer::sig_finished, transfer_, &FileTransfer::deleteLater);

    transfer_->start();
}

//--------------------------------------------------------------------------------------------------
void ClientFileTransfer::pumpRemoteTasks()
{
    // Everything before |remote_tasks_in_flight_| is already on the wire. Send what follows
    // until the window is full or the queue runs out.
    while (remote_tasks_in_flight_ < remote_task_queue_.size() &&
           remote_tasks_in_flight_ < kMaxInFlightRemoteTasks)
    {
        const proto::file_transfer::Request& request =
            remote_task_queue_.at(remote_tasks_in_flight_).request();

        if (!file_caps_sent_)
        {
            // Announce our capabilities on the very first request of the session, piggybacked so
            // the reply-to-request ordering is untouched. A copy is made because the queued request
            // belongs to the task that produced it. An older host ignores the extra field.
            proto::file_transfer::Request request_with_caps = request;
            common::setLocalFileCapabilities(request_with_caps.mutable_capabilities());
            file_caps_sent_ = true;

            sendMessage(serializer_.serialize(request_with_caps));
        }
        else
        {
            sendMessage(serializer_.serialize(request));
        }

        ++remote_tasks_in_flight_;
    }
}

//--------------------------------------------------------------------------------------------------
common::FileTaskFactory* ClientFileTransfer::taskFactory(common::FileTask::Target target)
{
    common::FileTaskFactory* task_factory;

    if (target == common::FileTask::Target::LOCAL)
    {
        task_factory = local_task_factory_;
    }
    else
    {
        DCHECK_EQ(target, common::FileTask::Target::REMOTE);
        task_factory = remote_task_factory_;
    }

    DCHECK(task_factory);
    return task_factory;
}

} // namespace client
