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
#include "base/codec/zstd_compress.h"

#include <chrono>

namespace common {

namespace {

// zstd level for clipboard payloads. Higher than the file-transfer path's level 1: clipboard events
// are small and infrequent, so the extra ratio on text/HTML is worth the negligible CPU.
const int kClipboardCompressionLevel = 3;

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

// Compression is only interesting to measure where it can actually pay for itself. Below this the
// transfer is dominated by the round trip rather than the payload, and logging every copied word
// would bury the log for no insight. Sizes and formats only - never the content itself.
const int kSizeLogThreshold = 16 * 1024; // 16 KB

} // namespace

const QString Clipboard::kMimeTypeTextUtf8 = QStringLiteral("text/plain; charset=UTF-8");
const QString Clipboard::kMimeTypeTextHtml = QStringLiteral("text/html");
const QString Clipboard::kMimeTypeImagePng = QStringLiteral("image/png");

//--------------------------------------------------------------------------------------------------
void compressClipboardEvent(proto::desktop::ClipboardEvent* event)
{
    if (event->compressed())
        return;

    // Adaptive: keep the compressed form only when it is actually smaller. Text and HTML shrink a lot;
    // an already-compressed PNG does not and is left untouched. The compressed flag covers the whole
    // event, so data and text_fallback are packed together or not at all.
    QByteArray data(event->data().data(), static_cast<int>(event->data().size()));

    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    QByteArray packed = base::ZstdCompress::compress(data, kClipboardCompressionLevel);
    const int elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());

    if (packed.isEmpty() || packed.size() >= data.size())
    {
        // Worth knowing: it means this format is spending CPU for nothing and the payload goes out
        // at full size anyway (an already-compressed image, typically).
        if (data.size() >= kSizeLogThreshold)
        {
            LOG(INFO) << "Clipboard not compressed, no gain."
                      << "Type:" << event->mime_type().c_str()
                      << "size:" << data.size() << "bytes"
                      << "(took" << elapsed_ms << "ms)";
        }
        return;
    }

    if (data.size() >= kSizeLogThreshold)
    {
        LOG(INFO) << "Clipboard compressed."
                  << "Type:" << event->mime_type().c_str()
                  << "size:" << data.size() << "->" << packed.size() << "bytes"
                  << "(saved" << (100 - (100LL * packed.size() / data.size())) << "%, took"
                  << elapsed_ms << "ms, level" << kClipboardCompressionLevel << ")";
    }

    event->set_data(packed.constData(), static_cast<size_t>(packed.size()));

    if (!event->text_fallback().empty())
    {
        QByteArray text_fallback(event->text_fallback().data(),
                                 static_cast<int>(event->text_fallback().size()));
        QByteArray packed_fallback =
            base::ZstdCompress::compress(text_fallback, kClipboardCompressionLevel);
        event->set_text_fallback(packed_fallback.constData(),
                                 static_cast<size_t>(packed_fallback.size()));
    }

    event->set_compressed(true);
}

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
void Clipboard::onFileList(const proto::desktop::ClipboardFileList& file_list)
{
    emit sig_clipboardFileList(file_list);
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
    // Decompress first when the sender used zstd (only ever toward a peer that negotiated it), so the
    // size check and everything downstream see the real, uncompressed bytes. ZstdCompress caps its
    // output at 64 MB internally, and the 4 MB clipboard limit below still applies to the result, so a
    // tiny compressed blob cannot expand into something oversized.
    QByteArray data;
    QByteArray text_fallback;

    if (event.compressed())
    {
        std::string decompressed = base::ZstdCompress::decompress(event.data());
        if (decompressed.empty() && !event.data().empty())
        {
            LOG(ERROR) << "Unable to decompress clipboard data of" << event.data().size() << "bytes";
            return;
        }
        data = QByteArray(decompressed.data(), static_cast<int>(decompressed.size()));

        // The receiving half of the measurement: what actually crossed the wire against what the
        // clipboard really holds. Logged from the size the sender put on the wire, so a run can be
        // read from either side's log alone.
        if (data.size() >= kSizeLogThreshold)
        {
            LOG(INFO) << "Clipboard decompressed."
                      << "Type:" << event.mime_type().c_str()
                      << "size:" << event.data().size() << "->" << data.size() << "bytes";
        }

        if (!event.text_fallback().empty())
        {
            std::string decompressed_fallback = base::ZstdCompress::decompress(event.text_fallback());
            text_fallback = QByteArray(decompressed_fallback.data(),
                                       static_cast<int>(decompressed_fallback.size()));
        }
    }
    else
    {
        data = QByteArray(event.data().data(), static_cast<int>(event.data().size()));
        text_fallback =
            QByteArray(event.text_fallback().data(), static_cast<int>(event.text_fallback().size()));
    }

    // The channel already refuses anything above its own limit, so this is a sanity check rather
    // than a defence: it keeps a peer built from different sources from filling the local clipboard
    // with something this side would never agree to send.
    if (static_cast<size_t>(data.size()) > kMaxClipboardDataSize)
    {
        LOG(WARNING) << "Received clipboard data is too large:" << data.size()
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
    last_data_ = data;

    // Plain text the sender computed for this formatted content. The platform layer uses it as the
    // plain-text side of the paste rather than parsing the HTML itself. Empty for text and images.
    injected_text_fallback_ = text_fallback;

    if (stats_)
    {
        if (mime_type == kMimeTypeTextUtf8)
            ++stats_->text_received;
        else if (mime_type == kMimeTypeTextHtml)
            ++stats_->html_received;
        else
            ++stats_->image_received;
    }

    // Trace the apply side, which is otherwise silent: this is what a received (e.g. host->client)
    // event actually puts on the local clipboard. Sizes only, never content.
    LOG(INFO) << "Applying received clipboard: mime=" << mime_type << "data=" << last_data_.size()
              << "bytes fallback=" << injected_text_fallback_.size() << "bytes";

    setData(last_mime_type_, last_data_);
}

//--------------------------------------------------------------------------------------------------
void Clipboard::injectClipboardFileList(const proto::desktop::ClipboardFileList& file_list)
{
    LOG(INFO) << "Applying received clipboard file list:" << file_list.file_size()
              << "top-level entries";

    setFileList(file_list);
}

//--------------------------------------------------------------------------------------------------
void Clipboard::provideRenderedFileList(const QStringList& paths)
{
    onRenderedFileList(paths);
}

//--------------------------------------------------------------------------------------------------
void Clipboard::requestRenderFileList(const proto::desktop::ClipboardFileList& file_list)
{
    emit sig_renderFileList(file_list);
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
    {
        // The local clipboard now matches what was just injected from the peer, so this change is
        // that injection echoing back - not something to send. Traced because a genuine local copy
        // wrongly matching here would look exactly like "the other direction does nothing".
        LOG(INFO) << "Clipboard change matches last injected, not sending back: mime=" << mime_type
                  << "size=" << data.size() << "bytes";
        return;
    }

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

    // The event goes out uncompressed here; the network boundary (ClientDesktop / ClientSessionDesktop)
    // compresses it via compressClipboardEvent() when the peer negotiated clipboard_zstd. Compressing
    // there rather than in this worker keeps the decision where the peer's capabilities are known - the
    // host's clipboard lives in a separate agent process that does not see them.
    emit sig_clipboardEvent(event);
}

} // namespace common
