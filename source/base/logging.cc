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

#include "base/logging.h"

#include "base/debug.h"

#if defined(Q_OS_WINDOWS)
#include "base/win/file_version_info.h"
#include "base/win/mini_dump_writer.h"
#include "build/version.h"

#include <qt_windows.h>
#include <comdef.h>
#include <Psapi.h>

#include <vector>
#endif // defined(Q_OS_WINDOWS)

#if defined(Q_OS_LINUX)
#include <linux/limits.h>
#include <unistd.h>
#endif // defined(Q_OS_LINUX)

#if defined(Q_OS_MACOS)
#include <mach-o/dyld.h>
#include <sys/syslimits.h>
#endif // defined(Q_OS_MACOS)

#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QThread>

namespace base {

namespace {

const LoggingSeverity kDefaultLogLevel = LOG_FATAL;
const qint64 kDefaultMaxLogFileSize = 2 * 1024 * 1024; // 2 Mb.
const qint64 kDefaultMaxLogFileAge = 14; // 14 days.

LoggingSeverity g_min_log_level = LOG_ERROR;
LoggingDestination g_logging_destination = LOG_DEFAULT;

qint64 g_max_log_file_size = kDefaultMaxLogFileSize;
qint64 g_max_log_file_age = kDefaultMaxLogFileAge;
int g_log_file_number = -1;

QString g_log_dir_path;
QString g_log_file_path;
QFile g_log_file;
QMutex g_log_file_lock;

//--------------------------------------------------------------------------------------------------
const QString& severityName(LoggingSeverity severity)
{
    static const QString kLogSeverityNames[] = { "INFO", "WARNING", "ERROR", "FATAL" };
    static const QString kUnknown("UNKNOWN");

    static_assert(LOG_NUMBER == std::size(kLogSeverityNames));

    if (severity >= 0 && severity < LOG_NUMBER)
        return kLogSeverityNames[severity];

    return kUnknown;
}

//--------------------------------------------------------------------------------------------------
void removeOldFiles(const QString& path, qint64 max_file_age)
{
    QDateTime time = QDateTime::currentDateTime().addDays(-max_file_age);
    QDir current_dir(path);

    QFileInfoList files = current_dir.entryInfoList();
    for (const auto& file : std::as_const(files))
    {
        if (file.birthTime() < time)
            QFile::remove(file.filePath());
    }
}

//--------------------------------------------------------------------------------------------------
QString defaultLogFileDir()
{
    return QDir::tempPath() + "/aspia";
}

#if defined(Q_OS_WINDOWS)
//--------------------------------------------------------------------------------------------------
// Returns the module this code belongs to. For the host that is aspia_host_core.dll, for the client
// and the console the executable itself.
HMODULE currentModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&defaultLogFileDir), &module);
    return module;
}

//--------------------------------------------------------------------------------------------------
// Returns the link time recorded in the PE header of |module|. Unlike the git commit, it changes on
// every build, which is what identifies a particular binary when several builds share one commit.
QString moduleBuildTimestamp(HMODULE module)
{
    if (!module)
        return QString();

    const quint8* image_base = reinterpret_cast<const quint8*>(module);

    const IMAGE_DOS_HEADER* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base);
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
        return QString();

    const IMAGE_NT_HEADERS* nt_headers =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(image_base + dos_header->e_lfanew);
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE)
        return QString();

    const quint32 timestamp = nt_headers->FileHeader.TimeDateStamp;

    // With reproducible builds the linker stores a hash here instead of a time. Sanity check the
    // value so that a nonsensical date is never reported.
    static const quint32 kMinValidTime = 1577836800; // 2020-01-01.
    static const quint32 kMaxValidTime = 4102444800; // 2100-01-01.

    if (timestamp < kMinValidTime || timestamp > kMaxValidTime)
        return QString();

    return QDateTime::fromSecsSinceEpoch(timestamp, Qt::UTC).toString("yyyy-MM-dd hh:mm:ss 'UTC'");
}

