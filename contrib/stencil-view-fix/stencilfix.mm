// stencil-view-fix: workaround for wrong stencil reads through a pixel-format view on the WhateverXe Graphics
// (AppleIntelTGLGraphicsMTLDriver) Metal driver, seen on an ADL-S/RPL-HX GT1 iGPU (8086:A78B, Dell G15 5530).
//
// Bug: rendering stencil into a Depth32Float_Stencil8 (or Depth24Unorm_Stencil8) texture and then sampling it in a
// shader through newTextureViewWithPixelFormat:X32_Stencil8 (X24_Stencil8) returns wrong data (about 43% of a stencil
// rectangle reads back, plus garbage outside it, in 16x16-aligned blocks). The stencil itself is correct: a stencil
// test (Equal) and a blit to a buffer (StencilFromDepthStencil) both see the exact values, and the view of a blit COPY
// of the same texture reads correctly. allowGPUOptimizedContents = NO and MTLTextureUsagePixelFormatView do not help.
// Unity 6 (Novabench 3D) samples stencil this way in its lighting passes -> white blocks on lit/reflective areas.
//
// Workaround: this dylib replaces impostor.dylib inside AppleIntelTGLGraphicsMTLDriver.bundle (the driver loads it
// via @loader_path in every Metal process) and re-exports the original, renamed impostor-real.dylib. It swizzles
// MTLIGAccelTexture so a stencil view of a depth-stencil texture is a view of a shadow copy, refreshed by a blit at the
// end of every render pass that writes the texture. System processes (/System, /usr: WindowServer) are left alone.
// Mode file /Library/Application Support/StencilViewFix/mode: copy (default) | noopt (allowGPUOptimizedContents = NO; does NOT fix it
// on this driver) | both | off.   Log: log show --last 10m --predicate 'subsystem == "org.wex.stencilviewfix"'
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <os/log.h>
#include <atomic>
#include <mach-o/dyld.h>

static os_log_t lg;
static bool doCopy, doNoopt;
static std::atomic<long> nShadow{0}, nCopies{0}, nNoopt{0};

static bool dsFormat(MTLPixelFormat f) { return f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatDepth24Unorm_Stencil8; }
static bool stencilView(MTLPixelFormat f) { return f == MTLPixelFormatX32_Stencil8 || f == MTLPixelFormatX24_Stencil8; }

// swizzle on this class only (an inherited method gets an override that calls the original)
static void hook(Class c, SEL s, id (^mk)(IMP)) {
    if (!c) return;
    Method m = class_getInstanceMethod(c, s);
    if (!m) { os_log(lg, "stencil-view-fix: %{public}s lacks %{public}s", class_getName(c), sel_getName(s)); return; }
    IMP o = method_getImplementation(m);
    class_replaceMethod(c, s, imp_implementationWithBlock(mk(o)), method_getTypeEncoding(m));
}

static const void *kShadow = &kShadow, *kSrc = &kSrc, *kCB = &kCB;

