#pragma once

#if defined(BK2_ANDROID)

#include <android/log.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

using BYTE = std::uint8_t;
using WORD = std::uint16_t;
using DWORD = std::uint32_t;
using QWORD = std::uint64_t;
using BOOL = int;
using UINT = unsigned int;
using LONG = long;
using WCHAR = wchar_t;
using LPARAM = long;
using WPARAM = unsigned long;
using LRESULT = long;
using HRESULT = long;
using HANDLE = void*;
using HINSTANCE = void*;
using HWND = void*;
using HMODULE = void*;
using HCURSOR = void*;
using LPCSTR = const char*;
using LPVOID = void*;
using LPTSTR = char*;

constexpr BOOL TRUE = 1;
constexpr BOOL FALSE = 0;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057L);
constexpr UINT CP_ACP = 0;
constexpr UINT CP_UTF8 = 65001;
constexpr DWORD INFINITE = 0xffffffff;
constexpr DWORD WAIT_OBJECT_0 = 0;
constexpr DWORD WAIT_TIMEOUT = 0x00000102;
constexpr DWORD WAIT_FAILED = 0xffffffff;
constexpr unsigned int _RC_CHOP = 0;
constexpr unsigned int _PC_24 = 0;
constexpr unsigned int _PC_53 = 0;
constexpr unsigned int _PC_64 = 0;
constexpr unsigned int _RC_NEAR = 0;
constexpr unsigned int _RC_DOWN = 0;
constexpr unsigned int _RC_UP = 0;
constexpr unsigned int _MCW_RC = 0;
constexpr unsigned int _MCW_PC = 0;
constexpr unsigned int _EM_INVALID = 0;
constexpr unsigned int _EM_ZERODIVIDE = 0;
constexpr unsigned int _EM_OVERFLOW = 0;
constexpr unsigned int _EM_UNDERFLOW = 0;
constexpr unsigned int _EM_INEXACT = 0;
constexpr unsigned int _EM_DENORMAL = 0;
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1))
#define _vsnprintf vsnprintf
#define MAKELONG(low, high) \
    static_cast<LONG>((static_cast<DWORD>(static_cast<WORD>(low))) | \
                      (static_cast<DWORD>(static_cast<WORD>(high)) << 16))

struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};

struct GUID {
    DWORD Data1;
    WORD Data2;
    WORD Data3;
    BYTE Data4[8];
};

inline BOOL IsEqualGUID(const GUID& left, const GUID& right) {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0 ? TRUE : FALSE;
}

inline HRESULT CoCreateGuid(GUID* guid) {
    if (guid == nullptr) {
        return E_INVALIDARG;
    }

    arc4random_buf(guid, sizeof(GUID));
    guid->Data3 = static_cast<WORD>((guid->Data3 & 0x0fff) | 0x4000);
    guid->Data4[0] = static_cast<BYTE>((guid->Data4[0] & 0x3f) | 0x80);
    return S_OK;
}

inline WORD LOWORD(DWORD value) {
    return static_cast<WORD>(value & 0xffff);
}

inline WORD HIWORD(DWORD value) {
    return static_cast<WORD>((value >> 16) & 0xffff);
}

