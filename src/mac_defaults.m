// mac_defaults.m — app-menu items that make terminal.app the default app for
// every file type it can open (images, audio, 3D models, text and code), and
// put the previous defaults back.
//
// Before taking a type over, the app that used to own it is saved to
// ~/.config/terminal/previous-defaults.plist so "Restore" can undo it.
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

static NSString *const kBundleID = @"xyz.finterm.terminal";

// one list per category; keep in sync with assets/Info.plist
static NSArray<NSString *> *exts_images(void) {
    return @[ @"png", @"jpg", @"jpeg", @"gif", @"bmp", @"tga", @"psd", @"hdr" ];
}
static NSArray<NSString *> *exts_audio(void) {
    return @[ @"mp3", @"m4a", @"wav", @"aiff", @"aif", @"flac", @"aac", @"caf" ];
}
static NSArray<NSString *> *exts_models(void) {
    return @[ @"obj", @"stl", @"ply" ];
}
static NSArray<NSString *> *exts_text(void) {
    // .ts is left out: macOS maps it to MPEG transport-stream video
    return @[ @"txt", @"log", @"md", @"json", @"yaml", @"yml", @"toml", @"csv",
              @"xml", @"conf", @"ini", @"env",
              @"py", @"js", @"mjs", @"jsx", @"tsx", @"rb", @"go", @"rs", @"swift",
              @"c", @"h", @"cpp", @"cc", @"hpp", @"m", @"mm", @"java", @"kt",
              @"sh", @"zsh", @"bash", @"fish", @"lua", @"php", @"pl", @"r",
              @"html", @"css", @"scss", @"sql", @"zig", @"dart", @"ex", @"exs",
              @"hs", @"ml", @"scala", @"clj", @"vim", @"el", @"make", @"cmake",
              @"gradle", @"proto", @"graphql" ];
}

static NSString *saved_path(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:
            @".config/terminal/previous-defaults.plist"];
}

static void alert(NSString *title, NSString *text) {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = title;
    a.informativeText = text;
    [a runModal];
}

// unique content types for all categories, keyed to a readable extension
static NSDictionary<NSString *, UTType *> *all_types(void) {
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    NSArray *lists = @[ exts_images(), exts_audio(), exts_models(), exts_text() ];
    for (NSArray *list in lists)
        for (NSString *ext in list) {
            UTType *t = [UTType typeWithFilenameExtension:ext];
            if (t && !out[t.identifier]) out[t.identifier] = t;
        }
    return out;
}

// sets each type's default app, then reports how many took
static void set_defaults(NSDictionary<NSString *, NSURL *> *apps, void (^done)(int ok, int fail)) {
    dispatch_group_t g = dispatch_group_create();
    __block int ok = 0, fail = 0;
    NSObject *lock = [NSObject new];
    for (NSString *uti in apps) {
        UTType *t = [UTType typeWithIdentifier:uti];
        if (!t) { fail++; continue; }
        dispatch_group_enter(g);
        [[NSWorkspace sharedWorkspace] setDefaultApplicationAtURL:apps[uti]
                                                toOpenContentType:t
                                                completionHandler:^(NSError *err) {
            @synchronized (lock) { if (err) fail++; else ok++; }
            dispatch_group_leave(g);
        }];
    }
    dispatch_group_notify(g, dispatch_get_main_queue(), ^{ done(ok, fail); });
}

@interface TermDefaultsMenu : NSObject
@end

@implementation TermDefaultsMenu