//--------------------------------------------------------------------------------------------------
// Logs the version and link time of every Aspia module loaded into this process. An executable from
// one build combined with a core library from another is a typical result of a partial or failed
// update, and is otherwise almost impossible to notice.
void logAspiaModules()
{
    HANDLE process = GetCurrentProcess();

    DWORD bytes_needed = 0;
    if (!EnumProcessModules(process, nullptr, 0, &bytes_needed) || !bytes_needed)
    {
        PLOG(ERROR) << "EnumProcessModules failed";
        return;
    }

    std::vector<HMODULE> modules(bytes_needed / sizeof(HMODULE));

    if (!EnumProcessModules(process, modules.data(),
                            static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes_needed))
    {
        PLOG(ERROR) << "EnumProcessModules failed";
        return;
    }

    modules.resize(bytes_needed / sizeof(HMODULE));

    const QString expected_version = QStringLiteral(ASPIA_VERSION_STRING);
    bool version_mismatch = false;

    for (HMODULE module : modules)
    {
        wchar_t module_path[MAX_PATH] = { 0 };
        if (!GetModuleFileNameExW(process, module, module_path, MAX_PATH))
            continue;

        const QString file_name = QFileInfo(QString::fromWCharArray(module_path)).fileName();

        // Only our own modules are of interest, the rest belong to the system.
        if (!file_name.startsWith(QLatin1String("aspia"), Qt::CaseInsensitive))
            continue;

        QString version;
        std::unique_ptr<FileVersionInfo> version_info =
            FileVersionInfo::createFileVersionInfoForModule(module);
        if (version_info)
            version = version_info->fileVersion();

        LOG(INFO) << "Module:" << file_name << "(version:" << version
                  << "built:" << moduleBuildTimestamp(module) << ")";

        if (!version.isEmpty() && version != expected_version)
            version_mismatch = true;
    }

    if (version_mismatch)
    {
        // Deliberately not fatal: differing versions are often still compatible, and refusing to
        // work would be worse than the mismatch itself. Just make it visible in the log.
        LOG(WARNING) << "Loaded modules have different versions (this one is built as"
                     << expected_version << "). This is usually caused by a partial or failed "
                        "update. Check that all files come from the same build.";
    }
}
#endif // defined(Q_OS_WINDOWS)

//--------------------------------------------------------------------------------------------------
bool initLoggingUnlocked(const QString& prefix)
{
    g_log_file.close();

    if (!(g_logging_destination & LOG_TO_FILE))
        return true;

    // The next log file must have a number higher than the current one.
    ++g_log_file_number;

    QString file_dir = g_log_dir_path;
    if (file_dir.isEmpty())
        file_dir = defaultLogFileDir();

    if (file_dir.isEmpty())
        return false;

    QDir dir(file_dir);
    if (!dir.exists())
    {
        if (!dir.mkpath(file_dir))
            return false;
    }

    QString time = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss.zzz");
    QString file_path = QString("%1/%2-%3.%4.log").arg(file_dir, prefix, time).arg(g_log_file_number);

    g_log_file.setFileName(file_path);
    if (!g_log_file.open(QFile::WriteOnly | QFile::Append | QFile::Text))
        return false;

    if (g_max_log_file_age != 0)
        removeOldFiles(file_dir, g_max_log_file_age);

    g_log_file_path = std::move(file_path);
    return true;
}

//--------------------------------------------------------------------------------------------------
base::LoggingSeverity qtMessageTypeToSeverity(QtMsgType type)
{
    switch (type)
    {
        case QtCriticalMsg:
        case QtFatalMsg:
            return base::LOG_ERROR;

        case QtWarningMsg:
            return base::LOG_WARNING;

        case QtDebugMsg:
        case QtInfoMsg:
        default:
            return base::LOG_INFO;
    }
}

//--------------------------------------------------------------------------------------------------
void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    const char* filename = context.file;
    if (!filename)
        filename = "<filename>";

    const char* function = context.function;
    if (!function)
        function = "<function>";

    base::LogMessage log_message(filename, context.line, function, qtMessageTypeToSeverity(type));
    log_message.stream() << msg;
}

} // namespace

// This is never instantiated, it's just used for EAT_STREAM_PARAMETERS to have an object of the
// correct type on the LHS of the unused part of the ternary operator.
QDebug* g_swallow_stream;

