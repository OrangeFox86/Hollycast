/*   SDLMain.m - main entry point for our Cocoa-ized SDL app
 Initial Version: Darrell Walisser <dwaliss1@purdue.edu>
 Non-NIB-Code & other changes: Max Horn <max@quendi.de>
 Portions Copyright 2026 The Hollycast Authors

 Feel free to customize this file to suit your needs
 */
#include <SDL.h>
#include "SDLApplicationDelegate.h"
#include "emulator.h"
#include <sys/param.h> /* for MAXPATHLEN */
#include <unistd.h>
#include "ui/gui.h"
#include "ui/gui_menu.h"
#include "oslib/oslib.h"
#include <functional>
#include <string>

#ifdef USE_BREAKPAD
#include "client/mac/handler/exception_handler.h"
#endif

/* For some reaon, Apple removed setAppleMenu from the headers in 10.4,
 but the method still is there and works. To avoid warnings, we declare
 it ourselves here. */
@interface NSApplication(SDL_Missing_Methods)
- (void)setAppleMenu:(NSMenu *)menu;
@end

/* Use this flag to determine whether we use CPS (docking) or not */
#define        SDL_USE_CPS        1
#ifdef SDL_USE_CPS
/* Portions of CPS.h */
typedef struct CPSProcessSerNum
{
    UInt32        lo;
    UInt32        hi;
} CPSProcessSerNum;

extern "C" {
    OSErr CPSGetCurrentProcess( CPSProcessSerNum *psn);
    OSErr CPSEnableForegroundOperation( CPSProcessSerNum *psn, UInt32 _arg2, UInt32 _arg3, UInt32 _arg4, UInt32 _arg5);
    OSErr CPSSetFrontProcess( CPSProcessSerNum *psn);
}
#endif /* SDL_USE_CPS */

static NSString *getApplicationName(void)
{
    const NSDictionary *dict;
    NSString *appName = 0;
    
    /* Determine the application name */
    dict = (const NSDictionary *)CFBundleGetInfoDictionary(CFBundleGetMainBundle());
    if (dict)
        appName = [dict objectForKey: @"CFBundleName"];
    
    if (![appName length])
        appName = [[NSProcessInfo processInfo] processName];
    
    return appName;
}

static bool eventIsFKeyWithoutFunction(NSEvent *event)
{
    if (event == nil || [event type] != NSEventTypeKeyDown)
        return false;

    NSString *characters = [event charactersIgnoringModifiers];
    return [characters length] == 1
        && [characters caseInsensitiveCompare:@"f"] == NSOrderedSame
        && ([event modifierFlags] & NSEventModifierFlagFunction) == 0;
}

static void restoreFunctionFullScreenShortcut(NSMenuItem *menuItem)
{
    [menuItem setKeyEquivalent:@"f"];
    [menuItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];
}

static void installFunctionFullScreenShortcutGuard(NSMenuItem *menuItem)
{
    static id monitor = nil;
    if (monitor != nil)
        return;

    monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *event) {
        if (eventIsFKeyWithoutFunction(event))
        {
            [menuItem setKeyEquivalent:@""];
            dispatch_async(dispatch_get_main_queue(), ^{
                restoreFunctionFullScreenShortcut(menuItem);
            });
        }
        return event;
    }] retain];
}

@interface NSApplication (SDLApplication)
@end

