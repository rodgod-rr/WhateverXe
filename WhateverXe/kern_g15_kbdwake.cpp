//
//  kern_g15_kbdwake.cpp
//  WhateverXe (fork G15): o teclado interno (PS/2) acorda o repouso Deep Idle do macOS.
//
//  09/11/2026, Dell G15 5530. Desmontagem do AppleACPIPlatform/AppleAPIC (docs/SONO-MACOS.md):
//  - AppleACPIPlatformExpert::sleepPlatform chama picSleepWake(1) -> AppleAPICInterruptController::prepareForSleep, que
//    MASCARA todas as linhas do IOAPIC; no Deep Idle so a SCI (GPEs) volta. O teclado PS/2 e a IRQ 1: fica mascarada.
//    Reabrir uma linha: callPlatformFunction("HandleSleepWake", false, 2, linha) -> prepareForDeepIdle(linha).
//  - O VoodooPS2 desliga o teclado no 8042 ao dormir (relogio e interrupcao do teclado no "command byte", e o teclado
//    para de enviar teclas). Mesmo com a IRQ 1 aberta, uma tecla nao geraria interrupcao.
//  Aqui:
//  - gancho em sleepPlatform (roda depois de todos os drivers dormirem): religa o teclado no 8042 (0xAE, command byte
//    com IRQ do teclado ligada e relogio ligado, 0xF4 = teclado volta a enviar teclas).
//  - gancho em picSleepWake: depois do modo 1 (mascarar tudo), reabre a IRQ 1 com HandleSleepWake(2, 1).
//  Trackpad (I2C HID Synaptics 06CB:CE81, TPD0 em modo GPIO, pino 355 = GPP_E3, nivel ativo em baixo): ao dormir o
//  VoodooI2CHID poe o trackpad em SLEEP e desliga o IE do pino; o controlador de GPIO e a IRQ 14 (mascarada no Deep
//  Idle). Aqui: no sleepPlatform liga o IE do GPP_E3 (comunidade 3 = 4o recurso de memoria do GPI0, GPI_IE 0x220 + 4 x
//  grupo 1, bit 3; limpa o GPI_IS antes) e no picSleepWake(1) reabre a IRQ 14. No picSleepWake(0) (despertar), ANTES
//  do AppleAPIC restaurar as linhas, devolve o IE ao valor de antes e limpa o IS: o driver ainda esta dormindo e uma
//  interrupcao de nivel do trackpad ficaria disparando. Desligar so o trackpad: -wex-g15-tpwake-off.
//  Na volta o AppleAPIC restaura todas as linhas (resumeFromSleep) e o VoodooPS2 reinicia o teclado como sempre.
//  Registro: propriedade G15KbdWake na raiz do ioreg (publicada no despertar). Desligar: boot-arg -wex-g15-kbdwake-off.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSData.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>

void g15KbdWakeInit();

