#include "bk2_android_database.h"

#include "GameX/stdafx.h"
#include "GameX/DBConsts.h"
#include "GameX/DBGameRoot.h"
#include "GameX/DBMPConsts.h"
#include "GameX/GetConsts.h"
#include "AILogic/DBAIConsts.h"
#include "Main/DBNetConsts.h"
#include "Misc/StrProc.h"
#include "SceneB2/DBSceneConsts.h"
#include "Stats_B2_M1/DBClientConsts.h"
#include "UISpecificB2/DBUISpecificB2.h"
#include "libdb/Db.h"

namespace NGameX {
namespace {

template <class T>
const T* GetConstsFromGlobal(const char* name) {
    if (!bk2::android::IsLegacyDatabaseOpen()) {
        return 0;
    }

    const string value = NStr::ToMBCS(NGlobal::GetVar(name, ""));
    if (value.empty() || NStr::IsDecNumber(value)) {
        return 0;
    }

    return NDb::Get<T>(CDBID(value));
}

template <class T>
const T* GetConstsFromDbid(const char* dbid) {
    if (!bk2::android::IsLegacyDatabaseOpen()) {
        return 0;
    }

    const CDBID id(dbid);
    if (!NDb::DoesObjectExist(id)) {
        return 0;
    }

    return NDb::Get<T>(id);
}

}  // namespace

const NDb::SGameRoot* GetGameRoot() {
    if (const NDb::SGameRoot* consts = GetConstsFromGlobal<NDb::SGameRoot>("game_root")) {
        return consts;
    }
    return GetConstsFromDbid<NDb::SGameRoot>("GameRoot.xdb");
}

const NDb::SGameConsts* GetGameConsts() {
    if (const NDb::SGameConsts* consts = GetConstsFromGlobal<NDb::SGameConsts>("game_consts")) {
        return consts;
    }
    if (const NDb::SGameRoot* root = GetGameRoot()) {
        return root->pConsts;
    }
    return 0;
}

const NDb::SSceneConsts* GetSceneConsts() {
    if (const NDb::SSceneConsts* consts = GetConstsFromGlobal<NDb::SSceneConsts>("scene_consts")) {
        return consts;
    }
    if (const NDb::SGameConsts* game_consts = GetGameConsts()) {
        return game_consts->pScene;
    }
    return 0;
}

const NDb::SClientGameConsts* GetClientConsts() {
    if (const NDb::SClientGameConsts* consts =
            GetConstsFromGlobal<NDb::SClientGameConsts>("client_consts")) {
        return consts;
    }
    if (const NDb::SGameConsts* game_consts = GetGameConsts()) {
        return game_consts->pClient;
    }
    return 0;
}

const NDb::SUIConstsB2* GetUIConsts() {
    if (const NDb::SUIConstsB2* consts = GetConstsFromGlobal<NDb::SUIConstsB2>("ui_consts")) {
        return consts;
    }
    if (const NDb::SGameConsts* game_consts = GetGameConsts()) {
        return static_cast_ptr<const NDb::SUIConstsB2*>(game_consts->pUI);
    }
    return 0;
}

const NDb::SAIGameConsts* GetAIConsts() {
    if (const NDb::SAIGameConsts* consts = GetConstsFromGlobal<NDb::SAIGameConsts>("ai_consts")) {
        return consts;
    }
    if (const NDb::SGameConsts* game_consts = GetGameConsts()) {
        return game_consts->pAI;
    }
    return 0;
}

const NDb::SNetGameConsts* GetNetConsts() {
    if (const NDb::SNetGameConsts* consts =
            GetConstsFromGlobal<NDb::SNetGameConsts>("net_consts")) {
        return consts;
    }
    if (const NDb::SGameConsts* game_consts = GetGameConsts()) {
        return game_consts->pNet;
    }
    return 0;
}

const NDb::SMultiplayerConsts* GetMPConsts() {
    if (const NDb::SMultiplayerConsts* consts =
            GetConstsFromGlobal<NDb::SMultiplayerConsts>("mp_consts")) {
        return consts;
    }
    if (const NDb::SGameConsts* game_consts = GetGameConsts()) {
        return game_consts->pMultiplayer;
    }
    return 0;
}

}  // namespace NGameX
