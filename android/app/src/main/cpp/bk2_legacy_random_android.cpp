#include "System/stdafx.h"

#include "System/RandomGen.h"
#include "System/Streams.h"
#include "System/XmlSaver.h"
#include "Misc/Win32Random.h"

namespace {

class CAndroidRandomSeed : public IRandomSeed {
public:
    explicit CAndroidRandomSeed(UINT seed = 0) : state(seed) {}

    void Init() override {
        state = GetTickCount();
        if (state == 0) {
            state = 0x6d2b79f5U;
        }
    }

    void InitByZeroSeed() override {
        state = 0x6d2b79f5U;
    }

    void Store(CDataStream* stream) override {
        stream->Write(&state, sizeof(state));
    }

    void Restore(CDataStream* stream) override {
        stream->Read(&state, sizeof(state));
    }

    int operator&(IXmlSaver&) override {
        return 0;
    }

    UINT GetState() const {
        return state;
    }

    void SetState(UINT seed) {
        state = seed;
    }

    int GetSizeOf() const override {
        return sizeof(CAndroidRandomSeed);
    }

protected:
    void DestroyContents() override {
        const int refs = nRefData;
        const int objs = nObjData;
        this->~CAndroidRandomSeed();
        new (this) CAndroidRandomSeed();
        nRefData += refs;
        nObjData += objs;
    }

    CObjectBase* MakeCopy() const override {
        return new CAndroidRandomSeed(state);
    }

    ~CAndroidRandomSeed() override = default;

private:
    UINT state;
};

UINT g_randomState = 0x6d2b79f5U;
NRandom::SRandomFunc g_randomFunc;

UINT NextRandomValue() {
    UINT x = g_randomState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_randomState = x == 0 ? 0x6d2b79f5U : x;
    return g_randomState;
}

}  // namespace

void NRandom::SetRandomSeed(IRandomSeed* seed) {
    if (CAndroidRandomSeed* androidSeed = checked_cast<CAndroidRandomSeed*>(seed)) {
        g_randomState = androidSeed->GetState();
    }
}

IRandomSeed* NRandom::CreateRandomSeedCopy() {
    return new CAndroidRandomSeed(g_randomState);
}

UINT NRandom::Random() {
    return NextRandomValue();
}

const NRandom::SRandomFunc& NRandom::RndFunc() {
    return g_randomFunc;
}

namespace NWin32Random {
namespace {

int g_seed = 0x6d2b79f5;
SRandomFunc g_win32_random_func;

}  // namespace

void Seed(const int seed) {
    g_seed = seed;
    g_randomState = static_cast<UINT>(seed);
    if (g_randomState == 0) {
        g_randomState = 0x6d2b79f5U;
    }
}

int GetSeed() {
    return g_seed;
}

unsigned int Random() {
    return NextRandomValue() & static_cast<unsigned int>(RAND_MAX);
}

const SRandomFunc& RndFunc() {
    return g_win32_random_func;
}

}  // namespace NWin32Random
