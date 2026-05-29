//
//  osx-main.cpp
//  emulator-osx
//
//  Created by admin on 8/5/15.
//  Copyright (c) 2015 reicast. All rights reserved.
//
#import <Carbon/Carbon.h>
#import <AppKit/AppKit.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <mach/task.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>

#include "types.h"
#include "cfg/option.h"
#include "log/LogManager.h"
#if defined(USE_SDL)
#include "sdl/sdl.h"
#endif
#include "stdclass.h"
#include "oslib/oslib.h"
#include "oslib/i18n.h"
#include "emulator.h"
#include "ui/mainui.h"
#include "ui/gui.h"
#include "SDLApplicationDelegate.h"
#include <future>
#include <exception>

int darw_printf(const char* text, ...)
{
    va_list args;

    char temp[2048];
    va_start(args, text);
    vsnprintf(temp, sizeof(temp), text, args);
    va_end(args);
    
    NSString* log = [NSString stringWithCString:temp encoding: NSUTF8StringEncoding];
    NSDictionary<NSString *, NSString *>* env = [[NSProcessInfo processInfo] environment];
    static bool isXcode = [env[@"OS_ACTIVITY_DT_MODE"] boolValue] || [env[@"COMMAND_MODE"] isEqualToString:@"unix2003"] || [env[@"TERM"] isEqualToString:@"dumb"];
	
    if (isXcode) // Xcode console does not support colors
    {
        log = [log stringByReplacingOccurrencesOfString:@"\x1b[0m" withString:@""];
        log = [log stringByReplacingOccurrencesOfString:@"\x1b[92m" withString:@"ℹ️ "];
        log = [log stringByReplacingOccurrencesOfString:@"\x1b[91m" withString:@"⚠️ "];
        log = [log stringByReplacingOccurrencesOfString:@"\x1b[93m" withString:@"🛑 "];
    }
    NSLog(@"%@", log);

    return 0;
}

void os_DoEvents() {
#if defined(USE_SDL)
	NSMenuItem *editMenuItem = [[NSApp mainMenu] itemAtIndex:1];
	[editMenuItem setEnabled:SDL_IsTextInputActive()];

	NSMenuItem *toggleMenuItem = [[[[NSApp mainMenu] itemAtIndex:0] submenu] itemWithTag:MENU_TAG_TOGGLE_MENU];
	if (toggleMenuItem) {
		[toggleMenuItem setEnabled:emu.running() || gui_state == GuiState::Commands];
	}
#endif
}

static int emu_flycast_init();

static void emu_flycast_term()
{
	flycast_term();
	LogManager::Shutdown();
}

extern "C" int SDL_main(int argc, char *argv[])
{
    char *home = getenv("HOME");
    if (home != NULL)
    {
        std::string config_dir = std::string(home) + "/.flycast/";
		if (!file_exists(config_dir))
			config_dir = std::string(home) + "/Library/Application Support/Flycast/";
		
		/* Different config folder for multiple instances */
		if (getppid() == 1)
		{
			int instanceNumber = (int)[[NSRunningApplication runningApplicationsWithBundleIdentifier:[[NSBundle mainBundle] bundleIdentifier]] count];
			if (instanceNumber > 1)
			{
				config_dir += std::to_string(instanceNumber) + "/";
				[[NSApp dockTile] setBadgeLabel:@(instanceNumber).stringValue];
			}
		}

        mkdir(config_dir.c_str(), 0755); // create the directory if missing
        set_user_config_dir(config_dir);
        add_system_data_dir(config_dir);
        config_dir += "data/";
        mkdir(config_dir.c_str(), 0755);
        set_user_data_dir(config_dir);
    }
    else
    {
        set_user_config_dir("./");
        set_user_data_dir("./");
    }
    // Add bundle resources path
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
    char path[PATH_MAX];
    if (CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8 *)path, PATH_MAX))
        add_system_data_dir(std::string(path) + "/");
    CFRelease(resourcesURL);
    CFRelease(mainBundle);

	if (emu_flycast_init() != 0)
		return 1;
	
	int boardId = config::loadInt("naomi", "BoardId");
	if (boardId > 0)
	{
		NSString *label = @"S";		// Slave
		if (config::MultiboardSlaves == 2) // 1 = Single, 2 = Deluxe
		{
			switch (boardId) {
				case 1:
					label = @"C";	// Center
					break;
				case 2:
					label = @"L";	// Left
					break;
				case 3:
					label = @"R";	// Right
			}
		}
		[[NSApp dockTile] setBadgeLabel:label];
	}
	
#ifdef USE_BREAKPAD
	auto async = std::async(std::launch::async, uploadCrashes, "/tmp");
#endif

	try {
		mainui_loop();
	} catch (const std::exception& e) {
		ERROR_LOG(BOOT, "mainui_loop error: %s", e.what());
	} catch (...) {
		ERROR_LOG(BOOT, "mainui_loop unknown exception");
	}

	emu_flycast_term();
	os_UninstallFaultHandler();

	return 0;
}

