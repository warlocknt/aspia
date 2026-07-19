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
// Names of every format currently on the clipboard, as one comma-separated string. The clipboard
// must already be open: EnumClipboardFormats() requires it, unlike IsClipboardFormatAvailable().
//
// Only the names are collected, never the contents. Clipboards routinely carry passwords and other
// secrets, and logs get gathered up and shipped elsewhere for analysis, so nothing that passes
// through here may end up in one.
QString availableFormats()
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

    return formats.join(QStringLiteral(", "));
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

//--------------------------------------------------------------------------------------------------
// Records what the clipboard holds whenever it changes to something this class cannot pass on, so
// that the formats worth implementing can be chosen from what actually turns up on working machines
// rather than from guesswork.
void logUnsupportedFormats(HWND owner)
{
    base::ScopedClipboard clipboard;
    if (!clipboard.init(owner))
        return;

    const QString formats = availableFormats();
    if (formats.isEmpty())
        return;

    LOG(INFO) << "Clipboard changed to content that is not supported. Available formats:"
              << formats;
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
    // Text wins when both are present. Applications that put a picture on the clipboard usually
    // offer text next to it, and that text is normally the content the user meant to copy - the
    // picture is a rendering of it. Copying a range out of a spreadsheet is the everyday example.
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

    logUnsupportedFormats(window_->hwnd());
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
        }
    }

    if (!data.isEmpty())
    {
        data.replace("\r\n", "\n");
        onData(Clipboard::kMimeTypeTextUtf8, data.toUtf8());
    }
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
