#pragma once

#include <windows.h>
#include <strsafe.h>

#include <cstdarg>

namespace Log {

inline void Write(const wchar_t* format, ...)
{
    wchar_t buffer[2048] = {};

    va_list args;
    va_start(args, format);
    StringCchVPrintfW(buffer, ARRAYSIZE(buffer), format, args);
    va_end(args);

    OutputDebugStringW(buffer);
    OutputDebugStringW(L"\r\n");
}

} // namespace Log
