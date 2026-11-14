# Deep Idle sleep on an ADL-S/RPL-HX laptop (Dell G15 5530)

Branch `g15-deep-idle` = WhateverXe 0.8.0 (db438fd) plus the changes below. Tested on a Dell G15 5530: i5-13450HX,
UHD Graphics 8086:A78B (16 EU), internal 1920x1080 120 Hz eDP on DDI A, macOS Tahoe 26.7.2, WhateverXe Graphics 0.4.0.
The firmware has Modern Standby only (no S3), so macOS sleeps in Deep Idle (S0 low-power idle).

Result: sleep and wake work repeatedly, with the internal panel back every time (5 RTC cycles in a row, idle sleep,
lid-close sleep, a 1 h 44 min sleep on battery). The machine wakes from the lid, the internal keyboard, the trackpad,
the power button and USB.

## Display and GPU on wake (`kern_xe.cpp`)

The GPU keeps its state across Deep Idle. The 0.8.0 wake path treats wake as a reset, which caused EL-null and
context-ID-mismatch panics and GPU hangs.

1. **CSB realigned on wake, not reinitialized** (`wrapTGLCSBWake`). `TGLCSB::initialize` forces the CSB pointers to 11,
   while the hardware write pointer has moved on. If the hardware write pointer is valid (<12) and equals the HWS copy
   (offset 0xBC), only the software read head (`state.head`, `streamer+0x1C04`) is moved to it. Otherwise the original
   initialize runs. Calling Apple's `resetOnWake` instead breaks CSB12 (the "native/software head mismatch" panic).
   Registry: `G15WakeResync`. Off: `-wex-g15-wake-resync-off`.
2. **LightUpEDP without the PWM capture** (`wrapADLSLightUpEDP`). Here the panel PWM capture always fails (fuse
   0x42000 bit 24 is clear), and the wrapper returned NotReady. At boot that is hidden, but the full modeset on wake
   calls LightUpEDP: "FB0: Hardware mode set failed, err = 0xe00002d8" and a black panel. Without the capture, it now
   takes the native path with the period read from 0xC8254.
3. **Backlight PWM enable without the capture** (`wrapADLSPanelPWMEnable`): it wrote nothing when the capture was invalid.
   It now writes `0x80000000` (plus the polarity bit) to 0xC8250, so the backlight comes back on wake.
4. **AUX A power well before the modeset after a sleep** (`wrapADLSSetMode`, a hook on `hwSetMode`). On the
   Doze -> Wake path LightUpEDP is not called and AUX timed out. After a sleep, the hook powers the panel on if
   PP_STATUS (0xC7200) bit 31 is clear, and enables PWR_WELL_CTL_AUX2 (0x45444) bit 1, waiting for bit 0.
   Registry: `G15ADLSSetModePanel`. Off: `-wex-g15-auxa-off`.

Diagnostics kept in the branch (observation only): `G15SleepCounters`, `G15EngineRegs`, `G15ActiveIdx`, `G15StaleLog`,
plus a "G15-sono" panic text with the counters if a CSB fatal happens after a sleep.

## Wake sources

Found by disassembling AppleACPIPlatform and AppleAPIC on Tahoe; the boot-arg `acpi=0xC` logs the wake GPE decisions
("GPE[xx] type .. can wake from S..", "rejected").

- **Lid / power button: ACPI only, no kext change** (`SSDT-LIDWAKE.dsl`). macOS already arms the `_PRW` GPE of the lid
  and power button (0x18 here), but the EC never raises it in S0. AppleACPIPlatform reads an optional `PRW0` method on
  each device with a `_PRW`; its GPE becomes `acpi-wake-s0-gpe`, the one armed for Deep Idle. Pointing the lid's PRW0
  at the EC's own GPE (`ECDV._GPE`, 0x6E with eSPI) makes EC events wake the machine, as in Linux s2idle. Log:
  "Wake from Deep Idle ... due to LID0/Lid Open". The power button then works too (it is an EC event as well).
  `SSDT-DeepIdle.dsl` (LPS0/LXEN) is the usual Deep Idle enabler.