inline std::uint64_t FileTimeToUInt64(const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

inline FILETIME UInt64ToFileTime(std::uint64_t value) {
    return {
        static_cast<DWORD>(value & 0xffffffff),
        static_cast<DWORD>(value >> 32),
    };
}

inline std::time_t FileTimeToUnixTime(const FILETIME& value) {
    constexpr std::uint64_t kUnixEpochAsFileTime = 116444736000000000ULL;
    const std::uint64_t fileTime = FileTimeToUInt64(value);
    if (fileTime <= kUnixEpochAsFileTime) {
        return 0;
    }
    return static_cast<std::time_t>((fileTime - kUnixEpochAsFileTime) / 10000000ULL);
}

inline FILETIME UnixTimeToFileTime(std::time_t value) {
    constexpr std::uint64_t kUnixEpochAsFileTime = 116444736000000000ULL;
    const std::uint64_t fileTime =
            static_cast<std::uint64_t>(value) * 10000000ULL + kUnixEpochAsFileTime;
    return UInt64ToFileTime(fileTime);
}

inline BOOL FileTimeToLocalFileTime(const FILETIME* input, FILETIME* output) {
    if (input == nullptr || output == nullptr) {
        return FALSE;
    }
    *output = *input;
    return TRUE;
}

inline BOOL LocalFileTimeToFileTime(const FILETIME* input, FILETIME* output) {
    if (input == nullptr || output == nullptr) {
        return FALSE;
    }
    *output = *input;
    return TRUE;
}

inline BOOL FileTimeToDosDateTime(const FILETIME* input, WORD* fatDate, WORD* fatTime) {
    if (input == nullptr || fatDate == nullptr || fatTime == nullptr) {
        return FALSE;
    }
    std::time_t unixTime = FileTimeToUnixTime(*input);
    std::tm localTime {};
    if (localtime_r(&unixTime, &localTime) == nullptr) {
        return FALSE;
    }
    int year = localTime.tm_year + 1900;
    if (year < 1980) {
        year = 1980;
    } else if (year > 2107) {
        year = 2107;
    }
    *fatDate = static_cast<WORD>(((year - 1980) << 9)
            | ((localTime.tm_mon + 1) << 5)
            | localTime.tm_mday);
    *fatTime = static_cast<WORD>((localTime.tm_hour << 11)
            | (localTime.tm_min << 5)
            | (localTime.tm_sec / 2));
    return TRUE;
}

inline BOOL DosDateTimeToFileTime(WORD fatDate, WORD fatTime, FILETIME* output) {
    if (output == nullptr) {
        return FALSE;
    }
    std::tm localTime {};
    localTime.tm_year = ((fatDate >> 9) & 0x7f) + 80;
    localTime.tm_mon = ((fatDate >> 5) & 0x0f) - 1;
    localTime.tm_mday = fatDate & 0x1f;
    localTime.tm_hour = (fatTime >> 11) & 0x1f;
    localTime.tm_min = (fatTime >> 5) & 0x3f;
    localTime.tm_sec = (fatTime & 0x1f) * 2;
    *output = UnixTimeToFileTime(mktime(&localTime));
    return TRUE;
}

inline BOOL IsDebuggerPresent() {
    return FALSE;
}

#define __debugbreak() __builtin_trap()

inline unsigned int _control87(unsigned int, unsigned int) {
    return 0;
}

using LPTHREAD_START_ROUTINE = DWORD (*)(LPVOID);

namespace bk2::android::wincompat {

struct HandleBase {
    virtual ~HandleBase() = default;
    virtual DWORD wait(DWORD milliseconds) = 0;
};

struct EventHandle final : HandleBase {
    std::mutex mutex;
    std::condition_variable condition;
    bool manual_reset;
    bool signaled;

    EventHandle(bool manualReset, bool initialState)
        : manual_reset(manualReset), signaled(initialState) {}

    DWORD wait(DWORD milliseconds) override {
        std::unique_lock<std::mutex> lock(mutex);
        if (milliseconds == 0) {
            if (!signaled) {
                return WAIT_TIMEOUT;
            }
        } else if (milliseconds == INFINITE) {
            condition.wait(lock, [this] { return signaled; });
        } else {
            const bool ready = condition.wait_for(
                    lock,
                    std::chrono::milliseconds(milliseconds),
                    [this] { return signaled; });
            if (!ready) {
                return WAIT_TIMEOUT;
            }
        }

        if (!manual_reset) {
            signaled = false;
        }
        return WAIT_OBJECT_0;
    }

    void set() {
        std::lock_guard<std::mutex> lock(mutex);
        signaled = true;
        if (manual_reset) {
            condition.notify_all();
        } else {
            condition.notify_one();
        }
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex);
        signaled = false;
    }
};

struct ThreadHandle final : HandleBase {
    std::thread thread;

    explicit ThreadHandle(std::thread&& value) : thread(std::move(value)) {}

    ~ThreadHandle() override {
        if (thread.joinable()) {
            thread.detach();
        }
    }

    DWORD wait(DWORD) override {
        if (thread.joinable()) {
            thread.join();
        }
        return WAIT_OBJECT_0;
    }
};

inline HandleBase* AsHandle(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    return reinterpret_cast<HandleBase*>(handle);
}

}  // namespace bk2::android::wincompat

