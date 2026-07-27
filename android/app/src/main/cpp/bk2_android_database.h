#pragma once

namespace bk2::android {

bool InitializeLegacyDatabase();
void ShutdownLegacyDatabase();
bool IsLegacyDatabaseOpen();

}  // namespace bk2::android
