# MUMBLE firmware

MUMBLE as its own FM-1 firmware, instead of a page inside FM1 Open: the whole FM-1 becomes the voice synth. It is built
on [fm1-base](https://github.com/charlesvestal/fm1-base) (MPL-2.0, pinned as the `base/` submodule), which runs on
JieLi's open AC79 SDK and its Linux toolchain rather than M-VAVE's binary SDK, so MUMBLE gets fm1-base's 581 KB app
slot instead of what is left of FM1 Open's.

The engine and the page are not copies: `app/engine.c` builds the repository's `app/mumble.c`, and `app/app.c`
includes `app/mumble_screen.inc` (with the cast's art) and gives it what FM1 Open's `screen.c` gives it in the
preview: the panel in FM1 Open's order, its painting (`app/paint.inc`, the same 5x7 font and colours) and the frame
loop. A change to MUMBLE shows in the browser preview and in the firmware alike.

| Path | What |
|---|---|
| `app/app.c` | The app's contract with fm1-base (`app_init`, `app_render`, `app_frame` ...) and the controls adapter |
| `app/paint.inc` | FM1 Open's damage-marking painter on fm1-base's `lcd_blit`, two band buffers in turn |
| `app/engine.c`, `app/mumble_port.h` | The engine as an -O2 unit; its audio path runs from RAM (`PLAT_RAM_CODE`) |
| `app/fm1.toml` | Build settings: name, package identity `FM-1_68`, USB MIDI only |
| `app/tests/basic.txt` | Host simulator script: typing, a demo line, PLAY, SAMPLE, the LOOP page; screenshots and a WAV |
| `tests/fuzz.c`, `tests/fuzz-seeds/` | libFuzzer harness and property checks over the app as the device builds it |
| `base/` | fm1-base, with felucca-bsp in `base/bsp/` |

## Build and test

The JieLi toolchain is Linux x86-64 only. On GitHub, `.github/workflows/mumble-fm1.yml` runs the host tests and builds
the package on every push that touches MUMBLE; the run's **mumble-fm1** artifact has `mumble.fwsc`, its checksum, the
test screenshots and a recording.

Locally (Linux x86-64; on a Mac the build runs the toolchain in Docker, see fm1-base's README):

~~~sh
git submodule update --init --recursive
cd firmware/mumble-fm1
base/tools/setup_toolchain.sh --only jieli,ac79-sdk,uv,python   # pinned, checked downloads into base/.toolchain
base/.venv/bin/python base/tools/build.py app --host             # the host simulator: any cc, no toolchain
app/build/host/mumble_host app/tests/basic.txt app/build/host
base/.venv/bin/python base/tools/build.py app                    # app/build/mumble.fwsc
~~~

### Fuzzing

`tests/fuzz.sh` builds `app/app.c`, `app/paint.inc` and the engine with clang, ASan and UBSan on a stand-in
platform and plays each fuzz input as a performance from a fresh boot (the app's state is put back between inputs, so
a crash file replays alone: `app/build/fuzz/mumble-fuzz CRASHFILE`). Besides memory errors and undefined behaviour it
fails when a blit leaves the screen or its band buffer, a band buffer changes while its DMA is still sending it, the
incremental redraw differs from a full repaint, an LED names a control the FM-1 lacks, HOME leaves the page, sound
outlasts a panic by 3 s, or the same input plays differently twice. After every frame it also checks the page's
state: each field in its range and agreeing with the others and the lights (S1), and the frame's change against a
model of the controls (S2: REC flips the loop, GLO steps the mode, SELECT and ALGO move by their detents, a knob moves
its parameter by the accelerated amount, typing adds exactly the keys' letters, only SAVE/EDIT+key touch a pad, and
with nothing pressed or turned no setting changes). Putting the state back needs the app's globals in
sections of their own, which ASan does not guard, so `tests/fuzz.sh --replay` also runs each seed and corpus input
alone in a build whose globals it does guard. CI runs the seeds, two minutes of fuzzing and that replay.

~~~sh
tests/fuzz.sh -max_total_time=600 -jobs=8       # clang with libFuzzer; corpus and crashes in app/build/fuzz/
tests/fuzz.sh --replay                           # the seeds and the corpus, ASan on the globals too
~~~

## Install

`base/tools/fm1_install.py app/build/mumble.fwsc` (needs `mido` and `python-rtmidi`), or any FM-1 installer that takes
a `.fwsc` (M-Flasher, the Firmware Hub). Back up the FM-1 first (M-Flasher's backup). fm1-base's update loader accepts
both flash chips (`0x856014` and the newer UC25HQ80 `0xB36014`), and the firmware updates from itself, so another
firmware or M-VAVE's own goes back on the same way.

## Status

Milestone 1: everything the preview's page does runs (TYPE, PLAY, SAMPLE, the LOOP page, the eight characters and
their FX) in the host simulator, and builds for the FM-1.

The first device build (CI, November 2026): app image 103,628 B of the 581 KB slot; RAM `.data`+`.bss` 65,416 of
98,304 B; RAM code 12,016 of 32,768 B (the engine's audio path 6,916 B of it); pool 84,484 of 344,064 B. The package
`mumble.fwsc` is 616,059 B. Not yet:

- **Not tried on an FM-1.** RAM use, the audio path's time in the interrupt and the output level are to be checked
  on the device.
- **Nothing is saved.** Lines, sounds and pads are lost at power-off (a store object is reserved for them).
- **No clock.** The loop runs on its own BPM; FM1 Open's sequencer sync is not there (MIDI clock is the plan).
- **HOME does nothing**, and Bluetooth MIDI is off.
