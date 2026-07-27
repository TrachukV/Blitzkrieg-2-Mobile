// stdafx.h : include file for standard system include files,
//	or project specific include files that are used frequently, but
//			are changed infrequently
//

#if !defined(AFX_STDAFX_H__A9DB83DB_A9FD_11D0_BFD1_444553540000__INCLUDED_)
#define AFX_STDAFX_H__A9DB83DB_A9FD_11D0_BFD1_444553540000__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#ifndef __AFX__
#define WIN32_LEAN_AND_MEAN							// Exclude rarely-used stuff from Windows headers

#define _WIN32_WINNT 0x400
#if defined(BK2_ANDROID)
#include "bk2_legacy_win_compat.h"
#else
#include <windows.h>
#endif
#if defined(BK2_ANDROID)
#include <typeinfo>
using std::type_info;
#else
#include <typeinfo.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#else
//#define _STLP_USE_MFC 1

#include <afxwin.h>											// MFC core and standard components
#include <afxext.h>											// MFC extensions
#include <afxdtctl.h>										// MFC support for Internet Explorer 4 Common Controls
#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>											// MFC support for Windows Common Controls
#endif // _AFX_NO_AFXCMN_SUPPORT
#include <comutil.h>
#endif // __AFX__

#if !defined(BK2_ANDROID)
#pragma component( mintypeinfo, on )
#endif
#include <math.h>
#include <memory.h>
#include <string.h>
// 
#include "..\Misc\Asserts.h"
//
#if !defined(BK2_ANDROID)
#pragma warning( disable: 4018 4355 4800 4244 4267 )
#pragma warning( disable: 4127 4100 4201 4512 4389 )
#endif
#ifdef NIVAL_DLL
#pragma warning( disable: 4273)
#endif

#include "..\Misc\nlist.h"
#if !defined(BK2_ANDROID)
#pragma component( mintypeinfo, off )
#endif
#include "..\Misc\nstring.h"
#include "..\Misc\nvector.h"
#if !defined(BK2_ANDROID)
#pragma component( mintypeinfo, on )
#endif
#include "..\Misc\nhash_map.h"
#include "..\Misc\nhash_set.h"
#include "..\Misc\nset.h"
#if !defined(BK2_ANDROID)
#pragma component( mintypeinfo, off )
#endif
//#pragma warning( disable : 4503 4018 4786 4800 4290 4146 4244 4284 4267 )

using namespace nstl;

#include "..\Misc\nhelpdebug.h"

//
#if defined(BK2_ANDROID)
typedef long long int64;									// due to lack of 'long long' type support
#else
typedef __int64 int64;									// due to lack of 'long long' type support
typedef unsigned __int64 QWORD;					// quadra word
#endif
#define for if(false); else for					// to achive standard variable scope resolving, declared inside 'for'

// define 'interface' keyword
#ifndef interface
#define interface struct
#endif // interface
// define pragma once
#if _MSC_VER > 1000
#define ONCE once
#else
#define ONCE message ""
#endif // _MSC_VER > 1000
//
namespace NTimer
{
	typedef DWORD STime;
};
//
#include "../System/System.h"
#include "../Misc/Tools.h"
#include "../System/Basic.h"
#include "../Misc/Geom.h"
#include "../System/Streams.h"
#include "../System/BinSaver.h"
#include "../System/GlobalVars.h"
#include "../System/ConsoleBuffer.h"
#include "../System/LogStream.h"
#include "../System/DB.h"
// in the file 'Specific.h' one can define ow n project-specific includes
#if !(defined(BK2_ANDROID) && defined(BK2_LEGACY_DB_TYPE_SOURCES_ENABLED))
#include "Specific.h"
#endif

// TODO: reference additional headers your program requires here

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_STDAFX_H__A9DB83DB_A9FD_11D0_BFD1_444553540000__INCLUDED_)
