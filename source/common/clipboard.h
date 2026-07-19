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

#ifndef COMMON_CLIPBOARD_H
#define COMMON_CLIPBOARD_H

#include <QObject>

#include "proto/desktop.h"

namespace common {

class Clipboard : public QObject
{
    Q_OBJECT

public:
    explicit Clipboard(QObject* parent);
    virtual ~Clipboard() = default;

    // The content types that can travel through the clipboard. Anything else is dropped, and what
    // was dropped is recorded by the platform implementation so that the list can grow from what
    // actually turns up rather than from guesswork.
    static const QString kMimeTypeTextUtf8;
    static const QString kMimeTypeImagePng;

public slots:
    void start();
    void injectClipboardEvent(const proto::desktop::ClipboardEvent& event);
    void clearClipboard();

signals:
    void sig_clipboardEvent(const proto::desktop::ClipboardEvent& event);

protected:
    virtual void init() = 0;

    // Content is carried as a mime type and raw bytes rather than as text: an image is not a
    // string, and neither is a file list. For text the bytes are UTF-8.
    virtual void setData(const QString& mime_type, const QByteArray& data) = 0;
    void onData(const QString& mime_type, const QByteArray& data);

private:
    // What was last injected from the other side. The platform implementation reports that as a
    // clipboard change of its own, and without remembering it the content would be sent straight
    // back where it came from.
    QString last_mime_type_;
    QByteArray last_data_;
};

} // namespace common

#endif // COMMON_CLIPBOARD_H
