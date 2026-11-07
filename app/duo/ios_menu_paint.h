// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// menu-redesign on the iPad (2026-10-25): paints a menu Scene (ios_menu_scenes.h) with CoreGraphics + CoreText the way android-qol's
// UiKit.Painter and local-analysis/menu-redesign/host/ScenePaint.swift do: the 1920 x 1080 panel scaled by u and centred in the view, the
// backdrop over the whole view, every text centred on the face's ascent 1.011 and descent 0.353, Nunito Sans (SIL OFL 1.1; the app's
// NunitoSans.ttf, claude_build.py) at the item's weight - set explicitly ('wght' 400 / 700 / 900): the variable font's default is 200.
// Without the bundled font the system font stands in (and logs so). The same measure the menu model fits its texts with (fontMeasure).
//
// PortMenuSceneView draws the menu over the game: under it a live blur of the game (UIVisualEffectView; Android blurs a captured copy),
// and the backdrop item is then the shade (MenuLayout.BACKDROP_SHADE, black 58 %). The pane's texts fade in over 120 ms after another
// page came up (MenuLayout.fadeAlpha). The restart ALERT is the owner's picture in owner builds (alert-restart.png), else the drawn notice.
#import <UIKit/UIKit.h>
#import <CoreText/CoreText.h>
#import <QuartzCore/QuartzCore.h>
#import <GameController/GameController.h>
#include <cstdio>
#include <map>
#include <string>
#include "ios_menu_scenes.h"