- **Keyboard and trackpad: `kern_g15_kbdwake.cpp`**, Lilu routes on `AppleACPIPlatformExpert::sleepPlatform` and
  `picSleepWake`.
  - Cause: `sleepPlatform` calls `picSleepWake(1)` -> `AppleAPICInterruptController::prepareForSleep`, which masks
    every IOAPIC line. In Deep Idle only the SCI line is reopened (`callPlatformFunction("HandleSleepWake", 2, line)` ->
    `prepareForDeepIdle(line)`). The PS/2 keyboard (IRQ 1) and the GPIO controller (IRQ 14) stay masked. On top of
    that, VoodooPS2 disables the keyboard interrupt in the 8042 command byte, and VoodooI2CHID masks the trackpad pad.
  - Keyboard: in `sleepPlatform`, after all drivers are asleep, enable the keyboard in the 8042 (0xAE, command byte with
    the keyboard IRQ and clock on, 0xF4). After `picSleepWake(1)`, reopen IRQ 1 with `HandleSleepWake(2, 1)`. Here the
    command byte went 0x64 -> 0x65 and the keyboard answered 0xFA. Log: "DarkWake to FullWake ... due to UserActivity".
  - Trackpad: enable GPI_IE of its pad and reopen IRQ 14. On `picSleepWake(0)`, before AppleAPIC restores the lines,
    the IE bit is restored and GPI_IS cleared, because the driver is still asleep and a level interrupt would keep
    firing. The I2C HID trackpad in SET_POWER(SLEEP) still signals a touch (GPI_IS bit set at wake). Log:
    "Wake from Deep Idle ... due to /HID Activity".
  - **Machine-specific:** the trackpad pad is hard-coded as GPP_E3 (VoodooGPIO pin 355, Alder Lake-S community 3 =
    4th memory resource of GPI0, GPI_IE 0x220 + 4 x group 1, bit 3). Take your values from `gpioPin` of the
    VoodooI2C device and VoodooGPIO's community tables. Registry: `G15KbdWake` and `G15KbdWakeReady`. Off:
    `-wex-g15-kbdwake-off` (both) or `-wex-g15-tpwake-off` (trackpad only).

## Earlier G15 changes in the same branch (see issue #31)

- VBT with two children on one DDI: keep the first, as i915 does, instead of dropping the whole table.
- Firmware CDCLK 172.8 MHz, outside the donor table: reprogram to 652.8 MHz before the probe.
- Backlight fallback to the donor path with the WhateverGreen BacklightRegistersFix rule (`wexblmin`, `wexblmax`).
- RPL-S GGTT entry mask (port of gkostadinov 8a29213), also for A78B rev 4.
- CSB lite-restore fix (port of gkostadinov 87df8e5).

## Build and tests

`python3 tools/build.py --sdk <MacKernelSDK> --lilu <Lilu.kext>`. `tools/test.py`: 201 of 208 pass. The 7 failures are
from the G15 changes:
- Mock-based tests that compile slices of `kern_xe.cpp`/`kern_xe.hpp` and do not know the new state (adlp_render,
  adls_panel_pwm, tgl_combo_pll_write, accelerator_resources).
- firmware_ports and tgl_csb, which assert the old VBT and CSB behaviour.
- boot_arguments: `wexblmin`/`wexblmax` do not start with `-wex`.
The tests were not updated.

## Later changes in the same branch (11-14 Nov)

- **Power in sleep:** solved. Overnight on battery the package stays in PC10 ~100% of the time, about 0.9-1.0 W
  (the earlier 19 W came from RC6 being stopped by the AGPM profile, see #87, and from the RTX path).
- **Execution forcewake** (`g15ExecWake`): render forcewake is held while an engine executes and released as soon as it is
  idle, for every engine (`-wex-g15-execwake-off` to disable). A still screen stays at about 1.2-1.7 W of package power.
- **Hang log:** each GPU restart writes per-engine scheduler state, registers and the CSB to
  `/private/var/log/g15-uhd-hang-*.txt` (FileIO through Lilu, because IOLog of an injected kext does not reach `log show`).
  `sudo sysctl debug.g15hang=1` writes one on demand.
- **eDP PSR1 (optional, `wexg15psr=3`):** hardware-tracked PSR1 for the internal panel, AUX A power held while it is on;
  with a still screen the package goes from about 2.05 W to 1.17 W. Runtime control with `sysctl debug.g15psr`.
- **Vblank waits with PSR active:** `hwWaitForVBlank` waits for the pipe frame counter, which stops while the panel is in
  PSR, so each wait timed out after 500 ms ("Wait for VBL timed out" twice a second, and the first frame after a still
  screen was delayed). A hook on `AppleIntelFramebufferController::hwWaitForVBlank` forces a PSR exit (write to
  `CURSURFLIVE` of pipe A, as i915 does) before the wait. Most waits come from `PerformFlipTransaction`. No measurable
  power cost (PSR residency still 97%). `sysctl debug.g15vbl=0` turns it off; counters in `debug.g15psr_info`.
- **#131 (boot logo repeated):** an experimental port of gupaixao's two steps is in the code but **off by default**
  (`-wex-g15-131`): on this unit `accel+0xF28` is 0x80000003 and every variant made the repeat longer. With the vblank
  hook above the repeat is down to a few frames.

## Not solved

- A rare UHD hang still happens (one on 14/11 after ~14 min of uptime: BCS context stuck, no GPU interrupt after any of
  the resets, WindowServer watchdog).
