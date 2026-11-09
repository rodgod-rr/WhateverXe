#ifndef KERN_RPLS_MEMORY_HPP
#define KERN_RPLS_MEMORY_HPP
#include <stdint.h>
#include <stddef.h>
// Gen12 system-memory GGTT entries hold the page address and present bit.
// GGTT flag inputs are separate from PPGTT PAT and permission fields.
namespace RPLSMemory {
constexpr uint64_t Old=0xFFFFFF8000000FFEULL,New=Old&~0xFFEULL;
constexpr unsigned Sites[] {0x1033D,0x104D2,0x106A1};
constexpr unsigned Functions[] {0x10324,0x103A0,0x10680};
constexpr uint8_t Instructions[3][13] {
 {0x48,0xB8,0xFE,0x0F,0,0,0x80,0xFF,0xFF,0xFF,0x49,0x21,0xC0},
 {0x48,0xB8,0xFE,0x0F,0,0,0x80,0xFF,0xFF,0xFF,0x49,0x21,0xC6},
 {0x48,0xB9,0xFE,0x0F,0,0,0x80,0xFF,0xFF,0xFF,0x48,0x21,0xCA}
};
inline bool admitted(uint32_t identity,uint32_t revision,bool physicalTahoe) {
 // G15: A78B (RPL-HX/ADL-S small GT) rev 4 uses the same donor accelerator sites.
 return (identity==0xA7808086U || identity==0xA78B8086U) && revision==4 && physicalTahoe;
}
inline bool prepare(unsigned site,const uint8_t *before,uint8_t *after,size_t n) {
 if(site>=3 || !before || !after || n!=13)return false;
 for(unsigned i=0;i<13;++i)if(before[i]!=Instructions[site][i])return false;
 for(unsigned i=0;i<13;++i)after[i]=before[i];
 after[2]=static_cast<uint8_t>(New);after[3]=static_cast<uint8_t>(New>>8);return true;
}
inline uint64_t mask(const uint8_t *p) {uint64_t v;__builtin_memcpy(&v,p+2,8);return v;}
struct Receipt {uint64_t q[16] {1,0,0,0,0,New};};
static_assert(sizeof(Receipt)==128,"memory receipt");
inline bool ready(const Receipt &r) {
 return r.q[0]==1 && r.q[1]==1 && r.q[2]==1 && r.q[3]==1 && r.q[4]==7 && r.q[5]==New && r.q[12]==7 &&
  r.q[6]==Old && r.q[7]==Old && r.q[8]==Old &&
  r.q[9]==New && r.q[10]==New && r.q[11]==New;
}
template<class IO> Receipt install(IO &io,bool requested,bool admittedProfile) {
 Receipt r;auto q=r.q;q[1]=requested;q[2]=admittedProfile;
 if(!admittedProfile)return r;
 uint8_t before[3][13] {},after[3][13] {},actual[3][13] {};
 for(unsigned i=0;i<3;++i) {
  if(io.symbol(i)!=Functions[i] || !io.copy(Sites[i],before[i],13) ||
   !prepare(i,before[i],after[i],13))return r;
  q[12]|=1U<<i;
 }
 q[3]=1;for(unsigned i=0;i<3;++i)q[6+i]=mask(before[i]);
 if(requested)for(unsigned i=0;i<3;++i) {
  if(!io.patch(Sites[i],before[i],after[i],13))break;
  if(!io.copy(Sites[i],actual[i],13))break;
  bool same=true;for(unsigned j=0;j<13;++j)if(actual[i][j]!=after[i][j])same=false;
  if(!same)break;q[4]|=1U<<i;
 }
 for(unsigned i=0;i<3;++i)if(io.copy(Sites[i],actual[i],13))q[9+i]=mask(actual[i]);
 return r;
}
}
#endif
