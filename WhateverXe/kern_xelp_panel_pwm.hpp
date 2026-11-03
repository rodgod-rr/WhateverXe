// CNP/ICP/ADP PWM facts (Linux v6.12 intel_backlight.c/intel_backlight_regs.h).
// Keep initial hardware period/polarity; VBT supplies missing initialization.
#ifndef KERN_XELP_PANEL_PWM_HPP
#define KERN_XELP_PANEL_PWM_HPP
#include <stdint.h>
namespace XELP {
// Donor-path brightness fallback (see XE::wrapADLSPanelBacklight): set once the
// PWM adapter declines the panel; Bypass marks the adapter's own duty write.
inline bool panelPWMDonorFallback=false,panelPWMBypass=false;
struct PanelPWM {
 uint32_t period {},polarity {};
 bool valid() const {return period && period!=~0U;}
 static PanelPWM select(uint32_t period,uint32_t control,uint32_t strap,uint32_t hz,bool activeLow) {
  if(period==~0U || control==~0U)return {};
  if(period)return {period,control&(1U<<29)};
  if(strap==~0U || !hz || hz>65535)return {};
  const auto clock=(strap&256)?24000000U:19200000U;
  return {(clock+hz/2)/hz,activeLow?1U<<29:0};
 }
 bool duty(uint32_t brightness,uint32_t &value) const {
  if(!valid() || brightness>65535)return false;
  // The donor's 32-bit multiply overflows for ordinary low-frequency panels.
  value=uint32_t(uint64_t(period)*brightness/65535);return true;
 }
};
}
#endif