namespace recomp::apple::menu::paint {
// A physical gamepad is connected (an extended gamepad that is not a snapshot; the launcher runs before the game's virtual controller
// exists, the in-game menu takes the overlay timer's answer instead): the menus' prompt strip, else the X and check icons.
inline bool physicalGamepad() {for(GCController* c in GCController.controllers)if(c.extendedGamepad && !c.snapshot)return true;return false;}
inline CTFontDescriptorRef baseDescriptor() {
    static CTFontDescriptorRef base=[]() -> CTFontDescriptorRef {
        NSString* path=[NSBundle.mainBundle pathForResource:@"NunitoSans" ofType:@"ttf"];
        CTFontDescriptorRef d=nullptr;
        if(path) {
            CFArrayRef list=CTFontManagerCreateFontDescriptorsFromURL((__bridge CFURLRef)[NSURL fileURLWithPath:path]);
            if(list && CFArrayGetCount(list)>0)d=(CTFontDescriptorRef)CFRetain(CFArrayGetValueAtIndex(list,0));
            if(list)CFRelease(list);
        }
        std::fprintf(stderr,"APPLE_IOS_MENU font=%s\n",d?"NunitoSans":"system (NunitoSans.ttf not in the app)");
        return d;
    }();
    return base;
}
inline CGFloat weightOf(int face) {return face==scene::BOLD?700:face==scene::HEAVY?900:400;}
// A font at a size in panel pixels and a face; cached.
inline CTFontRef font(CGFloat size,int face) {
    static std::map<std::pair<int,int>,CTFontRef> cache;
    const auto key=std::make_pair(int(std::lround(size*16)),int(weightOf(face)));
    if(auto it=cache.find(key);it!=cache.end())return it->second;
    CTFontRef f=nullptr;
    if(CTFontDescriptorRef base=baseDescriptor()) {
        CTFontDescriptorRef d=CTFontDescriptorCreateCopyWithVariation(base,(__bridge CFNumberRef)@(0x77676874),weightOf(face));
        f=CTFontCreateWithFontDescriptor(d,size,nullptr);CFRelease(d);
    } else {
        UIFont* sys=[UIFont systemFontOfSize:size weight:face==scene::BOLD?UIFontWeightBold:face==scene::HEAVY?UIFontWeightBlack:UIFontWeightRegular];
        f=(CTFontRef)CFBridgingRetain(sys);
    }
    cache[key]=f;
    return f;
}
inline CTLineRef line(const std::string& text,int size,int face,CGColorRef color) {
    NSString* s=[NSString stringWithUTF8String:text.c_str()]?:@"";
    NSDictionary* at=color?@{(id)kCTFontAttributeName:(__bridge id)font(size,face),(id)kCTForegroundColorAttributeName:(__bridge id)color}
                          :@{(id)kCTFontAttributeName:(__bridge id)font(size,face)};
    return CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)[[NSAttributedString alloc] initWithString:s attributes:at]);
}
// The menu model's measure: the typographic width of a text in panel pixels.
inline float measureText(const std::string& text,int size,int face) {
    CTLineRef l=line(text,size,face,nullptr);const double w=CTLineGetTypographicBounds(l,nullptr,nullptr,nullptr);CFRelease(l);return float(w);
}
inline Measure fontMeasure() {return [](const std::string& t,int size,int face){return measureText(t,size,face);};}
inline CGColorRef cg(std::uint32_t c) {
    static std::map<std::uint32_t,CGColorRef> cache;
    if(auto it=cache.find(c);it!=cache.end())return it->second;
    CGColorSpaceRef sp=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGFloat comp[]{((c>>16)&0xFF)/255.0,((c>>8)&0xFF)/255.0,(c&0xFF)/255.0,((c>>24)&0xFF)/255.0};
    CGColorRef r=CGColorCreate(sp,comp);CGColorSpaceRelease(sp);cache[c]=r;return r;
}
inline CGColorRef cgAlpha(std::uint32_t c,CGFloat a) {
    if(a>=1)return cg(c);
    return cg((std::uint32_t(std::lround(((c>>24)&0xFF)*a))<<24)|(c&0xFFFFFF));
}
// Paints a scene into the current context of a view w x h points: the panel scaled by u (points per panel pixel) and centred.
// liveBlur: the game's blur is under the view (the backdrop item is the shade only). contentAlpha: the pane's texts' fade.
inline void draw(CGContextRef ctx,const Scene& s,CGFloat w,CGFloat h,CGFloat panelW,CGFloat panelH,CGFloat u,bool liveBlur,CGFloat contentAlpha) {
    CGContextSaveGState(ctx);
    const CGFloat left=(w-panelW*u)/2,top=(h-panelH*u)/2;
    CGContextTranslateCTM(ctx,left,top);CGContextScaleCTM(ctx,u,u);
    const CGRect window=CGRectMake(-left/u,-top/u,w/u,h/u);
    CGContextSetTextMatrix(ctx,CGAffineTransformMakeScale(1,-1));
    for(const auto& it:s.items) {
        const CGFloat a=it.layer==scene::CONTENT?contentAlpha:1;
        CGContextSetFillColorWithColor(ctx,cgAlpha(it.color,a));CGContextSetStrokeColorWithColor(ctx,cgAlpha(it.color,a));
        switch(it.type) {
        case scene::RECT:CGContextFillRect(ctx,CGRectMake(it.x,it.y,it.w,it.h));break;
        case scene::DISC:CGContextFillEllipseInRect(ctx,CGRectMake(it.x,it.y,it.w,it.h));break;
        case scene::POLY:case scene::LINE: {
            if(it.points.size()<4)break;
            CGContextBeginPath(ctx);CGContextMoveToPoint(ctx,it.points[0],it.points[1]);
            for(std::size_t i=2;i+1<it.points.size();i+=2)CGContextAddLineToPoint(ctx,it.points[i],it.points[i+1]);
            if(it.type==scene::POLY){CGContextClosePath(ctx);CGContextFillPath(ctx);}
            else{CGContextSetLineWidth(ctx,it.stroke);CGContextSetLineJoin(ctx,kCGLineJoinMiter);CGContextSetLineCap(ctx,kCGLineCapButt);CGContextStrokePath(ctx);}
            break;
        }
        case scene::BACKDROP:
            if(liveBlur){CGContextSetFillColorWithColor(ctx,cg(layout::BACKDROP_SHADE));CGContextFillRect(ctx,window);}
            else {
                CGColorSpaceRef sp=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                const void* cs[]{cg(it.color),cg(it.color2)};CFArrayRef arr=CFArrayCreate(nullptr,cs,2,&kCFTypeArrayCallBacks);
                const CGFloat loc[]{0,1};CGGradientRef g=CGGradientCreateWithColors(sp,arr,loc);
                CGContextSaveGState(ctx);CGContextClipToRect(ctx,window);
                CGContextDrawLinearGradient(ctx,g,CGPointMake(0,CGRectGetMinY(window)),CGPointMake(0,CGRectGetMaxY(window)),0);
                CGContextRestoreGState(ctx);CGGradientRelease(g);CFRelease(arr);CGColorSpaceRelease(sp);
            }
            break;
        case scene::FILL:CGContextFillRect(ctx,window);break;
        case scene::ROUND: {
            const CGFloat r=std::min<CGFloat>(it.stroke,std::min(it.w,it.h)/2);
            CGPathRef p=CGPathCreateWithRoundedRect(CGRectMake(it.x,it.y,it.w,it.h),r,r,nullptr);
            CGContextAddPath(ctx,p);CGContextFillPath(ctx);CGPathRelease(p);break;
        }
        case scene::RING:CGContextSetLineWidth(ctx,it.stroke);CGContextStrokeEllipseInRect(ctx,CGRectMake(it.x,it.y,it.w,it.h));break;
        case scene::TEXT: {
            CTLineRef l=line(it.text,it.size,it.face,cgAlpha(it.color,a));
            const CGFloat tw=CTLineGetTypographicBounds(l,nullptr,nullptr,nullptr);
            const CGFloat baseline=it.y+it.h/2+(1.011-0.353)/2*it.size;
            const CGFloat tx=it.align==scene::LEFT?it.x:it.align==scene::RIGHT?it.x+it.w-tw:it.x+(it.w-tw)/2;
            if(it.shadow) {
                CGContextSaveGState(ctx);
                const CGFloat o=layout::shadowOffset(it.size)*u,b=layout::shadowBlur(it.size)*2*u; // (shadow sizes are in the device's base space)
                CGContextSetShadowWithColor(ctx,CGSizeMake(o,o),b,cgAlpha(layout::SHADOW,a));
            }
            CGContextSetTextPosition(ctx,tx,baseline);CTLineDraw(l,ctx);
            if(it.shadow)CGContextRestoreGState(ctx);
            CFRelease(l);break;
        }
        default:break;
        }
    }
    CGContextRestoreGState(ctx);
}
}

