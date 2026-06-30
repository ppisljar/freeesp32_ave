// Test fixtures for the Generator core round-trip + bug-contract suites.
// Plain Node ESM; no deps beyond node built-ins.

import { readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));

// The firmware example string, copied verbatim from
// config_parser_get_example() in main/config_parser.c. fetch() is unavailable
// in Node, so the canonical example lives here as a fixture.
export const EXAMPLE = `# 30-second demo: LED zones + binaural beat sweep
#
# LED line formats:
#   5-field (legacy):    time freq duty bright channel_mask
#   8-field (canonical): time freq duty bright R G B channel_mask
#
# White (W) channel is not supported. Old 9-field files must be re-saved
# as 8-field (drop the W column) or they will fail to parse.
#
# Channel mask bits (LED): bits 0-7 = channels 1-8 (uint8_t, 0x01-0xFF).
#   Bits 0-3 map the original four spec regions:
#     1=r1 inner-left, 2=r2 outer-left frame,
#     4=r3 outer-right frame, 8=r4 inner-right.
#   Bits 4-7 (channels 5-8, masks 0x10-0x80) are valid in the format;
#   they have no visible effect unless the channel-map (Kconfig) assigns
#   LED pixels to those channels.
#   Common values: 9=r1+r4, 15=all four legacy zones, 255=all 8 channels.
#
# Audio line:  A time freq pan volume mod channel  (channel index 1-16; 0 is rejected)
#
# Interpolation prefixes:  >value linear sweep,  *value quadratic ease,
#                          (no prefix) immediate step

# t = 0 — start binaural beat on ch1 (left pan) + ch2 (right pan),
# LED inner zones (r1+r4) BLUE at 8 Hz 30% brightness
0 8 50 30 0 0 255 9                  # 8-field: channels 1+4, blue
A 0 200 -100 60 0 1                  # ch1 audio: 200 Hz, left
A 0 208 100 60 0 2                   # ch2 audio: 208 Hz, right (= 8 Hz binaural)

# t = 10 s — sweep LED color blue→green and frequency 8 Hz→12 Hz,
# binaural beat sweeps from 8 Hz to 12 Hz (carrier stays 200 Hz)
10000 >12 50 30 0 >255 >0 9          # linear: freq 8→12, color blue→green
A 10000 200 -100 60 0 1               # ch1 holds at 200 Hz
A 10000 >212 100 60 0 2              # ch2 sweeps 208→212 Hz

# t = 20 s — quadratic ease back to slow alpha-band 8 Hz, color WHITE
20000 *8 50 30 *255 *255 *255 9      # quadratic ease freq + color to white
A 20000 200 -100 60 0 1
A 20000 *208 100 60 0 2

# t = 30 s — end: LEDs off, audio fades out linearly
30000 0 0 0 0 0 0 15                 # all 4 LED zones off (mask 15)
A 30000 200 -100 >0 0 1              # ch1 fade volume to 0
A 30000 208 100 >0 0 2               # ch2 fade volume to 0
`;

// Hand-written edge cases exercising every grammar construct.
export const EDGE_CASES = {
    legacy5: `# legacy 5-field LED
0 8 50 30 9
`,
    canonical8: `0 8 50 30 0 0 255 9
`,
    binaural_wave: `A 0 200 -100 60 0 1 208 1
`,
    noise_white: `A 0 0 0 20 0 9 0 4
`,
    noise_pink: `A 0 0 0 20 0 9 0 5
`,
    noise_brown: `A 0 0 0 20 0 9 0 6
`,
    sine_mods: `0 >12 ~60:90:100000 ~20:40:10000 25 50 0 255
`,
    all_periodic: `0 *8 ^10:20:500 50 /1:2:300 \\5:1:200 _0:255:1000 255
`,
    audio_mods: `A 0 ~100:200:1000 ^-50:50:2000 _0:80:500 /0:10:300 3
`,
    bg_line: `BG http://example.com/river.wav 50 30
0 8 50 30 0 0 255 9
`,
    bg_sdcard: `BG sdcard://surf.wav -25 80
A 0 200 -100 60 0 1
`,
    blanks_and_comments: `

# header comment

A 0 200 -100 60 0 1

# trailing comment
`,
    freq_r_only: `A 0 200 -100 60 0 1 208
`,
    channel_less: `A 0 200 -100 60 0
`,
    inline_comments: `0 8 50 30 0 0 255 9 # blue inner
A 0 200 -100 60 0 1 # left ear
`,
    mixed_batch: `0 8 50 30 0 0 255 255
A 0 200 -100 60 0 1
A 0 208 100 60 0 2
10000 >12 50 30 0 0 255 255
A 10000 >204 -100 60 0 1
A 10000 >212 100 60 0 2
`,
};

// Read the in-tree generator ledc corpus if available (read-only). Returns a
// map name -> text, or {} if the directory is unreadable.
export function loadLedcCorpus() {
    const dir = join(HERE, '..', '..', '..', 'freeesp32_ave_generator', 'ledc');
    const out = {};
    try {
        for (const name of readdirSync(dir)) {
            if (!name.endsWith('.ledc')) continue;
            try { out[name] = readFileSync(join(dir, name), 'utf8'); } catch (e) { /* skip */ }
        }
    } catch (e) { /* corpus not present — fixtures cover the cases */ }
    return out;
}
