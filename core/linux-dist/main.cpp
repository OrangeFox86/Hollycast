// Portions Copyright 2026 The Hollycast Authors
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS 1
#endif
#include "types.h"

#if defined(__unix__) || defined(__HAIKU__)
#include "log/LogManager.h"
#include "emulator.h"
#include "ui/mainui.h"
#include "oslib/directory.h"
#include "oslib/oslib.h"
#include "oslib/i18n.h"
#include "stdclass.h"

#include <csignal>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>
#include <exception>

#if defined(SUPPORT_X11)
	#include "x11.h"
#endif

#if defined(USE_SDL)
	#include "sdl/sdl.h"
#endif

#ifdef USE_BREAKPAD
#include "breakpad/client/linux/handler/exception_handler.h"
#endif

void os_DoEvents()
{
	#if defined(SUPPORT_X11)
		input_x11_handle();
		event_x11_handle();
	#endif
}

void common_linux_setup();

// Find the user config directory.
// $HOME/.config/flycast on linux
static std::string find_user_config_dir()
{
	std::string xdg_home;
	if (nowide::getenv("XDG_CONFIG_HOME") != nullptr)
		// If XDG_CONFIG_HOME is set explicitly, we'll use that instead of $HOME/.config
		xdg_home = (std::string)nowide::getenv("XDG_CONFIG_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		/* If $XDG_CONFIG_HOME is not set, we're supposed to use "$HOME/.config" instead.
		 * Consult the XDG Base Directory Specification for details:
		 *   http://standards.freedesktop.org/basedir-spec/basedir-spec-latest.html#variables
		 */
		xdg_home = (std::string)nowide::getenv("HOME") + "/.config";

	if (!xdg_home.empty())
	{
		std::string fullpath = xdg_home + "/flycast/";
		struct stat info;
		if (flycast::stat(fullpath.c_str(), &info) != 0 || (info.st_mode & S_IFDIR) == 0)
			// Create .config/flycast
			flycast::mkdir(fullpath.c_str(), 0755);

		return fullpath;
	}
	// Unable to detect config dir, use the current folder
	return ".";
}

// Find the user data directory.
// $HOME/.local/share/flycast on linux
static std::string find_user_data_dir()
{
	std::string xdg_home;
	if (nowide::getenv("XDG_DATA_HOME") != nullptr)
		// If XDG_DATA_HOME is set explicitly, we'll use that instead of $HOME/.local/share
		xdg_home = (std::string)nowide::getenv("XDG_DATA_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		/* If $XDG_DATA_HOME is not set, we're supposed to use "$HOME/.local/share" instead.
		 * Consult the XDG Base Directory Specification for details:
		 *   http://standards.freedesktop.org/basedir-spec/basedir-spec-latest.html#variables
		 */
		xdg_home = (std::string)nowide::getenv("HOME") + "/.local/share";

	if (!xdg_home.empty())
	{
		std::string fullpath = xdg_home + "/flycast/";
		struct stat info;
		if (flycast::stat(fullpath.c_str(), &info) != 0 || (info.st_mode & S_IFDIR) == 0)
			// Create .local/share/flycast
			flycast::mkdir(fullpath.c_str(), 0755);

		return fullpath;
	}
	// Unable to detect data dir, use the current folder
	return ".";
}

static void addDirectoriesFromPath(std::vector<std::string>& dirs, const std::string& path, const std::string& suffix)
{
	std::string::size_type pos = 0;
	std::string::size_type n = path.find(':', pos);
	while (n != std::string::npos)
	{
		if (n != pos)
			dirs.push_back(path.substr(pos, n - pos) + suffix);
		pos = n + 1;
		n = path.find(':', pos);
	}
	// Separator not found
	if (pos < path.length())
		dirs.push_back(path.substr(pos) + suffix);
}

// Find a file in the user and system config directories.
// The following folders are checked in this order:
// $HOME/.config/flycast
// if XDG_CONFIG_DIRS is defined:
//   <$XDG_CONFIG_DIRS>/flycast
// else
//   /etc/flycast/
//   /etc/xdg/flycast/
// .
static std::vector<std::string> find_system_config_dirs()
{
	std::vector<std::string> dirs;

	std::string xdg_home;
	if (nowide::getenv("XDG_CONFIG_HOME") != nullptr)
		// If XDG_CONFIG_HOME is set explicitly, we'll use that instead of $HOME/.config
		xdg_home = (std::string)nowide::getenv("XDG_CONFIG_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		xdg_home = (std::string)nowide::getenv("HOME") + "/.config";
	if (!xdg_home.empty())
		// XDG config locations
		dirs.push_back(xdg_home + "/flycast/");

	if (nowide::getenv("XDG_CONFIG_DIRS") != nullptr)
	{
		std::string path = (std::string)nowide::getenv("XDG_CONFIG_DIRS");
		addDirectoriesFromPath(dirs, path, "/flycast/");
	}
	else
	{
#ifdef FLYCAST_SYSCONFDIR
		const std::string config_dir(FLYCAST_SYSCONFDIR);
		dirs.push_back(config_dir);
#endif
		dirs.push_back("/etc/flycast/"); // This isn't part of the XDG spec, but much more common than /etc/xdg/
		dirs.push_back("/etc/xdg/flycast/");
	}
	dirs.push_back("./");

	return dirs;
}

// Find a file in the user data directories.
// The following folders are checked in this order:
// $HOME/.local/share/flycast
// if XDG_DATA_DIRS is defined:
//   <$XDG_DATA_DIRS>/flycast
// else
//   /usr/local/share/flycast
//   /usr/share/flycast
// <$FLYCAST_BIOS_PATH>
// ./
// ./data
static std::vector<std::string> find_system_data_dirs()
{
	std::vector<std::string> dirs;

	std::string xdg_home;
	if (nowide::getenv("XDG_DATA_HOME") != nullptr)
		// If XDG_DATA_HOME is set explicitly, we'll use that instead of $HOME/.local/share
		xdg_home = (std::string)nowide::getenv("XDG_DATA_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		xdg_home = (std::string)nowide::getenv("HOME") + "/.local/share";
	if (!xdg_home.empty())
		// XDG data locations
		dirs.push_back(xdg_home + "/flycast/");

	if (nowide::getenv("XDG_DATA_DIRS") != nullptr)
	{
		std::string path = (std::string)nowide::getenv("XDG_DATA_DIRS");
		addDirectoriesFromPath(dirs, path, "/flycast/");
	}
	else
	{
#ifdef FLYCAST_DATADIR
		const std::string data_dir(FLYCAST_DATADIR);
		dirs.push_back(data_dir);
#endif
		dirs.push_back("/usr/local/share/flycast/");
		dirs.push_back("/usr/share/flycast/");
	}
	if (nowide::getenv("FLYCAST_BIOS_PATH") != nullptr)
	{
		std::string path = (std::string)nowide::getenv("FLYCAST_BIOS_PATH");
		addDirectoriesFromPath(dirs, path, "/");
	}
	dirs.push_back("./");
	dirs.push_back("data/");

	return dirs;
}

static const char *selfPath;

void os_RunInstance(int argc, const char *argv[])
{
	if (fork() == 0)
	{
		std::vector<char *> localArgs;
		localArgs.push_back((char *)selfPath);
		for (int i = 0; i < argc; i++)
			localArgs.push_back((char *)argv[i]);
		localArgs.push_back(nullptr);
		execv(selfPath, &localArgs[0]);
	}
}

#ifdef DREAMPOTATO_INTEGRATED_MODE
std::string os_GetAppContainingDir()
{
	const char *appImagePath = nowide::getenv("APPIMAGE");
	if (appImagePath != nullptr && *appImagePath != '\0')
	{
		std::string path(appImagePath);
		size_t pos = get_last_slash_pos(path);
		if (pos != std::string::npos)
			return path.substr(0, pos);
		return ".";
	}

	if (selfPath != nullptr && *selfPath != '\0')
	{
		std::string path(selfPath);
		size_t pos = get_last_slash_pos(path);
		if (pos != std::string::npos)
			return path.substr(0, pos);
	}

	return ".";
}

os_Process os_Process::start(const std::string& executable, const std::vector<std::string>& args)
{
	os_Process proc;
	pid_t pid = fork();
	if (pid == 0)
	{
		// Close open file descriptors except for stdio
		int maxfd = sysconf(_SC_OPEN_MAX);
		for (int fd = STDERR_FILENO + 1; fd < maxfd; fd++)
			close(fd);

		std::vector<char *> cargs;
		cargs.push_back(const_cast<char *>(executable.c_str()));
		for (const auto& arg : args)
			cargs.push_back(const_cast<char *>(arg.c_str()));
		cargs.push_back(nullptr);
		execvp(executable.c_str(), cargs.data());
		_exit(127);
	}
	else if (pid > 0)
	{
		proc.pid = pid;
	}
	else
	{
		WARN_LOG(BOOT, "os_Process::start fork failed: %s", strerror(errno));
	}
	return proc;
}

bool os_Process::isRunning()
{
	if (!isValid())
		return false;
	int status;
	pid_t result = waitpid(pid, &status, WNOHANG);
	if (result == 0)
		return true;
	// Process has exited
	pid = -1;
	return false;
}

void os_Process::terminate()
{
	if (!isValid())
		return;
	kill(pid, SIGTERM);
	int status;
	waitpid(pid, &status, 0);
	pid = -1;
}
#endif // DREAMPOTATO_INTEGRATED_MODE

#if defined(USE_BREAKPAD)
static bool dumpCallback(const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded)
{
	if (succeeded)
		registerCrash(descriptor.directory().c_str(), descriptor.path());

	return succeeded;
}
#endif

int main(int argc, char* argv[])
{
	selfPath = argv[0];
#if defined(USE_BREAKPAD)
	google_breakpad::MinidumpDescriptor descriptor("/tmp");
	google_breakpad::ExceptionHandler eh(descriptor, nullptr, dumpCallback, nullptr, true, -1);
#endif

	LogManager::Init();
	i18n::init();

	// Set directories
	set_user_config_dir(find_user_config_dir());
	set_user_data_dir(find_user_data_dir());
	for (const auto& dir : find_system_config_dirs())
		add_system_config_dir(dir);
	for (const auto& dir : find_system_data_dirs())
		add_system_data_dir(dir);
	INFO_LOG(BOOT, "Config dir is: %s", get_writable_config_path("").c_str());
	INFO_LOG(BOOT, "Data dir is:   %s", get_writable_data_path("").c_str());

#if defined(USE_SDL)
	// init video now: on rpi3 it installs a sigsegv handler(?)
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		die("SDL: Initialization failed!");
	}
#endif

	common_linux_setup();

	if (flycast_init(argc, argv))
		die("Hollycast initialization failed\n");

#if defined(USE_BREAKPAD)
	auto async = std::async(std::launch::async, uploadCrashes, "/tmp");
#endif

	try {
		mainui_loop();
	} catch (const std::exception& e) {
		ERROR_LOG(BOOT, "mainui_loop error: %s", e.what());
	} catch (...) {
		ERROR_LOG(BOOT, "mainui_loop unknown exception");
	}

	flycast_term();
	os_UninstallFaultHandler();

	return 0;
}

[[noreturn]] void os_DebugBreak()
{
	raise(SIGTRAP);
	std::abort();
}

#endif // __unix__
