# MUMBLE

A retro-game voice synth as an FM-1 firmware: the whole FM-1 becomes MUMBLE. Eight characters sing one line of text a
syllable at a time, from formant synthesis (no recordings): type a line, sing it on the keys (chords spread over the
choir), and record what is sung into a loop of pitches that sings whatever the line says now.

It is built on [fm1-base](https://github.com/charlesvestal/fm1-base) (MPL-2.0, pinned as the `base/` submodule),
which runs on JieLi's open AC79 SDK and its Linux toolchain, with a 581 KB app slot. `app/engine.c` builds the voice
engine (`app/mumble.c`) as an -O2 unit, and `app/app.c` includes the page (`app/mumble_screen.inc`, with the cast's
art) and gives it the panel's controls, its painting (`app/paint.inc`) and the frame loop.

| Path | What |
|---|---|
| `app/app.c` | The app's contract with fm1-base (`app_init`, `app_render`, `app_frame` ...) and the controls adapter |
| `app/mumble.c`, `app/mumble.h` | The voice engine: text to syllables, the glottal source, formants, consonants, the choir, the loop, effects and the master compressor |
| `app/mumble_screen.inc`, `app/mumble_sprites.inc` | The page: TYPE, SING and LOOP, the choir, recording, the knob pages; the cast's art |
| `app/paint.inc` | The damage-marking painter on fm1-base's `lcd_blit`, two band buffers in turn (adapted from FM1 Open, see `LICENSE.md`) |
| `app/engine.c`, `app/mumble_port.h` | The engine as an -O2 unit; its audio path runs from RAM (`PLAT_RAM_CODE`) |
| `app/fm1.toml` | Build settings: name, package identity `FM-1_68`, USB MIDI only |
| `app/tests/basic.txt` | Host simulator script: typing the shared line, a demo line, SING, the loop and LOOP mode's lanes; screenshots and a WAV |
| `app/tests/line.txt` | Host simulator script: SING mode on a typed line, keys sharing the leading key's syllable, moving on after it, round the line, FX; a recorded note singing a new line typed while the loop plays; each pass from the line's top |
| `app/tests/controls.txt` | Host simulator script: TYPE's cursor and letter knobs, EDIT a word back and held to clear, HOME's help card, EDIT with a knob putting a sound back, LOOP mode's knobs; screenshots of each |
| `app/tests/save.txt` | Host simulator script: a line, a sound, a scale, a recorded loop, a mute and a tempo saved on their own and back after a reboot |
| `app/tests/record.txt` | Host simulator script: a character as a mono synth, a chord shared out across the choir, recording per singer and playback, the metronome, EDIT clearing a singer, then the loop |
| `tests/engine_test.c`, `tests/engine.sh` | The engine's host test: text analysis, timing, chords, the loop, the compressor, consonants, the voice's spectrum |
| `tests/fuzz.c`, `tests/fuzz-seeds/` | libFuzzer harness and property checks over the app as the device builds it |
| `base/` | fm1-base, with felucca-bsp in `base/bsp/` |

## Controls

Hold **HOME** for a card of these on the screen; every button also says on the hint line what it just did.

Everything you make is kept over power-off: the line, each character's sound, choir, mute and level, the loop and
its size, the scale and the arpeggiator. It saves itself 3 s after the last change, once no key is held and nothing
is recording.

GLO steps through three modes: **TYPE** (the keys type the line), **SING** (they sing it) and **LOOP** (the multitrack
view: a lane of the loop for each character, the keys singing as in SING). In TYPE the keys' lights mark the vowels; in SING and
LOOP a key is lit while it is held.

| Control | TYPE | SING | LOOP |
|---|---|---|---|
| Keys | Type at the cursor | Sing the line a syllable at a time; held keys make a chord | As in SING |
| K1 / K2 | The cursor / the letter under it (at the end: a new one) | The page's four sounds (K1–K4), shown with a bar of where each sits and a tick at the character's own | The character muted in the loop (right: muted) / its level |
| K3 / K4 | — | | The loop's bars / BPM |
| EDIT | A word back; held 1 s: clear the line, then the loop | The selected character's notes out of the loop; held 1 s: clear the loop. Held with a knob turned: that sound back to the character's | As in SING; held with a knob turned: that setting back |
| OCT- / OCT+ | Delete / CAPS (shouted words) | Octave down / up | As in SING |
| FX, SCL, ENV, LFO | Type `,` `.` `?` `!` | FX: back to the top of the line. SCL: a scale the keys keep to (C major, minor, pentatonic, blues; SCL lit). LFO: a new voice rolled for the character (EDIT with a knob puts a setting back) | As in SING |
| ALGO, SEQ | — | The page the knobs play: VOICE, FEEL, FX, STYLE (tabs at the top) | — |
| GLO | To SING | To LOOP (recording carries on) | To TYPE |
| SELECT / SAVE | The character / in or out of the choir | | |
| PRESET | A demo line | | |
| ARP | The arpeggiator on or off (ARP lit) | The keys held sung one at a time, a 16th each (on the loop's while it runs), up the keys and round the choir, a syllable each | As in SING |
| PLAY / REC | The loop on or off / record into it (from TYPE: to SING first) | | |

## Build and test

The JieLi toolchain is Linux x86-64 only. On GitHub, `.github/workflows/mumble-fm1.yml` runs the host tests and builds
the package on every push to `main`; the run's **mumble-fm1** artifact has `mumble.fwsc`, its checksum, the
test screenshots and a recording.

Locally (Linux x86-64; on a Mac the build runs the toolchain in Docker, see fm1-base's README):

~~~sh
git submodule update --init --recursive
tests/engine.sh app/build/engine                                 # the engine's tests (any cc), a WAV per character
base/tools/setup_toolchain.sh --only jieli,ac79-sdk,uv,python   # pinned, checked downloads into base/.toolchain
base/.venv/bin/python base/tools/build.py app --host             # the host simulator: any cc, no toolchain
app/build/host/mumble_host app/tests/basic.txt app/build/host
app/build/host/mumble_host app/tests/record.txt app/build/host
app/build/host/mumble_host app/tests/line.txt app/build/host
app/build/host/mumble_host app/tests/controls.txt app/build/host
app/build/host/mumble_host app/tests/save.txt app/build/host
base/.venv/bin/python base/tools/build.py app                    # app/build/mumble.fwsc
~~~

### Fuzzing

`tests/fuzz.sh` builds `app/app.c`, `app/paint.inc` and the engine with clang, ASan and UBSan on a stand-in
platform and plays each fuzz input as a performance from a fresh boot (the app's state is put back between inputs, so
a crash file replays alone: `app/build/fuzz/mumble-fuzz CRASHFILE`). Besides memory errors and undefined behaviour it
fails when a blit leaves the screen or its band buffer, a band buffer changes while its DMA is still sending it, the
incremental redraw differs from a full repaint, the state left alone 3 s is not saved or a power-off loses it, an LED names a control the FM-1 lacks, HOME leaves the page, sound
outlasts a panic by 3 s, or the same input plays differently twice. After every frame it also checks the page's
state: each field in its range and agreeing with the others and the lights (S1), and the frame's change against a
model of the controls (S2: REC flips the loop, GLO steps the mode, SELECT and ALGO move by their detents, a knob moves
its parameter by the accelerated amount, typing adds exactly the keys' letters, only SAVE/EDIT+key touch a pad, and
with nothing pressed or turned no setting changes). Putting the state back needs the app's globals in
sections of their own, which ASan does not guard, so `tests/fuzz.sh --replay` also runs each seed and corpus input
alone in a build whose globals it does guard. CI runs the seeds, two minutes of fuzzing and that replay; a failure there is shrunk and printed as the panel
actions that make it.

~~~sh
tests/fuzz.sh -max_total_time=600 -jobs=8       # clang with libFuzzer; corpus and crashes in app/build/fuzz/
tests/fuzz.sh --replay                           # the seeds and the corpus, ASan on the globals too
tests/fuzz.sh --minimize app/build/fuzz/crashes/crash-…   # a failure shrunk (libFuzzer's minimizer), then explained
tests/fuzz.sh --explain FILE                     # an input as the panel actions it plays: "48 ms  press GLO" …
tests/fuzz.sh --merge                            # the corpus shrunk to the fewest inputs with the same coverage
~~~

## Install

`base/tools/fm1_install.py app/build/mumble.fwsc` (needs `mido` and `python-rtmidi`), or any FM-1 installer that takes
a `.fwsc` (M-Flasher, the Firmware Hub). Back up the FM-1 first (M-Flasher's backup). fm1-base's update loader accepts
both flash chips (`0x856014` and the newer UC25HQ80 `0xB36014`), and the firmware updates from itself, so another
firmware or M-VAVE's own goes back on the same way.

## Status

Played on an FM-1 (November 2026). One shared line, TYPE, SING and LOOP modes, the choir, the recorded loop (pitches,
timing and syllable slots, singing the line as it is), consonants, glides and the master compressor.

The last measured device build (with consonants and glides, before the voice's top; CI, November 2026): app image
112,024 B of the 581 KB slot; RAM `.data`+`.bss` 73,568 of 98,304 B; RAM code 15,400 of 32,768 B; pool 84,484 of
344,064 B. The package `mumble.fwsc` is 616,059 B. Not yet:

- **No clock.** The loop runs on its own BPM (MIDI clock is the plan).
- **HOME does nothing**, and Bluetooth MIDI is off.

## License

GPL-3.0-only with an additional permission; see `LICENSE.md`, which also notes the code adapted from FM1 Open.
