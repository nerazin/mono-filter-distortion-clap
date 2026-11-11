# Developer tests

From the repository root (macOS or Linux; add `-ldl` on Linux):

```sh
cc -std=c11 -O2 tests/test_core.c -lm -o tests/test_core && tests/test_core
```

`test_core` checks the DSP against values recorded from a build that matched
the firmware sample for sample. It needs nothing else.

The remaining tests drive the actual CLAP library through a minimal offline
host and need Python with NumPy:

```sh
cc -std=c11 -O2 -fPIC -fvisibility=hidden -I vendor/clap/include \
  -shared source/mono_filter.c -lm -o tests/MonoFilterDist.clap
cc -std=c11 -O2 -I vendor/clap/include tests/clap_render.c \
  -lm -o tests/clap_render
python3 tests/test_audio.py
python3 tests/test_automation.py
```

`clap_render` accepts a stream of headerless float64 frames with stereo input
followed by eight parameters; see its source for arguments and output format.

`clap_contract.c` checks that plug-in and parameter IDs, saved state, ports
and latency stay compatible. It loads an older build next to the current one:

```sh
git show b2d154d:source/mono_filter.c > /tmp/mono_filter_020.c
cc -std=c11 -O2 -fPIC -fvisibility=hidden -I vendor/clap/include \
  -shared /tmp/mono_filter_020.c -lm -o /tmp/v020.clap
cc -std=c11 -O2 -I vendor/clap/include tests/clap_contract.c \
  -lm -o tests/clap_contract
tests/clap_contract /tmp/v020.clap tests/MonoFilterDist.clap
```

The optional `verify_macos_bundle.py` checks both architectures and code-page
hashes of an ad-hoc signed macOS bundle. Neither firmware nor an emulator is
needed for any of these. The comparison against the firmware is described in
the README; it needs the OS file and is not distributed here.
