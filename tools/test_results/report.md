# HackRF over-the-air TX test

| Mode | Status | Failed checks (measured vs limit) |
|---|---|---|
| TUNE | pass | — |
| USB | pass | — |
| LSB | pass | — |
| DIGU | pass | — |
| DIGL | pass | — |
| RTTY | pass | — |
| CW | pass | — |
| CWR | pass | — |
| AM | pass | — |
| SAM | pass | — |
| DSB | pass | — |
| FM | pass | — |
| FMN | pass | — |
| DFM | pass | — |

## Notes

- **TUNE:** carrier -1312.3 Hz from 915.500000 MHz (both radios' crystal error)
- **CW:** decoded 'TEST'
- **CWR:** decoded 'TEST'
- **FM:** Bessel fit: beta 2.515, residual 7.37e-06
- **FMN:** Bessel fit: beta 1.250, residual 1.26e-06
- **DFM:** Bessel fit: beta 1.260, residual 1.57e-06

## Run

- Frequency (MHz): 915.5
- Combined crystal error (ppm): -1.433
- Drift over the run (Hz): 74.2
- Drive (%): TUNE 10, USB 10, LSB 10, DIGU 10, DIGL 10, RTTY 10, CW 10, CWR 10, AM 10, SAM 10, DSB 10, FM 10, FMN 10, DFM 10
- RTL-SDR gain (dB): TUNE 20.7, USB 20.7, LSB 20.7, DIGU 20.7, DIGL 20.7, RTTY 20.7, CW 20.7, CWR 20.7, AM 20.7, SAM 20.7, DSB 20.7, FM 20.7, FMN 20.7, DFM 20.7
- Callsign: W5TSU
- AetherSDR commit: v26.10.5-17-gae7dd902
- freq_tol_ppm: 25.0
- max_power: 80
- power_ramp: 10 %: 71.5 dB

Exit code: 0
