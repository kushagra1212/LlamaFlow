#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

extern "C" void SetMacDockIcon(const unsigned char* png_data, unsigned int len) {
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    NSData* data = [NSData dataWithBytes:png_data length:len];
    NSImage* image = [[NSImage alloc] initWithData:data];
    if (image) {
        [[NSApplication sharedApplication] setApplicationIconImage:image];
        [image release];
    }
    [pool release];
}
