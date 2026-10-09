#pragma once

namespace composer::tests
{

/** Makes C runtime reports and crashes fail an unattended test process instead of opening
    dialogs that wait for someone to dismiss them. */
void reportFailuresWithoutDialogs();

} // namespace composer::tests
