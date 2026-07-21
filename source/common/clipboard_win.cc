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

#include "common/clipboard_win.h"

#include "base/logging.h"
#include "base/win/message_window.h"
#include "base/win/scoped_clipboard.h"
#include "base/win/scoped_hglobal.h"

#include <limits>

#include <QBuffer>
#include <QDataStream>
#include <QImage>
#include <QStringList>
#include <QTextDocumentFragment>

namespace common {

namespace {

// A .bmp file is a BITMAPFILEHEADER followed by exactly what CF_DIB holds. Converting between the
// two is therefore a matter of adding or removing these 14 bytes, which is a good deal less work
// than walking the bitmap layout by hand.
const int kBitmapFileHeaderSize = 14;

//--------------------------------------------------------------------------------------------------
// Offset from the start of a packed DIB to its pixels: the header, then the colour table if the
// format has one. Needed because the .bmp header records it and Qt's reader relies on it.
quint32 dibPixelDataOffset(const BITMAPINFOHEADER* header)
{
    quint32 table_size = 0;

    if (header->biBitCount <= 8)
    {
        // Indexed formats: one 32-bit entry per palette colour. biClrUsed of zero means the palette
        // is full for the given depth.
        const quint32 colors = header->biClrUsed ? header->biClrUsed : (1u << header->biBitCount);
        table_size = colors * sizeof(RGBQUAD);
    }
    else if (header->biCompression == BI_BITFIELDS)
    {
        // No palette, but three masks describing where the colour channels sit.
        table_size = 3 * sizeof(DWORD);
    }

    return kBitmapFileHeaderSize + header->biSize + table_size;
}

//--------------------------------------------------------------------------------------------------
// GetClipboardFormatName() only knows about registered formats and fails for the predefined ones,
// so those are named here. Anything not listed is reported by number, which is still enough to
// recognize it later.
QString standardFormatName(UINT format)
{
    switch (format)
    {
        case CF_TEXT:          return QStringLiteral("CF_TEXT");
        case CF_BITMAP:        return QStringLiteral("CF_BITMAP");
        case CF_METAFILEPICT:  return QStringLiteral("CF_METAFILEPICT");
        case CF_SYLK:          return QStringLiteral("CF_SYLK");
        case CF_DIF:           return QStringLiteral("CF_DIF");
        case CF_TIFF:          return QStringLiteral("CF_TIFF");
        case CF_OEMTEXT:       return QStringLiteral("CF_OEMTEXT");
        case CF_DIB:           return QStringLiteral("CF_DIB");
        case CF_PALETTE:       return QStringLiteral("CF_PALETTE");
        case CF_RIFF:          return QStringLiteral("CF_RIFF");
        case CF_WAVE:          return QStringLiteral("CF_WAVE");
        case CF_UNICODETEXT:   return QStringLiteral("CF_UNICODETEXT");
        case CF_ENHMETAFILE:   return QStringLiteral("CF_ENHMETAFILE");
        case CF_HDROP:         return QStringLiteral("CF_HDROP");
        case CF_LOCALE:        return QStringLiteral("CF_LOCALE");
        case CF_DIBV5:         return QStringLiteral("CF_DIBV5");
        default:               return QStringLiteral("unknown(%1)").arg(format);
    }
}

//--------------------------------------------------------------------------------------------------
// Names of every format currently on the clipboard. The clipboard must already be open:
// EnumClipboardFormats() requires it, unlike IsClipboardFormatAvailable().
//
// Only the names are collected, never the contents. Clipboards routinely carry passwords and other
// secrets, and logs get gathered up and shipped elsewhere for analysis, so nothing that passes
// through here may end up in one.
QStringList availableFormatList()
{
    QStringList formats;

    UINT format = 0;
    while ((format = EnumClipboardFormats(format)) != 0)
    {
        wchar_t name[128] = { 0 };

        if (GetClipboardFormatNameW(format, name, static_cast<int>(std::size(name))) > 0)
            formats << QStringLiteral("%1(%2)").arg(QString::fromWCharArray(name)).arg(format);
        else
            formats << standardFormatName(format);
    }

    return formats;
}

//--------------------------------------------------------------------------------------------------
// The same, as one comma-separated string.
QString availableFormats()
{
    return availableFormatList().join(QStringLiteral(", "));
}

//--------------------------------------------------------------------------------------------------
// Extracts the fragment from a Windows "HTML Format" buffer. That buffer is a short ASCII header of
// "Key:offset" lines followed by the HTML, where the offsets are byte positions from the start.
// The fragment is what the user actually selected, marked by StartFragment/EndFragment; the rest is
// the surrounding <html><body> Windows adds. Returns UTF-8 (the format is already UTF-8).
QByteArray htmlFormatToFragment(const QByteArray& cf_html)
{
    auto readOffset = [&cf_html](const char* key) -> int
    {
        int index = cf_html.indexOf(key);
        if (index < 0)
            return -1;

        index += static_cast<int>(strlen(key));

        int end = index;
        while (end < cf_html.size() && cf_html[end] >= '0' && cf_html[end] <= '9')
            ++end;

        if (end == index)
            return -1;

        return cf_html.mid(index, end - index).toInt();
    };

    const int start_fragment = readOffset("StartFragment:");
    const int end_fragment = readOffset("EndFragment:");
    if (start_fragment >= 0 && end_fragment >= start_fragment && end_fragment <= cf_html.size())
        return cf_html.mid(start_fragment, end_fragment - start_fragment);

    // Offsets were missing or nonsensical: fall back to the literal comment markers.
    int start = cf_html.indexOf("<!--StartFragment-->");
    const int end = cf_html.indexOf("<!--EndFragment-->");
    if (start >= 0 && end > start)
    {
        start += static_cast<int>(strlen("<!--StartFragment-->"));
        return cf_html.mid(start, end - start);
    }

    // Last resort: the whole document body between StartHTML and EndHTML.
    const int start_html = readOffset("StartHTML:");
    const int end_html = readOffset("EndHTML:");
    if (start_html >= 0 && end_html >= start_html && end_html <= cf_html.size())
        return cf_html.mid(start_html, end_html - start_html);

    return QByteArray();
}

//--------------------------------------------------------------------------------------------------
// Wraps a UTF-8 HTML fragment in the "HTML Format" header Windows requires, with the byte offsets
// filled in. The numbers are written as fixed-width placeholders first and then patched, so the
// offsets do not depend on how many digits they take.
QByteArray fragmentToHtmlFormat(const QByteArray& fragment)
{
    const QByteArray prefix = "<html>\r\n<body>\r\n<!--StartFragment-->";
    const QByteArray suffix = "<!--EndFragment-->\r\n</body>\r\n</html>";

    QByteArray buffer =
        "Version:0.9\r\n"
        "StartHTML:0000000000\r\n"
        "EndHTML:0000000000\r\n"
        "StartFragment:0000000000\r\n"
        "EndFragment:0000000000\r\n";

    const int header_size = buffer.size();
    buffer += prefix;
    const int start_fragment = buffer.size();
    buffer += fragment;
    const int end_fragment = buffer.size();
    buffer += suffix;
    const int end_html = buffer.size();

    auto patch = [&buffer](const char* key, int value)
    {
        int index = buffer.indexOf(key);
        if (index < 0)
            return;
        index += static_cast<int>(strlen(key));
        const QByteArray number = QByteArray::number(value).rightJustified(10, '0');
        memcpy(buffer.data() + index, number.constData(), 10);
    };

    patch("StartHTML:", header_size);
    patch("EndHTML:", end_html);
    patch("StartFragment:", start_fragment);
    patch("EndFragment:", end_fragment);

    return buffer;
}

//--------------------------------------------------------------------------------------------------
// Copies |bytes| into a moveable global and hands it to the clipboard, which takes ownership. The
// clipboard must already be open and emptied. Returns false and frees on any failure.
bool putClipboardData(base::ScopedClipboard& clipboard, UINT format, const void* bytes, size_t size)
{
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!global)
    {
        PLOG(ERROR) << "GlobalAlloc failed";
        return false;
    }

