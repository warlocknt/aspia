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

#include "common/desktop_config_logging.h"

#include <QStringList>

#include "proto/desktop.h"

namespace common {

namespace {

const char* onOff(bool value)
{
    return value ? "on" : "off";
}

} // namespace

//--------------------------------------------------------------------------------------------------
QString desktopConfigToString(const proto::desktop::Config& config)
{
    const quint32 flags = config.flags();
    auto has = [flags](quint32 flag) { return (flags & flag) != 0; };

    // Each part is built with a single-placeholder arg(), so the numbered markers cannot collide.
    QStringList parts;
    parts << QStringLiteral("flags=0x%1").arg(flags, 0, 16);
    parts << QStringLiteral("clipboard=%1").arg(onOff(has(proto::desktop::ENABLE_CLIPBOARD)));
    parts << QStringLiteral("clear_clipboard=%1").arg(onOff(has(proto::desktop::CLEAR_CLIPBOARD)));
    parts << QStringLiteral("cursor_shape=%1").arg(onOff(has(proto::desktop::ENABLE_CURSOR_SHAPE)));
    parts << QStringLiteral("cursor_position=%1").arg(onOff(has(proto::desktop::CURSOR_POSITION)));
    parts << QStringLiteral("block_input=%1").arg(onOff(has(proto::desktop::BLOCK_REMOTE_INPUT)));
    parts << QStringLiteral("lock_at_disconnect=%1").arg(onOff(has(proto::desktop::LOCK_AT_DISCONNECT)));
    parts << QStringLiteral("disable_effects=%1").arg(onOff(has(proto::desktop::DISABLE_EFFECTS)));
    parts << QStringLiteral("disable_wallpaper=%1").arg(onOff(has(proto::desktop::DISABLE_WALLPAPER)));
    parts << QStringLiteral("disable_font_smoothing=%1")
             .arg(onOff(has(proto::desktop::DISABLE_FONT_SMOOTHING)));
    parts << QStringLiteral("video_encoding=%1").arg(config.video_encoding());
    parts << QStringLiteral("audio_encoding=%1").arg(config.audio_encoding());
    parts << QStringLiteral("compress_ratio=%1").arg(config.compress_ratio());

    return parts.join(QLatin1Char(' '));
}

} // namespace common
