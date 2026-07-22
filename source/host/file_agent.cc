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

#include "host/file_agent.h"

#include "base/logging.h"
#include "base/serialization.h"
#include "common/file_capabilities.h"

#include <QCoreApplication>

namespace host {

//--------------------------------------------------------------------------------------------------
FileAgent::FileAgent(QObject* parent)
    : QObject(parent),
      worker_(new common::FileWorker(this))
{
    LOG(INFO) << "Ctor";
}

//--------------------------------------------------------------------------------------------------
FileAgent::~FileAgent()
{
    LOG(INFO) << "Dtor";
}

//--------------------------------------------------------------------------------------------------
void FileAgent::start(const QString& channel_id)
{
    LOG(INFO) << "Starting (channel_id=" << channel_id.data() << ")";

    ipc_channel_ = new base::IpcChannel(this);

    if (!ipc_channel_->connectTo(channel_id))
    {
        LOG(ERROR) << "Connection failed";
        return;
    }

    connect(ipc_channel_, &base::IpcChannel::sig_disconnected, this, &FileAgent::onIpcDisconnected);
    connect(ipc_channel_, &base::IpcChannel::sig_messageReceived, this, &FileAgent::onIpcMessageReceived);

    ipc_channel_->resume();
}

//--------------------------------------------------------------------------------------------------
void FileAgent::onIpcDisconnected()
{
    LOG(INFO) << "IPC channel disconnected";
    QCoreApplication::quit();
}

//--------------------------------------------------------------------------------------------------
void FileAgent::onIpcMessageReceived(const QByteArray& buffer)
{
    if (!request_.parse(buffer))
    {
        LOG(ERROR) << "Unable to parse message";
        return;
    }

    const proto::file_transfer::Request& request = request_.message();
    proto::file_transfer::Reply* reply = &reply_.newMessage();

    worker_->doRequest(request, reply);

    // The client carries its capabilities on the first request; echo ours on this reply so it knows
    // the host understands the mechanism. The real work above ran as usual - the field just rides
    // along. An older client never sends it, so this stays untouched and it is used as before.
    if (request.has_capabilities())
    {
        if (!client_supports_file_caps_)
        {
            client_supports_file_caps_ = true;
            client_file_caps_ = request.capabilities();
            LOG(INFO) << "Client supports file capabilities (max_packet_size="
                      << client_file_caps_.max_packet_size() << ")";
        }

        common::setLocalFileCapabilities(reply->mutable_capabilities());
    }

    ipc_channel_->send(reply_.serialize());
}

} // namespace host
