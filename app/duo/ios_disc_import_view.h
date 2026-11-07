// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios_disc_import_view.h (Claude 2026-11-05; the builder's iOS brief, common work item 9): the launcher's "Import Game Discs" page - the
// player's own disc images into the app, without a computer transfer. The logic is ios_disc_import.h (a port of android/'s import,
// host tested against synthetic images and hash-only against the real discs); this file is the UIKit glue only:
//  - sources: a file picked with the Files picker (open in place: a security-scoped URL, so an image on a USB drive works), or every
//    file in the app's Documents and Documents/Inbox (UIFileSharingEnabled: Finder / the Files app put them there; "Open in" lands in
//    Inbox). An image is never changed, moved or removed.
//  - the copy runs on a background queue; the screen stays awake while it runs; leaving the app ends it cleanly after the system's
//    background grace time (a stopped copy leaves at most one part file, removed by the next run; files already in place are not read
//    again - the resume is the plan itself).
//  - where: <home>/Library/r/private-trace/inputs/<sha256>.bin, excluded from iCloud backup (16.4 GB the player can import again).
//  - texts: the page shows Disc's message / next and Outcome's shown (never a file name of the disc or of the app's storage); the log
//    lines (stderr) carry the names: APPLE_IOS_DISC_IMPORT DISC / FILE / OUTCOME.
// Drawn with the menu redesign's scenes (ios_menu_scenes.h) in the launcher's frame; taps through the scene's hit rectangles.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <atomic>
#include <memory>
#include "ios_disc_import.h"
#include "ios_menu_paint.h"

@interface PortDiscImportController : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic,copy) void (^onClose)(BOOL complete);
@end

