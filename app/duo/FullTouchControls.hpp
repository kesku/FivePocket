// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#include "TouchControlBindings.hpp"
#include <chrono>

// App-owned views only. Never inspect/change Apple's private overlay hierarchy.
// Normal route uses the app-owned backend; physical touch remains to qualify.
@interface PortFullTouchElement : UIControl
@property(nonatomic,copy) void (^publish)(CGPoint,CGFloat);
@property(nonatomic,copy) void (^activity)(BOOL);
@property(nonatomic,assign) BOOL stick;
@property(nonatomic,assign) BOOL digitalDirection;
@property(nonatomic,assign) BOOL held;
@property(nonatomic,strong) UILabel* label;
@property(nonatomic,strong) UIView* knob;
- (instancetype)initWithTitle:(NSString*)title stick:(BOOL)stick;
@end

@implementation PortFullTouchElement
- (instancetype)initWithTitle:(NSString*)title stick:(BOOL)stick {
    if((self=[super initWithFrame:CGRectZero])) {
        self.stick=stick;self.multipleTouchEnabled=NO;
        self.backgroundColor=[UIColor colorWithWhite:0.05 alpha:0.35];
        self.layer.borderColor=[UIColor colorWithWhite:1 alpha:0.65].CGColor;
        self.layer.borderWidth=1.5;
        self.label=[UILabel new];self.label.text=title;self.label.textColor=UIColor.whiteColor;
        self.label.font=[UIFont boldSystemFontOfSize:17];self.label.textAlignment=NSTextAlignmentCenter;
        self.label.userInteractionEnabled=NO;[self addSubview:self.label];
        self.knob=[UIView new];self.knob.userInteractionEnabled=NO;
        self.knob.backgroundColor=[UIColor colorWithWhite:1 alpha:0.55];
        self.knob.hidden=!stick;[self addSubview:self.knob];
        self.accessibilityLabel=title;self.isAccessibilityElement=YES;
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];self.layer.cornerRadius=MIN(self.bounds.size.width,self.bounds.size.height)/2;
    self.label.frame=self.bounds;
    const CGFloat diameter=MIN(self.bounds.size.width,self.bounds.size.height)*0.3;
    self.knob.bounds=CGRectMake(0,0,diameter,diameter);self.knob.layer.cornerRadius=diameter/2;
    // A layout/rotation cancels the old stick geometry rather than sticking an axis.
    if(self.held)[self cancelTrackingWithEvent:nil];
    self.knob.center=CGPointMake(CGRectGetMidX(self.bounds),CGRectGetMidY(self.bounds));
}
- (void)updateStick:(UITouch*)touch {
    const CGPoint point=[touch locationInView:self];
    const auto p=self.digitalDirection?
        recomp::apple::fulltouch::dpadPosition(point.x,point.y,self.bounds.size.width,self.bounds.size.height):
        recomp::apple::fulltouch::stickPosition(point.x,point.y,self.bounds.size.width,self.bounds.size.height);
    const CGFloat radius=MIN(self.bounds.size.width,self.bounds.size.height)*0.32;
    self.knob.center=CGPointMake(CGRectGetMidX(self.bounds)+p.x*radius,CGRectGetMidY(self.bounds)-p.y*radius);
    if(self.publish)self.publish(CGPointMake(p.x,p.y),0);
}
- (BOOL)beginTracking:(UITouch*)touch withEvent:(UIEvent*)event {
    (void)event;if(self.held || !self.enabled)return NO;
    self.held=YES;if(self.activity)self.activity(YES);
    self.backgroundColor=[UIColor colorWithWhite:0.3 alpha:0.5];
    if(self.stick)[self updateStick:touch];else if(self.publish)self.publish(CGPointZero,1);
    return YES;
}
- (BOOL)continueTracking:(UITouch*)touch withEvent:(UIEvent*)event {
    (void)event;if(!self.held)return NO;
    if(self.stick)[self updateStick:touch];return YES;
}
- (void)releaseTouch {
    if(!self.held)return;self.held=NO;
#if !defined(APPLE_TOUCH_MUTANT_RELEASE)
    if(self.publish)self.publish(CGPointZero,0); // End AND cancellation release.
#endif
    if(self.activity)self.activity(NO);
    self.backgroundColor=[UIColor colorWithWhite:0.05 alpha:0.35];
    self.knob.center=CGPointMake(CGRectGetMidX(self.bounds),CGRectGetMidY(self.bounds));
}
- (void)endTracking:(UITouch*)touch withEvent:(UIEvent*)event {(void)touch;(void)event;[self releaseTouch];}
- (void)cancelTrackingWithEvent:(UIEvent*)event {[self releaseTouch];[super cancelTrackingWithEvent:event];}
@end

@interface PortFullTouchControls : UIView
// App-owned view. The publisher is supplied by the app-owned input backend;
// no GCVirtualController or GameController handler is installed here.
- (instancetype)initWithPublisher:(void (^)(recomp::apple::fulltouch::Element,CGPoint,CGFloat))publisher;
- (instancetype)initWithPublisher:(void (^)(recomp::apple::fulltouch::Element,CGPoint,CGFloat))publisher
                     elementLimit:(NSUInteger)limit;
- (void)close;
- (void)setApplicationActive:(BOOL)active;
@end

