# The volume curve is too steep, and the fix is one line

From the Diatom side, 2026-08-28, measured on hardware. No Diatom change is
needed; the defect and the fix are both in `src/platform.c`.

## Symptom, in Eric's words

> "When I was using minarch as the base for PlayOS, the audio was much louder at
> max, and it didn't drop off like it does now. So 60% was fairly loud before.
> Now it is barely audible."

## The measurement

`tools/micprobe.sh` in the Diatom repo, playing a 440 Hz tone at -1.4 dBFS and
capturing on the device's own microphone. Room baseline ~40 rms.

| `digital volume` | rms | vs room | modelled |
|---|---|---|---|
| 0 | 10034 | 201x | 0 dB |
| 16 | 1485 | 30x | -18.6 dB |
| 31 | 114 | 2.3x | -36 dB |
| 47 | 34 | 0.7x, silent | -55 dB |

Measured points track the codec's own 1.16 dB/step figure closely (predicted
10034 / 1174 / 158 / 19). Repeated with a second, independently written probe
and agreed to within 3% at both ends.

**The inversion your comments describe is correct.** 0 is loudest. I raised the
opposite theory during this investigation and it was wrong.

But the earlier *proof* of it was circular: it diffed PlayOS's own writes across
a volume-up press, which establishes what `apply_volume` does, not what the
codec does. The table above is the non-circular version, so the comment in
`platform.c` can now cite a real measurement.

## The defect

`apply_volume` spreads 21 positions linearly across the **entire 73 dB**
register:

    raw = GAIN_RAW_MAX - ((long)v * GAIN_RAW_MAX + VOL_MAX / 2) / VOL_MAX;

That is 3.65 dB per press, so:

| shelf | raw | attenuation |
|---|---|---|
| 100% | 0 | 0 dB |
| 80% | 13 | -15 dB |
| 60% | 25 | **-29 dB** |
| 50% | 31 | -36 dB |
| 25% | 47 | **-55 dB, measured as silent** |

Everything usable is in the top two or three positions and the bottom two
thirds of the scale is inaudible. This is exactly the report: max is fine, 60%
is barely audible, and it "drops off".

libmsettings evidently mapped its positions across a narrower range. Nothing
about the hardware changed in the migration; the mapping did.

## Suggested fix

Cap the usable attenuation instead of spending the whole register. About 40 dB
over 21 positions is ~2 dB per press, which feels like a normal control:

    #define GAIN_RAW_USABLE 34        /* ~40 dB; the rest of the register is
                                       * below the noise floor on this speaker */
    raw = ((long)(VOL_MAX - v) * GAIN_RAW_USABLE + VOL_MAX / 2) / VOL_MAX;

Position 0 keeps working because `HpSpeaker Switch` already carries the true
mute, which is why the bottom of the register is not needed for silence.

34 is a starting point from the table above, not a measured optimum. The honest
way to land it is to pick the raw value that is *just* audible in a quiet room
and use that as the floor. `tools/micprobe.sh --tone` in the Diatom repo gives
the number; `DIATOM_GAIN=<n>` selects the raw value.

## One caveat before trusting old numbers

`micprobe --tone` was broken until today: the tone was three seconds against a
`sleep 5` before the capture, so it recorded the room after the tone had ended.
Any `--tone` figure from before 2026-08-28 is room noise. The game path was
unaffected. Fixed in Diatom; the table above is from the fixed version and was
cross-checked by hand.

## Not the cause, ruled out with evidence

- **Mixer contamination from Diatom's test harness.** `brick-device-run.sh`
  writes four controls and restores none of them, which made it a good suspect.
  A clean reboot shows `Soft Volume Master` at 255 and `DAC volume` at 160 -
  both driver defaults, unchanged. Not it. The harness bug is real and worth
  fixing anyway, since its justifying comment claims PlayOS resets the mixer
  when PlayOS only resets one of the four.
- **Volume not persisting.** It persists correctly. `levels.cfg volume=5`
  survived a reboot and produced raw 47 exactly as `apply_volume` predicts.
- **Per-core loudness differences.** Real and measured - a 13 dB spread across a
  nine-run survey - but the spread *within* a system is as large as between
  systems, so it is per-game dynamic range and no per-system trim addresses it.
  Recorded in Diatom's register §6. A separate, smaller problem, and not what
  Eric noticed.