@implementation NSApplication (SDLApplication)
/* Invoked from the Quit menu item */
- (void)quitAction:(id)sender
{
    /* Post a SDL_QUIT event */
    SDL_Event event;
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

- (void)undoAction:(id)sender
{
    gui_keyboard_key(0xE3, true); // Cmd
    gui_keyboard_key(0x1D, true); // Z
    gui_keyboard_key(0x1D, false);
    gui_keyboard_key(0xE3, false);
}

- (void)redoAction:(id)sender
{
    gui_keyboard_key(0xE3, true); // Cmd
    gui_keyboard_key(0xE1, true); // Shift
    gui_keyboard_key(0x1D, true); // Z
    gui_keyboard_key(0x1D, false);
    gui_keyboard_key(0xE1, false);
    gui_keyboard_key(0xE3, false);
}

- (void)cutAction:(id)sender
{
    gui_keyboard_key(0xE3, true); // Cmd
    gui_keyboard_key(0x1B, true); // X
    gui_keyboard_key(0x1B, false);
    gui_keyboard_key(0xE3, false);
}

- (void)copyAction:(id)sender
{
    gui_keyboard_key(0xE3, true); // Cmd
    gui_keyboard_key(0x06, true); // C
    gui_keyboard_key(0x06, false);
    gui_keyboard_key(0xE3, false);
}

- (void)pasteAction:(id)sender
{
    gui_keyboard_key(0xE3, true); // Cmd
    gui_keyboard_key(0x19, true); // V
    gui_keyboard_key(0x19, false);
    gui_keyboard_key(0xE3, false);
}

- (void)selectAllAction:(id)sender
{
    gui_keyboard_key(0xE3, true); // Cmd
    gui_keyboard_key(0x04, true); // A
    gui_keyboard_key(0x04, false);
    gui_keyboard_key(0xE3, false);
}


@end

/* The main class of the application, the application's delegate */
@implementation SDLApplicationDelegate

static void runMenuAction(const std::function<void()>& action)
{
	gui_runOnUiThread(action);
}

static NSMenuItem *addMenuItem(NSMenu *menu, NSString *title, SEL action, NSString *keyEquivalent, NSInteger tag)
{
	NSMenuItem *menuItem = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:keyEquivalent ?: @""];
	if (tag != 0)
		[menuItem setTag:tag];
	[menu addItem:menuItem];
	[menuItem release];
	return [menu itemAtIndex:[menu numberOfItems] - 1];
}

static void addMenuSeparator(NSMenu *menu)
{
	[menu addItem:[NSMenuItem separatorItem]];
}

