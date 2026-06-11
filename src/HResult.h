#pragma once

#include "Log.h"

#include <windows.h>
#include <strsafe.h>

#include <string>

inline std::wstring HResultToString(HRESULT hr)
{
    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS;

    const DWORD length = FormatMessageW(
        flags,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);

    std::wstring result;
    if (length != 0 && message != nullptr) {
        result.assign(message, length);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
            result.pop_back();
        }
        LocalFree(message);
    } else {
        wchar_t buffer[64] = {};
        StringCchPrintfW(buffer, ARRAYSIZE(buffer), L"0x%08X", static_cast<unsigned int>(hr));
        result = buffer;
    }

    return result;
}

inline void LogHResult(const wchar_t* context, HRESULT hr)
{
    Log::Write(L"%s failed: 0x%08X (%s)", context, static_cast<unsigned int>(hr), HResultToString(hr).c_str());
}

inline HRESULT HResultFromLastError()
{
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
}

#define RETURN_IF_FAILED_LOG(expr, context) \
    do { \
        const HRESULT _hr = (expr); \
        if (FAILED(_hr)) { \
            LogHResult((context), _hr); \
            return _hr; \
        } \
    } while (false)

#define LOG_IF_FAILED(expr, context) \
    do { \
        const HRESULT _hr = (expr); \
        if (FAILED(_hr)) { \
            LogHResult((context), _hr); \
        } \
    } while (false)
