# Using PureSignal on the Hermes-Lite 2

PureSignal measures what your transmitter actually puts out, compares it with what
it was asked to send, and predistorts the signal to cancel the difference. Turn it
on with the **PS** button beside TUNE and MOX. It keys nothing by itself: it learns
from what you transmit.

The **PureSignal window** shows what it's doing, and can send a test signal for it to
learn from or for you to measure.

## Opening the window

Connect to a Hermes-Lite 2, then either:

- choose **Help → PureSignal...**, or
- right-click the **PS** button and choose **PureSignal window...**

The menu entry is greyed out on radios without PureSignal.

## What it shows

| Readout | Meaning |
|---|---|
| **State** | `off`, `idle`, `calibrating`, `correcting`, `feedback too weak` or `feedback too strong` |
| **Feedback level** | How strong the feedback is. 129 to 181 is good, 91 to 200 is usable, and anything else is out of range |
| **Receive gain** | The gain PS has set to keep the feedback in range (`-12` to `+48` dB) |
| **Calibrations** | How many succeeded, of how many attempted |
| **Feedback / reference** | Peak levels of what came back and what was sent, in dBFS |
| **Last result** | WDSP's result codes from the last calibration. All zeros means it passed |

The three plots update after each successful calibration. A plot stays empty until
the first one.

- **Amplitude:** dots show output against drive. The dashed line is what a perfectly
  linear transmitter would give. The further the dots bow away from it, the more the
  transmit chain is compressing.
- **Phase:** dots show how far the phase turns as the level rises. The orange line is
  the phase correction being applied, and should follow the dots.
- **Correction:** the gain being applied at each level.

### Comparing two curves

**Hold curve** keeps the current curve on the plots in grey, and later calibrations
draw over it. **Clear held** removes it.

This is how to see what PS is looking at when you run an amplifier:

1. Put the amplifier in Standby, calibrate, and press **Hold curve**.
2. Put it in Operate and calibrate again.
3. Compare. If the curve doesn't move and the feedback level stays the same, PS is
   probably not seeing the amplifier and isn't correcting it.

A curve that moves shows the amplifier is probably in the loop, but doesn't prove
PS is cleaning it up. Only a measurement outside the radio can do that, such as a
station monitor reading the third-order products with PS off and on.

## Test signals

**These go out on your antenna, or whatever the radio is connected to.** Check your
frequency and antenna first.

Set the controls along the bottom, then press **Transmit**:

| Mode | What it sends | When it stops |
|---|---|---|
| **Calibrate** | Two-tone | PS is correcting and has had 3 more good calibrations, or the time limit. Needs PS on |
| **A/B compare** | Two-tone, with the correction off and on every 3 s | The time limit. Needs PS on. Good for watching the waterfall or a station monitor |
| **Two-tone** | Two-tone at 700 and 1903 Hz | The time limit |
| **Single tone** | A carrier | The time limit. PS can't calibrate on this, because a carrier shows it only one point on the curve |

- **Power** is 1 to 80%. It replaces the TUNE power for the run, and your TUNE power
  comes back afterwards.
- **Time limit** is 5 to 30 s.
- **CW ID** sends your callsign at 12 WPM when the signal ends. A run won't start with
  this ticked and no callsign set.

Before keying, the window listens for 1.5 s and refuses if the frequency reads above
-80 dBm, or if it has no reading at all.

**To stop a run,** press **Stop**, close the window, or disconnect. Any of these unkeys
straight away and skips the CW ID, so ID separately if you need to. The run also ends,
without an ID, if something else unkeys the radio, such as the TUNE button.

The A/B compare switches abruptly, so you may see a small step at each change.

## What PS can't do yet

- **Through an amplifier, it hasn't been shown to work.** On 2026-11-01 the feedback
  came from coupling inside the HL2, not from the sampler on the I/O board's PURE
  input, so PS corrected the HL2's own stage and couldn't see the amplifier. Use the
  comparison above to check your station.
- **CW and a single-carrier TUNE stay uncorrected.**
- **While PS is on,** two of the HL2's four receivers carry feedback, so at most two
  slices are available, and PS can't run on the widest span.

## From the automation bridge

`ps status`, `ps on` and `ps off` read and set PS. `ps curves` returns the data behind
the plots. See [automation-bridge.md](automation-bridge.md).
