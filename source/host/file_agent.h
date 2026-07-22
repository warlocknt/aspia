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

#ifndef HOST_FILE_AGENT_H
#define HOST_FILE_AGENT_H

#include "base/serialization.h"
#include "base/ipc/ipc_channel.h"
#include "common/file_worker.h"

namespace host {

class FileAgent final : public QObject
{
    Q_OBJECT

public:
    explicit FileAgent(QObject* parent = nullptr);
    ~FileAgent();

    void start(const QString& channel_id);

private slots:
    void onIpcDisconnected();
    void onIpcMessageReceived(const QByteArray& buffer);

private:
    base::IpcChannel* ipc_channel_ = nullptr;
    common::FileWorker* worker_ = nullptr;

    base::Parser<proto::file_transfer::Request> request_;
    base::Serializer<proto::file_transfer::Reply> reply_;

    // What the client announced on its first request. client_supports_file_caps_ stays false for an
    // older client, which never carries the field. Feature-gating tasks (compression, ...) read
    // client_file_caps_.
    bool client_supports_file_caps_ = false;
    proto::file_transfer::FileCapabilities client_file_caps_;

    Q_DISABLE_COPY(FileAgent)
};

} // namespace host

#endif // HOST_FILE_AGENT_H
