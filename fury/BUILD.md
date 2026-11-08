# fury-audio-rc2-sarfix

The js-dos emulators build that ninetysix.io (v3 site) serves to browsers with JSPI, as
`wdosbox-x-jspi.js` / `wdosbox-x-jspi.wasm`. Browsers without JSPI get
js-dos's stock 8.4.1 build.

Source: this commit. It is `47c228c` with the `native/dosbox-x` submodule
pointed at `maitchison/dosbox-x` (js-dos's own repo does not have our commits)
and pinned to `971acb44d8`, the DOSBox-X core the build used (tag `fury-audio-rc2-sarfix`
there). Nothing else in the tree differs.

Configure (emsdk; Ninja):

    emcmake cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSDOS_OPT=-O3 '-DJSDOS_EXTRA_CFLAGS=-DFURY_WASMJIT -DFURY_HOT_THRESHOLD=1' -DCMAKE_EXE_LINKER_FLAGS=-Wl,--threads=1

These are the flags its configure log recorded. Then `ninja -C build/release`.

When a site installs the build, its emulator JS files get one change:
`disk-activity.js` (here) is inserted, to count guest file reads for the
page's disk light. Both the JSPI build and the stock fallback get it.

Licence: GPL v2 (`LICENSE`), as js-dos and DOSBox-X.
