#pragma once

namespace bk2::android {

bool InitializeLegacyVfs();
void ShutdownLegacyVfs();
bool IsLegacyVfsInitialized();

}  // namespace bk2::android