inline HANDLE CreateEvent(LPVOID, BOOL manualReset, BOOL initialState, LPCSTR) {
    return reinterpret_cast<HANDLE>(
            new bk2::android::wincompat::EventHandle(manualReset != FALSE, initialState != FALSE));
}

inline BOOL SetEvent(HANDLE handle) {
    auto* event = dynamic_cast<bk2::android::wincompat::EventHandle*>(
            bk2::android::wincompat::AsHandle(handle));
    if (event == nullptr) {
        return FALSE;
    }
    event->set();
    return TRUE;
}

inline BOOL ResetEvent(HANDLE handle) {
    auto* event = dynamic_cast<bk2::android::wincompat::EventHandle*>(
            bk2::android::wincompat::AsHandle(handle));
    if (event == nullptr) {
        return FALSE;
    }
    event->reset();
    return TRUE;
}

inline DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    auto* base = bk2::android::wincompat::AsHandle(handle);
    return base == nullptr ? WAIT_FAILED : base->wait(milliseconds);
}

inline BOOL CloseHandle(HANDLE handle) {
    auto* base = bk2::android::wincompat::AsHandle(handle);
    if (base == nullptr) {
        return FALSE;
    }
    delete base;
    return TRUE;
}

inline HANDLE CreateThread(
        LPVOID,
        size_t,
        LPTHREAD_START_ROUTINE startRoutine,
        LPVOID parameter,
        DWORD,
        DWORD* threadId) {
    if (startRoutine == nullptr) {
        return nullptr;
    }
    std::thread worker([startRoutine, parameter] {
        startRoutine(parameter);
    });
    if (threadId != nullptr) {
        *threadId = 0;
    }
    return reinterpret_cast<HANDLE>(
            new bk2::android::wincompat::ThreadHandle(std::move(worker)));
}

inline DWORD GetTickCount() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

