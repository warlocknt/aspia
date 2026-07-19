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

#include "common/clipboard.h"

#include "base/logging.h"

namespace common {

namespace {

// A message that does not fit into base::NetworkChannel::kMaxMessageSize (7 MB) is not merely
// refused: the channel reports INVALID_PROTOCOL and the connection is dropped. Copying a large
// enough block of text would therefore end the session, which is a poor trade for a clipboard that
// failed to synchronize.
//
// The limit is deliberately well below 7 MB rather than exactly computed. On top of the payload
// come the protobuf framing, the encryption tag and nonce, and the outer message the event is
// nested in; leaving room for all of that costs nothing, while measuring it precisely would tie
// this constant to details that are free to change.
const size_t kMaxClipboardDataSize = 4 * 1024 * 1024; // 4 MB

} // namespace

const QString Clipboard::kMimeTypeTextUtf8 = QStringLiteral("text/plain; charset=UTF-8");
const QString Clipboard::kMimeTypeImagePng = QStringLiteral("image/png");

//--------------------------------------------------------------------------------------------------
Clipboard::Clipboard(QObject* parent)
    : QObject(parent)
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
void Clipboard::start()
{
    init();
}

//--------------------------------------------------------------------------------------------------
void Clipboard::injectClipboardEvent(const proto::desktop::ClipboardEvent& event)
{
    // The channel already refuses anything above its own limit, so this is a sanity check rather
    // than a defence: it keeps a peer built from different sources from filling the local clipboard
    // with something this side would never agree to send.
    if (event.data().size() > kMaxClipboardDataSize)
    {
        LOG(WARNING) << "Received clipboard data is too large:" << event.data().size()
                     << "bytes (limit" << kMaxClipboardDataSize << "). Ignored.";
        return;
    }

    const QString mime_type = QString::fromStdString(event.mime_type());

    if (mime_type != kMimeTypeTextUtf8 && mime_type != kMimeTypeImagePng)
    {
        LOG(ERROR) << "Unsupported mime type:" << mime_type;
        return;
    }

    // Remembered before it is applied: putting it on the clipboard raises a change notification,
    // and without this the content would be sent straight back to the side it came from.
    last_mime_type_ = mime_type;

    // The size was checked against kMaxClipboardDataSize above, so it fits the int this takes.
    last_data_ = QByteArray(event.data().data(), static_cast<int>(event.data().size()));

    setData(last_mime_type_, last_data_);
}

//--------------------------------------------------------------------------------------------------
void Clipboard::clearClipboard()
{
    setData(kMimeTypeTextUtf8, QByteArray());
}

//--------------------------------------------------------------------------------------------------
void Clipboard::onData(const QString& mime_type, const QByteArray& data)
{
    if (mime_type == last_mime_type_ && data == last_data_)
        return;

    // Dropping the event costs the user a clipboard that did not travel. Sending it would cost
    // them the whole session, because the channel treats an oversized message as a protocol
    // violation and disconnects.
    if (static_cast<size_t>(data.size()) > kMaxClipboardDataSize)
    {
        LOG(WARNING) << "Clipboard data is too large to send:" << data.size()
                     << "bytes (limit" << kMaxClipboardDataSize << "). Not synchronized.";
        return;
    }

    proto::desktop::ClipboardEvent event;
    event.set_mime_type(mime_type.toStdString());
    event.set_data(data.constData(), static_cast<size_t>(data.size()));

    emit sig_clipboardEvent(event);
}

} // namespace common