#ifndef PORT_DISC_IMPORT_INTERFACE_ONLY
namespace di=recomp::apple::discimport;
namespace dims=recomp::apple::menu::scenes;
namespace diml=recomp::apple::menu::layout;
@implementation PortDiscImportController {
    PortMenuSceneView* _canvas;recomp::apple::menu::Scene _scene;
    std::string _root;                    // <home>/Library/r/private-trace
    std::shared_ptr<std::atomic<bool>> _cancel;BOOL _busy;
    std::string _line,_detail;int _tone;  // the page's status line and its detail sentence
    double _done,_total;int _file,_files;
    UIBackgroundTaskIdentifier _bg;NSInteger _rowFocused;
    NSTimer* _cmdTimer;struct timespec _cmdSeen; // (TEST ONLY: private-trace/ios-import-cmd, as the side menu's ios-sidemenu-cmd)
}
- (instancetype)init {
    if((self=[super init])) {
        _root=std::string(NSHomeDirectory().fileSystemRepresentation)+"/Library/r/private-trace";
        _cancel=std::make_shared<std::atomic<bool>>(false);_bg=UIBackgroundTaskInvalid;_rowFocused=0;
        _line="Choose the image of each disc: Disc 1 (install) and Disc 2 (play).";_tone=dims::TONE_NORMAL;
        if([self complete]){_line=di::TEXT_BOTH_DISCS_IN_PLACE;_tone=dims::TONE_GOOD;} // (244: opened from the launcher with both discs in, it said "Choose...")
        self.modalPresentationStyle=UIModalPresentationFullScreen;
        const uint64_t removed=di::removeLeftOvers(_root);
        if(removed)std::fprintf(stderr,"APPLE_IOS_DISC_IMPORT leftovers_removed=%llu\n",(unsigned long long)removed);
    }
    return self;
}
- (void)viewDidLoad {
    [super viewDidLoad];self.view.backgroundColor=UIColor.blackColor;
    _canvas=[[PortMenuSceneView alloc] initWithFrame:self.view.bounds];_canvas.liveBlur=NO;
    _canvas.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    __weak PortDiscImportController* weak=self;
    _canvas.scene=[weak]() -> const recomp::apple::menu::Scene* {PortDiscImportController* s=weak;return s?&s->_scene:nullptr;};
    [self.view addSubview:_canvas];
    [self.view addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped:)]];
    [self redraw];
    {struct stat st{};if(::stat((_root+"/ios-import-cmd").c_str(),&st)==0)_cmdSeen=st.st_mtimespec;} // (a command written before the page: not acted on)
    _cmdTimer=[NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(command) userInfo:nil repeats:YES];
    std::fprintf(stderr,"APPLE_IOS_DISC_IMPORT page shown install=%d play=%d\n",[self inPlace:di::catalog::INSTALL],[self inPlace:di::catalog::PLAY]);
}
// TEST ONLY (the device check from the Mac, which cannot tap): private-trace/ios-import-cmd written with "files" (Import From The Files
// Folder), "stop" or "close"; acted on when the file's time changes.
- (void)command {
    struct stat st{};
    if(::stat((_root+"/ios-import-cmd").c_str(),&st)!=0)return;
    if(st.st_mtimespec.tv_sec==_cmdSeen.tv_sec && st.st_mtimespec.tv_nsec==_cmdSeen.tv_nsec)return;
    _cmdSeen=st.st_mtimespec;
    NSString* text=[[NSString stringWithContentsOfFile:[NSString stringWithUTF8String:(_root+"/ios-import-cmd").c_str()] encoding:NSASCIIStringEncoding error:nil]
                    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    std::fprintf(stderr,"APPLE_IOS_DISC_IMPORT command=%s busy=%d\n",text.UTF8String?:"?",int(_busy));
    if([text isEqualToString:@"files"] && !_busy)[self scanDocuments];
    else if([text isEqualToString:@"stop"] && _busy)_cancel->store(true);
    else if([text isEqualToString:@"close"])[self back];
}
- (void)viewDidDisappear:(BOOL)animated {[super viewDidDisappear:animated];[_cmdTimer invalidate];_cmdTimer=nil;}
- (void)viewDidLayoutSubviews {[super viewDidLayoutSubviews];[self redraw];}
- (BOOL)prefersStatusBarHidden {return YES;} // (presented full screen, this page decides the status bar, not the root: hidden like the launcher)
// ---- what is in place (by name and size: the copy's own rule)
- (int)inPlace:(const char*)disc {
    const auto pins=di::catalog::pins(disc);return int(pins.size()-di::missing(_root+"/"+di::INPUTS,pins).size());
}
- (BOOL)complete {return di::missing(_root+"/"+di::INPUTS,di::catalog::pins()).empty();}
static std::string discValue(int have,int of) {return have==of?"Done":have==0?"Not imported":std::to_string(have)+" of "+std::to_string(of)+" files";}
- (dims::Page)page {
    dims::Page p;
    p.title="FivePocket";p.status={{"Game Data",dims::TONE_NORMAL}};
    p.tabs={"Import Game Discs"};p.tab=0;p.shoulders=false;
    const int d1=[self inPlace:di::catalog::INSTALL],d2=[self inPlace:di::catalog::PLAY];
    const auto row=[&](const char* label,std::string value,bool enabled,bool hasValue){dims::Row r;r.label=label;r.value=std::move(value);r.hasValue=hasValue;r.enabled=enabled;return r;};
    p.rows.push_back(row("Install Disc (Disc 1)",discValue(d1,8),true,true));
    p.rows.push_back(row("Play Disc (Disc 2)",discValue(d2,10),true,true));
    p.rows.push_back(row(_busy?"Stop":"Choose A Disc Image",{},true,false));
    p.rows.push_back(row("Import From The Files Folder",{},!_busy,false));
    p.rowFocused=int(_rowFocused);
    std::string info=_busy && _files>0?("File "+std::to_string(_file+1)+" of "+std::to_string(_files)+" · "+di::gigabytes(uint64_t(_done),false)+" of "+di::gigabytes(uint64_t(_total),true)):_line;
    p.info.push_back({info,_tone});
    if(!_detail.empty())p.help.push_back({_detail,dims::TONE_NORMAL});
    else p.help.push_back({di::pageHelp(d1,d2,_busy),dims::TONE_NORMAL}); // (both discs Done: the images may go)
    p.prompts.push_back({"Back","B",diml::nav::BACK});p.prompts.push_back({"Select","A",diml::nav::CONFIRM});
    p.touchIcons=!recomp::apple::menu::paint::physicalGamepad();
    return p;
}
- (void)redraw {
    if(!self.isViewLoaded)return;
    const CGFloat w=_canvas.bounds.size.width,h=_canvas.bounds.size.height,u=std::min(w/diml::W,h/diml::H);
    _scene=dims::build([self page],recomp::apple::menu::paint::fontMeasure(),diml::Frame::screen(diml::panelHeight(float(w),float(h),float(u))));
    [_canvas setNeedsDisplay];
}
- (void)tapped:(UITapGestureRecognizer*)tap {
    const CGPoint p=[tap locationInView:_canvas];const CGFloat w=_canvas.bounds.size.width,h=_canvas.bounds.size.height,u=std::min(w/diml::W,h/diml::H);
    const int id=_scene.hitAt(float((p.x-(w-_scene.panelW*u)/2)/u),float((p.y-(h-_scene.panelH*u)/2)/u));
    if(id<0)return;
    if(id==diml::HIT_PROMPT+diml::nav::BACK){[self back];return;}
    if(id==diml::HIT_PROMPT+diml::nav::CONFIRM){[self activate:_rowFocused];return;}
    if(id>=diml::HIT_ROW && id<diml::HIT_ROW+4) {const NSInteger r=id-diml::HIT_ROW;if(r==_rowFocused || r>=2)[self activate:r];_rowFocused=r;[self redraw];}
}
- (void)back {
    if(_busy){_cancel->store(true);return;}   // (B while copying: stop first; the page stays)
    [self dismissViewControllerAnimated:YES completion:nil];
    if(self.onClose)self.onClose([self complete]);
}
- (void)activate:(NSInteger)row {
    if(row==2){if(_busy)_cancel->store(true);else [self pick];}
    else if(row==3 && !_busy)[self scanDocuments];
    else if(row<2 && !_busy)[self pick];
}
// ---- sources
- (void)pick {
    NSMutableArray<UTType*>* types=[NSMutableArray arrayWithObject:UTTypeData];
    if(UTType* iso=[UTType typeWithFilenameExtension:@"iso"])[types insertObject:iso atIndex:0];
    UIDocumentPickerViewController* picker=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:NO]; // (open in place: no 8 GB copy into the app's inbox)
    picker.delegate=self;picker.allowsMultipleSelection=YES;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController*)picker didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {(void)picker;[self importURLs:urls scoped:YES];}