//--------------------------------------------------------------------------------------------------
LoggingSettings::LoggingSettings()
    : min_log_level(kDefaultLogLevel),
      max_log_file_size(kDefaultMaxLogFileSize),
      max_log_file_age(kDefaultMaxLogFileAge)
{
    if (qEnvironmentVariableIsSet("ASPIA_LOG_LEVEL"))
    {
        bool ok = false;
        int log_level_var = qEnvironmentVariableIntValue("ASPIA_LOG_LEVEL", &ok);
        if (ok)
        {
            int log_level = std::max(log_level_var, LOG_INFO);
            log_level = std::min(log_level, LOG_FATAL);

            min_log_level = log_level;
        }
    }

    int log_to_file = 0;
    if (LOG_DEFAULT & LOG_TO_FILE)
        log_to_file = 1;

    if (qEnvironmentVariableIsSet("ASPIA_LOG_TO_FILE"))
    {
        bool ok = false;
        int value = qEnvironmentVariableIntValue("ASPIA_LOG_TO_FILE", &ok);
        if (ok)
            log_to_file = value;
    }

    int log_to_stdout = 0;
    if (LOG_DEFAULT & LOG_TO_STDOUT)
        log_to_stdout = 1;

    if (qEnvironmentVariableIsSet("ASPIA_LOG_TO_STDOUT"))
    {
        bool ok = false;
        int value = qEnvironmentVariableIntValue("ASPIA_LOG_TO_STDOUT", &ok);
        if (ok)
            log_to_stdout = value;
    }

    if (log_to_file != 0 && log_to_stdout != 0)
        destination = LOG_TO_ALL;
    else if (log_to_file != 0 && log_to_stdout == 0)
        destination = LOG_TO_FILE;
    else if (log_to_file == 0 && log_to_stdout != 0)
        destination = LOG_TO_STDOUT;
    else
        destination = LOG_NONE;

    if (qEnvironmentVariableIsSet("ASPIA_MAX_LOG_FILE_SIZE"))
    {
        bool ok = false;
        int value = qEnvironmentVariableIntValue("ASPIA_MAX_LOG_FILE_SIZE", &ok);
        if (ok)
        {
            static const int kMinValue = 1024;
            static const int kMaxValue = 10 * 1024 * 1024;

            max_log_file_size = static_cast<size_t>(std::min(std::max(value, kMinValue), kMaxValue));
        }
    }

    if (qEnvironmentVariableIsSet("ASPIA_MAX_LOG_FILE_AGE"))
    {
        bool ok = false;
        int value = qEnvironmentVariableIntValue("ASPIA_MAX_LOG_FILE_AGE", &ok);
        if (ok)
        {
            max_log_file_age = static_cast<size_t>(std::min(value, 366));
        }
    }
}

