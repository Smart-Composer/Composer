#include "TestProcess.h"

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <crtdbg.h>
 #include <cstdlib>
 #include <initializer_list>
#endif

namespace composer::tests
{

void reportFailuresWithoutDialogs()
{
#if defined(_WIN32)
    // An unattended run has nobody to dismiss a dialog: send C runtime reports to stderr and
    // let crashes terminate the process instead of waiting for Windows Error Reporting.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

   #if defined(_DEBUG)
    // Only the debug C runtime raises these reports.
    for (const int reportType : { _CRT_ERROR, _CRT_ASSERT })
    {
        _CrtSetReportMode(reportType, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(reportType, _CRTDBG_FILE_STDERR);
    }
   #endif
#endif
}

} // namespace composer::tests
