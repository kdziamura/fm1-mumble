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
| `app/mumble_screen.inc` | The page: TYPE, SING and LOOP, the choir, recording, the knob pages |
| `app/mumble_faces.inc`, `app/mumble_sprites.inc` | The cast's faces, built from their voices (portrait, icon, the rig that talks and blinks), and their palette |
| `app/paint.inc` | The damage-marking painter on fm1-base's `lcd_blit`, two band buffers in turn (adapted from FM1 Open, see `LICENSE.md`) |
| `app/engine.c`, `app/mumble_port.h` | The engine as an -O2 unit; its audio path runs from RAM (`PLAT_RAM_CODE`) |
| `app/fm1.toml` | Build settings: name, package identity `FM-1_68`, USB MIDI only |
| `app/tests/basic.txt` | Host simulator script: the first line, typing the shared line, SING, the loop and LOOP mode's lanes; screenshots and a WAV |
| `app/tests/line.txt` | Host simulator script: SING mode on a typed line, keys sharing the leading key's syllable, moving on after it, round the line, FX; a recorded note singing a new line typed while the loop plays; each pass from the line's top |
| `app/tests/controls.txt` | Host simulator script: TYPE's cursor and letter knobs, EDIT a word back and held to clear, HOME's help card, EDIT with a knob putting a sound back, LOOP mode's knobs; screenshots of each |
| `app/tests/save.txt` | Host simulator script: a line, a sound, a scale, a recorded loop, a mute and a tempo saved on their own and back after a reboot |
| `app/tests/clock.txt` | Host simulator script: MIDI clock: Start, the loop on the clock's tempo as it changes, Stop, the clock gone |
| `app/tests/record.txt` | Host simulator script: a character as a mono synth, a chord shared out across the choir, recording per singer and playback, the metronome, EDIT clearing a singer, then the loop |
| `tests/engine_test.c`, `tests/engine.sh` | The engine's host test: text analysis, timing, chords, the loop, the compressor, consonants, the voice's spectrum |
| `tools/faces.py`, `tests/faces.sh` | The face generator in Python, for contact sheets (the cast by hand and from their voices, each setting swept, rolled voices, the rig); the check that the firmware's draws the same pixels |
| `tests/fuzz.c`, `tests/fuzz-seeds/` | libFuzzer harness and property checks over the app as the device builds it |
| `base/` | fm1-base, with felucca-bsp in `base/bsp/` |

## Controls

Hold **HOME** for a card of these on the screen; every button also says on the hint line what it just did.

Each character's face is built from their voice, and changes with it: the head's proportions follow its size,
pitch and effort, and what stands out in the voice against the rest of the cast picks the parts (a hood for ROOM, an
antenna for RADIO, a beanie for a high voice, a knight's helmet for a drilled one, a beard for WOBBLE, a robot for
CRUSH, a ghost for AIR ...); `tools/faces.py` draws the same faces into contact sheets. LFO's rolled voice gets a new
face.

Everything you make is kept over power-off: the line, each character's sound, choir and level, the loop and its
size, the scale and its root, the arpeggiator and auto-chord. It saves itself 3 s after the last change, once the FM-1 has been quiet for
a second (the loop stopped, nothing sounding or held): writing the flash holds the whole FM-1 for a moment, so it
never happens during a performance. Change something while the loop plays and it is saved when you stop.

The band is spread across the stereo field: those who sing (the choir, and anyone with unmuted notes in the loop), in
the cast's order, each in the middle of their share of it. One alone is in the middle, two at a quarter and three
quarters, three at a sixth, a half and five sixths, and so on, gliding there as the band changes. The shared echo and
room duck under the voices, so they fill the gaps rather than blur the singing; a rolled voice adds only a little of
either, and hardly any WOBBLE. SPEED sets how fast a character talks in TYPE and how fast their WOBBLE goes (2 Hz at
100 %). Consonants are bright and crisp before a vowel, a percussive attack, and softer as an
ending; in a chord only the leading singer sings them, the others the vowel alone.

The loop follows a MIDI clock from USB or the TRS jack: Start starts it from the top on the clock, its tempo sizes it
(LOOP shows SYNC), Stop stops it; without a clock it keeps its own BPM.

The first boot (and a reset) gives the line "Our princess is in another castle!", yours to change or clear.

ALGO turns between three modes: **TYPE** (the keys type the line), **SING** (they sing it) and **LOOP** (the multitrack
view: a lane of the loop for each character, the keys singing as in SING). In TYPE the keys' lights mark the vowels; in SING and
LOOP a key is lit while it is held.

