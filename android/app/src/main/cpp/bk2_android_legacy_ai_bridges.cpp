#include "../../../../../Versions/Temporary/Engine/Sources/AILogic/stdafx.h"

#include "../../../../../Versions/Temporary/Engine/Sources/AILogic/AIDebugInfo.h"
#include "../../../../../Versions/Temporary/Engine/Sources/AILogic/BalanceTest.h"
#include "../../../../../Versions/Temporary/Engine/Sources/Input/Bind.h"
#include "../../../../../Versions/Temporary/Engine/Sources/Misc/HPTimer.h"

#include "bk2_android_legacy_game_runtime.h"

#include <chrono>

namespace {

using Clock = std::chrono::steady_clock;

NHPTimer::STime ClockNow()
{
    return static_cast<NHPTimer::STime>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now().time_since_epoch())
                    .count());
}

}

namespace NHPTimer {

double GetSeconds(const STime& value)
{
    return static_cast<double>(value) * 1.0e-9;
}

void GetTime(STime* time)
{
    if (time != nullptr)
    {
        *time = ClockNow();
    }
}

double GetTimePassed(STime* time)
{
    if (time == nullptr)
    {
        return 0.0;
    }
    const STime previous = *time;
    *time = ClockNow();
    return GetSeconds(*time - previous);
}

double GetClockRate()
{
    return 1.0e-9;
}

void UpdateHPTimerFrequency()
{
}

}

void CBalanceTest::SegmentBalanceTest()
{
}

void CBalanceTest::InitBalanceTest(const NDb::SMapInfo*)
{
    Clear();
}

const NDb::SUnitStatsModifier* CBalanceTest::GetModifier(int) const
{
    return nullptr;
}

void CBalanceTest::UnitDead(CAIUnit*)
{
}

namespace NAIVisInfo {

void AddProfile(
        int,
        const CVec3&,
        WORD,
        const NDb::SPassProfile&)
{
}

void RemoveProfile(int)
{
}

void ToggleLockProfiles()
{
}

}

namespace NInput {

void PostEvent(const string& eventName, int, int)
{
    bk2::android::HandleLegacyInputEvent(eventName.c_str());
}

}