//--------------------------------------------------------------------------------------------------
QString applicationFilePath()
{
    QString file_path;

#if defined(Q_OS_WINDOWS)
    wchar_t buffer[MAX_PATH] = { 0 };
    GetModuleFileNameExW(GetCurrentProcess(), nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    file_path = QString::fromWCharArray(buffer);
#elif defined(Q_OS_LINUX)
    char buffer[PATH_MAX] = { 0 };
    if (readlink("/proc/self/exe", buffer, std::size(buffer)) == -1)
        return QString();
    file_path = buffer;
#elif defined(Q_OS_MACOS)
    char buffer[PATH_MAX] = { 0 };
    quint32 buffer_size = std::size(buffer);
    _NSGetExecutablePath(buffer, &buffer_size);
    file_path = buffer;
#else
#error Not implemented
#endif

    return file_path;
}

//--------------------------------------------------------------------------------------------------
QString logFilePrefix()
{
    return QFileInfo(applicationFilePath()).completeBaseName();
}

//--------------------------------------------------------------------------------------------------
bool initLogging(const LoggingSettings& settings)
{
#if defined(Q_OS_WINDOWS)
    installFailureHandler();
#endif // defined(Q_OS_WINDOWS)

    {
        QMutexLocker lock(&g_log_file_lock);

        g_logging_destination = settings.destination;
        g_min_log_level = settings.min_log_level;
        g_log_dir_path = settings.log_dir;
        g_max_log_file_size = settings.max_log_file_size;
        g_max_log_file_age = settings.max_log_file_age;

        if (!initLoggingUnlocked(logFilePrefix()))
            return false;
    }

    qInstallMessageHandler(qtMessageHandler);

    // The block below identifies the build and its environment. It is the first thing needed when
    // investigating any report, and it is written exactly once per run, so it is always logged: with
    // the configured level it would be dropped on installations that only keep warnings or errors,
    // which are precisely the ones from which logs are usually requested.
    const LoggingSeverity configured_log_level = g_min_log_level;
    g_min_log_level = LOG_INFO;

    LOG(INFO) << "Executable file:" << applicationFilePath();
    if (g_logging_destination & LOG_TO_FILE)
    {
        // If log output is enabled, then we output information about the file.
        LOG(INFO) << "Logging file:" << g_log_file_path;
    }

    // Report the level that was actually requested, not the one forced for this block.
    LOG(INFO) << "Logging level:" << configured_log_level;

#if defined(NDEBUG)
    LOG(INFO) << "Debug build: No";
#else
    LOG(INFO) << "Debug build: Yes";
#endif // defined(NDEBUG)

#if defined(Q_OS_WINDOWS)
    // The git commit alone does not identify a binary: several builds can share one commit. The link
    // time does, and it is recorded by the linker itself, so it cannot go stale.
    const QString build_timestamp = moduleBuildTimestamp(currentModule());
    if (!build_timestamp.isEmpty())
        LOG(INFO) << "Build time:" << build_timestamp;
#endif // defined(Q_OS_WINDOWS)

#if defined(GIT_TREE_DIRTY) && GIT_TREE_DIRTY
    LOG(INFO) << "Built from modified sources: Yes (working tree had uncommitted changes)";
#endif // defined(GIT_TREE_DIRTY) && GIT_TREE_DIRTY

#if defined(Q_OS_WINDOWS)
    logAspiaModules();
#endif // defined(Q_OS_WINDOWS)

    LOG(INFO) << "Logging started";

    // Startup block is over, from here on the configured level applies again.
    g_min_log_level = configured_log_level;
    return true;
}

//--------------------------------------------------------------------------------------------------
void shutdownLogging()
{
    LOG(INFO) << "Logging finished";

    QMutexLocker lock(&g_log_file_lock);
    g_log_file.close();
}

//--------------------------------------------------------------------------------------------------
QString loggingDirectory()
{
    if (g_log_dir_path.isEmpty())
        return defaultLogFileDir();

    return g_log_dir_path;
}

//--------------------------------------------------------------------------------------------------
QString loggingFile()
{
    return g_log_file_path;
}

//--------------------------------------------------------------------------------------------------
bool shouldCreateLogMessage(LoggingSeverity severity)
{
    if (severity < g_min_log_level)
        return false;

    // Return true here unless we know ~LogMessage won't do anything. Note that
    // ~LogMessage writes to stderr if severity_ >= kAlwaysPrintErrorLevel, even
    // when g_logging_destination is LOG_NONE.
    return g_logging_destination != LOG_NONE || severity >= LOG_ERROR;
}

//--------------------------------------------------------------------------------------------------
void makeCheckOpValueString(QDebug* os, std::nullptr_t /* p */)
{
    (*os) << "nullptr";
}

//--------------------------------------------------------------------------------------------------
QString* makeCheckOpString(const int& v1, const int& v2, const char* names)
{
    return makeCheckOpString<int, int>(v1, v2, names);
}

//--------------------------------------------------------------------------------------------------
QString* makeCheckOpString(const unsigned long& v1, const unsigned long& v2, const char* names)
{
    return makeCheckOpString<unsigned long, unsigned long>(v1, v2, names);
}

//--------------------------------------------------------------------------------------------------
QString* makeCheckOpString(const unsigned int& v1, const unsigned int& v2, const char* names)
{
    return makeCheckOpString<unsigned int, unsigned int>(v1, v2, names);
}

//--------------------------------------------------------------------------------------------------
QString* makeCheckOpString(const unsigned long long& v1,
                               const unsigned long long& v2,
                               const char* names)
{
    return makeCheckOpString<unsigned long long, unsigned long long>(v1, v2, names);
}

//--------------------------------------------------------------------------------------------------
QString* makeCheckOpString(const unsigned long& v1, const unsigned int& v2, const char* names)
{
    return makeCheckOpString<unsigned long, unsigned int>(v1, v2, names);
}

//--------------------------------------------------------------------------------------------------
QString* makeCheckOpString(const unsigned int& v1, const unsigned long& v2, const char* names)
{
    return makeCheckOpString<unsigned int, unsigned long>(v1, v2, names);
}

//--------------------------------------------------------------------------------------------------
QString* makeCheckOpString(const QString& v1, const QString& v2, const char* names)
{
    return makeCheckOpString<QString, QString>(v1, v2, names);
}

//--------------------------------------------------------------------------------------------------
LogMessage::LogMessage(std::string_view file,
                       int line,
                       std::string_view function,
                       LoggingSeverity severity)
    : severity_(severity),
      stream_(&string_)
{
    init(file, line, function);
}

//--------------------------------------------------------------------------------------------------
LogMessage::LogMessage(std::string_view file,
                       int line,
                       std::string_view function,
                       const char* condition)
    : severity_(LOG_FATAL),
      stream_(&string_)
{
    init(file, line, function);
    stream_ << "Check failed: " << condition << ". ";
}

//--------------------------------------------------------------------------------------------------
LogMessage::LogMessage(std::string_view file,
                       int line,
                       std::string_view function,
                       QString* result)
    : severity_(LOG_FATAL),
      stream_(&string_)
{
    std::unique_ptr<QString> result_deleter(result);
    init(file, line, function);
    stream_ << "Check failed: " << result->data();
}

//--------------------------------------------------------------------------------------------------
LogMessage::LogMessage(std::string_view file,
                       int line,
                       std::string_view function,
                       LoggingSeverity severity,
                       QString* result)
    : severity_(severity),
      stream_(&string_)
{
    std::unique_ptr<QString> result_deleter(result);
    init(file, line, function);
    stream_ << "Check failed: " << result->data();
}

//--------------------------------------------------------------------------------------------------
LogMessage::~LogMessage()
{
    stream_ << Qt::endl;

    QByteArray message = string_.toUtf8();

    if ((g_logging_destination & LOG_TO_STDOUT) != 0)
    {
        debugPrint(message.data());

        fwrite(message.data(), message.size(), 1, stderr);
        fflush(stderr);
    }
    else if (severity_ >= LOG_ERROR)
    {
        // When we're only outputting to a log file, above a certain log level, we
        // should still output to stderr so that we can better detect and diagnose
        // problems with unit tests, especially on the buildbots.
        fwrite(message.data(), message.size(), 1, stderr);
        fflush(stderr);
    }

    // Write to log file.
    if ((g_logging_destination & LOG_TO_FILE) != 0)
    {
        QMutexLocker lock(&g_log_file_lock);

        if (g_log_file.size() >= g_max_log_file_size)
        {
            // The maximum size of the log file has been exceeded. Close the current log file and
            // create a new one.
            initLoggingUnlocked(logFilePrefix());
        }

        g_log_file.write(message.data(), message.size());
        g_log_file.flush();
    }

    if (severity_ == LOG_FATAL)
    {
        // Crash the process.
        debugBreak();
    }
}

//--------------------------------------------------------------------------------------------------
// Writes the common header info to the stream.
void LogMessage::init(std::string_view file, int line, std::string_view function)
{
    size_t last_slash_pos = file.find_last_of("\\/");
    if (last_slash_pos != std::string_view::npos)
        file.remove_prefix(last_slash_pos + 1);

    stream_ << severityName(severity_)
            << QDateTime::currentDateTime().toString("yyyy/MM/dd hh:mm:ss.zzz")
            << QThread::currentThreadId()
            << file.data() << ':' << line << function.data() << "]";
}

//--------------------------------------------------------------------------------------------------
ErrorLogMessage::ErrorLogMessage(std::string_view file,
                                 int line,
                                 std::string_view function,
                                 LoggingSeverity severity,
                                 SystemError error)
    : error_(error),
      log_message_(file, line, function, severity)
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
ErrorLogMessage::~ErrorLogMessage()
{
    stream() << ":" << error_.toString();
}

} // namespace base

