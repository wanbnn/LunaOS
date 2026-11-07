/* Intel ICH AC'97 PCM output. Physical DMA addresses are identity mapped. */
#include "luna.h"
#include "settings.h"
static uint16_t mixer, bus;
static bool ac97_ready;
static uint32_t audio_end;
typedef struct {
    uint32_t address;
    uint16_t samples, flags;
} Descriptor;
static Descriptor descriptors[32] __attribute__((aligned(16)));
static int16_t pcm[48000] __attribute__((aligned(16)));
void audio_init(void) {
    for (int b = 0; b < 256; b++)
        for (int d = 0; d < 32; d++)
            for (int f = 0; f < 8; f++) {
                if (pci_read(b, d, f, 0) != 0x24158086)
                    continue;
                uint32_t a = pci_read(b, d, f, 0x10), c = pci_read(b, d, f, 0x14);
                if (!(a & 1) || !(c & 1))
                    continue;
                mixer = a & ~3u;
                bus = c & ~3u;
                uint32_t command = pci_read(b, d, f, 4);
                pci_write(b, d, f, 4, (command & 65535) | 5);
                outl(bus + 0x2c, inl(bus + 0x2c) | 2);
                uint32_t start = ticks;
                while (!(inl(bus + 0x30) & 0x100) && ticks - start < 100)
                    __asm__ volatile("hlt");
                if (!(inl(bus + 0x30) & 0x100))
                    return;
                outw(mixer, 0);
                outw(mixer + 2, 0x0808);
                outw(mixer + 0x18, 0x0808);
                outw(mixer + 0x20, 0);
                ac97_ready = true;
                audio_apply_settings();
                serial_write("AC97: Intel ICH PCM output ready / 48 kHz stereo\n");
                return;
            }
}
const char *audio_name(void) {
    return ac97_ready ? "AC97 / 48 kHz stereo" : "PIT / PC speaker";
}
void audio_beep(unsigned hz, unsigned duration) {
    if (prefs.muted || prefs.volume == 0)
        return;
    if (hz < 20 || hz > 20000)
        return;
    if (!ac97_ready) {
        speaker_beep(hz, duration);
        return;
    }
    unsigned frames = duration * 480;
    if (frames > 24000)
        frames = 24000;
    if (!frames)
        return;
    outb(bus + 0x1b, 0);
    outb(bus + 0x1b, 2);
    for (int i = 0; i < 100000 && (inb(bus + 0x1b) & 2); i++)
        ;
    unsigned phase = 0;
    for (unsigned i = 0; i < frames; i++) {
        phase += hz;
        if (phase >= 48000)
            phase -= 48000;
        int amplitude = 4500;
        if (i < 240)
            amplitude = amplitude * i / 240;
        if (frames - i < 240)
            amplitude = amplitude * (frames - i) / 240;
        int16_t sample = phase < 24000 ? amplitude : -amplitude;
        pcm[i * 2] = sample;
        pcm[i * 2 + 1] = sample;
    }
    memset(descriptors, 0, sizeof(descriptors));
    descriptors[0].address = (uint32_t)pcm;
    descriptors[0].samples = frames * 2;
    descriptors[0].flags = 0x8000;
    outl(bus + 0x10, (uint32_t)descriptors);
    outb(bus + 0x15, 0);
    outw(bus + 0x16, 0x1c);
    __asm__ volatile("" ::: "memory");
    outb(bus + 0x1b, 1);
    audio_end = ticks + duration + 10;
}
void audio_poll(void) {
    speaker_poll();
    if (ac97_ready && audio_end && (int32_t)(ticks - audio_end) >= 0) {
        outb(bus + 0x1b, 0);
        audio_end = 0;
    }
}
void audio_status(char *out, size_t cap) {
    out[0] = 0;
    append(out, "Audio: ", cap);
    append(out, audio_name(), cap);
    append(out, "\n", cap);
    if (ac97_ready) {
        char b[16];
        append(out, "PCM DMA status: ", cap);
        hex(b, inw(bus + 0x16));
        append(out, b, cap);
        append(out, "\n", cap);
    } else
        append(out, "PCM unavailable; PIT fallback only.\n", cap);
}

void audio_apply_settings(void) {
    if (!ac97_ready)
        return;
    int l = prefs.volume, r = prefs.volume;
    if (prefs.balance > 0)
        l = l * (100 - prefs.balance) / 100;
    else if (prefs.balance < 0)
        r = r * (100 + prefs.balance) / 100;
    unsigned gain = ((31 - l * 31 / 100) << 8) | (31 - r * 31 / 100);
    if (prefs.muted || !prefs.volume)
        gain |= 0x8000;
    outw(mixer + 2, gain);
}
