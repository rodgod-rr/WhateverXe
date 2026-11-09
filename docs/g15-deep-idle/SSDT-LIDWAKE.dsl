/*
 * Lid wakes macOS from Deep Idle (Dell G15 5530).
 * For every device with a _PRW, AppleACPIPlatform also reads PRW0 (real Macs have it): the GPE in PRW0 becomes
 * "acpi-wake-s0-gpe", the GPE armed for S0 (Deep Idle) sleep instead of the _PRW one. The lid _PRW GPE (0x18 here) is
 * armed, but the EC never raises it in S0. Point the lid at the EC's own GPE instead (ECDV._GPE = 0x6E with eSPI), as
 * Linux does in s2idle (the EC GPE stays enabled for wake). Only macOS reads PRW0.
 * Adapt: use your EC _GPE value and your lid device path. Side effect to watch: other EC events waking the machine
 * (not seen here in a 1 h 44 min sleep on battery).
 */
DefinitionBlock ("", "SSDT", 2, "G15", "LidWake", 0x00001000)
{
    External (\_SB.LID0, DeviceObj)

    Scope (\_SB.LID0)
    {
        Name (PRW0, Package (0x02) { 0x6E, 0x03 })
    }
}
