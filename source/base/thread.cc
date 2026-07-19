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

#include "base/thread.h"

#include "base/asio_event_dispatcher.h"
#include "base/logging.h"

#include <optional>

#if defined(Q_OS_WINDOWS)
#include "base/win/scoped_com_initializer.h"
#endif // defined(Q_OS_WINDOWS)

namespace base {

//--------------------------------------------------------------------------------------------------
Thread::Thread(EventDispatcher dispatcher, QObject* parent)
    : QThread(parent),
      dispatcher_(dispatcher)
{
    if (dispatcher == AsioDispatcher)
        setEventDispatcher(new AsioEventDispatcher());
}

//--------------------------------------------------------------------------------------------------
void Thread::stop()
{
    quit();
    wait();
}

//--------------------------------------------------------------------------------------------------
void Thread::run()
{
#if defined(Q_OS_WINDOWS)
    // Asio threads join the multithreaded apartment. The default single threaded apartment creates
    // a hidden OLE message window on the thread, and WASAPI - the reason any of this matters here -
    // expects to be driven from an MTA.
    std::optional<ScopedCOMInitializer> com_initializer;

    if (dispatcher_ == AsioDispatcher)
        com_initializer.emplace(ScopedCOMInitializer::kMTA);
    else
        com_initializer.emplace();

    CHECK(com_initializer->isSucceeded());
#endif // defined(Q_OS_WINDOWS)

    // Emitted here rather than relying on QThread::started, which fires before run() is entered:
    // anything it triggers would run without an apartment. Likewise sig_afterRunning below goes out
    // while the apartment is still up, so that COM objects can be released before it is torn down.
    emit sig_beforeRunning();
    exec();
    emit sig_afterRunning();
}

} // namespace base
