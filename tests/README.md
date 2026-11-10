# Developer tests

On Linux, from the repository root, compile a native CLAP library and the
minimal offline host:

```sh
cc -std=c11 -O2 -fPIC -fvisibility=hidden -I vendor/clap/include \
  -shared source/mono_filter.c -lm -o tests/MonoFilterDist.clap
cc -std=c11 -O2 -I vendor/clap/include tests/clap_render.c \
  -ldl -lm -o tests/clap_render
python3 tests/test_audio.py
python3 tests/test_automation.py
```

The Python audio tests require NumPy. `clap_render` also accepts a stream of
headerless float64 frames with stereo input followed by eight parameters; see
its source for arguments and output format.

`clap_contract.c` exercises shared IDs, state and latency. To run it you need
native Linux builds of versions 0.1.0 and 0.2.0:

```sh
cc -std=c11 -O2 -I vendor/clap/include tests/clap_contract.c \
  -ldl -lm -o tests/clap_contract
tests/clap_contract /path/to/0.1.0.clap tests/MonoFilterDist.clap
```

The optional `verify_macos_bundle.py` checks both architectures and code-page
hashes of an ad-hoc signed macOS bundle. Firmware and emulator are
not needed for these checks. Reference audio captured from the firmware is not
distributed here; the README summarises the comparison methodology and limits.
