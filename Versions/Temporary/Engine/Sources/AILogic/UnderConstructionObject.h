#pragma once
#if defined(BK2_ANDROID)
#include "../Stats_B2_M1/ActionCommand.h"
#endif

class CLongObjectCreation;
class CGivenPassabilityStObject;
struct SAIObjectsUnderConstructionUpdate;
class CAILogic;
#if !defined(BK2_ANDROID)
enum EActionCommand;
#endif
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// stores objects under construction (that player is being ordered to build)
// untill played issues command to actually build this object
// also hold update about these objects
class CUnderConstructionObject
{
	void SendClearUpdate();

public:
	void Clear();
	void ShowUnderConstruction( EActionCommand eCommand, const CVec2 &vStart, const CVec2 &vFinish, bool bFinished, CAILogic *pAI );
	int operator&( IBinSaver &saver );
};
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
