/*   SDLMain.m - main entry point for our Cocoa-ized SDL app
       Initial Version: Darrell Walisser <dwaliss1@purdue.edu>
       Non-NIB-Code & other changes: Max Horn <max@quendi.de>
       Portions Copyright 2026 The Hollycast Authors
    Feel free to customize this file to suit your needs
*/

#ifndef _SDLMain_h_
#define _SDLMain_h_

#import <Cocoa/Cocoa.h>

@interface SDLApplicationDelegate : NSObject <NSApplicationDelegate>
@end

#define MENU_TAG_TOGGLE_MENU 501
#define MENU_TAG_GAME_REQUIRED 502
#define MENU_TAG_PAUSE_RESUME 503
#define MENU_TAG_DISABLED 504

#endif /* _SDLMain_h_ */