| Control | TYPE | SING | LOOP |
|---|---|---|---|
| Keys | Type at the cursor | Sing the line a syllable at a time; held keys make a chord | As in SING |
| SELECT | The character | | |
| ALGO | The mode: TYPE, SING, LOOP | | |
| PRESET | — | The sound page the knobs play: VOICE, FEEL, FX, STYLE (tabs at the top) | The tab the knobs set: MIX, ARP, CHORD, SCALE (tabs at the top) |
| K1–K4 | K1 the cursor, K2 the letter under it (at the end: a new one) | The page's four sounds, shown with a bar of where each sits and a tick at the character's own | MIX: the character's level (0: muted), swing, bars, BPM. ARP: rate (1/4 .. 1/32, triplets), pattern (up, down, up-down, random, as played), octaves (1-3), gate (10-100 %). CHORD: the shape (triad, 7th, power, sus2, sus4, add9), inversion, spread (close, open, wide). SCALE: the root (C .. B), the scale |
| EDIT | A word back; held 1 s: clear the line, then the loop | The selected character's notes out of the loop; held 1 s: clear the loop. Held with a knob turned: that setting back | As in SING |
| OCT- / OCT+ | Delete / CAPS (shouted words) | Octave down / up | As in SING |
| FX | The cursor to the line's start | Back to the top of the line | As in SING |
| SCL | The next scale (major, minor, pentatonic, blues, dorian, mixolydian, harmonic minor; SCL lit) the keys keep to | | |
| LFO | A random syllable at the cursor (however long it is held) | A new voice rolled for the selected character; held 1 s: for everyone | As in SING |
| ENV | The selected character's own voice back; held 1 s: everyone's | | |
| ARP | The arpeggiator on or off (ARP lit; LOOP's ARP tab sets it) | The keys held sung one at a time in the pattern (on the loop's time while it runs), round the choir, a syllable each | As in SING |
| SEQ | Auto-chord on or off (SEQ lit; LOOP's CHORD tab sets it) | A key sings a chord built up the scale (no scale: major on the root), a note a singer: as many of its notes, from the root up, as the choir has singers (one alone: the root) | As in SING |
| GLO | The system screen, open or closed: the audio's load (now and the most since boot), dropouts, the battery, how long the FM-1 has been on, the MIDI clock. There EDIT tapped, then held 3 s, resets everything to the first boot | | |
| SAVE | The character in or out of the choir | | |
| PLAY / REC | The loop on or off / record into it (from TYPE: to SING first). With the loop stopped REC waits, blinking, with the metronome counting the tempo, and the first key starts the loop and the recording | | |

## Build and test

The JieLi toolchain is Linux x86-64 only. On GitHub, `.github/workflows/mumble-fm1.yml` runs the host tests and builds
the package on every push to `main`; the run's **mumble-fm1** artifact has `mumble.fwsc`, its checksum, the
test screenshots and a recording.

Locally (Linux x86-64; on a Mac the build runs the toolchain in Docker, see fm1-base's README):

~~~sh
git submodule update --init --recursive
tests/engine.sh app/build/engine                                 # the engine's tests (any cc), a WAV per character
tests/faces.sh                                                   # the firmware's faces against tools/faces.py's
base/tools/setup_toolchain.sh --only jieli,ac79-sdk,uv,python   # pinned, checked downloads into base/.toolchain
base/.venv/bin/python base/tools/build.py app --host             # the host simulator: any cc, no toolchain
app/build/host/mumble_host app/tests/basic.txt app/build/host
app/build/host/mumble_host app/tests/record.txt app/build/host
app/build/host/mumble_host app/tests/line.txt app/build/host
app/build/host/mumble_host app/tests/controls.txt app/build/host
app/build/host/mumble_host app/tests/save.txt app/build/host
app/build/host/mumble_host app/tests/clock.txt app/build/host
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

Played on an FM-1 (November 2026): one shared line, TYPE and SING, the choir, the recorded loop (pitches, timing and
syllable slots, singing the line as it is), consonants, glides and the master compressor.

Tested in the simulator and the fuzzer, not yet on an FM-1: the faces built from the voices, LOOP mode, the friendlier controls, the scale lock, the
arpeggiator, quantized recording, the typewriter, saving, and the MIDI clock (whose timing on the device rests on
`midi_stamp`).

The last measured device build (with consonants and glides, before the voice's top; CI, November 2026): app image
112,024 B of the 581 KB slot; RAM `.data`+`.bss` 73,568 of 98,304 B; RAM code 15,400 of 32,768 B; pool 84,484 of
344,064 B. The package `mumble.fwsc` is 616,059 B. Not yet:

- **Bluetooth MIDI is off**, and no MIDI notes come in (only the clock).

## License

GPL-3.0-only with an additional permission; see `LICENSE.md`, which also notes the code adapted from FM1 Open.
