#ifndef LUNA_SDHCI_POLICY_H
#define LUNA_SDHCI_POLICY_H
/* PCI IDs denote dedicated, soldered eMMC functions, not removable SD slots.
 * Protocol remains SDHCI PIO; no vendor-specific MMIO offsets are guessed. */
static inline bool sdhci_pci_embedded(uint16_t vendor,uint16_t device) {
    if(vendor!=0x8086)return false;
    switch(device) {
    case 0x0f14: case 0x0f50: case 0x2294: /* Bay Trail / Braswell */
    case 0x0acc: case 0x1acc: case 0x5acc: /* Broxton / Apollo Lake */
    case 0x31cc: case 0x9d2b: case 0x9dc4: /* Gemini Lake / PCH */
    case 0x34c4: case 0x4b47: case 0x02c4:
    case 0x4dc4: case 0x98c4: case 0x54c4:
        return true;
    default:return false;
    }
}
/* SDHCI v3 has a 10-bit divided-clock field (even divisors through 2046).
 * v1/v2 supports only powers of two through 256. Never overclock at init. */
static inline bool sdhci_clock_divider(unsigned base_hz,unsigned hz,
                                      unsigned version,uint16_t *encoded) {
    if(!base_hz || !hz || !encoded)return false;
    unsigned div=1;
    if(version>=2) {
        if(base_hz>hz) {
            div=base_hz/hz+(base_hz%hz!=0);
            if(div&1u)div++;
            if(div>2046)return false;
        }
    } else {
        while((base_hz+div-1)/div>hz && div<256)div*=2;
        if((base_hz+div-1)/div>hz)return false;
    }
    unsigned field=div==1?0:div/2;
    *encoded=(uint16_t)(((field&255u)<<8)|((field&0x300u)>>2));
    return true;
}
#endif
