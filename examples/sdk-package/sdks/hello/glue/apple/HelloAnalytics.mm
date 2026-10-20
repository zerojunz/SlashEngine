// Glue code for an SDK whose API is Objective-C or Swift: a class conforming to the engine's
// protocol, which sdk.json's "backends" names and the game makes when it starts. Built with ARC.

#import "slash/services/apple.h"

@interface HelloAnalytics : NSObject <SlashAnalyticsBackend>
@end

@implementation HelloAnalytics

- (NSString*)name {
    return @"hello_objc";
}

- (void)logEvent:(NSString*)name parameters:(NSDictionary<NSString*, id>*)parameters {
    NSLog(@"hello analytics event %@ %@", name, parameters);
}

@end