@implementation PortFullTouchControls {
    recomp::apple::fulltouch::FadePolicy policy_;
    NSMutableArray<PortFullTouchElement*>* elements_;
    NSTimer* idleTimer_;
}
static double portFullTouchNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
- (instancetype)initWithPublisher:(void (^)(recomp::apple::fulltouch::Element,CGPoint,CGFloat))publisher {
    return [self initWithPublisher:publisher elementLimit:recomp::apple::fulltouch::ElementCount];
}
- (instancetype)initWithPublisher:(void (^)(recomp::apple::fulltouch::Element,CGPoint,CGFloat))publisher
                     elementLimit:(NSUInteger)limit {
    if(limit!=10 && limit!=recomp::apple::fulltouch::ElementCount)
        @throw [NSException exceptionWithName:NSInvalidArgumentException reason:@"Exact touch producer binding selection required." userInfo:nil];
    if((self=[super initWithFrame:CGRectZero])) {
        policy_=recomp::apple::fulltouch::FadePolicy(portFullTouchNow());
        self.backgroundColor=UIColor.clearColor;self.multipleTouchEnabled=YES;
        elements_=[NSMutableArray new];
        NSArray<NSString*>* names=recomp::apple::fulltouch::elementNames();
        NSArray<NSString*>* titles=recomp::apple::fulltouch::elementTitles();
        NSCAssert(names.count==recomp::apple::fulltouch::ElementCount,@"exact control bindings");
        __weak PortFullTouchControls* weak=self;
        for(NSUInteger i=0;i<limit;++i) {
            const auto binding=static_cast<recomp::apple::fulltouch::Element>(i);
            const BOOL stick=recomp::apple::fulltouch::directionElement(binding);
            PortFullTouchElement* element=[[PortFullTouchElement alloc] initWithTitle:titles[i] stick:stick];
            element.digitalDirection=binding==recomp::apple::fulltouch::Element::Dpad;
            element.accessibilityIdentifier=[@"game.touch." stringByAppendingString:titles[i]];
            element.publish=^(CGPoint position,CGFloat value) {
                if(publisher)publisher(binding,position,value);
            };
            element.activity=^(BOOL held){[weak activity:held];};
            [elements_ addObject:element];[self addSubview:element];
        }
        [self scheduleIdle];
    }
    return self;
}
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent*)event {
    if(policy_.closed() || !self.userInteractionEnabled)return NO;
    // Outside control hit regions, let the game's own setup UI receive touches.
    for(PortFullTouchElement* element in elements_)
        if([element pointInside:[self convertPoint:point toView:element] withEvent:event])return YES;
    return NO;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    const CGRect area=UIEdgeInsetsInsetRect(self.bounds,self.safeAreaInsets);
    const auto frames=recomp::apple::fulltouch::layout(area.origin.x,area.origin.y,area.size.width,area.size.height);
    for(NSUInteger i=0;i<elements_.count;++i) {
        const auto& r=frames.elements[i];elements_[i].frame=CGRectMake(r.x,r.y,r.width,r.height);
    }
}
- (void)activity:(BOOL)held {
    if(policy_.closed())return;
    [idleTimer_ invalidate];idleTimer_=nil;
    if(held)policy_.press(portFullTouchNow());else policy_.release(portFullTouchNow());
    [self.layer removeAllAnimations];
    [UIView performWithoutAnimation:^{self.alpha=1;}];
    if(!policy_.held())[self scheduleIdle];
}
- (void)scheduleIdle {
    if(policy_.closed() || policy_.held())return;
    [idleTimer_ invalidate];__weak PortFullTouchControls* weak=self;
    idleTimer_=[NSTimer timerWithTimeInterval:recomp::apple::fulltouch::IdleSeconds repeats:NO block:^(NSTimer*) {
        PortFullTouchControls* owner=weak;if(!owner || !owner->policy_.idle(portFullTouchNow()))return;
        [UIView animateWithDuration:recomp::apple::fulltouch::FadeSeconds delay:0
            options:UIViewAnimationOptionAllowUserInteraction|UIViewAnimationOptionBeginFromCurrentState
            animations:^{owner.alpha=recomp::apple::fulltouch::IdleAlpha;} completion:nil];
    }];
    [NSRunLoop.mainRunLoop addTimer:idleTimer_ forMode:NSRunLoopCommonModes];
}
- (void)close {
    if(policy_.closed())return;policy_.close();[idleTimer_ invalidate];idleTimer_=nil;
    for(PortFullTouchElement* element in elements_) {
        [element cancelTrackingWithEvent:nil];element.enabled=NO;
    }
    [self.layer removeAllAnimations];self.userInteractionEnabled=NO;[self removeFromSuperview];
}
- (void)setApplicationActive:(BOOL)active {
    if(policy_.closed())return;
    if(!active) {
        for(PortFullTouchElement* element in elements_)[element cancelTrackingWithEvent:nil];
        [idleTimer_ invalidate];idleTimer_=nil;
        [self.layer removeAllAnimations];
    }
    self.userInteractionEnabled=active;
#if !defined(APPLE_TOUCH_MUTANT_DISABLED)
    for(PortFullTouchElement* element in elements_)element.enabled=active;
#endif
    if(active)[self scheduleIdle];
}
- (void)dealloc {[idleTimer_ invalidate];}
@end
