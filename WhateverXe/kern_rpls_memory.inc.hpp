// Startup-only code correction. No new MMIO, GPU commands, buffer edits,
// forcewake, timers or routed runtime methods. No observer prerequisites.
static bool rplsMemoryAdmitted,rplsMemoryRequested;
static RPLSMemory::Receipt rplsMemoryReceipt;
static void rplsMemoryInstall(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size,
 KernelPatcher::KextInfo *graphics) {
 struct IO {
  KernelPatcher &patcher;size_t index;mach_vm_address_t base;size_t size;
  KernelPatcher::KextInfo *graphics;
  uintptr_t symbol(unsigned i) {
   const char *names[] {
    "__ZN25IGHardwareGlobalPageTable8mapRangeERK14IGAddressRangeyy",
    "__ZN25IGHardwareGlobalPageTable15mapRangeRotatedER33IGAddressRangeRotatedPageIteratorR25IGPhysicalSegmentIteratory",
    "__ZN25IGHardwareGlobalPageTable13mapRangeDummyERK14IGAddressRangey"
   };
   const auto result=patcher.solveSymbol<mach_vm_address_t>(index,names[i],base,size);
   patcher.clearError();return result>=base?result-base:UINTPTR_MAX;
  }
  bool copy(unsigned site,uint8_t *out,size_t n) {
   if(!graphics || site>size || n>size-site)return false;
   memcpy(out,reinterpret_cast<const void *>(base+site),n);return true;
  }
  bool patch(unsigned site,const uint8_t *before,const uint8_t *after,size_t n) {
   if(!graphics || site>size || n+1>size-site)return false;
   KernelPatcher::LookupPatch p {graphics,before,after,n,1};
   patcher.applyLookupPatch(&p,reinterpret_cast<uint8_t *>(base+site),n+1);
   const bool ok=patcher.getError()==KernelPatcher::Error::NoError;
   patcher.clearError();return ok;
  }
 } io {patcher,index,address,size,graphics};
 rplsMemoryReceipt=RPLSMemory::install(io,rplsMemoryRequested,rplsMemoryAdmitted);
 if(rplsMemoryAdmitted)if(auto data=OSData::withBytes(&rplsMemoryReceipt,sizeof(rplsMemoryReceipt))) {
  IORegistryEntry::getRegistryRoot()->setProperty("WEXRPLSMemory",data);data->release();
 }
}
