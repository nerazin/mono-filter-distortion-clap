# Mono Filter Distortion 0.5.0

The Elektron Monomachine's track EQ, filter and distortion as a stereo CLAP
effect for Bitwig: EQF and EQG, then BASE, WIDTH, HP Q, LP Q and DIST, in the
hardware's order, working on incoming audio. Since 0.5.0 it also has the
kit's key tracking: notes sent to the plug-in move the high-pass and the
low-pass with their pitch, as on a Monomachine FX track. There is no filter
envelope.

Since 0.3.0 the DSP is no longer a curve-fitted approximation. It is a native
fixed-point port of the signal path in Monomachine OS 1.32B, written from the
DSP code and checked **sample for sample** against that code running in an
emulator (see [Accuracy](#accuracy)). The plug-in contains no emulator, no
firmware and no firmware data: every coefficient table is generated from a
formula when it starts.

## Build and install on macOS

The source consists of two C files (`source/mono_filter.c`, the CLAP wrapper,
and `source/mono_core.h`, the DSP) and the included CLAP headers. You'll need
Apple's command-line developer tools. Once those are installed, building
requires no firmware, emulator, JUCE or further network downloads.

1. If needed, open Terminal and install the tools:

   ```sh
   xcode-select --install
   ```

2. Clone this repository and build the plug-in:

   ```sh
   git clone https://github.com/nerazin/mono-filter-distortion-clap.git
   cd mono-filter-distortion-clap
   ./build_macos.sh
   ```

   The script creates and ad-hoc signs the universal Apple Silicon and Intel
   bundle at `build/Mono Filter Distortion.clap`. macOS 11 or later is required.
3. Quit Bitwig. In Finder, choose **Go → Go to Folder…** and enter
   `~/Library/Audio/Plug-Ins/CLAP/`. Create the `CLAP` folder if necessary.
4. Copy the **whole** `Mono Filter Distortion.clap` bundle from `build/` into
   that folder. Replace an older installed version rather than leaving two
   copies in the scanned plug-in directories.
5. Open Bitwig and rescan plug-ins under **Settings → Locations** if needed.

Version 0.5.0 keeps the plug-in ID and all parameter IDs of 0.1.0–0.4.0 and
adds LP KEY, HP KEY and a note input. It opens projects saved by any older
version with both switches **off**, so they sound as they did; a new instance
has both on, as the hardware's kit does. The latency is unchanged. A project
saved by 0.5.0 does not open in an older version, so keep a backup of
important projects before replacing one. 0.4.0 added EQF and EQG; 0.3.0
changed the sound and latency of 0.1.0 and 0.2.0. This is a separate effect
from Mono Chorus.

`build_macos.sh` was run on an Apple Silicon Mac and the resulting universal
bundle was loaded and rendered by the offline test host. It has **not** been
loaded in Bitwig as part of this work.

If macOS explicitly reports that the installed bundle is quarantined, quit
Bitwig and run:

```sh
xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/CLAP/Mono Filter Distortion.clap"
```

If it still doesn't load, run the included read-only diagnostic: type `bash `
in Terminal, drag `Diagnose_Mono_Filter_Distortion.sh` into the window and
press Return. It prints loader and signature details without sending data.

## Controls

| Control | Range and meaning |
| --- | --- |
| BASE | 0–127; high-pass position. |
| WIDTH | 0–127; low-pass position relative to BASE. A smaller value narrows the band. |
| HP Q | 0–127; high-pass resonance. |
| LP Q | 0–127; low-pass resonance. |
| DIST | −64 to +63, as on the Monomachine's display; 0 is the centre. Negative values only turn the level into the filter down (1.35 dB per step). Positive values raise the gain into the clipper that follows the filters. |
| EQF | 0–127; the EQ's centre frequency: 31.6 Hz at 0, 810 Hz at 64, 19.8 kHz at 127, about 14 steps per octave. It moves smoothly between steps. |
| EQG | −64 to +63, as on the display; the EQ's gain at EQF. −1 is flat. Below that each step cuts 0.47 dB more, to −29.6 dB at −64; from 0 up the boost runs from +0.9 dB to +36 dB at +63. Whole steps only: a value in between acts as the step below it. |
| Input | −24 to +24 dB before the EQ, filter and distortion. Not a Monomachine control. |
| Output | −24 to +24 dB after the wet/dry mix. Not a Monomachine control. |
| Mix | 0–100%; dry to filtered/distorted. Not a Monomachine control. |
| LP KEY | ON/OFF, on by default; the low-pass follows the note (KIT ASSIGN KEY: LPF). |
| HP KEY | ON/OFF, on by default; the high-pass follows the note (KIT ASSIGN KEY: HPF). |

All controls support CLAP automation, and all but the two switches support
modulation. The two channels are processed independently with the same
settings.

The seven Monomachine controls behave as its knobs do. A whole number is a knob
position; values in between are accepted and land between positions, as an LFO
does on the hardware. A change to a control's value is slewed the way the
Monomachine's main CPU slews a knob: every 384 samples the control moves a
quarter of the way to its new value. CLAP *modulation* (Bitwig modulators) is
added on top without that slew, once every 48 samples, as the hardware adds
its LFOs.

0 dBFS in the host is full scale at the Monomachine's converter. Both the
distortion and the resonance respond to level, so Input sets how hard they are
driven. At DIST 0 with a quiet signal the plug-in is at unity gain; a
full-scale sine comes out clipped at about −4.7 dBFS.

BASE 0 and WIDTH 127 are the most open settings, but nothing is bypassed
there: the EQ, the filters and the clipper are always in the path, and the top
octave rolls off (−2.6 dB at 15 kHz, −9 dB at 20 kHz). The EQ's defaults
(EQF 64, EQG 0) are not quite flat but +0.9 dB at 810 Hz; EQG −1 is flat.
Use Mix 0% for the dry signal.

The EQ is a single bell with a fixed width (Q about 2) and sits in front of
the filters, as on the hardware. So EQG also changes the distortion and the
resonance: a boost drives that band harder into the clipper, a cut takes it
out. Away from EQF the level does not change with EQG.

One knob quirk follows from the slew described above: a word moving up stops
one LSB short of its target, and EQG uses whole steps, so after you turn EQG
*up* it acts as the step below the one shown. Turned down, or set by loading a
project, it lands on the step shown. This is what Monomodule's model of the
main CPU does. It was not checked on a real unit. At host rates other than 44.1 kHz even the dry
path is band-limited by sample-rate conversion.

## Notes and key tracking

The plug-in has a note input (CLAP notes or MIDI). Notes set the track's
pitch and nothing else: they trigger no envelope and make no sound. As on the
hardware, the pitch stays at the last note after it is released. With one
voice, the last note held wins, and releasing it returns to the previous note
still held. Until a note arrives, and after a reset, the pitch is C4.

With a switch on, its filter moves 1:1 with the pitch: an octave up in pitch
is an octave up in cutoff, which is 8 BASE steps (a semitone is 2/3 of one).
The offset is absolute, not relative to some reference note. At C4 it is 661
positions of the filter's 128 per octave, 5.2 octaves, as much as 41 BASE
steps. So with HP KEY on and no note played, BASE 0 puts the high-pass where
BASE 41 would put it with the switch off, and the defaults cut bass that 0.4.0
passed: −4 dB at 100 Hz, −10.5 dB at 65 Hz and −21 dB at 40 Hz, measured
against HP KEY off. Turn HP KEY off, or play low notes, for the old open
bottom end. With WIDTH 127 the low-pass
stays fully open with or without LP KEY.

The pitch is what the Monomachine computes for an FX track at its defaults:
the note, plus the fixed +2 semitones an FX track's TUNE adds, with PORT at 0,
so a new note is reached in two blocks (127/128 of the way in the first). A
note change takes effect at the next 48-sample frame, as the main CPU sends
it. PORT, master tune, pitch bend and the LFO pitch page, which also move the
pitch on the hardware, are not controls here.

For a resonant, distorted band on a snare, try:

```text
BASE 32   WIDTH 52   HP Q 16   LP Q 48
DIST +20  Input 0 dB  Output -6 dB  Mix 100%
```

High resonance can ring for a long time and become loud.

## Sample rate and latency

The Monomachine runs at 44.1 kHz in blocks of 16 samples, with the filters and
clipper at 88.2 kHz inside. The plug-in does the same; at other host rates it
converts to and from 44.1 kHz with band-limited resampling. Latency is reported
to the host: **22 samples at 44.1 kHz, 114 at 48 kHz, 226 at 96 kHz and 448 at
192 kHz** (16 for the block, 6 through the filters, the rest for resampling).
The supported activation range is 1–768 kHz. Frequencies above the 44.1 kHz
path's Nyquist are not preserved.

## Accuracy

The reference is Monomachine OS 1.32B's DSP code running in
[Monomodule](https://github.com/shnolk/monomodule)'s emulated DSP56300, on a
THRU track with the filter envelope at rest and the rest of the EFX page at
its defaults. All figures are at 44.1 kHz.

| Check | Result |
| --- | --- |
| Filter-section output, 200 random cases: noise, sines and impulses from full scale to −40 dB, all seven controls jumping every few blocks (whole steps and words in between), key tracking in all four settings and switched now and then, the note word changing every 1–40 blocks (notes, and in half the cases also any 24-bit word) | 6,400,000 of 6,400,000 frames identical |
| The high-pass and low-pass positions (X:$5d9, Y:$5da) in every block of those cases | 400,000 of 400,000 blocks identical |
| The EQ's coefficients in every block of those cases | 400,000 of 400,000 blocks identical |
| The ten coefficient tables generated by the plug-in against the firmware's | 10,653 of 10,653 entries identical |
| The built plug-in against the firmware driven by Monomodule's own host model, 160 cases with all seven knobs moving at random times (so the knob slew is included), notes pressed and released every 1–30 blocks and the switches flipped now and then | 7,674,880 of 7,674,880 samples identical |
| The 77 impulse captures used to judge 0.1.0 and 0.2.0, at the filter section (0.3.0; the EQ at its defaults computes the same coefficients in 0.4.0) | identical in all 77 |
| The same 77 at the track's final output, after the hardware's VOL, pan and LEVEL | within 3 LSB of 24 bits |

For comparison, 0.2.0 on those 77 captures averaged 0.87 dB of magnitude error
with a worst case of 29.6 dB, and a median waveform error of 16% (worst 83%).

What this does and does not establish:

- "Identical" means the same 24-bit integers as the firmware produces in the
  emulator. It was **not** compared with a recording of a physical
  Monomachine, so it inherits whatever differences the emulator and
  Monomodule's host model have from the real machine, and it leaves out the
  analogue converters and output stage entirely.
- The plug-in's output is the signal after the filter section, scaled to unity
  and turned the right way up (the hardware's clipper inverts). On the
  hardware the rest of the track follows; with SRR and the delay send off that
  is a plain gain and two samples of delay.
- The filter envelope, SRR and the delay of a Monomachine track are not here;
  they are off. The pitch sources other than the note (PORT, master tune, pitch
  bend, the LFO pitch page) are at their defaults.
- The host-model comparison replaces the master-tune word Monomodule sends
  (−1 at 440 Hz) with the main OS's 0. The note stack is the same one on both
  sides of that comparison; how the main OS orders held notes was not traced.
- The note input was tried only in the offline test host, **not** in Bitwig,
  so how Bitwig routes notes to an audio effect was not checked.
- Modulation follows the timing and range of the hardware's LFOs but was not
  checked against them.
- At host rates other than 44.1 kHz the resampling around the core is this
  project's own and has no hardware counterpart.

`tests/test_core.c` pins the verified behaviour without needing the firmware:
it fails if the tables or the output for two fixed stimuli (one with key
tracking and moving notes) ever change. The
firmware comparison itself needs the OS file and a Monomodule build and is not
part of this repository.

## Attribution and licence

- The DSP in `source/mono_core.h` was written for this project from the
  behaviour of Monomachine OS 1.32B. It contains no Elektron code or data.
- [Monomodule](https://github.com/shnolk/monomodule) (AGPL-3.0), commit
  `23e541021b92d1a5978f36c0a552ac443376df1a`, was the reference for testing.
  No Monomodule or emulator code is linked or included.
- The plug-in source is GPL-3.0-only; see `LICENSE`. CLAP headers retain
  their own licence in `vendor/clap/LICENSE`.

This independent project is not affiliated with or endorsed by Elektron,
Bitwig or the Monomodule project.
