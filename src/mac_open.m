// mac_open.m — receive "open documents" requests (Finder double-click /
// Open With / `open -a`) and forward the file paths to the C core.
//
// AppKit delivers these to the app delegate's application:openURLs:. GLFW owns
// the delegate and doesn't implement it, so AppKit would answer "could not
// open". We graft the method onto GLFW's delegate class before glfwInit(),
// which is when the launch-time request is dispatched.
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

static void (*g_open_cb)(const char *path);

static void open_urls(id self, SEL _cmd, NSApplication *app, NSArray<NSURL *> *urls) {
    for (NSURL *url in urls)
        if (url.isFileURL && url.path && g_open_cb) g_open_cb(url.path.UTF8String);
}

void mac_open_install(void (*cb)(const char *path)) {
    g_open_cb = cb;
    Class cls = NSClassFromString(@"GLFWApplicationDelegate");
    if (cls)
        class_addMethod(cls, @selector(application:openURLs:),
                        (IMP)open_urls, "v@:@@");
}

// ---------------------------------------------------------------- borderless
// Extend the content view under the title bar so the terminal fills the whole
// window; the traffic lights float over the grid. Since the GL view now
// swallows clicks up there, a drag in the title-bar strip moves the window
// and a double-click zooms, like a normal title bar.

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

static id g_drag_monitor;

void mac_fullsize_content(void *glfw_window) {
    NSWindow *win = glfwGetCocoaWindow((GLFWwindow *)glfw_window);
    if (!win) return;
    win.styleMask |= NSWindowStyleMaskFullSizeContentView;
    win.titlebarAppearsTransparent = YES;
    win.titleVisibility = NSWindowTitleHidden;

    __weak NSWindow *weak = win;
    g_drag_monitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
        handler:^NSEvent *(NSEvent *e) {
            NSWindow *w = weak;
            if (!w || e.window != w) return e;
            CGFloat bar = NSHeight(w.frame) - NSHeight(w.contentLayoutRect);
            NSPoint p = e.locationInWindow;
            if (p.y < NSHeight(w.frame) - bar) return e;
            // let the traffic lights handle their own clicks
            NSView *hit = [w.contentView.superview hitTest:p];
            if ([hit isKindOfClass:[NSControl class]]) return e;
            if (e.clickCount == 2) [w performZoom:nil];
            else [w performWindowDragWithEvent:e];
            return nil;
        }];
}

// hide the close/minimize/zoom buttons (window still drags from the top edge,
// cmd+w / cmd+m still work)
void mac_hide_window_buttons(void *glfw_window) {
    NSWindow *win = glfwGetCocoaWindow((GLFWwindow *)glfw_window);
    if (!win) return;
    for (NSWindowButton b = NSWindowCloseButton; b <= NSWindowZoomButton; b++)
        [win standardWindowButton:b].hidden = YES;
}

// Where the traffic lights end, in points from the window's top-left corner:
// right edge of the zoom button and bottom of the cluster. Zero when they're
// not over the content (fullscreen).
void mac_titlebar_buttons(void *glfw_window, float *right, float *bottom) {
    *right = *bottom = 0;
    NSWindow *win = glfwGetCocoaWindow((GLFWwindow *)glfw_window);
    if (!win || (win.styleMask & NSWindowStyleMaskFullScreen)) return;
    NSButton *zoom = [win standardWindowButton:NSWindowZoomButton];
    if (!zoom || zoom.hidden) return;
    NSRect f = [zoom convertRect:zoom.bounds toView:nil];
    *right = (float)NSMaxX(f);
    *bottom = (float)(NSHeight(win.contentView.frame) - NSMinY(f));
}
