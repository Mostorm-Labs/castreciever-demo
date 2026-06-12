#pragma once

#include <windows.h>

#include <string>

namespace Log {

void Initialize();
void Shutdown();
std::wstring FilePath();
void Write(const wchar_t* format, ...);
LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionPointers);
void TerminateHandler();

} // namespace Log
