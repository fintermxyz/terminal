// mac_audio.m — music playback for player tabs (AVFoundation) plus the
// song's title/artist/album from its metadata
#import <AVFoundation/AVFoundation.h>
#include "player.h"

struct Player {
    void *av;   // AVAudioPlayer, retained via __bridge_retained
    char title[256], artist[256], album[256], name[256];
};

static void copy_str(char *dst, size_t cap, NSString *s) {
    snprintf(dst, cap, "%s", s.length ? s.UTF8String : "");
}

static AVAudioPlayer *av(Player *p) { return (__bridge AVAudioPlayer *)p->av; }

Player *player_open(const char *path) {
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    NSError *err = nil;
    AVAudioPlayer *a = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:&err];
    if (!a) return NULL;
    [a prepareToPlay];

    Player *p = calloc(1, sizeof *p);
    p->av = (__bridge_retained void *)a;
    copy_str(p->name, sizeof p->name, url.lastPathComponent);

    // common metadata keys; load synchronously, files are local
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    for (AVMetadataItem *item in asset.commonMetadata) {
        NSString *v = item.stringValue;
        if (!v) continue;
        if ([item.commonKey isEqualToString:AVMetadataCommonKeyTitle]) copy_str(p->title, sizeof p->title, v);
        else if ([item.commonKey isEqualToString:AVMetadataCommonKeyArtist]) copy_str(p->artist, sizeof p->artist, v);
        else if ([item.commonKey isEqualToString:AVMetadataCommonKeyAlbumName]) copy_str(p->album, sizeof p->album, v);
    }
#pragma clang diagnostic pop
    if (!p->title[0]) { // fall back to the file name without its extension
        copy_str(p->title, sizeof p->title, url.lastPathComponent.stringByDeletingPathExtension);
    }
    [a play];
    return p;
}

void player_free(Player *p) {
    if (!p) return;
    AVAudioPlayer *a = (__bridge_transfer AVAudioPlayer *)p->av;
    [a stop];
    a = nil;
    free(p);
}

void player_toggle(Player *p) {
    AVAudioPlayer *a = av(p);
    if (a.playing) [a pause];
    else {
        if (a.duration - a.currentTime < 0.05) a.currentTime = 0; // replay from the end
        [a play];
    }
}

void player_seek(Player *p, double t) {
    AVAudioPlayer *a = av(p);
    if (t < 0) t = 0;
    if (t > a.duration) t = a.duration;
    a.currentTime = t;
}

double player_pos(Player *p) { return av(p).currentTime; }
double player_dur(Player *p) { return av(p).duration; }
bool player_playing(Player *p) { return av(p).playing; }
const char *player_title(Player *p) { return p->title; }
const char *player_artist(Player *p) { return p->artist; }
const char *player_album(Player *p) { return p->album; }
const char *player_name(Player *p) { return p->name; }
