# Timegrapher

A terminal timegrapher for mechanical watches. It listens to the ticking through a microphone (or a contact pickup) and shows what a hardware timegrapher shows: rate, amplitude, beat error, and the paper-style trace.

## What it measures

| Reading | Meaning |
|---|---|
| **Rate (20 s)** | Seconds gained (+) or lost (−) per day, from a robust linear fit over the last 20 s of beats. |
| **Rate (avg)** | Same, over all retained beats (appears after about 40 s). |
| **Amplitude** | Balance-wheel swing in degrees, from the time between the unlocking sound and the drop sound of each beat. |
| **Beat error** | Half the difference between the tick→tock and tock→tick intervals, in ms. 0 means the escapement is perfectly in beat. |
| **Jitter** | Residual spread of beat times around the fitted line, in ms. A health indicator for the signal. |
| **SNR / det** | Signal-to-noise ratio, and the percentage of expected beats actually detected in the last 5 s. |

The display also has:

- **Trace.** Time on the x axis, deviation from the ideal beat grid on y (±3 ms, wrapped, 30 s window). Ticks are yellow and tocks are cyan. A flat line means a regulated watch, slope means rate error, and vertical separation between the two lines means beat error.
- **Last beat.** The filtered waveform of the latest beat, with the detected sounds marked in red.
- **History graphs.** Rate, amplitude and beat error, one point per second.

## Files

| File | Purpose |
|---|---|
| `main.cpp` | PortAudio capture, analysis thread, FTXUI screen. |
| `tg_core.hpp` | The analysis engine. Header-only, no dependencies, no audio or UI code. |
| `test_synth.cpp` | Offline test: generates synthetic watch audio with known values and checks the engine recovers them. |
| `CMakeLists.txt` | Build file. |

## Requirements

- C++17 compiler and CMake 3.16 or newer
- PortAudio (macOS: `brew install portaudio`, Debian/Ubuntu: `sudo apt install libportaudio-dev`)
- FTXUI, which CMake downloads automatically (needs internet on the first configure)

FFTW is not needed. All processing is in the time domain.

## Build

```
cmake -B build
cmake --build build
```

This produces `build/timegrapher` and `build/test_synth`.

## Run

```
./build/timegrapher [options]
```

| Option | Default | Description |
|---|---|---|
| `-b <bph>` | auto | Fix the beat rate (e.g. `28800`). Without it, the rate is detected from the first dozen beats and locked to the nearest standard value (14400, 18000, 19800, 21600, 25200, 28800, 36000, 43200). |
| `-l <deg>` | 52 | Lift angle of the movement. **Amplitude is only correct if this matches your movement.** |
| `-c <s/day>` | 0 | Calibration offset subtracted from the rate. See below. |
| `-f <Hz>` | 2000 | High-pass cutoff. Raise it if low-frequency handling noise gets through. |
| `-d <index>` | system default | Input device index. |
| `-L` | | List input devices and exit. |

Keys:

| Key | Action |
|---|---|
| `q` / `Esc` | Quit |
| `r` | Reset the measurement |
| `+` / `-` | Raise / lower the detection threshold |

## Getting a good measurement

1. Place the watch directly on or against the microphone, in a quiet room. A contact pickup works better than an air microphone.
2. Watch the level bar and the **det** figure. Adjust the threshold with `+`/`-` until det stays near 100%. Too low and noise gets counted as beats; too high and quiet beats are missed.
3. Give it about 20 s for the rate and about 40 s for the average to settle.
4. If amplitude shows `--`, the program is only finding one sound per beat. Improve the contact or lower the threshold so both the unlocking and drop sounds are caught.
5. Measure in the position you care about. Rate, amplitude and beat error all change with orientation, as on any timegrapher.

## Calibrating the sound card

Timing comes from the audio device's sample clock, which has its own small error (often a few s/day). To remove it, measure a watch you know is accurate, or a signal of known timing, and note the rate it shows. Then pass that value with `-c`. For example, if a watch known to be perfect reads +4 s/day, run with `-c 4`.

## How it works

1. Audio is captured at the device's native sample rate and passed through a lock-free ring buffer to an analysis thread, so the audio callback never blocks.
2. A 4th-order high-pass filter removes handling noise and hum. The signal envelope is smoothed over 0.25 ms.
3. An adaptive threshold (a multiple of the learned noise floor) finds peaks. Peak times are refined by parabolic interpolation, giving sub-sample timing.
4. Sounds less than 20 ms apart are grouped into one beat (unlock, impulse, drop). The first sound gives the beat time and the first-to-last spacing gives the lift time.
5. Beats are numbered, with missed beats filled in, and a linear fit with outlier rejection gives the rate. All timing uses the sample counter, never the wall clock, so program scheduling does not affect the result.
6. Amplitude is computed as `lift_angle / (2 · sin(π · t_lift / T_osc))`, where `T_osc` is two beat periods. Values outside 100° to 360° are discarded.
7. If no beat arrives for 10 s, the measurement restarts so a new watch can be measured.

## Testing the engine

`test_synth` needs no audio hardware:

```
./build/test_synth [bph] [rate s/d] [beat error ms] [amplitude deg] [noise sigma] [seconds]
./build/test_synth 28800 12 0.35 270
```

It prints the true values next to what the engine recovered. In testing at 18000, 21600, 28800 and 36000 bph, the rate matched within 0.03 s/day, and beat error and amplitude matched the inputs.

## Limitations

- Verified on synthetic audio and compiled against the real libraries. Behaviour with real watches and microphones depends on your signal quality and may need threshold and `-f` tuning.
- Amplitude depends on the lift angle you give. A wrong value gives a proportionally wrong amplitude.
- Quartz watches, very quiet movements, and noisy rooms may not be detected reliably.
- Beat rates outside the standard list need `-b` to be set explicitly.

## Troubleshooting

- **Linker error "`_main` not found"**: the `main.cpp` being compiled has no `main`. Check you saved the full file, then rebuild from scratch with `rm -rf build`.
- **"No input device" / silence**: check microphone permission for your terminal (macOS: System Settings → Privacy & Security → Microphone), or pick a device with `-L` and `-d`.
- **Stuck on "searching…"**: raise the signal (closer contact) or lower the threshold with `-`.
- **"AUDIO OVERRUN" shown**: the analysis thread fell behind. Close other heavy programs.
