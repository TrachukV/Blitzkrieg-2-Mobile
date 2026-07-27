#include "System/stdafx.h"

namespace bk2::android {

DWORD ProbeLegacySystemStdafx() {
    OutputDebugString("Legacy System/stdafx.h compiled for Android.");
    Sleep(0);
    return GetTickCount();
}

}  // namespace bk2::android