// The view the in-game menu is drawn in (over a live blur of the game). Takes no touches (PortQolTouchView routes them to the model).
@interface PortMenuSceneView : UIView
@property(nonatomic) std::function<const recomp::apple::menu::Scene*()> scene;   // the frame to draw now (nullptr: nothing)
@property(nonatomic) std::function<bool()> alertShown;
@property(nonatomic) BOOL liveBlur;
@end
#if !defined(PORT_MENU_SCENE_VIEW_INTERFACE_ONLY) // (the class is defined once: in Main.mm's unit; LauncherController.mm uses its interface)
@implementation PortMenuSceneView {CADisplayLink* fade_;std::string key_;CFTimeInterval since_;}
- (instancetype)initWithFrame:(CGRect)frame {
    if((self=[super initWithFrame:frame])){self.opaque=NO;self.backgroundColor=UIColor.clearColor;self.userInteractionEnabled=NO;self.contentMode=UIViewContentModeRedraw;}
    return self;
}
// SPEC 12.3 v2 (owner 2026-10-25: "Use alert layout A (message between the lines) on all devices."): the game's own alert screen with
// the message (alert-restart.png in the app, the owner's builds only: claude_build.py), scaled to fit on black. Without it (a public
// build) the drawn notice (MenuScenes.restartAlert).
+ (UIImage*)alertArt {
    static UIImage* art=[]{NSString* p=[NSBundle.mainBundle pathForResource:@"alert-restart" ofType:@"png"];
        UIImage* i=p?[UIImage imageWithContentsOfFile:p]:nil;
        std::fprintf(stderr,"APPLE_IOS_SIDE_MENU alert_art=%s\n",i?"game":"drawn");return i;}();
    return art;
}
- (void)fadeTick {[self setNeedsDisplay];if(CACurrentMediaTime()-since_>recomp::apple::menu::layout::FADE_MS/1000.0+0.02){[fade_ invalidate];fade_=nil;}}
- (void)drawRect:(CGRect)rect {
    (void)rect;
    namespace mp=recomp::apple::menu::paint;namespace ml=recomp::apple::menu::layout;
    if(!_scene)return;
    CGContextRef c=UIGraphicsGetCurrentContext();
    const CGFloat w=self.bounds.size.width,h=self.bounds.size.height;
    if(_alertShown && _alertShown())if(UIImage* art=[PortMenuSceneView alertArt];art && art.size.width>0 && art.size.height>0) {
        CGContextSetRGBFillColor(c,0,0,0,1);CGContextFillRect(c,self.bounds);
        const CGFloat k=std::min(w/art.size.width,h/art.size.height),aw=art.size.width*k,ah=art.size.height*k;
        [art drawInRect:CGRectMake((w-aw)*0.5,(h-ah)*0.5,aw,ah)];
        return;
    }
    const auto* s=_scene();if(!s)return;
    if(s->contentKey!=key_) { // (another page: its texts fade in)
        key_=s->contentKey;since_=CACurrentMediaTime();
        if(!fade_){fade_=[CADisplayLink displayLinkWithTarget:self selector:@selector(fadeTick)];[fade_ addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];}
    }
    const CGFloat alpha=ml::fadeAlpha((long long)((CACurrentMediaTime()-since_)*1000));
    const CGFloat u=std::min(w/ml::W,h/ml::H);
    mp::draw(c,*s,w,h,s->panelW,s->panelH,u,self.liveBlur,alpha); // (the scene's own panel: taller in the launcher's Frame::screen)
}
- (void)removeFromSuperview {[fade_ invalidate];fade_=nil;[super removeFromSuperview];}
@end
#endif