- (void)makeDefault:(id)sender {
    NSURL *me = [NSBundle mainBundle].bundleURL;
    if (![[NSBundle mainBundle].bundleIdentifier isEqualToString:kBundleID]) {
        alert(@"Open terminal.app to do this",
              @"This copy of terminal isn't running from its app bundle, so macOS "
              @"can't register it. Run `make app` and open terminal.app.");
        return;
    }

    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"Make terminal the default app for all its file types?";
    a.informativeText =
        @"Double-clicking these files in Finder will open them in terminal:\n\n"
        @"• Images: png, jpg, gif, bmp, tga, psd, hdr\n"
        @"• Music: mp3, m4a, wav, aiff, flac, aac, caf\n"
        @"• 3D models: obj, stl, ply\n"
        @"• Text and code: txt, md, json, yaml, py, js, sh, c, go, rs and ~50 more\n\n"
        @"Your current default apps are saved. Use \"Restore Previous Defaults\" "
        @"in this menu to put them back.";
    [a addButtonWithTitle:@"Make Default"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;

    LSRegisterURL((__bridge CFURLRef)me, true);

    // remember who owned each type, unless we already do (keep older records)
    NSMutableDictionary *saved =
        [[NSDictionary dictionaryWithContentsOfFile:saved_path()] mutableCopy]
        ?: [NSMutableDictionary dictionary];
    NSDictionary<NSString *, UTType *> *types = all_types();
    NSMutableDictionary<NSString *, NSURL *> *targets = [NSMutableDictionary dictionary];
    for (NSString *uti in types) {
        NSURL *cur = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenContentType:types[uti]];
        NSString *curID = cur ? [NSBundle bundleWithURL:cur].bundleIdentifier : nil;
        if (cur && ![curID isEqualToString:kBundleID] && !saved[uti]) saved[uti] = cur.path;
        targets[uti] = me;
    }
    [[NSFileManager defaultManager]
        createDirectoryAtPath:[saved_path() stringByDeletingLastPathComponent]
  withIntermediateDirectories:YES attributes:nil error:nil];
    [saved writeToFile:saved_path() atomically:YES];

    set_defaults(targets, ^(int ok, int fail) {
        alert(@"terminal is now the default app",
              fail ? [NSString stringWithFormat:@"Set for %d file types. %d couldn't be "
                                                @"changed; macOS may protect them.", ok, fail]
                   : [NSString stringWithFormat:@"Set for %d file types.", ok]);
    });
}

- (void)restoreDefaults:(id)sender {
    NSDictionary *saved = [NSDictionary dictionaryWithContentsOfFile:saved_path()];
    if (!saved.count) {
        alert(@"Nothing to restore",
              @"terminal hasn't replaced any default apps on this Mac.");
        return;
    }
    NSMutableDictionary<NSString *, NSURL *> *targets = [NSMutableDictionary dictionary];
    for (NSString *uti in saved) {
        NSString *path = saved[uti];
        if ([[NSFileManager defaultManager] fileExistsAtPath:path])
            targets[uti] = [NSURL fileURLWithPath:path];
    }
    set_defaults(targets, ^(int ok, int fail) {
        [[NSFileManager defaultManager] removeItemAtPath:saved_path() error:nil];
        alert(@"Previous defaults restored",
              [NSString stringWithFormat:@"Restored %d file types.%@", ok,
               fail ? [NSString stringWithFormat:@" %d couldn't be changed.", fail] : @""]);
    });
}

@end

static TermDefaultsMenu *g_menu_target;

// adds the two items to the app menu (the bold "terminal" menu); call after
// glfwInit(), which builds the menu bar
void mac_defaults_menu_install(void) {
    NSMenu *appMenu = [NSApp.mainMenu itemAtIndex:0].submenu;
    if (!appMenu) return;
    g_menu_target = [TermDefaultsMenu new];

    NSMenuItem *make = [[NSMenuItem alloc] initWithTitle:@"Make terminal the Default App…"
                                                  action:@selector(makeDefault:)
                                           keyEquivalent:@""];
    NSMenuItem *restore = [[NSMenuItem alloc] initWithTitle:@"Restore Previous Defaults"
                                                     action:@selector(restoreDefaults:)
                                              keyEquivalent:@""];
    make.target = restore.target = g_menu_target;

    // right below "About terminal"
    NSInteger at = appMenu.numberOfItems > 0 ? 1 : 0;
    [appMenu insertItem:[NSMenuItem separatorItem] atIndex:at];
    [appMenu insertItem:make atIndex:at + 1];
    [appMenu insertItem:restore atIndex:at + 2];
}
