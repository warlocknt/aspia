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

#include "client/file_transfer.h"

#include "base/logging.h"
#include "client/file_transfer_queue_builder.h"
#include "common/file_packet.h"

namespace client {

namespace {

auto g_errorType = qRegisterMetaType<client::FileTransfer::Error::Type>();
auto g_actionType = qRegisterMetaType<client::FileTransfer::Error::Action>();

struct ActionsMap
{
    FileTransfer::Error::Type type;
    quint32 available_actions;
    FileTransfer::Error::Action default_action;
} static const kActions[] =
{
    {
        FileTransfer::Error::Type::CREATE_DIRECTORY,
        FileTransfer::Error::ACTION_ABORT | FileTransfer::Error::ACTION_SKIP |
            FileTransfer::Error::ACTION_SKIP_ALL,
        FileTransfer::Error::ACTION_ASK
    },
    {
        FileTransfer::Error::Type::CREATE_FILE,
        FileTransfer::Error::ACTION_ABORT | FileTransfer::Error::ACTION_SKIP |
            FileTransfer::Error::ACTION_SKIP_ALL,
        FileTransfer::Error::ACTION_ASK
    },
    {
        FileTransfer::Error::Type::OPEN_FILE,
        FileTransfer::Error::ACTION_ABORT | FileTransfer::Error::ACTION_SKIP |
            FileTransfer::Error::ACTION_SKIP_ALL,
        FileTransfer::Error::ACTION_ASK
    },
    {
        FileTransfer::Error::Type::ALREADY_EXISTS,
        FileTransfer::Error::ACTION_ABORT | FileTransfer::Error::ACTION_SKIP |
            FileTransfer::Error::ACTION_SKIP_ALL | FileTransfer::Error::ACTION_REPLACE |
            FileTransfer::Error::ACTION_REPLACE_ALL,
        FileTransfer::Error::ACTION_ASK
    },
    {
        FileTransfer::Error::Type::WRITE_FILE,
        FileTransfer::Error::ACTION_ABORT | FileTransfer::Error::ACTION_SKIP |
            FileTransfer::Error::ACTION_SKIP_ALL,
        FileTransfer::Error::ACTION_ASK
    },
    {
        FileTransfer::Error::Type::READ_FILE,
        FileTransfer::Error::ACTION_ABORT | FileTransfer::Error::ACTION_SKIP |
            FileTransfer::Error::ACTION_SKIP_ALL,
        FileTransfer::Error::ACTION_ASK
    },
    {
        FileTransfer::Error::Type::OTHER,
        FileTransfer::Error::ACTION_ABORT,
        FileTransfer::Error::ACTION_ASK
    }
};

//--------------------------------------------------------------------------------------------------
qint64 calculateSpeed(qint64 last_speed, const FileTransfer::Milliseconds& duration, qint64 bytes)
{
    static const double kAlpha = 0.9;
    return static_cast<qint64>(
        (kAlpha * ((1000.0 / static_cast<double>(duration.count())) * static_cast<double>(bytes))) +
        ((1.0 - kAlpha) * static_cast<double>(last_speed)));
}

} // namespace

//--------------------------------------------------------------------------------------------------
FileTransfer::FileTransfer(Type type,
                           const QString& source_path,
                           const QString& target_path,
                           const QList<Item>& items,
                           QObject* parent)
    : QObject(parent),
      type_(type),
      source_path_(source_path),
      target_path_(target_path),
      items_(items),
      cancel_timer_(new QTimer(this)),
      speed_update_timer_(new QTimer(this))
{
    LOG(INFO) << "Ctor";

    connect(speed_update_timer_, &QTimer::timeout, this, &FileTransfer::doUpdateSpeed);

    cancel_timer_->setSingleShot(true);
    connect(cancel_timer_, &QTimer::timeout, this, [this]()
    {
        onFinished(FROM_HERE);
    });
}

//--------------------------------------------------------------------------------------------------
FileTransfer::~FileTransfer()
{
    LOG(INFO) << "Dtor";
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::start()
{
    LOG(INFO) << "File transfer start";

    common::FileTaskFactory* task_factory_local =
        new common::FileTaskFactory(common::FileTask::Target::LOCAL);

    connect(task_factory_local, &common::FileTaskFactory::sig_taskDone,
            this, &FileTransfer::onTaskDone);

    common::FileTaskFactory* task_factory_remote =
        new common::FileTaskFactory(common::FileTask::Target::REMOTE);

    connect(task_factory_remote, &common::FileTaskFactory::sig_taskDone,
            this, &FileTransfer::onTaskDone);

    if (type_ == Type::DOWNLOADER)
    {
        task_factory_source_ = std::move(task_factory_remote);
        task_factory_target_ = std::move(task_factory_local);
    }
    else
    {
        DCHECK_EQ(type_, Type::UPLOADER);

        task_factory_source_ = std::move(task_factory_local);
        task_factory_target_ = std::move(task_factory_remote);
    }

    // Asynchronously start UI.
    emit sig_started();

    queue_builder_ = new FileTransferQueueBuilder(task_factory_source_->target(), this);

    connect(queue_builder_, &FileTransferQueueBuilder::sig_doTask, this, &FileTransfer::sig_doTask);
    connect(queue_builder_, &FileTransferQueueBuilder::sig_finished,
            this, [this](proto::file_transfer::ErrorCode error_code)
    {
        if (error_code == proto::file_transfer::ERROR_CODE_SUCCESS)
        {
            tasks_ = queue_builder_->takeQueue();
            total_size_ = queue_builder_->totalSize();

            if (tasks_.empty())
            {
                onFinished(FROM_HERE);
            }
            else
            {
                doFrontTask(false);
            }
        }
        else
        {
            onError(Error::Type::QUEUE, proto::file_transfer::ERROR_CODE_UNKNOWN);
        }

        queue_builder_->deleteLater();
    });

    speed_update_timer_->start(Milliseconds(1000));

    // Start building a list of objects for transfer.
    queue_builder_->start(source_path_, target_path_, items_);
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::stop()
{
    LOG(INFO) << "File transfer stop";

    if (queue_builder_)
    {
        delete queue_builder_;
        onFinished(FROM_HERE);
    }
    else
    {
        is_canceled_ = true;
        cancel_timer_->start(std::chrono::seconds(5));
    }
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::setActionForErrorType(Error::Type error_type, Error::Action action)
{
    LOG(INFO) << "Set action for error" << static_cast<int>(error_type) << ":"
              << static_cast<int>(action);
    actions_[error_type] = action;
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::onTaskDone(const common::FileTask& task)
{
    if (type_ == Type::DOWNLOADER)
    {
        if (task.target() == common::FileTask::Target::LOCAL)
        {
            targetReply(task.request(), task.reply());
        }
        else
        {
            DCHECK_EQ(task.target(), common::FileTask::Target::REMOTE);

            sourceReply(task.request(), task.reply());
        }
    }
    else
    {
        DCHECK_EQ(type_, Type::UPLOADER);

        if (task.target() == common::FileTask::Target::LOCAL)
        {
            sourceReply(task.request(), task.reply());
        }
        else
        {
            DCHECK_EQ(task.target(), common::FileTask::Target::REMOTE);

            targetReply(task.request(), task.reply());
        }
    }
}

//--------------------------------------------------------------------------------------------------
FileTransfer::Task& FileTransfer::frontTask()
{
    return tasks_.front();
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::targetReply(
    const proto::file_transfer::Request& request, const proto::file_transfer::Reply& reply)
{
    if (tasks_.empty())
        return;

    if (request.has_create_directory_request())
    {
        if (reply.error_code() == proto::file_transfer::ERROR_CODE_SUCCESS ||
            reply.error_code() == proto::file_transfer::ERROR_CODE_PATH_ALREADY_EXISTS)
        {
            doNextTask();
            return;
        }

        onError(Error::Type::CREATE_DIRECTORY, reply.error_code(), frontTask().targetPath());
    }
    else if (request.has_upload_request())
    {
        if (reply.error_code() != proto::file_transfer::ERROR_CODE_SUCCESS)
        {
            Error::Type error_type = Error::Type::CREATE_FILE;

            if (reply.error_code() == proto::file_transfer::ERROR_CODE_PATH_ALREADY_EXISTS)
                error_type = Error::Type::ALREADY_EXISTS;

            onError(error_type, reply.error_code(), frontTask().targetPath());
            return;
        }

        if (type_ == Type::UPLOADER)
        {
            // The source is the local disk: reads are serial, and the window forms on the send
            // side as read packets are dispatched without waiting for the previous ack.
            requestNextSourcePacket();
        }
        else
        {
            // The source is the remote side: the window forms here, as several packet requests
            // put on the wire at once. Requests beyond the end of the file would hit a closed
            // packetizer, so the burst is capped by the expected packet count; if the file grows
            // meanwhile, the steady state below keeps at least one request outstanding anyway.
            const qint64 file_size = frontTask().size();
            const qint64 expected_packets =
                std::max<qint64>(1, (file_size + common::kMaxFilePacketSize - 1) /
                                        common::kMaxFilePacketSize);
            const int burst = static_cast<int>(std::min<qint64>(kPacketWindow, expected_packets));

            for (int i = 0; i < burst; ++i)
            {
                emit sig_doTask(task_factory_source_->packetRequest(
                    proto::file_transfer::PacketRequest::NO_FLAGS));
                ++remote_in_flight_;
            }
        }
    }
    else if (request.has_packet())
    {
        // Stragglers of an abandoned file arrive before any reply of the next one - the queue is
        // strictly ordered - so they are swallowed by count, not misattributed.
        if (drain_target_replies_ > 0)
        {
            --drain_target_replies_;
            return;
        }

        // This reply is off the wire either way - account for it before deciding anything, so
        // that an error does not count itself into the drain.
        if (type_ == Type::UPLOADER)
            --remote_in_flight_;
        else
            local_write_pending_ = false;

        if (reply.error_code() != proto::file_transfer::ERROR_CODE_SUCCESS)
        {
            abandonCurrentFile();
            onError(Error::Type::WRITE_FILE, reply.error_code(), frontTask().targetPath());
            return;
        }

        if (file_failed_)
            return;

        const qint64 full_task_size = frontTask().size();
        if (full_task_size && total_size_)
        {
            qint64 packet_size = common::kMaxFilePacketSize;

            task_transfered_size_ += packet_size;

            if (task_transfered_size_ > full_task_size)
            {
                packet_size = task_transfered_size_ - full_task_size;
                task_transfered_size_ = full_task_size;
            }

            total_transfered_size_ += packet_size;
            bytes_per_time_ += packet_size;

            const int task_percentage =
                static_cast<int>(task_transfered_size_ * 100 / full_task_size);
            const int total_percentage =
                static_cast<int>(total_transfered_size_ * 100 / total_size_);

            if (task_percentage != task_percentage_ || total_percentage != total_percentage_)
            {
                task_percentage_ = task_percentage;
                total_percentage_ = total_percentage;

                emit sig_progressChanged(total_percentage_, task_percentage_);
            }
        }

        if (request.packet().flags() & proto::file_transfer::Packet::LAST_PACKET)
        {
            doNextTask();
            return;
        }

        if (type_ == Type::UPLOADER)
        {
            // An ack freed a slot in the window. Resume producing if the source still has data
            // and no read is already on its way (the window being full is what paused it).
            if (!source_exhausted_ && !source_request_pending_)
                requestNextSourcePacket();
        }
        else
        {
            // Write the next buffered packet, if the remote side is ahead of the disk.
            if (!pending_writes_.isEmpty())
            {
                local_write_pending_ = true;
                emit sig_doTask(task_factory_target_->packet(pending_writes_.dequeue()));
            }

            // A write completing frees budget that the arrival path may have been unable to
            // spend; top the window back up so a slow disk does not starve the pipe once it
            // catches up.
            if (!source_exhausted_)
            {
                const int budgeted = remote_in_flight_ + pending_writes_.size() +
                                     (local_write_pending_ ? 1 : 0);
                if (budgeted < kPacketWindow)
                {
                    quint32 flags = proto::file_transfer::PacketRequest::NO_FLAGS;
                    if (is_canceled_)
                        flags = proto::file_transfer::PacketRequest::CANCEL;

                    emit sig_doTask(task_factory_source_->packetRequest(flags));
                    ++remote_in_flight_;
                }
            }
        }
    }
    else
    {
        onError(Error::Type::OTHER, proto::file_transfer::ERROR_CODE_UNKNOWN);
    }
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::sourceReply(
    const proto::file_transfer::Request& request, const proto::file_transfer::Reply& reply)
{
    if (tasks_.empty())
    {
        LOG(INFO) << "No more tasks";
        return;
    }

    if (request.has_download_request())
    {
        Task& front_task = frontTask();

        if (reply.error_code() != proto::file_transfer::ERROR_CODE_SUCCESS)
        {
            onError(Error::Type::OPEN_FILE, reply.error_code(), front_task.sourcePath());
            return;
        }

        emit sig_doTask(task_factory_target_->upload(front_task.targetPath(), front_task.overwrite()));
    }
    else if (request.has_packet_request())
    {
        // Stragglers of an abandoned or shrunken file; see drain_target_replies_.
        if (drain_source_replies_ > 0)
        {
            --drain_source_replies_;
            return;
        }

        if (type_ == Type::UPLOADER)
            source_request_pending_ = false;
        else
            --remote_in_flight_;

        if (reply.error_code() != proto::file_transfer::ERROR_CODE_SUCCESS)
        {
            abandonCurrentFile();
            onError(Error::Type::READ_FILE, reply.error_code(), frontTask().sourcePath());
            return;
        }

        if (file_failed_)
            return;

        const bool last_packet =
            (reply.packet().flags() & proto::file_transfer::Packet::LAST_PACKET) != 0;

        if (last_packet)
        {
            source_exhausted_ = true;

            if (type_ == Type::DOWNLOADER)
            {
                // Requests that overshot the end of the file will come back as errors from a
                // packetizer that no longer exists; swallow them.
                drain_source_replies_ += remote_in_flight_;
                remote_in_flight_ = 0;
            }
        }

        if (type_ == Type::UPLOADER)
        {
            emit sig_doTask(task_factory_target_->packet(reply.packet()));
            ++remote_in_flight_;

            // Keep the pipe full: ask the disk for the next packet while this one is on the
            // wire, unless the window is full - then the next ack resumes the reads.
            if (!last_packet && remote_in_flight_ < kPacketWindow)
                requestNextSourcePacket();
        }
        else
        {
            // Local writes stay strictly serial and ordered; buffer what the network delivered
            // ahead of the disk.
            if (local_write_pending_)
            {
                pending_writes_.enqueue(reply.packet());
            }
            else
            {
                local_write_pending_ = true;
                emit sig_doTask(task_factory_target_->packet(reply.packet()));
            }

            // Replace the consumed request - but only while the total of requests on the wire
            // and packets waiting for the disk stays within the window. Without this cap a disk
            // slower than the network would buffer the whole file in memory; with it, once the
            // buffer fills the requests stop, and each completed write issues the next one.
            if (!last_packet)
            {
                const int budgeted = remote_in_flight_ + pending_writes_.size() +
                                     (local_write_pending_ ? 1 : 0);
                if (budgeted < kPacketWindow)
                {
                    quint32 flags = proto::file_transfer::PacketRequest::NO_FLAGS;
                    if (is_canceled_)
                        flags = proto::file_transfer::PacketRequest::CANCEL;

                    emit sig_doTask(task_factory_source_->packetRequest(flags));
                    ++remote_in_flight_;
                }
            }
        }
    }
    else
    {
        onError(Error::Type::OTHER, proto::file_transfer::ERROR_CODE_UNKNOWN);
    }
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::setAction(Error::Type error_type, Error::Action action)
{
    LOG(INFO) << "Set action for error" << static_cast<int>(error_type) << ":"
              << static_cast<int>(action);

    switch (action)
    {
        case Error::ACTION_ABORT:
            onFinished(FROM_HERE);
            break;

        case Error::ACTION_REPLACE:
        case Error::ACTION_REPLACE_ALL:
        {
            if (action == Error::ACTION_REPLACE_ALL)
                setActionForErrorType(error_type, action);

            doFrontTask(true);
        }
        break;

        case Error::ACTION_SKIP:
        case Error::ACTION_SKIP_ALL:
        {
            if (action == Error::ACTION_SKIP_ALL)
                setActionForErrorType(error_type, action);

            doNextTask();
        }
        break;

        default:
            NOTREACHED();
            break;
    }
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::requestNextSourcePacket()
{
    quint32 flags = proto::file_transfer::PacketRequest::NO_FLAGS;
    if (is_canceled_)
        flags = proto::file_transfer::PacketRequest::CANCEL;

    source_request_pending_ = true;
    emit sig_doTask(task_factory_source_->packetRequest(flags));
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::abandonCurrentFile()
{
    // The file is done for, but its window may still have replies on the way back. Everything
    // in flight now belongs to nobody: arrange for it to be swallowed, in order, before the
    // replies of whatever file comes next.
    file_failed_ = true;

    if (type_ == Type::UPLOADER)
    {
        drain_target_replies_ += remote_in_flight_;
        if (source_request_pending_)
            ++drain_source_replies_;
    }
    else
    {
        drain_source_replies_ += remote_in_flight_;
        pending_writes_.clear();
    }

    remote_in_flight_ = 0;
    source_request_pending_ = false;
    local_write_pending_ = false;
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::doFrontTask(bool overwrite)
{
    task_percentage_ = 0;
    task_transfered_size_ = 0;

    // Per-file transfer state. The drain counters are deliberately NOT reset: they swallow
    // stragglers of the previous file, which arrive before anything sent for this one.
    remote_in_flight_ = 0;
    source_exhausted_ = false;
    source_request_pending_ = false;
    local_write_pending_ = false;
    file_failed_ = false;
    pending_writes_.clear();

    Task& front_task = frontTask();
    front_task.setOverwrite(overwrite);

    emit sig_currentItemChanged(front_task.sourcePath(), front_task.targetPath());

    if (front_task.isDirectory())
    {
        emit sig_doTask(task_factory_target_->createDirectory(front_task.targetPath()));
    }
    else
    {
        emit sig_doTask(task_factory_source_->download(front_task.sourcePath()));
    }
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::doNextTask()
{
    if (is_canceled_)
    {
        while (!tasks_.empty())
            tasks_.pop_front();
    }

    if (!tasks_.empty())
    {
        // Delete the task only after confirmation of its successful execution.
        tasks_.pop_front();
    }

    if (tasks_.empty())
    {
        if (cancel_timer_->isActive())
            cancel_timer_->stop();

        onFinished(FROM_HERE);
        return;
    }

    doFrontTask(false);
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::doUpdateSpeed()
{
    TimePoint current_time = Clock::now();
    Milliseconds duration = std::chrono::duration_cast<Milliseconds>(current_time - begin_time_);

    speed_ = calculateSpeed(speed_, duration, bytes_per_time_);

    begin_time_ = current_time;
    bytes_per_time_ = 0;

    emit sig_currentSpeedChanged(speed_);
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::onError(Error::Type type, proto::file_transfer::ErrorCode code, const QString& path)
{
    auto default_action = actions_.find(type);
    if (default_action != actions_.end())
    {
        setAction(type, default_action.value());
        return;
    }

    emit sig_errorOccurred(Error(type, code, path));
}

//--------------------------------------------------------------------------------------------------
void FileTransfer::onFinished(const base::Location& location)
{
    LOG(INFO) << "File transfer finished (from" << location << ")";
    speed_update_timer_->stop();
    emit sig_finished();
}

//--------------------------------------------------------------------------------------------------
quint32 FileTransfer::Error::availableActions() const
{
    for (size_t i = 0; i < sizeof(kActions) / sizeof(kActions[0]); ++i)
    {
        if (kActions[i].type == type_)
            return kActions[i].available_actions;
    }

    return 0;
}

//--------------------------------------------------------------------------------------------------
FileTransfer::Error::Action FileTransfer::Error::defaultAction() const
{
    for (size_t i = 0; i < sizeof(kActions) / sizeof(kActions[0]); ++i)
    {
        if (kActions[i].type == type_)
            return kActions[i].default_action;
    }

    return Action::ACTION_ABORT;
}

//--------------------------------------------------------------------------------------------------
FileTransfer::Task::Task(QString&& source_path, QString&& target_path,
                         bool is_directory, qint64 size)
    : source_path_(std::move(source_path)),
      target_path_(std::move(target_path)),
      is_directory_(is_directory),
      size_(size)
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
FileTransfer::Task::Task(Task&& other) noexcept
    : source_path_(std::move(other.source_path_)),
      target_path_(std::move(other.target_path_)),
      is_directory_(other.is_directory_),
      size_(other.size_)
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
FileTransfer::Task& FileTransfer::Task::operator=(Task&& other) noexcept
{
    source_path_ = std::move(other.source_path_);
    target_path_ = std::move(other.target_path_);
    is_directory_ = other.is_directory_;
    size_ = other.size_;
    return *this;
}

} // namespace client
