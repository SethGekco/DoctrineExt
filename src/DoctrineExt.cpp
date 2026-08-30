#include "DoctrineExt.h"

#include <Phobos.h>
#include <Syringe.h>
#include <Utilities/Patch.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

HANDLE DoctrineExtDLL::hInstance = nullptr;

char DoctrineExtDLL::readBuffer[DoctrineExtDLL::readLength];
wchar_t DoctrineExtDLL::wideBuffer[DoctrineExtDLL::readLength];

void DoctrineExtDLL::ExeRun()
{
	Patch::ApplyStatic();
}

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		DoctrineExtDLL::hInstance = hInstance;
		Phobos::hInstance = hInstance; // needed by Patch::ApplyStatic
	}
	return true;
}

SYRINGE_HANDSHAKE(pInfo)
{
	pInfo->Message = const_cast<char*>("DoctrineExt");
	return S_OK;
}

// Main-loop entry, so static patches apply at the right time.
DEFINE_HOOK(0x7CD810, DoctrineExt_ExeRun, 0x9)
{
	DoctrineExtDLL::ExeRun();
	return 0;
}

// Flush the deferred debug log once the command line has been parsed.
DEFINE_HOOK(0x52F639, DoctrineExt_CmdLineParse, 0x5)
{
	Debug::LogDeferredFinalize();
	return 0;
}
