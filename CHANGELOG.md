# Changelog

Each release's section becomes its notes on GitHub (`.github/workflows/release.yml`). Versions are `X.Y`, `X.Y.Z`
for a fix, `X.Y-beta1` for a prerelease.

## 0.1 (unreleased)

The first release.

- **Eight characters** (HERO, GUARD, SISTER, KID, PIRATE, ELDER, BOT, GHOST), each with a voice of 16 settings
  (pitch, tube, mouth, air, speed, wobble, jitter, tune, crush, radio, echo, room, glide, vibrato, diction, effort)
  and a pixel face built from it. LFO rolls a new voice (held: for everyone), ENV puts their own back.
- **SING** (the mode at boot): the keys sing the line a syllable at a time; held keys make a chord across the choir.
  Auto-chord (SEQ: triad, 7th, power, sus2, sus4, add9, with inversion and spread), an arpeggiator (ARP: rate,
  pattern, octaves, gate, swing) and a scale lock (SCL, with its root).
- **LOOP**: a multitrack loop, a lane for each singer, recorded pass over pass (REC; stopped, it waits for the first
  key with the metronome counting). Its notes keep their pitch, time and syllable slot, so a new line changes what
  the loop sings. Level, swing, bars and BPM; it follows a MIDI clock from USB or TRS.
- **TYPE**: type the line on the keys and hear it spoken as you type. The first boot's line: "Our princess is in
  another castle!"
- The band spread across the stereo field by who sings; a shared echo and room that duck under the voices; a master
  compressor and limiter.
- Everything saved on its own when the FM-1 is quiet, and back after power-off. GLO's system screen shows the version,
  the audio load, dropouts, the battery and the clock, and resets everything (EDIT, then held 3 s).
