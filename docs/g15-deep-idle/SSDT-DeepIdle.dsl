/*
 * Deep Idle (S0 low-power idle) for macOS on a Dell G15 5530, whose firmware has Modern Standby only (no S3).
 * AppleACPIPlatform looks for two methods before it sleeps in S0:
 *   \_SB.LPS0  = 1  the platform supports Low Power S0 Idle (the FADT already has the low-power-S0 flag)
 *   \_GPE.LXEN = 1  run the pending _Lxx methods of wake GPEs on wake
 * macOS only (_OSI Darwin). Idea from OC-Little (AOAC). The 5530 DSDT has neither method.
 */
DefinitionBlock ("", "SSDT", 2, "G15", "DeepIdl", 0x00001000)
{
    Scope (\_SB)
    {
        Method (LPS0, 0, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                Return (One)
            }
            Return (Zero)
        }
    }

    Scope (\_GPE)
    {
        Method (LXEN, 0, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                Return (One)
            }
            Return (Zero)
        }
    }
}
