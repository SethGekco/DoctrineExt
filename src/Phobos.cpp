#include <Phobos.h>

// Static member definitions required by Container.h and other Phobos utilities.
// AcademyExt links a handful of Phobos utility headers (Container, TemplateDef,
// Debug, Stream, Patch) but is NOT Phobos and does not run its lifecycle, so
// these are minimal stubs -- only what our project actually needs.

HANDLE Phobos::hInstance = nullptr;

char Phobos::readBuffer[Phobos::readLength];
wchar_t Phobos::wideBuffer[Phobos::readLength];

// Lifecycle methods we deliberately do not use.
void Phobos::CmdLineParse(char**, int) { }
void Phobos::ExeRun() { }
void Phobos::ExeTerminate() { }
