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
const QString Clipboard::kMimeTypeTextHtml = QStringLiteral("text/html");
const QString Clipboard::kMimeTypeImagePng = QStringLiteral("image/png");

//--------------------------------------------------------------------------------------------------
Clipboard::Clipboard(QObject* parent)
    : QObject(parent)
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
void Clipboard::setStats(std::shared_ptr<ClipboardStats> stats)
{
    stats_ = std::move(stats);
}

//--------------------------------------------------------------------------------------------------
void Clipboard::logSessionSummary()
{
    if (!stats_)
        return;

    // A single content-free line, so a session's clipboard activity can be read back from the log
    // without the statistics window having been open. Types are named, never content.
    const ClipboardStats::Snapshot s = stats_->snapshot();

    LOG(INFO) << "Clipboard summary. Sent - text:" << s.text_sent << "html:" << s.html_sent
              << "image:" << s.image_sent << ". Received - text:" << s.text_received
              << "html:" << s.html_received << "image:" << s.image_received
              << ". Dropped - unsupported out:" << s.unsupported_out
              << "degraded out:" << s.degraded_out << "unsupported in:" << s.unsupported_in
              << "oversized:" << s.oversized;

    const QString unsupported = unsupportedFormatsSummary();
    if (!unsupported.isEmpty())
        LOG(INFO) << "Clipboard formats seen but not carried:" << unsupported;
}

//--------------------------------------------------------------------------------------------------
void Clipboard::countUnsupportedOut()
{
    if (stats_)
        ++stats_->unsupported_out;
}

//--------------------------------------------------------------------------------------------------
void Clipboard::countDegradedOut()
{
    if (stats_)
        ++stats_->degraded_out;
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
        if (stats_)
            ++stats_->oversized;
        return;
    }

    const QString mime_type = QString::fromStdString(event.mime_type());

    if (mime_type != kMimeTypeTextUtf8 && mime_type != kMimeTypeTextHtml &&
        mime_type != kMimeTypeImagePng)
    {
        LOG(ERROR) << "Unsupported mime type:" << mime_type;
        if (stats_)
            ++stats_->unsupported_in;
        return;
    }

    // Remembered before it is applied: putting it on the clipboard raises a change notification,
    // and without this the content would be sent straight back to the side it came from.
    last_mime_type_ = mime_type;

    // The size was checked against kMaxClipboardDataSize above, so it fits the int this takes.
    last_data_ = QByteArray(event.data().data(), static_cast<int>(event.data().size()));

    if (stats_)
    {
        if (mime_type == kMimeTypeTextUtf8)
            ++stats_->text_received;
        else if (mime_type == kMimeTypeTextHtml)
            ++stats_->html_received;
        else
            ++stats_->image_received;
    }

    setData(last_mime_type_, last_data_);
}

//--------------------------------------------------------------------------------------------------
void Clipboard::clearClipboard()
{
    setData(kMimeTypeTextUtf8, QByteArray());
}

//--------------------------------------------------------------------------------------------------
void Clipboard::onData(const QString& mime_type, const QByteArray& data,
                       const QByteArray& text_fallback)
{
    if (mime_type == last_mime_type_ && data == last_data_)
        return;

    // Dropping the event costs the user a clipboard that did not travel. Sending it would cost
    // them the whole session, because the channel treats an oversized message as a protocol
    // violation and disconnects. The fallback rides in the same message, so it counts against
    // the limit too.
    if (static_cast<size_t>(data.size() + text_fallback.size()) > kMaxClipboardDataSize)
    {
        LOG(WARNING) << "Clipboard data is too large to send:" << (data.size() + text_fallback.size())
                     << "bytes (limit" << kMaxClipboardDataSize << "). Not synchronized.";
        if (stats_)
            ++stats_->oversized;
        return;
    }

    if (stats_)
    {
        if (mime_type == kMimeTypeTextUtf8)
            ++stats_->text_sent;
        else if (mime_type == kMimeTypeTextHtml)
            ++stats_->html_sent;
        else if (mime_type == kMimeTypeImagePng)
            ++stats_->image_sent;
    }

    proto::desktop::ClipboardEvent event;
    event.set_mime_type(mime_type.toStdString());
    event.set_data(data.constData(), static_cast<size_t>(data.size()));
    if (!text_fallback.isEmpty())
        event.set_text_fallback(text_fallback.constData(), static_cast<size_t>(text_fallback.size()));

    emit sig_clipboardEvent(event);
}

} // namespace common
