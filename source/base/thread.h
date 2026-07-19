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

#ifndef BASE_THREAD_H
#define BASE_THREAD_H

#include <QThread>

namespace base {

class Thread final : public QThread
{
    Q_OBJECT

public:
    enum EventDispatcher { AsioDispatcher, QtDispatcher };

    Thread(EventDispatcher dispatcher, QObject* parent = nullptr);

    void stop();

signals:
    // Use these instead of QThread::started and QThread::finished for anything that has to run on
    // the thread itself.
    //
    // started() is emitted before run() begins and finished() after it returns, which leaves both
    // of them outside the window where this thread's COM apartment exists. Work driven by started()
    // reaches COM before it has been initialized - that is how audio capture came to fail with
    // CO_E_NOTINITIALIZED on every attempt - and cleanup driven by finished() releases COM objects
    // after CoUninitialize has already run.
    //
    // These two are emitted from inside run(), where the apartment is in place.
    void sig_beforeRunning();
    void sig_afterRunning();

protected:
    // QThread implementation.
    void run() final;

private:
    const EventDispatcher dispatcher_;

    Q_DISABLE_COPY(Thread)
};

} // namespace base

#endif // BASE_THREAD_H