namespace {

const char *acpiPath[] {"/System/Library/Extensions/AppleACPIPlatform.kext/Contents/MacOS/AppleACPIPlatform"};
KernelPatcher::KextInfo acpiPlatform {"com.apple.driver.AppleACPIPlatform", acpiPath, arrsize(acpiPath), {true}, {},
                                       KernelPatcher::KextInfo::Unloaded};

using SleepPlatformFn = IOReturn (*)(void *);
using PicSleepWakeFn = void (*)(void *, uint32_t);
SleepPlatformFn orgSleepPlatform {nullptr};
PicSleepWakeFn orgPicSleepWake {nullptr};

IOService *apic {nullptr};
const OSSymbol *handleSleepWake {nullptr};

// Registro: [0] marcador 0x6B57, [1] sono, [2] despertar, [3] modos picSleepWake vistos (bits), [4] status 0x64 antes,
// [5] command byte antes, [6] command byte depois, [7] resposta ao 0xF4, [8] erros do 8042, [9] HandleSleepWake(2,1),
// [10] apic achado, [11] campo 0xc8 do AppleAPIC (prepareForDeepIdle so age com 0x40), [12] entrada da linha 1 (baixo),
// [13] linha 1 alto, [14] entrada da linha 9 (SCI) baixo, [15] reservado. Trackpad: [16] GPIO mapeado, [17] GPI_IE do
// grupo antes, [18] GPI_IE ligado, [19] GPI_IS no despertar (bit 3 = o trackpad pediu), [20] HandleSleepWake(2,14),
// [21] entrada da linha 14, [22] GPI_IE devolvido, [23] vezes que o IE foi ligado
uint32_t rec[24] {0x6B57};
bool tpWake {true};
IOMemoryMap *gpioMap {nullptr};
volatile uint32_t *gpioIE {nullptr}, *gpioIS {nullptr};
uint32_t savedIE {0};
bool tpArmed {false};
constexpr uint32_t TP_BIT = 1u << 3;                 // GPP_E3 no grupo E (reg_num 1) da comunidade 3

inline uint8_t inb(uint16_t p) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
inline void outb(uint16_t p, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }

bool waitIn() {                                      // buffer de entrada do 8042 vazio (pode escrever)
    for (int i = 0; i < 2000; i++) { if (!(inb(0x64) & 2)) return true; IODelay(10); }
    return false;
}
bool readOut(uint8_t *v) {                           // byte disponivel na saida
    for (int i = 0; i < 2000; i++) { if (inb(0x64) & 1) { *v = inb(0x60); return true; } IODelay(10); }
    return false;
}
void flush() { for (int i = 0; i < 32 && (inb(0x64) & 1); i++) { (void)inb(0x60); IODelay(10); } }

void keyboardOn() {
    uint8_t cb = 0, ack = 0;
    uint32_t err = 0;
    rec[4] = inb(0x64);
    flush();
    if (!waitIn()) err |= 1; outb(0x64, 0xAE);                           // liga a interface do teclado
    if (!waitIn()) err |= 2; outb(0x64, 0x20);                           // le o command byte
    if (!readOut(&cb)) err |= 4;
    rec[5] = cb;
    uint8_t ncb = static_cast<uint8_t>((cb | 0x01) & ~0x10);             // IRQ do teclado ligada, relogio ligado
    if (!waitIn()) err |= 8; outb(0x64, 0x60);
    if (!waitIn()) err |= 16; outb(0x60, ncb);
    rec[6] = ncb;
    if (!waitIn()) err |= 32; outb(0x60, 0xF4);                          // teclado: enviar teclas
    if (!readOut(&ack)) err |= 64;
    rec[7] = ack;
    flush();
    rec[8] = err;
}

void findApic() {
    if (apic) return;
    OSDictionary *m = IOService::serviceMatching("AppleAPICInterruptController");
    if (!m) return;
    if (OSIterator *it = IOService::getMatchingServices(m)) {
        if (auto s = OSDynamicCast(IOService, it->getNextObject())) { s->retain(); apic = s; }
        it->release();
    }
    m->release();
    rec[10] = apic != nullptr;
}

void findGpio() {
    if (gpioMap) return;
    OSDictionary *m = IOService::nameMatching("GPI0");
    if (!m) return;
    if (OSIterator *it = IOService::getMatchingServices(m)) {
        if (auto dev = OSDynamicCast(IOService, it->getNextObject())) {
            if (IODeviceMemory *mem = dev->getDeviceMemoryWithIndex(3)) {
                if (mem->getLength() >= 0x1000) gpioMap = mem->map(kIOMapInhibitCache);
            }
        }
        it->release();
    }
    m->release();
    if (!gpioMap) return;
    auto base = reinterpret_cast<volatile uint8_t *>(gpioMap->getVirtualAddress());
    gpioIE = reinterpret_cast<volatile uint32_t *>(base + 0x220 + 1 * 4);
    gpioIS = reinterpret_cast<volatile uint32_t *>(base + 0x200 + 1 * 4);
    rec[16] = 1;
}

void trackpadArm() {
    if (!tpWake) return;
    findGpio();
    if (!gpioIE) return;
    savedIE = *gpioIE;
    rec[17] = savedIE;
    *gpioIS = TP_BIT;                                // limpa um pedido velho
    *gpioIE = savedIE | TP_BIT;
    rec[18] = *gpioIE;
    tpArmed = true;
    rec[23]++;
}

void trackpadDisarm() {
    if (!tpArmed) return;
    rec[19] = *gpioIS;
    *gpioIE = (*gpioIE & ~TP_BIT) | (savedIE & TP_BIT);
    *gpioIS = TP_BIT;
    rec[22] = *gpioIE;
    tpArmed = false;
}

IOReturn wrapSleepPlatform(void *pe) {
    rec[1]++;
    findApic();
    keyboardOn();
    trackpadArm();
    return orgSleepPlatform(pe);
}

void wrapPicSleepWake(void *pe, uint32_t mode) {
    if (mode == 0) trackpadDisarm();                 // antes do AppleAPIC reabrir a IRQ 14 com o driver dormindo
    orgPicSleepWake(pe, mode);
    if (mode < 8) rec[3] |= 1u << mode;
    if (mode == 1 && apic && handleSleepWake) {
        rec[9] = apic->callPlatformFunction(handleSleepWake, false, reinterpret_cast<void *>(2), reinterpret_cast<void *>(1),
                                            nullptr, nullptr);
        auto base = reinterpret_cast<uint8_t *>(apic);
        rec[11] = *reinterpret_cast<uint32_t *>(base + 0xc8);
        auto shadow = *reinterpret_cast<uint32_t **>(base + 0xd8);
        if (shadow && *reinterpret_cast<uint32_t *>(base + 0xe0) > 14) { rec[12] = shadow[2]; rec[13] = shadow[3]; rec[14] = shadow[18]; rec[21] = shadow[28]; }
        if (tpArmed)
            rec[20] = apic->callPlatformFunction(handleSleepWake, false, reinterpret_cast<void *>(2), reinterpret_cast<void *>(14),
                                                 nullptr, nullptr);
    } else if (mode == 0) {
        rec[2]++;
        if (auto d = OSData::withBytes(rec, sizeof rec)) {
            IORegistryEntry::getRegistryRoot()->setProperty("G15KbdWake", d);
            d->release();
        }
    }
}

void processAcpi(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
    if (index != acpiPlatform.loadIndex) return;
    handleSleepWake = OSSymbol::withCStringNoCopy("HandleSleepWake");
    KernelPatcher::RouteRequest routes[] {
        {"__ZN23AppleACPIPlatformExpert13sleepPlatformEv", wrapSleepPlatform, orgSleepPlatform},
        {"__ZN23AppleACPIPlatformExpert12picSleepWakeEj", wrapPicSleepWake, orgPicSleepWake},
    };
    bool ok = patcher.routeMultiple(index, routes, address, size);
    patcher.clearError();
    IORegistryEntry::getRegistryRoot()->setProperty("G15KbdWakeReady", ok && orgSleepPlatform && orgPicSleepWake);
}

} // namespace

void g15KbdWakeInit() {
    if (checkKernelArgument("-wex-g15-kbdwake-off")) return;
    tpWake = !checkKernelArgument("-wex-g15-tpwake-off");
    lilu.onKextLoadForce(&acpiPlatform, 1, processAcpi, nullptr);
}