// shadow copy with the same type/slices/mips (Unity uses 2D and 1-slice 2DArray); multisample and buffer textures are skipped
static id<MTLTexture> shadowFor(id<MTLTexture> t) {
    id<MTLTexture> sh = objc_getAssociatedObject(t, kShadow);
    if (sh) return sh;
    MTLTextureType ty = t.textureType;
    bool ok = (ty == MTLTextureType2D || ty == MTLTextureType2DArray || ty == MTLTextureTypeCube || ty == MTLTextureTypeCubeArray)
              && t.sampleCount == 1 && !t.buffer && !t.parentTexture;
    if (!ok) { os_log(lg, "stencil-view-fix: SKIPPED %lux%lu fmt %lu type %lu samples %lu slices %lu parent %d buffer %d", (unsigned long)t.width,
                      (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long)ty, (unsigned long)t.sampleCount,
                      (unsigned long)t.arrayLength, t.parentTexture != nil, t.buffer != nil); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = ty; d.pixelFormat = t.pixelFormat; d.width = t.width; d.height = t.height; d.depth = t.depth;
    d.arrayLength = t.arrayLength; d.mipmapLevelCount = t.mipmapLevelCount; d.sampleCount = 1;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    d.storageMode = MTLStorageModePrivate;
    sh = [t.device newTextureWithDescriptor:d];
    if (sh) { objc_setAssociatedObject(t, kShadow, sh, OBJC_ASSOCIATION_RETAIN); ++nShadow; }
    os_log(lg, "stencil-view-fix: shadow copy %lux%lu fmt %lu type %lu slices %lu mips %lu %{public}s (total %ld)", (unsigned long)t.width,
           (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long)ty, (unsigned long)t.arrayLength,
           (unsigned long)t.mipmapLevelCount, sh ? "ok" : "FAILED", nShadow.load());
    return sh;
}

static void install() {
    Class tex = objc_getClass("MTLIGAccelTexture"), cbc = objc_getClass("MTLIGAccelCommandBuffer"),
          rec = objc_getClass("MTLIGAccelRenderCommandEncoder"), dev = objc_getClass("MTLIGAccelDevice");
    os_log(lg, "stencil-view-fix: loaded in %{public}s (copy %d, noopt %d; classes %d%d%d%d)", getprogname(), doCopy, doNoopt,
           tex != nil, cbc != nil, rec != nil, dev != nil);
    if (doNoopt && dev) {
        SEL s = @selector(newTextureWithDescriptor:);
        hook(dev, s, ^id(IMP o) { return ^id(id self, MTLTextureDescriptor *d) {
            if (d && dsFormat(d.pixelFormat) && (d.usage & (MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView)) &&
                d.allowGPUOptimizedContents) {
                MTLTextureDescriptor *dd = [d copy]; dd.allowGPUOptimizedContents = NO;
                if (++nNoopt <= 8) os_log(lg, "stencil-view-fix: noopt %lux%lu fmt %lu", (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat);
                return ((id (*)(id, SEL, id))o)(self, s, dd);
            }
            return ((id (*)(id, SEL, id))o)(self, s, d); }; });
    }
    if (!doCopy || !tex || !cbc || !rec) return;
    SEL v1 = @selector(newTextureViewWithPixelFormat:);
    hook(tex, v1, ^id(IMP o) { return ^id(id self, MTLPixelFormat f) {
        id<MTLTexture> t = self;
        if (dsFormat(t.pixelFormat) && stencilView(f)) {
            os_log(lg, "stencil-view-fix: view %lu->%lu %lux%lu", (unsigned long)t.pixelFormat, (unsigned long)f, (unsigned long)t.width, (unsigned long)t.height);
            if (id<MTLTexture> sh = shadowFor(t)) return ((id (*)(id, SEL, MTLPixelFormat))o)(sh, v1, f); }
        return ((id (*)(id, SEL, MTLPixelFormat))o)(self, v1, f); }; });
    SEL v2 = @selector(newTextureViewWithPixelFormat:textureType:levels:slices:);
    hook(tex, v2, ^id(IMP o) { return ^id(id self, MTLPixelFormat f, MTLTextureType ty, NSRange l, NSRange sl) {
        id<MTLTexture> t = self;
        if (dsFormat(t.pixelFormat) && stencilView(f)) {
            os_log(lg, "stencil-view-fix: view2 %lu->%lu type %lu %lux%lu", (unsigned long)t.pixelFormat, (unsigned long)f, (unsigned long)ty, (unsigned long)t.width, (unsigned long)t.height);
            if (id<MTLTexture> sh = shadowFor(t)) return ((id (*)(id, SEL, MTLPixelFormat, MTLTextureType, NSRange, NSRange))o)(sh, v2, f, ty, l, sl); }
        return ((id (*)(id, SEL, MTLPixelFormat, MTLTextureType, NSRange, NSRange))o)(self, v2, f, ty, l, sl); }; });
    SEL rs = @selector(renderCommandEncoderWithDescriptor:);
    hook(cbc, rs, ^id(IMP o) { return ^id(id self, MTLRenderPassDescriptor *d) {
        id enc = ((id (*)(id, SEL, id))o)(self, rs, d);
        id<MTLTexture> t = d.depthAttachment.texture ?: d.stencilAttachment.texture;
        if (t && !objc_getAssociatedObject(t, kShadow) && t.parentTexture) t = t.parentTexture;  // pass rendered through a view (slice/mip)
        if (enc && t && objc_getAssociatedObject(t, kShadow)) {
            objc_setAssociatedObject(enc, kSrc, t, OBJC_ASSOCIATION_RETAIN);
            objc_setAssociatedObject(enc, kCB, self, OBJC_ASSOCIATION_ASSIGN);
        }
        return enc; }; });
    SEL ee = @selector(endEncoding);
    hook(rec, ee, ^id(IMP o) { return ^void(id self) {
        ((void (*)(id, SEL))o)(self, ee);
        id<MTLTexture> src = objc_getAssociatedObject(self, kSrc);
        if (!src) return;
        id<MTLTexture> sh = objc_getAssociatedObject(src, kShadow);
        id<MTLCommandBuffer> cb = objc_getAssociatedObject(self, kCB);
        objc_setAssociatedObject(self, kSrc, nil, OBJC_ASSOCIATION_RETAIN);
        if (sh && cb) { id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl copyFromTexture:src toTexture:sh]; [bl endEncoding];
            long c = ++nCopies; if (c == 1 || c % 500 == 0) os_log(lg, "stencil-view-fix: %ld blit copies", c); } }; });
}

__attribute__((constructor)) static void init() { @autoreleasepool {
    lg = os_log_create("org.wex.stencilviewfix", "fix");
    // safety: do nothing in system processes (WindowServer etc.)
    char exe[1024]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) == 0 && (!strncmp(exe, "/System/", 8) || !strncmp(exe, "/usr/", 5))) return;
    NSString *m = [NSString stringWithContentsOfFile:@"/Library/Application Support/StencilViewFix/mode" encoding:NSUTF8StringEncoding error:nil];
    m = [m stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"copy";
    if (!m.length) m = @"copy";
    doCopy = [m isEqualToString:@"copy"] || [m isEqualToString:@"both"];
    doNoopt = [m isEqualToString:@"noopt"] || [m isEqualToString:@"both"];
    install();
} }
