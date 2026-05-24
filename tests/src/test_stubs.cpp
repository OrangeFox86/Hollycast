#if !defined(__ANDROID__) && !defined(__APPLE__)
#include <cstdlib>
#include "oslib/oslib.h"

[[noreturn]] void os_DebugBreak()
{
	std::abort();
}

void os_DoEvents()
{
}

void os_RunInstance(int argc, const char *argv[])
{
}

os_Process os_Process::start(const std::string& executable, const std::vector<std::string>& args)
{
	return os_Process();
}

bool os_Process::isRunning()
{
	return false;
}

void os_Process::terminate()
{
}

#ifdef _WIN32
void os_SetThreadName(const char *name)
{
}
const char *getThreadName()
{
	return "threadname";
}
#endif
#endif
