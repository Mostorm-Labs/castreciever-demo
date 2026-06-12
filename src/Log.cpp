#include "Log.h"

#include <strsafe.h>

#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>

namespace {

std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
std::wstring g_logFilePath;
std::atomic_bool g_logInitialized { false };

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectory(const std::wstring& path)
{
    if (path.empty() || DirectoryExists(path)) {
        return true;
    }

    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        if (!EnsureDirectory(path.substr(0, slash))) {
            return false;
        }
    }

    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }

    const DWORD error = GetLastError();
    return error == ERROR_ALREADY_EXISTS && DirectoryExists(path);
}

std::wstring DirectoryName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }

    return path.substr(0, slash);
}

std::wstring ExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) {
        return {};
    }

    return DirectoryName(path);
}

std::wstring LocalAppDataDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) {
        return {};
    }

    return path;
}

std::wstring TempDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetTempPathW(ARRAYSIZE(path), path);
    if (length == 0 || length >= ARRAYSIZE(path)) {
        return {};
    }

    std::wstring result = path;
    while (!result.empty() && (result.back() == L'\\' || result.back() == L'/')) {
        result.pop_back();
    }
    return result;
}

std::wstring BuildLogFileName()
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    wchar_t name[128] = {};
    StringCchPrintfW(
        name,
        ARRAYSIZE(name),
        L"UsbCastReceiver-%04u%02u%02u-%02u%02u%02u-%lu.log",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        GetCurrentProcessId());
    return name;
}

std::wstring BuildPreferredLogPath()
{
    const std::wstring fileName = BuildLogFileName();
    const std::wstring localAppData = LocalAppDataDirectory();
    if (!localAppData.empty()) {
        return localAppData + L"\\UsbCastReceiver\\logs\\" + fileName;
    }

    const std::wstring exeDir = ExecutableDirectory();
    if (!exeDir.empty()) {
        return exeDir + L"\\logs\\" + fileName;
    }

    const std::wstring tempDir = TempDirectory();
    if (!tempDir.empty()) {
        return tempDir + L"\\UsbCastReceiver-" + fileName;
    }

    return fileName;
}

HANDLE OpenLogFile(std::wstring& path)
{
    path = BuildPreferredLogPath();

    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::wstring directory = DirectoryName(path);
        if (!directory.empty()) {
            EnsureDirectory(directory);
        }

        HANDLE file = CreateFileW(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            return file;
        }

        if (attempt == 0) {
            const std::wstring exeDir = ExecutableDirectory();
            if (!exeDir.empty()) {
                path = exeDir + L"\\logs\\" + BuildLogFileName();
                continue;
            }
        } else if (attempt == 1) {
            const std::wstring tempDir = TempDirectory();
            if (!tempDir.empty()) {
                path = tempDir + L"\\" + BuildLogFileName();
                continue;
            }
        }

        break;
    }

    path.clear();
    return INVALID_HANDLE_VALUE;
}

std::string WideToUtf8(const wchar_t* text)
{
    if (text == nullptr) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
    if (!result.empty()) {
        result.pop_back();
    }
    return result;
}

void WriteUtf8LineToFile(const wchar_t* line)
{
    if (g_logFile == INVALID_HANDLE_VALUE) {
        return;
    }

    std::string utf8 = WideToUtf8(line);
    utf8.append("\r\n");

    DWORD written = 0;
    WriteFile(g_logFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    FlushFileBuffers(g_logFile);
}

void WriteLineLocked(const wchar_t* line)
{
    OutputDebugStringW(line);
    OutputDebugStringW(L"\r\n");
    WriteUtf8LineToFile(line);
}

void WriteRaw(const wchar_t* format, va_list args)
{
    wchar_t message[2048] = {};
    StringCchVPrintfW(message, ARRAYSIZE(message), format, args);

    SYSTEMTIME now = {};
    GetLocalTime(&now);

    wchar_t line[2300] = {};
    StringCchPrintfW(
        line,
        ARRAYSIZE(line),
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u [tid:%lu] %s",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        GetCurrentThreadId(),
        message);

    std::lock_guard<std::mutex> lock(g_logMutex);
    WriteLineLocked(line);
}

} // namespace

namespace Log {

void Initialize()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logInitialized.load()) {
        return;
    }

    g_logFile = OpenLogFile(g_logFilePath);
    g_logInitialized.store(true);

    wchar_t line[2300] = {};
    if (g_logFile != INVALID_HANDLE_VALUE) {
        StringCchPrintfW(line, ARRAYSIZE(line), L"Log file: %s", g_logFilePath.c_str());
    } else {
        StringCchCopyW(line, ARRAYSIZE(line), L"Log file could not be opened; falling back to OutputDebugStringW only.");
    }

    WriteLineLocked(line);
}

void Shutdown()
{
    Write(L"Log shutdown.");

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    g_logInitialized.store(false);
}

std::wstring FilePath()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logFilePath;
}

void Write(const wchar_t* format, ...)
{
    if (!g_logInitialized.load()) {
        Initialize();
    }

    va_list args;
    va_start(args, format);
    WriteRaw(format, args);
    va_end(args);
}

LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionPointers)
{
    if (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr) {
        Write(
            L"Unhandled exception: code=0x%08X flags=0x%08X address=0x%p",
            static_cast<unsigned int>(exceptionPointers->ExceptionRecord->ExceptionCode),
            static_cast<unsigned int>(exceptionPointers->ExceptionRecord->ExceptionFlags),
            exceptionPointers->ExceptionRecord->ExceptionAddress);
    } else {
        Write(L"Unhandled exception: <no exception record>");
    }

    Shutdown();
    return EXCEPTION_EXECUTE_HANDLER;
}

void TerminateHandler()
{
    Write(L"std::terminate called.");
    Shutdown();
    std::abort();
}

} // namespace Log
