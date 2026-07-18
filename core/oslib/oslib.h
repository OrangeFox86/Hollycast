// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "types.h"
#include <string>
#include <vector>
#if defined(DREAMPOTATO_INTEGRATED_MODE) && !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(__SWITCH__)
#include <malloc.h>
#endif

void os_DoEvents();
void os_CreateWindow();
void os_DestroyWindow();
void os_SetupInput();
void os_TermInput();
void os_UpdateInputState();
void os_InstallFaultHandler();
void os_UninstallFaultHandler();
void os_RunInstance(int argc, const char *argv[]);
void os_SetThreadName(const char *name);
void os_notify(const char *msg, int durationMs = 2000, const char *details = nullptr);

#ifdef DREAMPOTATO_INTEGRATED_MODE
//! Get the containing directory of the current '.app'/'AppImage' bundle, if applicable, or of the current executable
std::string os_GetAppContainingDir();

// Cross-platform process handle.
class os_Process
{
public:
	bool isValid() const {
#ifdef _WIN32
		return handle != nullptr;
#else
		return pid > 0;
#endif
	}

	static os_Process start(const std::string& executable, const std::vector<std::string>& args = {});
	bool isRunning();
	void terminate();

private:
#ifdef _WIN32
	void *handle = nullptr; // HANDLE
#else
	pid_t pid = -1;
#endif
};
#endif // DREAMPOTATO_INTEGRATED_MODE

// raii thread name setter
class ThreadName
{
public:
	ThreadName(const char *name) {
		os_SetThreadName(name);
	}
	~ThreadName() {
		// default name
		os_SetThreadName("flycast");
	}
};

#ifdef _MSC_VER
#include <intrin.h>
#endif

u32 static inline bitscanrev(u32 v)
{
#ifdef __GNUC__
	return 31-__builtin_clz(v);
#else
	unsigned long rv;
	_BitScanReverse(&rv,v);
	return rv;
#endif
}

namespace hostfs
{
	bool isConfiguredVmuFileNameValid(const std::string& name);
	std::string getVmuPath(const std::string& port, bool save);
#ifdef DREAMPOTATO_INTEGRATED_MODE
	std::string getDreamPotatoPath();
#endif

	std::string getArcadeFlashPath();

	std::string findFlash(const std::string& prefix, const std::string& names);
	std::string getFlashSavePath(const std::string& prefix, const std::string& name);
	std::string findNaomiBios(const std::string& name);

	std::string getSavestatePath(int index, bool writable);

	std::string getTextureLoadPath(const std::string& gameId);
	std::string getTextureDumpPath();

	std::string getShaderCachePath(const std::string& filename);
	void saveScreenshot(const std::string& name, const std::vector<u8>& data);

	const std::vector<std::string>& getCdromDrives();
#ifdef __ANDROID__
	void importHomeDirectory();
	void exportHomeDirectory();
#endif
}

static inline void *allocAligned(size_t alignment, size_t size)
{
#ifdef _WIN32
	return _aligned_malloc(size, alignment);
#elif defined(__SWITCH__)
   return memalign(alignment, size);
#else
	void *data;
	if (posix_memalign(&data, alignment, size) != 0)
		return nullptr;
	else
		return data;
#endif
}

static inline void freeAligned(void *p)
{
#ifdef _WIN32
	_aligned_free(p);
#else
	free(p);
#endif
}

void registerCrash(const char *directory, const char *path);
void uploadCrashes(const std::string& directory);
