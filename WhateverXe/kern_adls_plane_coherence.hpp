#ifndef KERN_ADLS_PLANE_COHERENCE_HPP
#define KERN_ADLS_PLANE_COHERENCE_HPP
#include <stdint.h>
// ADL-S eDP: keep PLANE_CTL / PLANE_STRIDE / PLANE_SURF of pipe A plane 1 coherent while the firmware (GOP)
// console is still the scanned-out buffer. Donor: AppleIntelTGLGraphicsFramebuffer 16.0.0.
//
// Invariant (the same as i915: skl_program_plane_arm writes PLANE_SURF last and intel_plane_initial adopts the
// BIOS plane with its real modifier): the hardware never arms a PLANE_SURF whose buffer does not match the armed
// PLANE_CTL (tiling) and PLANE_STRIDE. On Gen9+ CTL/STRIDE latch on the PLANE_SURF write, so the rule is applied
// at the MMIO boundary, on that write.
//
// Donor behavior this addresses:
//  - setupPlane (+0x1F50) shadows SURF (plane+0x120) from fb+0x4330, which is 0 for framebuffer 0: its memory is
//    GGTT 0, the LINEAR console left by the GOP. The shadow CTL (+0x100) is always tiled (X, 0x400) and the
//    shadow STRIDE (+0x118) is rowBytes >> 9.
//  - enablePlane (+0x1108) writes STRIDE/CTL and not its own SURF; programPipeScaler (+0x661C9) re-arms the
//    SURF read back from the hardware.
//  - At boot the SURF read back is 0 (console), so CTL X and STRIDE rowBytes/512 are armed over the linear
//    console: the boot logo is repeated (or squashed with a mirrored display) until the first flip. After a
//    display sleep a second re-arm from the shadow writes SURF 0 alone under CTL X until the first flip.
//
// Rules (state in State, pure decisions below; the caller serializes with a spinlock):
//  Boot   - when the hardware still holds the GOP plane after setupPlane (enabled, LINEAR, pitch of
//           boot_args.Video, SURF in GGTT 0 = memory of framebuffer 0, console size, 32 bpp, graphics mode, no -v)
//           the hardware triple is adopted: donor CTL/STRIDE writes are held (the hardware receives the adopted
//           value) while the SURF to arm is still the console. At the first SURF of another buffer the donor's
//           pair is written first and SURF last. Once per boot.
//  Wake   - when setupPlane switched the shadow SURF from a valid buffer V to the console (fb+0x4330 == 0) with
//           the same tiling/stride and the hardware still scans V with that descriptor, every console SURF write
//           is replaced by V until the first SURF of another buffer (the first flip).
//  Inert  - outside plane 1A, plane off (CTL bit 31 clear), layout changed by the donor, console not inherited,
//           -v / text mode, boot-arg -wex-g15-plane-coherence-off.
// Never: hold the plane off, use a timer, or write LINEAR over X content.
namespace ADLSPlaneCoherence {
constexpr uint32_t PlaneCtl=0x70180,PlaneStride=0x70188,PlaneSurf=0x7019C,PlaneSurfLive=0x701AC,PlaneSize=0x70190;
constexpr uint32_t Enable=0x80000000U,TilingMask=0x1C00U,StrideMask=0x7FFU,SurfMask=0xFFFFF000U;
// boot_args.Video.v_display (pexpert/i386/boot.h): 1 = GRAPHICS_MODE, 2 = FB_TEXT_MODE (-v).
constexpr uint32_t GraphicsMode=1;
// PLANE_CTL bits 12:10: 0 LINEAR, 1 X, 4 Y, 5 Yf (i915 PLANE_CTL_TILED_*).
inline uint32_t tiling(uint32_t ctl) {return (ctl&TilingMask)>>10;}
// GGTT 0 is where the GOP left the console and where the donor places the memory of framebuffer 0.
inline bool console(uint32_t surf) {return !(surf&SurfMask);}
inline bool sameLayout(uint32_t ctlA,uint32_t strideA,uint32_t ctlB,uint32_t strideB) {
 return tiling(ctlA)==tiling(ctlB) && (strideA&StrideMask)==(strideB&StrideMask);
}
// PLANE_STRIDE in bytes: linear 64 B, X 512 B, Y/Yf 128 B (the donor uses >> 9 and >> 7 in setupPlane).
inline uint32_t strideBytes(uint32_t ctl,uint32_t stride) {
 const uint32_t t=tiling(ctl),units=t==0 ? 64 : t==1 ? 512 : 128;return (stride&StrideMask)*units;
}
enum Reason : uint32_t {
 Unreadable=1,      // register unreadable (0xFFFFFFFF), PW1 off or framebuffer offline
 PlaneOff=2,        // plane disabled (CTL bit 31 clear)
 NotLinear=4,       // boot: the hardware no longer holds the LINEAR GOP plane
 NotConsole=8,      // boot: SURF outside GGTT 0, or framebuffer 0 with its own memory (fb+0x4330 != 0)
 Pitch=0x10,        // boot: STRIDE x 64 != boot_args.Video.v_rowBytes
 Size=0x20,         // boot: PLANE_SIZE != v_width x v_height
 Depth=0x40,        // boot: v_depth != 32
 Verbose=0x80,      // boot: -v or v_display is not graphics mode (console text would be X-tiled)
 NoPrior=0x100,     // wake: the shadow had no valid buffer before setupPlane
 NotReset=0x200,    // wake: setupPlane did not switch SURF to the console (nothing to correct)
 Layout=0x400,      // wake: tiling/stride changed (the previous buffer does not fit the new descriptor)
 Mismatch=0x800,    // wake: the hardware is not scanning the previous buffer with that descriptor
 Disabled=0x2000,   // the donor disabled the plane during the window: state closed
 Changed=0x4000,    // the donor changed tiling/stride during the wake window: state closed
 Incomplete=0x8000, // boot: own SURF arrived without a known donor CTL/STRIDE pair (passes as is)
 Tried=0x10000      // boot: adoption was already decided in this boot
};
struct Video {uint32_t display,rowBytes,width,height,depth;bool verbose;};
struct Hardware {bool read;uint32_t ctl,stride,surf,size;};
struct Shadow {uint32_t ctl,stride,surf;};
inline uint32_t adoptReasons(const Hardware &h,uint32_t fb4330,const Video &v) {
 if(!h.read || h.ctl==~0U || h.stride==~0U || h.surf==~0U || h.size==~0U)return Unreadable;
 uint32_t r=0;
 if(!(h.ctl&Enable))r|=PlaneOff;
 if(tiling(h.ctl))r|=NotLinear;
 if(!console(h.surf) || fb4330)r|=NotConsole;
 if(!v.rowBytes || strideBytes(h.ctl,h.stride)!=v.rowBytes)r|=Pitch;
 if(!v.width || !v.height || h.size!=(((v.height-1)<<16)|(v.width-1)))r|=Size;
 if(v.depth!=32)r|=Depth;
 if(v.verbose || v.display!=GraphicsMode)r|=Verbose;
 return r;
}
inline uint32_t wakeReasons(const Shadow &pre,const Shadow &post,uint32_t fb4330,const Hardware &h) {
 uint32_t r=0;
 if(console(pre.surf))r|=NoPrior;
 if(!console(post.surf) || post.surf!=fb4330)r|=NotReset;
 if(!(pre.ctl&Enable) || !(post.ctl&Enable))r|=PlaneOff;
 if(!tiling(post.ctl) || !sameLayout(pre.ctl,pre.stride,post.ctl,post.stride))r|=Layout;
 if(!h.read || h.ctl==~0U || h.stride==~0U || h.surf==~0U)r|=Unreadable;
 else if((h.surf&SurfMask)!=(pre.surf&SurfMask) || !(h.ctl&Enable) || !sameLayout(h.ctl,h.stride,pre.ctl,pre.stride))r|=Mismatch;
 return r;
}
enum class Mode : uint32_t {Idle=0,Boot=1,Wake=2};
struct State {
 Mode mode {Mode::Idle};
 bool bootTried {};
 uint32_t ctl {},stride {},surf {};        // coherent triple: adopted (boot) or the previous buffer's (wake)
 uint32_t reqCtl {},reqStride {};          // boot: last pair requested by the donor
 bool haveCtl {},haveStride {};
 uint32_t hwCtl {},hwStride {};            // boot: what was written to the hardware
 uint32_t lastReason {};
};
// Decision taken after setupPlane. pre/post: shadows before/after setupPlane; h: hardware at that point.
inline void decide(State &s,const Shadow &pre,const Shadow &post,uint32_t fb4330,const Hardware &h,const Video &v) {
 if(s.mode==Mode::Boot)return;  // later modesets of the same boot: the hold continues
 uint32_t r;
 if(console(pre.surf)) {
  r=s.bootTried ? uint32_t(Tried) : adoptReasons(h,fb4330,v);s.bootTried=true;
  if(!r) {
   s.mode=Mode::Boot;s.ctl=s.hwCtl=h.ctl;s.stride=s.hwStride=h.stride;s.surf=h.surf;
   s.haveCtl=s.haveStride=false;return;
  }
 } else {
  s.bootTried=true;
  r=wakeReasons(pre,post,fb4330,h);
  if(!r) {s.mode=Mode::Wake;s.ctl=pre.ctl;s.stride=pre.stride;s.surf=pre.surf;return;}
 }
 s.mode=Mode::Idle;s.lastReason=r;
}
inline bool active(const State &s) {return s.mode!=Mode::Idle;}
// Result of a CTL/STRIDE/SURF write: the value to write and, before it, up to two writes (STRIDE, CTL).
struct Write {uint32_t value;uint32_t count;uint32_t reg[2],data[2];bool changed;};
inline Write filter(State &s,uint32_t reg,uint32_t value) {
 Write w {value,0,{0,0},{0,0},false};
 if(s.mode==Mode::Idle || (reg!=PlaneCtl && reg!=PlaneStride && reg!=PlaneSurf))return w;
 const auto close=[&](uint32_t why){s.mode=Mode::Idle;s.lastReason=why;};
 if(s.mode==Mode::Boot) {
  if(reg==PlaneCtl) {
   if(!(value&Enable)){close(Disabled);return w;}  // never re-enable what the donor disables
   s.reqCtl=value;s.haveCtl=true;
   if(value!=s.ctl){w.value=s.ctl;w.changed=true;}
   s.hwCtl=w.value;return w;
  }
  if(reg==PlaneStride) {
   s.reqStride=value;s.haveStride=true;
   if(value!=s.stride){w.value=s.stride;w.changed=true;}
   s.hwStride=w.value;return w;
  }
  if(console(value))return w;  // re-arm of the console with the adopted descriptor (what the hardware has)
  // First own SURF: the donor's pair first, SURF last (a single arm).
  if(!s.haveCtl || !s.haveStride){close(Incomplete);return w;}
  if(s.reqStride!=s.hwStride){w.reg[w.count]=PlaneStride;w.data[w.count++]=s.reqStride;}
  if(s.reqCtl!=s.hwCtl){w.reg[w.count]=PlaneCtl;w.data[w.count++]=s.reqCtl;}
  s.mode=Mode::Idle;
  return w;
 }
 // Wake: CTL/STRIDE pass; if the donor changes the layout the previous buffer no longer fits.
 if(reg==PlaneCtl) {
  if(!(value&Enable))close(Disabled);
  else if(tiling(value)!=tiling(s.ctl))close(Changed);
  return w;
 }
 if(reg==PlaneStride) {
  if((value&StrideMask)!=(s.stride&StrideMask))close(Changed);
  return w;
 }
 if(console(value)){w.value=s.surf;w.changed=true;return w;}
 // Re-arm of the previous buffer itself (programPipeScaler: SURF read from the hardware) keeps the window open.
 if((value&SurfMask)==(s.surf&SurfMask))return w;
 s.mode=Mode::Idle;  // first SURF of another buffer (WindowServer flip): end of the window
 return w;
}
}
#endif
