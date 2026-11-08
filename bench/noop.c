/* Smallest possible Windows process: used to measure WSL interop spawn cost. */
#include <windows.h>
void __stdcall start(void) { ExitProcess(0); }