#if defined(Q_OS_WINDOWS)
//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, const std::wstring& wstr)
{
    return out << QString::fromStdWString(wstr);
}

//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, const wchar_t* wstr)
{
    return out << (wstr ? QString::fromWCharArray(wstr) : "nullptr");
}

//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, const _com_error& error)
{
    const wchar_t* message = error.ErrorMessage();
    const quint32 code = static_cast<quint32>(error.Error());

    return out << (message ? QString::fromWCharArray(message) : "nullptr")
               << "(" << QString::number(code, 16) << ")";
}
#endif // defined(Q_OS_WINDOWS)

//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, const char8_t* ustr)
{
    return out << (ustr ? QString::fromUtf8(reinterpret_cast<const char*>(ustr)) : "nullptr");
}

//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, const std::u8string& ustr)
{
    return out << QString::fromUtf8(reinterpret_cast<const char*>(ustr.c_str()),
                                    static_cast<QString::size_type>(ustr.size()));
}

//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, const char16_t* ustr)
{
    return out << (ustr ? QString::fromUtf16(ustr) : "nullptr");
}

//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, const std::u16string& ustr)
{
    return out << QString::fromStdU16String(ustr);
}

//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, const std::filesystem::path& path)
{
#if defined(Q_OS_WINDOWS)
    return out << QString::fromWCharArray(path.c_str());
#else
    return out << QString::fromLocal8Bit(path.c_str());
#endif
}

//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, const std::error_code& error)
{
    const std::string message = error.message();
    const int value  = error.value();

    return out << QString::fromLocal8Bit(message.c_str(), static_cast<QString::size_type>(message.size()))
               << "(" << value << ')';
}

//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, const QStringList& qstrlist)
{
    out << "QStringList(";

    for (QStringList::size_type i = 0; i < qstrlist.size(); ++i)
    {
        out << qstrlist.at(i);

        if (i != qstrlist.size() - 1)
            out << ",";
    }

    return out << ')';
}

//--------------------------------------------------------------------------------------------------
QDebug operator<<(QDebug out, Qt::HANDLE handle)
{
    return out << reinterpret_cast<quint64>(handle);
}