- (void)scanDocuments {
    NSURL* docs=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    NSMutableArray<NSURL*>* found=[NSMutableArray new];
    for(NSURL* dir in @[docs,[docs URLByAppendingPathComponent:@"Inbox"]]) {
        NSArray<NSURL*>* items=[NSFileManager.defaultManager contentsOfDirectoryAtURL:dir includingPropertiesForKeys:@[NSURLIsRegularFileKey,NSURLFileSizeKey] options:NSDirectoryEnumerationSkipsHiddenFiles error:nil];
        for(NSURL* u in items){NSNumber* reg=nil;[u getResourceValue:&reg forKey:NSURLIsRegularFileKey error:nil];NSNumber* sz=nil;[u getResourceValue:&sz forKey:NSURLFileSizeKey error:nil];
            if(reg.boolValue && sz.unsignedLongLongValue>=33ull*di::SECTOR)[found addObject:u];}
    }
#if defined(RECOMP_DUO_EDITION) // ios-duo: the folder's name in Files (the app's display name)
    if(!found.count){_line="No file in the Files folder. Put the disc images into the FivePocket Duo folder (the Files app: On My iPhone, or Finder with the device connected).";_tone=dims::TONE_WARNING;[self redraw];return;}
#else
    if(!found.count){_line="No file in the Files folder. Put the disc images into FivePocket's folder (the Files app, or Finder with the device connected).";_tone=dims::TONE_WARNING;[self redraw];return;}
#endif
    [self importURLs:found scoped:NO];
}
// ---- the copy
- (void)importURLs:(NSArray<NSURL*>*)urls scoped:(BOOL)scoped {
    if(_busy)return;
    _busy=YES;_cancel->store(false);_detail.clear();_line="Reading the disc image…";_tone=dims::TONE_NORMAL;_files=0;[self redraw];
    UIApplication.sharedApplication.idleTimerDisabled=YES;
    __weak PortDiscImportController* weak=self;auto cancel=_cancel;
    _bg=[UIApplication.sharedApplication beginBackgroundTaskWithName:@"disc import" expirationHandler:^{cancel->store(true);}];
    const std::string root=_root;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
        std::string line,detail;int tone=dims::TONE_GOOD;bool any=false;
        for(NSURL* url in urls) {
            if(cancel->load())break;
            const BOOL access=scoped && [url startAccessingSecurityScopedResource];
            std::shared_ptr<di::Source> image;
            try {image=std::make_shared<di::FileSource>(url.fileSystemRepresentation);}
            catch(const std::exception& e){line="This file could not be opened.";detail=di::NEXT_RECONNECT;tone=dims::TONE_ERROR;
                std::fprintf(stderr,"APPLE_IOS_DISC_IMPORT OPEN failed: %s\n",e.what());if(access)[url stopAccessingSecurityScopedResource];continue;}
            const CFAbsoluteTime started=CFAbsoluteTimeGetCurrent();
            const di::Disc disc=di::open(image,di::catalog::game());
            std::fprintf(stderr,"APPLE_IOS_DISC_IMPORT DISC %s\n",disc.logged().c_str());
            if(!disc.usable()){line=disc.message;detail=disc.next;tone=dims::TONE_ERROR;if(access)[url stopAccessingSecurityScopedResource];continue;}
            {std::string err;try{di::makeDirectory(root+"/"+di::INPUTS);}catch(const std::exception& e){err=e.what();}
             NSURL* inputs=[NSURL fileURLWithPath:[NSString stringWithUTF8String:(root+"/"+di::INPUTS).c_str()] isDirectory:YES];
             [inputs setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];}
            const auto expected=di::catalog::pins(disc.kind==di::Kind::Play?di::catalog::PLAY:di::catalog::INSTALL);
            di::DiskStore store(root);di::Progress progress;
            progress.cancelled=[cancel]{return cancel->load();};
            progress.update=[weak,last=CFAbsoluteTime(0)](uint64_t done,uint64_t total,int i,int n,const std::string&) mutable {
                const CFAbsoluteTime now=CFAbsoluteTimeGetCurrent();if(now-last<0.25 && done<total)return;last=now; // (the page redraws at most 4 times a second)
                static uint64_t logged=0;if(done<logged)logged=0;
                if(done-logged>=(1ull<<31) || done==total){logged=done;std::fprintf(stderr,"APPLE_IOS_DISC_IMPORT progress file=%d/%d bytes=%llu/%llu\n",i+1,n,(unsigned long long)done,(unsigned long long)total);}
                dispatch_async(dispatch_get_main_queue(),^{PortDiscImportController* s=weak;if(!s)return;s->_done=double(done);s->_total=double(total);s->_file=i;s->_files=n;[s redraw];});
            };
            const di::Outcome o=di::run(disc,expected,di::catalog::pins(),{},root,store,progress,di::diskSpace,[](const di::FileResult& f){
                std::fprintf(stderr,"APPLE_IOS_DISC_IMPORT FILE %d/%d %s bytes=%llu sha256=%s kept=%d\n",f.index+1,f.files,f.name.c_str(),(unsigned long long)f.bytes,f.sha256.c_str(),int(f.kept));});
            if(access)[url stopAccessingSecurityScopedResource];
            std::fprintf(stderr,"APPLE_IOS_DISC_IMPORT OUTCOME complete=%d stopped=%d kept=%d read=%d bytes=%llu seconds=%.1f error=%s\n",int(o.complete),int(o.stopped),o.kept(),o.filesToRead,
                         (unsigned long long)o.bytesToRead,CFAbsoluteTimeGetCurrent()-started,o.error.c_str());
            any=true;
            if(o.stopped){line="Stopped. Choose the image again to go on: the files already in place are kept.";tone=dims::TONE_WARNING;detail.clear();break;}
            if(!o.complete){line=o.shown;tone=dims::TONE_ERROR;detail=o.problem==di::Problem::DiscCopyBad?di::NEXT_NEW_COPY:o.problem==di::Problem::SourceUnreadable?di::NEXT_RECONNECT:"";continue;}
            line=disc.message+" Its files are in place.";tone=dims::TONE_GOOD;detail.clear();
        }
        dispatch_async(dispatch_get_main_queue(),^{
            PortDiscImportController* s=weak;
            [UIApplication.sharedApplication endBackgroundTask:s?s->_bg:UIBackgroundTaskInvalid];
            UIApplication.sharedApplication.idleTimerDisabled=NO;
            if(!s)return;
            s->_bg=UIBackgroundTaskInvalid;s->_busy=NO;
            if(any || !line.empty()){s->_line=line;s->_detail=detail;s->_tone=tone;}
            if([s complete]){s->_line=di::TEXT_BOTH_DISCS_IN_PLACE;s->_tone=dims::TONE_GOOD;s->_detail.clear();}
            [s redraw];
        });
    });
}
@end
#endif