/* Set the working directory to the .app's parent directory */
- (void) setupWorkingDirectory
{
	if([[NSProcessInfo processInfo] environment][@"PWD"] == NULL && [[[NSFileManager defaultManager] currentDirectoryPath] isEqualToString:@"/"])
    {
		chdir([[[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent] cStringUsingEncoding:NSUTF8StringEncoding]);
    }
}

- (void)newInstance:(id)sender
{
    [NSTask launchedTaskWithLaunchPath:@"/usr/bin/open" arguments:@[@"-n", [[NSBundle mainBundle] bundlePath]]];
}

- (void)toggleMenu:(id)sender
{
    gui_open_settings();
}

- (void)setRomDirectory:(id)sender
{
	NSOpenPanel *panel = [NSOpenPanel openPanel];
	[panel setCanChooseFiles:NO];
	[panel setCanChooseDirectories:YES];
	[panel setAllowsMultipleSelection:NO];
	[panel setPrompt:@"Open"];
	if ([panel runModal] != NSModalResponseOK)
		return;

	NSString *path = [[[panel URLs] firstObject] path];
	if (path == nil)
		return;

	std::string selected([path UTF8String]);
	runMenuAction([selected]() { GuiMenu::addRomDirectory(selected); });
}

- (void)rescanRomDirectory:(id)sender
{
	runMenuAction([]() { GuiMenu::rescanRomDirectory(); });
}

- (void)loadRom:(id)sender
{
	NSOpenPanel *panel = [NSOpenPanel openPanel];
	[panel setCanChooseFiles:YES];
	[panel setCanChooseDirectories:NO];
	[panel setAllowsMultipleSelection:NO];
	[panel setPrompt:@"Open"];
	if ([panel runModal] != NSModalResponseOK)
		return;

	NSString *path = [[[panel URLs] firstObject] path];
	if (path == nil)
		return;

	std::string selected([path UTF8String]);
	runMenuAction([selected]() { GuiMenu::loadRomFile(selected); });
}

- (void)saveState:(id)sender
{
	runMenuAction([]() { GuiMenu::saveState(); });
}

- (void)loadState:(id)sender
{
	runMenuAction([]() { GuiMenu::loadState(); });
}

- (void)exitEmulator:(id)sender
{
	runMenuAction([]() { GuiMenu::exitEmulator(); });
}

- (void)pauseResume:(id)sender
{
	runMenuAction([]() { GuiMenu::pauseOrResume(); });
}

- (void)restartGame:(id)sender
{
	runMenuAction([]() { GuiMenu::restartGame(); });
}

- (void)toggleFastForward:(id)sender
{
	runMenuAction([]() { GuiMenu::toggleFastForward(); });
}

- (void)takeScreenshot:(id)sender
{
	runMenuAction([]() { GuiMenu::takeScreenshot(); });
}

- (void)openCheats:(id)sender
{
	runMenuAction([]() { GuiMenu::openCheats(); });
}

- (void)openCustomBoxart:(id)sender
{
	runMenuAction([]() { GuiMenu::openCustomBoxartSettings(); });
}

- (void)openGeneralSettings:(id)sender
{
	runMenuAction([]() { GuiMenu::openGeneralSettings(); });
}

- (void)openVideoSettings:(id)sender
{
	runMenuAction([]() { GuiMenu::openVideoSettings(); });
}

- (void)openAudioSettings:(id)sender
{
	runMenuAction([]() { GuiMenu::openAudioSettings(); });
}

- (void)openControlsSettings:(id)sender
{
	runMenuAction([]() { GuiMenu::openControlsSettings(); });
}

- (void)openNetworkSettings:(id)sender
{
	runMenuAction([]() { GuiMenu::openNetworkSettings(); });
}

- (void)openAdvancedSettings:(id)sender
{
	runMenuAction([]() { GuiMenu::openAdvancedSettings(); });
}

- (void)openAboutHollycast:(id)sender
{
	runMenuAction([]() { GuiMenu::openAboutSettings(); });
}

- (void)openDiscord:(id)sender
{
	[[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://discord.gg/X8YWP8w"]];
}

- (void)reportBug:(id)sender
{
	[[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://github.com/flyinghead/flycast/issues/new/choose"]];
}

- (void)checkForUpdates:(id)sender
{
	[[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://flyinghead.github.io/flycast-builds/"]];
}

- (BOOL)validateMenuItem:(NSMenuItem *)menuItem
{
	switch ([menuItem tag])
	{
	case MENU_TAG_GAME_REQUIRED:
		return GuiMenu::isGameRunning();
	case MENU_TAG_PAUSE_RESUME:
		[menuItem setTitle:(gui_state == GuiState::Closed) ? @"Pause" : @"Resume"];
		return GuiMenu::isGameRunning();
	case MENU_TAG_DISABLED:
		return NO;
	default:
		return YES;
	}
}

static void setApplicationMenu(void)
{
    /* warning: this code is very odd */
    NSMenu *appleMenu;
    NSMenuItem *menuItem;
    NSString *title;
    NSString *appName;
    
    appName = getApplicationName();
    appleMenu = [[NSMenu alloc] initWithTitle:@""];
    
    /* Add menu items */
    title = [@"About " stringByAppendingString:appName];
    [appleMenu addItemWithTitle:title action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    
    [appleMenu addItem:[NSMenuItem separatorItem]];
    
    [appleMenu addItemWithTitle:@"New Instance" action:@selector(newInstance:) keyEquivalent:@"n"];

    NSMenuItem *toggleMenuItem = [appleMenu addItemWithTitle:@"Toggle Menu" action:@selector(toggleMenu:) keyEquivalent:@"M"];
    [toggleMenuItem setTag:MENU_TAG_TOGGLE_MENU];
    [appleMenu setAutoenablesItems:NO];

    [appleMenu addItem:[NSMenuItem separatorItem]];
    
    title = [@"Hide " stringByAppendingString:appName];
    [appleMenu addItemWithTitle:title action:@selector(hide:) keyEquivalent:@"h"];
    
    menuItem = (NSMenuItem *)[appleMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [menuItem setKeyEquivalentModifierMask:(NSEventModifierFlagOption | NSEventModifierFlagCommand)];
    
    [appleMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    
    [appleMenu addItem:[NSMenuItem separatorItem]];
    
    title = [@"Quit " stringByAppendingString:appName];
    [appleMenu addItemWithTitle:title action:@selector(quitAction:) keyEquivalent:@"q"];
    
    
    /* Put menu into the menubar */
    menuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    [menuItem setSubmenu:appleMenu];
    [[NSApp mainMenu] addItem:menuItem];

    /* Tell the application object that this is now the application menu */
    [NSApp setAppleMenu:appleMenu];

    /* Finally give up our references to the objects */
    [appleMenu release];
    [menuItem release];
}

static void setupHollycastFileMenu(void)
{
	NSMenuItem *fileMenuItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
	NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
	addMenuItem(fileMenu, @"Set ROM Directory...", @selector(setRomDirectory:), @"", 0);
	addMenuItem(fileMenu, @"Rescan ROM Directory", @selector(rescanRomDirectory:), @"r", 0);
	addMenuSeparator(fileMenu);
	addMenuItem(fileMenu, @"Load ROM...", @selector(loadRom:), @"o", 0);
	addMenuSeparator(fileMenu);
	addMenuItem(fileMenu, @"Save State", @selector(saveState:), @"s", MENU_TAG_GAME_REQUIRED);
	addMenuItem(fileMenu, @"Load State", @selector(loadState:), @"l", MENU_TAG_GAME_REQUIRED);
	addMenuSeparator(fileMenu);
	addMenuItem(fileMenu, @"Exit Emulator", @selector(exitEmulator:), @"", 0);
	[fileMenuItem setSubmenu:fileMenu];
	[[NSApp mainMenu] addItem:fileMenuItem];
	[fileMenu release];
	[fileMenuItem release];
}

static void setupEditMenu(void)
{
    NSMenuItem *editMenuItem = [[NSMenuItem alloc] initWithTitle:@"Edit" action:nil keyEquivalent:@""];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItemWithTitle:@"Undo" action:@selector(undoAction:) keyEquivalent:@"z"];
    [editMenu addItemWithTitle:@"Redo" action:@selector(redoAction:) keyEquivalent:@"Z"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Cut" action:@selector(cutAction:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"Copy" action:@selector(copyAction:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Paste" action:@selector(pasteAction:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"Select All" action:@selector(selectAllAction:) keyEquivalent:@"a"];
    [editMenuItem setSubmenu:editMenu];
    [[NSApp mainMenu] addItem:editMenuItem];

    [editMenuItem release];
    [editMenu release];
}

static void setupHollycastMenus(void)
{
	NSMenuItem *systemMenuItem = [[NSMenuItem alloc] initWithTitle:@"System" action:nil keyEquivalent:@""];
	NSMenu *systemMenu = [[NSMenu alloc] initWithTitle:@"System"];
	addMenuItem(systemMenu, @"Pause", @selector(pauseResume:), @"", MENU_TAG_PAUSE_RESUME);
	addMenuItem(systemMenu, @"Restart", @selector(restartGame:), @"", MENU_TAG_GAME_REQUIRED);
	addMenuSeparator(systemMenu);
	addMenuItem(systemMenu, @"Fast Forward", @selector(toggleFastForward:), @"", MENU_TAG_GAME_REQUIRED);
	addMenuItem(systemMenu, @"Screenshot", @selector(takeScreenshot:), @"", MENU_TAG_GAME_REQUIRED);
	addMenuSeparator(systemMenu);
	addMenuItem(systemMenu, @"Cheats", @selector(openCheats:), @"", MENU_TAG_GAME_REQUIRED);
	[systemMenuItem setSubmenu:systemMenu];
	[[NSApp mainMenu] addItem:systemMenuItem];
	[systemMenu release];
	[systemMenuItem release];

	NSMenuItem *toolsMenuItem = [[NSMenuItem alloc] initWithTitle:@"Tools" action:nil keyEquivalent:@""];
	NSMenu *toolsMenu = [[NSMenu alloc] initWithTitle:@"Tools"];
	addMenuItem(toolsMenu, @"CHD Convert", nil, @"", MENU_TAG_DISABLED);
	addMenuItem(toolsMenu, @"Custom Boxart", @selector(openCustomBoxart:), @"", 0);
	[toolsMenuItem setSubmenu:toolsMenu];
	[[NSApp mainMenu] addItem:toolsMenuItem];
	[toolsMenu release];
	[toolsMenuItem release];

	NSMenuItem *settingsMenuItem = [[NSMenuItem alloc] initWithTitle:@"Settings" action:nil keyEquivalent:@""];
	NSMenu *settingsMenu = [[NSMenu alloc] initWithTitle:@"Settings"];
	addMenuItem(settingsMenu, @"General", @selector(openGeneralSettings:), @"", 0);
	addMenuItem(settingsMenu, @"Video", @selector(openVideoSettings:), @"", 0);
	addMenuItem(settingsMenu, @"Audio", @selector(openAudioSettings:), @"", 0);
	addMenuItem(settingsMenu, @"Controls", @selector(openControlsSettings:), @"", 0);
	addMenuItem(settingsMenu, @"Network", @selector(openNetworkSettings:), @"", 0);
	addMenuItem(settingsMenu, @"Advanced", @selector(openAdvancedSettings:), @"", 0);
	[settingsMenuItem setSubmenu:settingsMenu];
	[[NSApp mainMenu] addItem:settingsMenuItem];
	[settingsMenu release];
	[settingsMenuItem release];
}

/* Create a window menu */
static void setupWindowMenu(void)
{
    NSMenu      *windowMenu;
    NSMenuItem  *windowMenuItem;
    NSMenuItem  *menuItem;
    
    windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    
    /* "Minimize" item */
    menuItem = [[NSMenuItem alloc] initWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windowMenu addItem:menuItem];
    [menuItem release];
    
    menuItem = [[NSMenuItem alloc] initWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    [menuItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];
    [windowMenu addItem:menuItem];
    installFunctionFullScreenShortcutGuard(menuItem);
    [menuItem release];
    
    /* "Ctrl + Cmd + F" was the standard Full Screen shortcut from OS X 10.7 Lion through macOS 11 Big Sur.
       Add it back as an alternative shortcut for user without an Apple keyboard */
    menuItem = [[NSMenuItem alloc] initWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    [menuItem setKeyEquivalentModifierMask:(NSEventModifierFlagControl | NSEventModifierFlagCommand)];
    [menuItem setAlternate:YES];
    [windowMenu addItem:menuItem];
    [menuItem release];
    
    /* Put menu into the menubar */
    windowMenuItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
    [windowMenuItem setSubmenu:windowMenu];
    [[NSApp mainMenu] addItem:windowMenuItem];
    
    /* Tell the application object that this is now the window menu */
    [NSApp setWindowsMenu:windowMenu];
    
    /* Finally give up our references to the objects */
    [windowMenu release];
    [windowMenuItem release];
}

/* Create a help menu - sample entries */
static void setupHelpMenu(void)
{
    NSMenu      *helpMenu;
    NSMenuItem  *helpMenuItem;
    
    helpMenu = [[NSMenu alloc] initWithTitle:@"Help"];
    addMenuItem(helpMenu, @"Discord", @selector(openDiscord:), @"", 0);
    addMenuItem(helpMenu, @"Report Bug", @selector(reportBug:), @"", 0);
    addMenuItem(helpMenu, @"Check for Updates", @selector(checkForUpdates:), @"", 0);
    addMenuSeparator(helpMenu);
    addMenuItem(helpMenu, @"About Hollycast", @selector(openAboutHollycast:), @"", 0);
    
    /* Put menu into the menubar */
    helpMenuItem = [[NSMenuItem alloc] initWithTitle:@"Help" action:nil keyEquivalent:@""];
    [helpMenuItem setSubmenu:helpMenu];
    [[NSApp mainMenu] addItem:helpMenuItem];
    
    /* Finally give up our references to the objects */
    [helpMenu release];
    [helpMenuItem release];
}
/* end help menu */


/* Replacement for NSApplicationMain */
static void CustomApplicationMain (int argc, char **argv)
{
    NSAutoreleasePool    *pool = [[NSAutoreleasePool alloc] init];
    SDLApplicationDelegate                *appDelegate;

    /* Ensure the application object is initialised */
    [NSApplication sharedApplication];
    
#ifdef SDL_USE_CPS
    {
        CPSProcessSerNum PSN;
        /* Tell the dock about us */
        if (!CPSGetCurrentProcess(&PSN))
            if (!CPSEnableForegroundOperation(&PSN,0x03,0x3C,0x2C,0x1103))
                if (!CPSSetFrontProcess(&PSN))
                    [NSApplication sharedApplication];
    }
#endif /* SDL_USE_CPS */
    
    /* Set up the menubar */
    [NSApp setMainMenu:[[[NSMenu alloc] init] autorelease]];
    setApplicationMenu();
    setupHollycastFileMenu();
    setupEditMenu();
    setupHollycastMenus();
    setupWindowMenu();
    setupHelpMenu(); /* needed for help menu */
    
    /* Create SDLMain and make it the app delegate */
    appDelegate = [[SDLApplicationDelegate alloc] init];
    [NSApp setDelegate:appDelegate];
    
    /* Start the main event loop */
    [NSApp run];
    
    [appDelegate release];
    [pool release];
}


#ifdef USE_BREAKPAD
static bool dumpCallback(const char *dump_dir, const char *minidump_id, void *context, bool succeeded)
{
	if (succeeded)
	{
	    char path[512];
	    sprintf(path, "%s/%s.dmp", dump_dir, minidump_id);
	    printf("Minidump saved to '%s'\n", path);
	    registerCrash(dump_dir, path);
	}
    return succeeded;
}
#endif
/*
 * Catch document open requests...this lets us notice files when the app
 *  was launched by double-clicking a document, or when a document was
 *  dragged/dropped on the app's icon. You need to have a
 *  CFBundleDocumentsType section in your Info.plist to get this message,
 *  apparently.
 *
 * Files are added to gArgv, so to the app, they'll look like command line
 *  arguments. Previously, apps launched from the finder had nothing but
 *  an argv[0].
 *
 * This message may be received multiple times to open several docs on launch.
 *
 * This message is ignored once the app's mainline has been called.
 */
- (BOOL)application:(NSApplication *)theApplication openFile:(NSString *)filename
{
	dispatch_async(dispatch_get_main_queue(), ^(){
		gui_start_game([filename cStringUsingEncoding:NSUTF8StringEncoding]);
	});

    return TRUE;
}


/* Called when the internal event loop has just started running */
- (void) applicationDidFinishLaunching: (NSNotification *) note
{
#ifdef USE_BREAKPAD
    google_breakpad::ExceptionHandler eh("/tmp", NULL, dumpCallback, NULL, true, NULL);
    task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);
#endif
    
    int status;
    
    /* Set the working directory to the .app's parent directory */
    [self setupWorkingDirectory];
    
    status = SDL_main(NULL, NULL);
    
    /* We're done, thank you for playing */
    exit(status);
}

- (NSMenu *)applicationDockMenu:(NSApplication *)sender
{
    NSMenu* menu = [[NSMenu alloc] init];
    [menu addItemWithTitle:@"New Instance" action:@selector(newInstance:) keyEquivalent:@"n"];
    return menu;
}

// Handle Dock menu's Quit action
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    [[NSApplication sharedApplication] quitAction:sender];
    return NSTerminateNow;
}
@end


@implementation NSString (ReplaceSubString)

- (NSString *)stringByReplacingRange:(NSRange)aRange with:(NSString *)aString
{
    unsigned int bufferSize;
    unsigned int selfLen = [self length];
    unsigned int aStringLen = [aString length];
    unichar *buffer;
    NSRange localRange;
    NSString *result;
    
    bufferSize = selfLen + aStringLen - aRange.length;
    buffer = (unichar *)NSAllocateMemoryPages(bufferSize*sizeof(unichar));
    
    /* Get first part into buffer */
    localRange.location = 0;
    localRange.length = aRange.location;
    [self getCharacters:buffer range:localRange];
    
    /* Get middle part into buffer */
    localRange.location = 0;
    localRange.length = aStringLen;
    [aString getCharacters:(buffer+aRange.location) range:localRange];
    
    /* Get last part into buffer */
    localRange.location = aRange.location + aRange.length;
    localRange.length = selfLen - localRange.location;
    [self getCharacters:(buffer+aRange.location+aStringLen) range:localRange];
    
    /* Build output string */
    result = [NSString stringWithCharacters:buffer length:bufferSize];
    
    NSDeallocateMemoryPages(buffer, bufferSize);
    
    return result;
}

@end



#ifdef main
#  undef main
#endif


/* Main entry point to executable - should *not* be SDL_main! */
int main (int argc, char **argv)
{
    if (getppid() != 1) {
        /* Make LLDB ignore EXC_BAD_ACCESS for debugging */
        task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);
    }
    
    CustomApplicationMain (argc, argv);
    return 0;
}

