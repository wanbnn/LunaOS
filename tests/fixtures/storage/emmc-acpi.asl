DefinitionBlock ("", "SSDT", 2, "LUNAOS", "EMMCMMIO", 1)
{
    Scope (\_SB)
    {
        Device (EMMC)
        {
            Name (_HID, "80860F14")
            Name (_UID, "1")
            Name (PWRD, Zero)
            Name (HPWR, Zero)
            PowerResource (PWR0, 0, 0)
            {
                Method (_STA, 0, NotSerialized) { Return (HPWR) }
                Method (_ON, 0, NotSerialized) { Store (One, HPWR) }
                Method (_OFF, 0, NotSerialized) { Store (Zero, HPWR) }
            }
            Name (_PR0, Package () { PWR0 })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
            Method (_PS0, 0, NotSerialized)
            {
                If (LEqual (HPWR, One)) { Store (One, PWRD) }
            }
            Method (_PS3, 0, NotSerialized) { Store (Zero, PWRD) }
            Name (RSRC, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0xFED80000, 0x1000)
            })
            Method (_CRS, 0, NotSerialized)
            {
                // Discovery must request D0 before consuming resources.
                If (LEqual (PWRD, Zero)) { Return (Buffer () { 0x79, 0x00 }) }
                Return (RSRC)
            }
        }
    }
}