inline void Sleep(DWORD millis) {
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

inline void OutputDebugStringA(LPCSTR message) {
    if (message != nullptr) {
        __android_log_write(ANDROID_LOG_DEBUG, "Blitzkrieg2Legacy", message);
    }
}

inline void OutputDebugString(LPCSTR message) {
    OutputDebugStringA(message);
}

inline int MessageBox(HWND, LPCSTR text, LPCSTR caption, UINT) {
    if (caption != nullptr) {
        __android_log_write(ANDROID_LOG_WARN, "Blitzkrieg2Legacy", caption);
    }
    if (text != nullptr) {
        __android_log_write(ANDROID_LOG_WARN, "Blitzkrieg2Legacy", text);
    }
    return 0;
}

inline char* itoa(int value, char* buffer, int radix) {
    if (buffer == nullptr) {
        return nullptr;
    }
    if (radix < 2 || radix > 36) {
        buffer[0] = '\0';
        return buffer;
    }

    const bool negative = value < 0 && radix == 10;
    unsigned int magnitude = 0;
    if (value < 0) {
        magnitude = static_cast<unsigned int>(-(value + 1)) + 1;
    } else {
        magnitude = static_cast<unsigned int>(value);
    }

    char digits[33];
    int count = 0;
    do {
        const unsigned int digit = magnitude % static_cast<unsigned int>(radix);
        digits[count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        magnitude /= static_cast<unsigned int>(radix);
    } while (magnitude != 0);

    char* out = buffer;
    if (negative) {
        *out++ = '-';
    }
    while (count > 0) {
        *out++ = digits[--count];
    }
    *out = '\0';
    return buffer;
}

inline char* gcvt(double value, int digits, char* buffer) {
    if (buffer == nullptr) {
        return nullptr;
    }
    std::snprintf(buffer, 128, "%.*g", digits, value);
    return buffer;
}

inline double _wtof(const wchar_t* value) {
    if (value == nullptr) {
        return 0.0;
    }
    return std::wcstod(value, nullptr);
}

inline UINT GetACP() {
    return CP_UTF8;
}

inline int WideCharToMultiByte(
        UINT codePage,
        DWORD,
        const wchar_t* source,
        int sourceLength,
        char* destination,
        int destinationLength,
        const char*,
        BOOL*) {
    if (source == nullptr) {
        return 0;
    }
    if (sourceLength < 0) {
        sourceLength = static_cast<int>(std::wcslen(source));
    }
    if (destination == nullptr || destinationLength <= 0) {
        return sourceLength;
    }

    int written = 0;
    for (int i = 0; i < sourceLength && written < destinationLength; ++i) {
        const wchar_t ch = source[i];
        if (codePage == CP_UTF8 && ch >= 0x80) {
            if (ch < 0x800 && written + 1 < destinationLength) {
                destination[written++] = static_cast<char>(0xC0 | ((ch >> 6) & 0x1F));
                destination[written++] = static_cast<char>(0x80 | (ch & 0x3F));
            } else if (written + 2 < destinationLength) {
                destination[written++] = static_cast<char>(0xE0 | ((ch >> 12) & 0x0F));
                destination[written++] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                destination[written++] = static_cast<char>(0x80 | (ch & 0x3F));
            }
        } else {
            destination[written++] = static_cast<char>(ch <= 0xFF ? ch : '?');
        }
    }
    return written;
}

inline int MultiByteToWideChar(
        UINT codePage,
        DWORD,
        const char* source,
        int sourceLength,
        wchar_t* destination,
        int destinationLength) {
    if (source == nullptr) {
        return 0;
    }
    if (sourceLength < 0) {
        sourceLength = static_cast<int>(std::strlen(source));
    }
    if (destination == nullptr || destinationLength <= 0) {
        return sourceLength;
    }

    int written = 0;
    for (int i = 0; i < sourceLength && written < destinationLength; ++i) {
        unsigned char ch = static_cast<unsigned char>(source[i]);
        if (codePage == CP_UTF8 && ch >= 0xC0 && i + 1 < sourceLength) {
            const unsigned char ch2 = static_cast<unsigned char>(source[i + 1]);
            if ((ch & 0xE0) == 0xC0 && (ch2 & 0xC0) == 0x80) {
                destination[written++] = static_cast<wchar_t>(((ch & 0x1F) << 6) | (ch2 & 0x3F));
                ++i;
                continue;
            }
            if ((ch & 0xF0) == 0xE0 && i + 2 < sourceLength) {
                const unsigned char ch3 = static_cast<unsigned char>(source[i + 2]);
                if ((ch2 & 0xC0) == 0x80 && (ch3 & 0xC0) == 0x80) {
                    destination[written++] = static_cast<wchar_t>(
                            ((ch & 0x0F) << 12) | ((ch2 & 0x3F) << 6) | (ch3 & 0x3F));
                    i += 2;
                    continue;
                }
            }
        }
        destination[written++] = static_cast<wchar_t>(ch);
    }
    return written;
}

#define WINAPI
#define CALLBACK
#define __cdecl
#define __stdcall
#define __forceinline inline
#define __declspec(x)

#ifndef interface
#define interface struct
#endif

#endif  // defined(BK2_ANDROID)