static int emu_flycast_init()
{
	LogManager::Init();
	i18n::init();
	os_InstallFaultHandler();
	NSArray *arguments = [[NSProcessInfo processInfo] arguments];
	unsigned long argc = [arguments count];
	char **argv = (char **)malloc(argc * sizeof(char*));
	int paramCount = 0;
	for (unsigned long i = 0; i < argc; i++)
	{
		const char *arg = [[arguments objectAtIndex:i] UTF8String];
		if (!strncmp(arg, "-psn_", 5))
			// ignore Process Serial Number argument on first launch
			continue;
		argv[paramCount++] = strdup(arg);
	}
	
	int rc = flycast_init(paramCount, argv);
	
	for (unsigned long i = 0; i < paramCount; i++)
		free(argv[i]);
	free(argv);

#if defined(DEBUG) || defined(DEBUGFAST)
    int ret = task_set_exception_ports(
                                       mach_task_self(),
                                       EXC_MASK_BAD_ACCESS,
                                       MACH_PORT_NULL,
                                       EXCEPTION_DEFAULT,
                                       0);
    
    if (ret != KERN_SUCCESS) {
        printf("task_set_exception_ports: %s\n", mach_error_string(ret));
    }
#endif
	
	return rc;
}

std::string os_PrecomposedString(std::string string){
    return [[[NSString stringWithUTF8String:string.c_str()] precomposedStringWithCanonicalMapping] UTF8String];
}

void os_RunInstance(int argc, const char *argv[])
{
	if (fork() == 0)
	{
		std::vector<char *> localArgs;
		NSArray *arguments = [[NSProcessInfo processInfo] arguments];
		const char *selfPath = [[arguments objectAtIndex:0] UTF8String];
		localArgs.push_back((char *)selfPath);
		for (int i = 0; i < argc; i++)
			localArgs.push_back((char *)argv[i]);
		localArgs.push_back(nullptr);
		execv(selfPath, &localArgs[0]);
		ERROR_LOG(BOOT, "Error %d launching Flycast instance %s", errno, selfPath);
		die("execv failed");
	}
}

#ifdef DREAMPOTATO_INTEGRATED_MODE
std::string os_GetAppContainingDir()
{
	NSBundle *bundle = [NSBundle mainBundle];
	if (bundle != nil)
	{
		// path to the .app folder
		NSString *bundlePath = [bundle bundlePath];
		if (bundlePath != nil && [bundlePath length] > 0)
			return [[[bundlePath stringByDeletingLastPathComponent] stringByStandardizingPath] UTF8String];
	}

	// Not packaged in .app. Return the directory containing the executable.
	NSArray *arguments = [[NSProcessInfo processInfo] arguments];
	if ([arguments count] == 0)
		return "";

	NSString *selfPath = [arguments objectAtIndex:0];
	return [[[selfPath stringByDeletingLastPathComponent] stringByStandardizingPath] UTF8String];
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

#import <Syphon/Syphon.h>
#import <cfg/cfg.h>
static SyphonOpenGLServer* syphonGLServer;

void os_VideoRoutingPublishFrameTexture(GLuint texID, GLuint texTarget, float w, float h)
{
	if (syphonGLServer == NULL)
	{
		int boardID = config::loadInt("naomi", "BoardId");
		syphonGLServer = [[SyphonOpenGLServer alloc] initWithName:[NSString stringWithFormat:(boardID == 0 ? @"Video Content" : @"Video Content - %d"), boardID] context:[SDL_GL_GetCurrentContext() CGLContextObj] options:nil];
	}
	CGLLockContext([syphonGLServer context]);
	[syphonGLServer publishFrameTexture:texID textureTarget:texTarget imageRegion:NSMakeRect(0, 0, w, h) textureDimensions:NSMakeSize(w, h) flipped:NO];
	CGLUnlockContext([syphonGLServer context]);
}

void os_VideoRoutingTermGL()
{
	[syphonGLServer stop];
	[syphonGLServer release];
	syphonGLServer = NULL;
}

#ifdef USE_VULKAN
#include "rend/vulkan/vulkan.h"

static SyphonMetalServer* syphonMtlServer;

void os_VideoRoutingPublishFrameTexture(const vk::Device& device, const vk::Image& image, const vk::Queue& queue, float x, float y, float w, float h)
{
	if (syphonMtlServer == NULL)
	{
		vk::ExportMetalDeviceInfoEXT deviceInfo;
		auto objectsInfo = vk::ExportMetalObjectsInfoEXT(&deviceInfo);
		device.exportMetalObjectsEXT(&objectsInfo);
		
		int boardID = config::loadInt("naomi", "BoardId");
		syphonMtlServer = [[SyphonMetalServer alloc] initWithName:[NSString stringWithFormat:(boardID == 0 ? @"Video Content" : @"Video Content - %d"), boardID] device:deviceInfo.mtlDevice options:nil];
	}
	
	auto textureInfo = vk::ExportMetalTextureInfoEXT(image);
	auto commandInfo = vk::ExportMetalCommandQueueInfoEXT(queue);
	commandInfo.pNext = &textureInfo;
	auto objectsInfo = vk::ExportMetalObjectsInfoEXT(&commandInfo);
	device.exportMetalObjectsEXT(&objectsInfo);
	
	auto commandBuffer = [commandInfo.mtlCommandQueue commandBufferWithUnretainedReferences];
	[syphonMtlServer publishFrameTexture:textureInfo.mtlTexture onCommandBuffer:commandBuffer imageRegion:NSMakeRect(x, y, w, h) flipped:YES];
	[commandBuffer commit];
}

void os_VideoRoutingTermVk()
{
	[syphonMtlServer stop];
	[syphonMtlServer release];
	syphonMtlServer = NULL;
}
#endif

namespace hostfs
{

std::string getScreenshotsPath()
{
	NSArray *paths = NSSearchPathForDirectoriesInDomains(NSPicturesDirectory, NSUserDomainMask, YES);
	return [[paths objectAtIndex:0] UTF8String];
}

}

namespace i18n
{

std::string getSystemLocale()
{
	return [[[NSLocale preferredLanguages] objectAtIndex:0] UTF8String];
}

}