    void* locked = GlobalLock(global);
    if (!locked)
    {
        PLOG(ERROR) << "GlobalLock failed";
        GlobalFree(global);
        return false;
    }

    memcpy(locked, bytes, size);
    GlobalUnlock(global);

    clipboard.setData(format, global);
    return true;
}

//--------------------------------------------------------------------------------------------------
// True when the clipboard holds a picture or a file list next to the text. Only one of them can be
// passed on, so whatever is not chosen is lost - worth noticing even on the path that succeeds.
bool hasOtherContent()
{
    return IsClipboardFormatAvailable(CF_DIB) ||
           IsClipboardFormatAvailable(CF_BITMAP) ||
           IsClipboardFormatAvailable(CF_HDROP);
}

} // namespace

//--------------------------------------------------------------------------------------------------
ClipboardWin::ClipboardWin(QObject* parent)
    : Clipboard(parent)
{
    LOG(INFO) << "Ctor";
}

//--------------------------------------------------------------------------------------------------
ClipboardWin::~ClipboardWin()
{
    LOG(INFO) << "Dtor";

    if (!window_)
    {
        LOG(ERROR) << "Window not created";
        return;
    }

    RemoveClipboardFormatListener(window_->hwnd());
    window_.reset();
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::init()
{
    if (window_)
    {
        LOG(ERROR) << "Window already created";
        return;
    }

    // Windows carries formatted text under a registered format named "HTML Format". Resolving its
    // id once here; if it fails, HTML is simply never offered and plain text is used throughout.
    html_format_ = RegisterClipboardFormatW(L"HTML Format");
    if (!html_format_)
        PLOG(ERROR) << "RegisterClipboardFormat(HTML Format) failed";

    window_ = std::make_unique<base::MessageWindow>();

    if (!window_->create(std::bind(&ClipboardWin::onMessage,
                                   this,
                                   std::placeholders::_1, std::placeholders::_2,
                                   std::placeholders::_3, std::placeholders::_4)))
    {
        LOG(ERROR) << "Couldn't create clipboard window";
        return;
    }

    if (!AddClipboardFormatListener(window_->hwnd()))
    {
        PLOG(ERROR) << "AddClipboardFormatListener failed";
        return;
    }
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::setData(const QString& mime_type, const QByteArray& data)
{
    if (!window_)
    {
        LOG(ERROR) << "Window not created";
        return;
    }

    if (mime_type == Clipboard::kMimeTypeTextUtf8)
    {
        setDataText(data);
    }
    else if (mime_type == Clipboard::kMimeTypeTextHtml)
    {
        setDataHtml(data);
    }
    else if (mime_type == Clipboard::kMimeTypeImagePng)
    {
        setDataImage(data);
    }
    else
    {
        LOG(ERROR) << "Unsupported mime type:" << mime_type;
    }
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::setDataHtml(const QByteArray& data)
{
    if (!html_format_)
    {
        // Could not register the format at startup; fall back to plain text so the copy still lands.
        LOG(WARNING) << "HTML format unavailable, applying as plain text";
        setDataText(data);
        return;
    }

    LOG(INFO) << "Applying HTML to clipboard, fragment" << data.size() << "bytes";

    const QByteArray cf_html = fragmentToHtmlFormat(data);

    // A plain-text rendering placed next to the HTML, so applications that read only CF_UNICODETEXT
    // (Notepad, a terminal) still receive the text instead of nothing. Derived locally from the
    // fragment here, independent of the text_fallback the sender carries for the network boundary.
    QString plain = QTextDocumentFragment::fromHtml(QString::fromUtf8(data)).toPlainText();
    plain.replace(QLatin1String("\n"), QLatin1String("\r\n"));

    base::ScopedClipboard clipboard;
    if (!clipboard.init(window_->hwnd()))
    {
        PLOG(ERROR) << "Couldn't open the clipboard";
        return;
    }

    clipboard.empty();

    // The HTML Format buffer is UTF-8 and conventionally NUL-terminated.
    QByteArray cf_html_z = cf_html;
    cf_html_z.append('\0');
    putClipboardData(clipboard, html_format_, cf_html_z.constData(),
                     static_cast<size_t>(cf_html_z.size()));

    if (!plain.isEmpty())
    {
        const std::wstring text = plain.toStdWString();
        putClipboardData(clipboard, CF_UNICODETEXT, text.c_str(),
                         (text.size() + 1) * sizeof(wchar_t));
    }
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::setDataImage(const QByteArray& data)
{
    QImage image;
    if (!image.loadFromData(data, "PNG"))
    {
        LOG(ERROR) << "Couldn't decode received image of" << data.size() << "bytes";
        return;
    }

    // Qt writes a .bmp, which is a device independent bitmap with a 14 byte file header in front,
    // so the header is written and then dropped rather than the bitmap being assembled by hand.
    //
    // The conversion to RGB32 discards any transparency: a device independent bitmap has no
    // dependable way to carry it, and applications reading one disagree about what the fourth byte
    // means. Clipboard images are overwhelmingly screenshots and photographs, which have none.
    QByteArray bmp;
    QBuffer buffer(&bmp);
    buffer.open(QIODevice::WriteOnly);

    if (!image.convertToFormat(QImage::Format_RGB32).save(&buffer, "BMP"))
    {
        LOG(ERROR) << "Couldn't convert received image to a bitmap";
        return;
    }

    buffer.close();

    if (bmp.size() <= kBitmapFileHeaderSize)
    {
        LOG(ERROR) << "Converted bitmap is too small:" << bmp.size() << "bytes";
        return;
    }

    const int dib_size = bmp.size() - kBitmapFileHeaderSize;

    base::ScopedClipboard clipboard;
    if (!clipboard.init(window_->hwnd()))
    {
        PLOG(ERROR) << "Couldn't open the clipboard";
        return;
    }

    clipboard.empty();

    HGLOBAL image_global = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(dib_size));
    if (!image_global)
    {
        PLOG(ERROR) << "GlobalAlloc failed";
        return;
    }

    void* image_global_locked = GlobalLock(image_global);
    if (!image_global_locked)
    {
        PLOG(ERROR) << "GlobalLock failed";
        GlobalFree(image_global);
        return;
    }

    memcpy(image_global_locked, bmp.constData() + kBitmapFileHeaderSize,
           static_cast<size_t>(dib_size));

    GlobalUnlock(image_global);

    clipboard.setData(CF_DIB, image_global);
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::setDataText(const QByteArray& data)
{
    QString text = QString::fromUtf8(data);
    text.replace("\n", "\r\n");

    base::ScopedClipboard clipboard;
    if (!clipboard.init(window_->hwnd()))
    {
        PLOG(ERROR) << "Couldn't open the clipboard";
        return;
    }

    clipboard.empty();

    if (text.isEmpty())
        return;

    HGLOBAL text_global = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!text_global)
    {
        PLOG(ERROR) << "GlobalAlloc failed";
        return;
    }

    LPWSTR text_global_locked = reinterpret_cast<LPWSTR>(GlobalLock(text_global));
    if (!text_global_locked)
    {
        PLOG(ERROR) << "GlobalLock failed";
        GlobalFree(text_global);
        return;
    }

    memcpy(text_global_locked, text.utf16(), text.size() * sizeof(wchar_t));
    text_global_locked[text.size()] = 0;

    GlobalUnlock(text_global);

    clipboard.setData(CF_UNICODETEXT, text_global);
}

//--------------------------------------------------------------------------------------------------
bool ClipboardWin::onMessage(UINT message, WPARAM /* wParam */, LPARAM /* lParam */, LRESULT& result)
{
    switch (message)
    {
        case WM_CLIPBOARDUPDATE:
            onClipboardUpdate();
            break;

        default:
            return false;
    }

    result = 0;
    return true;
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::onClipboardUpdate()
{
    // Formatted text wins when present: HTML carries the formatting the user copied, and a plain
    // text rendering rides along in the same event so a peer that cannot take HTML is downgraded to
    // text at the network boundary rather than here - this side stays peer-agnostic. A hiccup
    // reading the HTML falls through to plain text below, so a copy is never lost to it.
    if (html_format_ && IsClipboardFormatAvailable(html_format_))
    {
        if (onClipboardHtml())
            return;
    }

    // Text wins over an image when both are present. Applications that put a picture on the
    // clipboard usually offer text next to it, and that text is normally the content the user meant
    // to copy - the picture is a rendering of it. Copying a range out of a spreadsheet is the
    // everyday example.
    if (IsClipboardFormatAvailable(CF_UNICODETEXT))
    {
        onClipboardText();
        return;
    }

    // Windows synthesizes CF_DIB from CF_BITMAP and CF_DIBV5 on its own, so asking for this one
    // format covers everything that put an image on the clipboard.
    if (IsClipboardFormatAvailable(CF_DIB))
    {
        onClipboardImage();
        return;
    }

    recordUnsupported();
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::onClipboardText()
{
    QString data;

    // Add a scope, so that we keep the clipboard open for as short a time as possible.
    {
        base::ScopedClipboard clipboard;

        if (!clipboard.init(window_->hwnd()))
        {
            PLOG(ERROR) << "Couldn't open the clipboard";
            return;
        }

        HGLOBAL text_global = clipboard.data(CF_UNICODETEXT);
        if (!text_global)
        {
            PLOG(ERROR) << "Couldn't get data from the clipboard";
            return;
        }

        {
            base::ScopedHGLOBAL<wchar_t> text_lock(text_global);
            if (!text_lock.get())
            {
                PLOG(ERROR) << "Couldn't lock clipboard data";
                return;
            }

            data = QString::fromWCharArray(text_lock.get());
        }

        // The text was taken and whatever else was on the clipboard is being dropped. Recorded
        // separately from the case where nothing usable was found at all: here the content is
        // silently degraded rather than lost outright, and this is the more common of the two.
        //
        // Done while the clipboard is still open, since enumerating the formats requires it.
        if (hasOtherContent())
        {
            LOG(INFO) << "Clipboard text taken, other content dropped. Available formats:"
                      << availableFormats();
            countDegradedOut();
        }
    }

    if (!data.isEmpty())
    {
        data.replace("\r\n", "\n");
        onData(Clipboard::kMimeTypeTextUtf8, data.toUtf8());
    }
}

//--------------------------------------------------------------------------------------------------
bool ClipboardWin::onClipboardHtml()
{
    QByteArray cf_html;

    // Add a scope, so that we keep the clipboard open for as short a time as possible.
    {
        base::ScopedClipboard clipboard;

        if (!clipboard.init(window_->hwnd()))
        {
            PLOG(ERROR) << "Couldn't open the clipboard";
            return false;
        }

        HGLOBAL html_global = clipboard.data(html_format_);
        if (!html_global)
        {
            // The format was advertised but produced no data - typically an application that offers
            // HTML by delayed rendering and did not satisfy the request. Fall back to plain text.
            PLOG(WARNING) << "HTML format available but GetClipboardData returned nothing";
            return false;
        }

        base::ScopedHGLOBAL<char> html_lock(html_global);
        if (!html_lock.get())
        {
            PLOG(ERROR) << "Couldn't lock clipboard HTML";
            return false;
        }

        const SIZE_T html_size = GlobalSize(html_global);
        if (html_size > static_cast<SIZE_T>(std::numeric_limits<int>::max()))
        {
            LOG(ERROR) << "Clipboard HTML is implausibly large:" << html_size << "bytes";
            return false;
        }

        // The buffer is conventionally NUL-terminated; take only up to the terminator so the header
        // offsets and the fragment are not thrown off by trailing padding.
        int length = static_cast<int>(html_size);
        const char* bytes = html_lock.get();
        int terminator = 0;
        while (terminator < length && bytes[terminator] != '\0')
            ++terminator;
        length = terminator;

        cf_html = QByteArray(bytes, length);
    }

    const QByteArray fragment = htmlFormatToFragment(cf_html);
    if (fragment.isEmpty())
    {
        // The header could not be parsed. Let the caller fall back to plain text rather than send
        // an empty or malformed fragment.
        LOG(WARNING) << "Couldn't extract fragment from clipboard HTML of" << cf_html.size()
                     << "bytes";
        return false;
    }

    // A plain-text rendering rides along so the network boundary can downgrade this event to text
    // for a peer that has not negotiated HTML. Prefer the clipboard's own CF_UNICODETEXT (what the
    // source application chose) and fall back to stripping the fragment.
    QByteArray text_fallback;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT))
    {
        base::ScopedClipboard clipboard;
        if (clipboard.init(window_->hwnd()))
        {
            HGLOBAL text_global = clipboard.data(CF_UNICODETEXT);
            if (text_global)
            {
                base::ScopedHGLOBAL<wchar_t> text_lock(text_global);
                if (text_lock.get())
                {
                    QString text = QString::fromWCharArray(text_lock.get());
                    text.replace("\r\n", "\n");
                    text_fallback = text.toUtf8();
                }
            }
        }
    }

    if (text_fallback.isEmpty())
    {
        const QString plain =
            QTextDocumentFragment::fromHtml(QString::fromUtf8(fragment)).toPlainText();
        text_fallback = plain.toUtf8();
    }

    LOG(INFO) << "Clipboard HTML taken, fragment" << fragment.size() << "bytes, text fallback"
              << text_fallback.size() << "bytes";

    onData(Clipboard::kMimeTypeTextHtml, fragment, text_fallback);
    return true;
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::recordUnsupported()
{
    base::ScopedClipboard clipboard;
    if (!clipboard.init(window_->hwnd()))
        return;

    const QStringList formats = availableFormatList();
    if (formats.isEmpty())
        return;

    // Recorded so the formats worth implementing can be chosen from what actually turns up on
    // working machines rather than from guesswork. Names only, never content.
    LOG(INFO) << "Clipboard changed to content that is not supported. Available formats:"
              << formats.join(QStringLiteral(", "));

    for (const QString& format : formats)
        ++unsupported_seen_[format];

    countUnsupportedOut();
}

//--------------------------------------------------------------------------------------------------
QString ClipboardWin::unsupportedFormatsSummary() const
{
    if (unsupported_seen_.isEmpty())
        return QString();

    QStringList parts;
    for (auto it = unsupported_seen_.constBegin(); it != unsupported_seen_.constEnd(); ++it)
        parts << QStringLiteral("%1 x%2").arg(it.key()).arg(it.value());

    return parts.join(QStringLiteral(", "));
}

//--------------------------------------------------------------------------------------------------
void ClipboardWin::onClipboardImage()
{
    QByteArray dib;

    // Add a scope, so that we keep the clipboard open for as short a time as possible.
    {
        base::ScopedClipboard clipboard;

        if (!clipboard.init(window_->hwnd()))
        {
            PLOG(ERROR) << "Couldn't open the clipboard";
            return;
        }

        HGLOBAL image_global = clipboard.data(CF_DIB);
        if (!image_global)
        {
            PLOG(ERROR) << "Couldn't get image from the clipboard";
            return;
        }

        base::ScopedHGLOBAL<char> image_lock(image_global);
        if (!image_lock.get())
        {
            PLOG(ERROR) << "Couldn't lock clipboard image";
            return;
        }

        const SIZE_T image_size = GlobalSize(image_global);

        // Guarded before narrowing: the size comes from whatever put the image on the clipboard,
        // and QByteArray counts bytes in an int here.
        if (image_size > static_cast<SIZE_T>(std::numeric_limits<int>::max()))
        {
            LOG(ERROR) << "Clipboard image is implausibly large:" << image_size << "bytes";
            return;
        }

        dib = QByteArray(image_lock.get(), static_cast<int>(image_size));
    }

    if (dib.size() < static_cast<int>(sizeof(BITMAPINFOHEADER)))
    {
        LOG(ERROR) << "Clipboard image is too small to be a bitmap:" << dib.size() << "bytes";
        return;
    }

    // Put the file header back in front of the bitmap so that Qt will read it. Writing the fields
    // by hand keeps this independent of how the compiler lays the structure out.
    const BITMAPINFOHEADER* header = reinterpret_cast<const BITMAPINFOHEADER*>(dib.constData());
    const quint32 file_size = static_cast<quint32>(kBitmapFileHeaderSize + dib.size());

    QByteArray bmp;
    bmp.reserve(static_cast<qsizetype>(file_size));

    QDataStream stream(&bmp, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);

    stream.writeRawData("BM", 2);
    stream << file_size;
    stream << static_cast<quint16>(0) << static_cast<quint16>(0); // Reserved.
    stream << dibPixelDataOffset(header);
    stream.writeRawData(dib.constData(), static_cast<int>(dib.size()));

    QImage image;
    if (!image.loadFromData(bmp, "BMP"))
    {
        LOG(ERROR) << "Couldn't decode clipboard image of" << dib.size() << "bytes";
        return;
    }

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);

    // PNG because it is lossless: the clipboard carries screenshots and diagrams as often as
    // photographs, and those are exactly what a lossy format ruins.
    if (!image.save(&buffer, "PNG"))
    {
        LOG(ERROR) << "Couldn't encode clipboard image as PNG";
        return;
    }

    buffer.close();

    onData(Clipboard::kMimeTypeImagePng, png);
}

} // namespace common
