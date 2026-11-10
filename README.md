# Mono Filter Distortion 0.2.0

A stereo CLAP effect for Bitwig with Monomachine-inspired BASE, WIDTH, HP Q,
LP Q and DIST controls. Key tracking and the filter envelope are disabled:
control incoming audio without MIDI notes. Bitwig provides the plug-in's
parameter interface.

Version 0.2.0 runs the filter path at twice the internal audio rate, uses the
traced interpolation and decimation structure, recalibrates the filter curves,
and revises the high DIST gain and clipping threshold. It is still a **native
floating-point reconstruction**, not an exact translation of the original
filter algorithm. It contains no DSP emulator, synthesiser, or firmware.

## Build and install on macOS

The source consists of one C file and the included CLAP headers. You'll need
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

Version 0.2.0 retains 0.1.0's plug-in ID, parameter IDs and saved-state
format, so projects should restore their parameter values. Its sound and
reported latency have changed: keep a backup of important projects before
replacing 0.1.0. This is a separate effect from Mono Chorus.

A previous cross-built universal bundle was checked structurally and its
signature hashes were verified, but it has **not** been run on a Mac or in
Bitwig in this environment. `build_macos.sh` is provided for a native build;
the script itself has not been tested on a Mac here.

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
| DIST | −64 to +63; 0 is the neutral reference setting. Negative values reduce gain into the filter; positive values change the clipping threshold and then increase gain. |
| Input | −24 to +24 dB before filtering and distortion. |
| Output | −24 to +24 dB after the wet/dry mix. |
| Mix | 0–100%; dry to filtered/distorted. |

All controls support CLAP automation and modulation. Stereo channels are
processed independently, with matching settings. Changes are gently
smoothed; fast automation doesn't exactly reproduce the hardware's timing.

BASE 0 and WIDTH 127 are the most open *settings*, but they do not completely
bypass the filters. The reference also rolls off the high end there. Use Mix 0%
for the dry path. At host rates other than 44.1 kHz, even the dry path is
band-limited by sample-rate conversion and is not bit-transparent.

For a resonant, distorted band on a snare, try:

```text
BASE 32   WIDTH 52   HP Q 16   LP Q 48
DIST +20  Input 0 dB  Output -6 dB  Mix 100%
```

High resonance can ring for a long time and become loud. The distortion also
depends on incoming level: Input controls how strongly it is driven.

## Sample rate and latency

The plug-in's audio path runs at 44.1 kHz and the filter/clip stages run at
88.2 kHz inside it. At other host rates it converts to and from 44.1 kHz with
band-limited resampling. Latency is reported to the host: **8 samples at
44.1 kHz, 99 at 48 kHz, 195 at 96 kHz and 387 at 192 kHz**. The supported
activation range is 1–768 kHz; ordinary audio rates are recommended. Frequencies
above the internal 44.1 kHz path's Nyquist are not preserved.

## Accuracy and remaining differences

The model was calibrated and tested against recordings captured previously
from OS 1.32B running in Monomodule's THRU track, with key tracking disabled
and other effects neutral. It was not compared with a recording from physical
hardware or its analogue converters. The Monomodule track produces around
+2.3 dB more at 997 Hz than the plug-in's chosen unity-oriented level; that
gain difference was compensated in these measurements.

| Comparison at 44.1 kHz | 0.1.0 | 0.2.0 |
| --- | ---: | ---: |
| Mean magnitude error, all 77 saved low-level impulse cases | 1.31 dB | 0.87 dB |
| Mean magnitude error, 22 cases at Q values held out from curve fitting | 0.94 dB | 0.55 dB |
| Median normalised waveform error, all 77 cases | 0.61 | 0.16 |

The Q holdouts were 48 and 80 at cutoff positions also present in training;
they are not an exhaustive test of unmeasured settings. Magnitude errors use
log-spaced bins from 40 Hz to 19 kHz whose reference magnitude was above
−30.5 dB relative to the impulse. Waveform error is RMS error divided by the
reference RMS, after compensating the fixed gain difference and choosing the
best whole-sample alignment within ±20 samples. The reported waveform numbers
are **errors, not a percentage similarity or an accuracy score**.

For one −40 dBFS impulse with BASE 40, WIDTH 127 and zero Q, the reference
was −9.58 dB at 20 kHz; 0.1.0 measured −26.43 dB and 0.2.0 −9.41 dB.
For the strongest DIST setting, fitting at several input levels reduced a large
third-harmonic error at low level. At a separately checked 997 Hz, 0.05-peak
sine, the third-harmonic error was about +0.47 dB in 0.1.0 and +0.01 dB in
0.2.0. These examples do not establish accuracy across all sounds and settings.

**Not yet reproduced:** at some high HP Q settings the saved reference settles
into a low-level persistent oscillation after an impulse, while this model's
floating-point state decays. The exact internal saturation and 24/56-bit
arithmetic were not reconstructed. In particular, the available recordings do
not establish high-level saturation accuracy with resonant high-pass settings.
A straightforward attempt to quantise the model's states produced worse
results and unwanted residues, so it is not included. Parameter modulation,
stereo interaction and physical hardware remain areas for further comparison.
There is no supported claim of 97% or bit-perfect accuracy.

## Validation, attribution and licence

The Linux CLAP build passed 35 validator tests, with 9 skipped and no failures.
Separate checks covered 0.1.0 state compatibility, automation, independent
channels, extreme resonance, stable finite output and reported latency.
The Mac bundle was checked structurally and cryptographically but not loaded
on macOS or in Bitwig here.

- Monomodule reference: https://github.com/shnolk/monomodule, commit
  `23e541021b92d1a5978f36c0a552ac443376df1a` (AGPL-3.0). No Monomodule code
  or emulator is linked or included.
- MONO2K calibration: https://github.com/InigoHernandez/mono2k (GPL-3.0-only),
  by Sequencist; its published calibration informed the initial reconstruction.
- The plug-in source is GPL-3.0-only; see `LICENSE`. CLAP headers retain
  their own licence in `vendor/clap/LICENSE`.

This independent project is not affiliated with or endorsed by Elektron,
Bitwig, the Monomodule project, or MONO2K's author.
