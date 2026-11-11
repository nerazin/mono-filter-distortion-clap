# Mono Filter Distortion 0.3.0

The Elektron Monomachine's track filter and distortion as a stereo CLAP effect
for Bitwig: BASE, WIDTH, HP Q, LP Q and DIST, working directly on incoming
audio with no MIDI, key tracking or filter envelope.

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

Version 0.3.0 keeps the plug-in ID, parameter IDs and saved-state format of
0.1.0 and 0.2.0, so projects restore their parameter values. Its sound and
reported latency have changed: keep a backup of important projects before
replacing an older version. This is a separate effect from Mono Chorus.

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
| Input | −24 to +24 dB before the filter and distortion. Not a Monomachine control. |
| Output | −24 to +24 dB after the wet/dry mix. Not a Monomachine control. |
| Mix | 0–100%; dry to filtered/distorted. Not a Monomachine control. |

All controls support CLAP automation and modulation. The two channels are
processed independently with the same settings.

The five Monomachine controls behave as its knobs do. A whole number is a knob
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
there: the filters, the clipper and the track EQ that sits in front of them on
the hardware are always in the path. The EQ is held at the Monomachine's
defaults, which are not quite flat (a lift of up to 0.8 dB centred on
800 Hz), and the top octave rolls off (−2.6 dB at 15 kHz, −9 dB at 20 kHz).
Use Mix 0% for the dry signal. At host rates other than 44.1 kHz even the dry
path is band-limited by sample-rate conversion.

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
THRU track with key tracking off, the filter envelope at rest and the EFX page
at its defaults. All figures are at 44.1 kHz.

| Check | Result |
| --- | --- |
| Filter-section output, 60 random cases: noise, sines and impulses from full scale to −40 dB, all five controls jumping every few blocks | 384,000 of 384,000 frames identical |
| The six coefficient tables generated by the plug-in against the firmware's | 9,070 of 9,070 entries identical |
| The built plug-in against the firmware driven by Monomodule's own host model, knobs moving at random times (so the knob slew is included) | 383,744 of 383,744 samples identical |
| The 77 impulse captures used to judge 0.1.0 and 0.2.0, at the filter section | identical in all 77 |
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
- The EQ, key tracking, filter envelope, SRR and delay of a Monomachine track
  are not controls here: the EQ is fixed at its defaults, the others are off.
- Modulation follows the timing and range of the hardware's LFOs but was not
  checked against them.
- At host rates other than 44.1 kHz the resampling around the core is this
  project's own and has no hardware counterpart.

`tests/test_core.c` pins the verified behaviour without needing the firmware:
it fails if the tables or the output for a fixed stimulus ever change. The
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
