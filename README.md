# STM32 Offline Shazam — F407G-DISC1

A bare-metal, fully-offline Shazam-style audio fingerprinter running on an
STM32F407G-DISC1 discovery board. No microphone, no codec, no real-time
audio path. A mock PCM clip is generated at boot, fingerprinted using an
FFT pipeline on the Cortex-M4 FPU, and matched against a mock "song
database" stored in Flash. The matched song ID is blinked out on the
on-board LEDs and also printed over SWO/ITM.

If you ever want to recreate the project from zero, this README is the
recipe. Every CubeMX click and every CubeIDE setting you need is listed.

---

## 1. Hardware

- **Board**: STM32F407G-DISC1 (STM32F407VGTx, Cortex-M4F, 168 MHz max,
  192 KB SRAM, 1 MB Flash, hardware single-precision FPU).
- **LEDs** used as output:
  - `PD12` — green   (LD4)
  - `PD13` — orange  (LD3)
  - `PD14` — red     (LD5)   ← result indicator
  - `PD15` — blue    (LD6)
- **Debugger**: on-board ST-LINK/V2-A (USB-mini). Also carries SWO for
  ITM `printf`.
- **Clock in this project**: **HSI 16 MHz, PLL OFF**. We intentionally
  stay at 16 MHz because it is the simplest clock to get right; the
  full pipeline still finishes in a few seconds.

---

## 2. Toolchain

- STM32CubeIDE 2.1.x (CubeMX integrated).
- STM32Cube FW package for F4 (V1.28.3) — installed via CubeMX
  *Manage embedded software packages*. Path on this machine:
  ```
  C:\Users\jishu\STM32Cube\Repository\STM32Cube_FW_F4_V1.28.3
  ```
- **CMSIS-DSP**: the FW package only ships source files; no prebuilt
  `.a` library is installed. We work around this in two places in this
  project (see §6).

---

## 3. Project layout

```
hardware_project/
├── README.md                       <- this file
├── Wang03-shazam.pdf               <- original Shazam paper (reference)
└── stm32_blinky/stm32_blinky/      <- the actual CubeIDE project
    ├── stm32_blinky.ioc            <- CubeMX config (source of truth)
    ├── .project / .cproject        <- Eclipse/CubeIDE project files
    ├── STM32F407VGTX_FLASH.ld
    ├── Core/
    │   ├── Inc/main.h              <- LED + USER_BTN pin macros
    │   ├── Inc/shazam.h            <- public API + data structs
    │   ├── Src/main.c              <- button wait + result LED loop
    │   ├── Src/shazam.c            <- STFT + hashing + matching
    │   ├── Src/shazam_data.c       <- mock audio + mock DB
    │   ├── Src/syscalls.c          <- _write redirected to ITM
    │   ├── Src/arm_*.c             <- 13 CMSIS-DSP source files
    │   └── Lib/                    <- libarm_cortexM4lf_math.a (optional)
    └── Drivers/                    <- CubeMX-generated HAL + CMSIS

---

## 4. CubeMX configuration (reproducing the `.ioc`)

Open CubeMX (or the `.ioc` inside CubeIDE) and do the following.

### 4.1 New project

- *Board Selector* → **STM32F407G-DISC1** → *Start Project*.
- When asked "initialize peripherals with their default mode?" choose
  **No** — we want a minimal config, not the full board BSP.

### 4.2 RCC / clock

- *System Core → RCC*:
  - *High Speed Clock (HSE)* = **Disable**
  - *Low Speed Clock (LSE)* = Disable
- *Clock Configuration* tab:
  - *HSI* source selected (16 MHz).
  - *PLL* = OFF.
  - SYSCLK Mux = HSI.
  - HCLK = 16 MHz. APB1/APB2 dividers = /1.
  - Flash latency = 0 WS (auto).

### 4.3 SYS

- *System Core → SYS*:
  - Debug = **Serial Wire** (keeps SWD + SWO pins wired so you can flash
    and see ITM output).
  - Timebase source = **SysTick**.

### 4.4 GPIO — the four LEDs

In the *Pinout view* click each pin and set it to **GPIO_Output**, then
add a *User Label*:

| Pin  | Mode        | User Label   |
|------|-------------|--------------|
| PD12 | GPIO_Output | `LED_GREEN`  |
| PD13 | GPIO_Output | `LED_ORANGE` |
| PD14 | GPIO_Output | `LED_RED`    |
| PD15 | GPIO_Output | `LED_BLUE`   |
| PA0  | GPIO_Input  | `USER_BTN`   |

In *System Core → GPIO → PD12..PD15* set:
- Output level = Low
- Mode = Output Push Pull
- Pull-up/Pull-down = No pull
- Max speed = Low

The user labels matter: CubeMX will generate `LED_RED_Pin` /
`LED_RED_GPIO_Port` etc. in `main.h`, and `main.c` refers to those names.

### 4.5 Project Manager

- *Project Manager → Project*:
  - Toolchain/IDE = **STM32CubeIDE**
  - Linker settings → Minimum heap size = `0x200` (we don't use heap but
    newlib-nano wants some).
  - Minimum stack size = `0x1000` (the FFT/constellation buffers are on
    the stack briefly during init).
- *Project Manager → Code Generator*:
  - **Generate peripheral initialization as a pair of '.c/.h' files per
    peripheral** = off (keep everything in `main.c`).
  - **Keep User Code when re-generating** = on.
- *Project Manager → Advanced Settings*:
  - Leave `HAL` driver selection at defaults.

Hit *Project → Generate Code*. CubeIDE opens the project.

---

## 5. CubeIDE build settings

Open *Project → Properties → C/C++ Build → Settings*.

### 5.1 Preprocessor symbols (MCU GCC Compiler → Preprocessor)

Add (if not already present):
```
USE_HAL_DRIVER     (CubeMX adds this)
STM32F407xx        (CubeMX adds this)
ARM_MATH_CM4
__FPU_PRESENT=1
DEBUG
```

`ARM_MATH_CM4` tells `arm_math.h` which core to compile for.
`__FPU_PRESENT=1` gates the hardfloat intrinsics.

### 5.2 Include paths (MCU GCC Compiler → Include paths)

In addition to the CubeMX defaults (`Core/Inc`, the `Drivers/...`
entries) add:

```
C:/Users/jishu/STM32Cube/Repository/STM32Cube_FW_F4_V1.28.3/Drivers/CMSIS/DSP/Include
```

This is where `arm_math.h` lives.

### 5.3 FPU / floating-point ABI

*MCU Settings* in the same dialog:
- Floating-point unit = `fpv4-sp-d16`
- Floating-point ABI = `hard`

These match `-mfpu=fpv4-sp-d16 -mfloat-abi=hard` in the compile command.

### 5.4 Optimization

- Debug build: `-O0 -g3` (default). The pipeline still runs, just slowly
  (tens of seconds). Fine for this demo.
- For faster runs, switch *Optimization Level* to `-O2` or `-Ofast`.

---

## 6. Linking CMSIS-DSP

CMSIS 5 (shipped with V1.28.3) ships **source only** — there is no
`libarm_cortexM4lf_math.a` under `Lib/GCC/`. There are two ways to get
the DSP code into your binary; this project uses **Option A**.

### Option A (used here) — compile the sources into the project

Copy the following 13 `arm_*.c` files from

```
.../Drivers/CMSIS/DSP/Source/
    TransformFunctions/arm_rfft_fast_f32.c
    TransformFunctions/arm_rfft_fast_init_f32.c
    TransformFunctions/arm_cfft_f32.c
    TransformFunctions/arm_cfft_init_f32.c
    TransformFunctions/arm_cfft_radix8_f32.c
    TransformFunctions/arm_bitreversal2.c
    CommonTables/arm_common_tables.c
    CommonTables/arm_const_structs.c
    ComplexMathFunctions/arm_cmplx_mag_f32.c
    BasicMathFunctions/arm_mult_f32.c
    StatisticsFunctions/arm_max_f32.c
    FastMathFunctions/arm_cos_f32.c
    FastMathFunctions/arm_sin_f32.c
```

into `Core/Src/`. CubeIDE auto-compiles everything in `Core/Src/` so
no further build config is needed.

### Option B — link a prebuilt static library

If you have `libarm_cortexM4lf_math.a` from an older CMSIS pack:
1. Drop it into `Core/Lib/libarm_cortexM4lf_math.a`.
2. Properties → C/C++ Build → Settings → *MCU GCC Linker → Libraries*:
   - Libraries (`-l`): add `arm_cortexM4lf_math`
   - Library search path (`-L`): add `"${workspace_loc:/${ProjName}/Core/Lib}"`

This project's `.cproject` already has Option B wired up too, so either
path works. With Option A active, the linker silently ignores the
missing library lookup.

---

## 7. ITM / SWO printf

Goal: read `printf` output over the ST-Link USB cable without a UART.

### 7.1 Firmware side — `Core/Src/syscalls.c`

Add `#include "stm32f4xx_hal.h"` near the top and replace `_write` with:

```c
__attribute__((weak)) int _write(int file, char *ptr, int len)
{
    (void)file;
    for (int i = 0; i < len; i++) {
        ITM_SendChar((uint32_t)(uint8_t)ptr[i]);
    }
    return len;
}
```

`ITM_SendChar` is defined in CMSIS `core_cm4.h` (pulled in by
`stm32f4xx_hal.h`). It blocks until the ITM FIFO has space and does
nothing if the debugger is not connected — safe on a standalone board.

### 7.2 Debugger side — SWV configuration

*Run → Debug Configurations → your `.launch` → Debugger tab*:
- **Serial Wire Viewer (SWV)** = Enable
- **Core Clock** = **16.0 MHz** (must match HCLK)
- **SWO Clock** = Auto or 2000 kHz
- *Window → Show View → SWV → SWV ITM Data Console*
- In the ITM console gear icon: enable **Port 0**.
- Start debug, hit the red record button in the SWV console, run.

You should see lines like:
```
shazam: song=1 offset=1000ms votes=21 confidence=0.525
```

---

## 8. Bugs that bit us (lessons learned)

These are real issues we hit during this project — keep the list around
for next time.

1. **Wrong LED port**. Original code was ported from an STM32F0 Discovery
   that uses `PC9`. On F407G-DISC1 there is no LED on PC9 — the LEDs are
   on `PD12..PD15`. Symptom: board runs, nothing blinks.
2. **Missing `MX_GPIO_Init`**. After a CubeMX regenerate with no GPIO
   pins configured, the function is not emitted at all. Always make sure
   PD12..PD15 are set to GPIO_Output with user labels in the `.ioc`
   *before* regenerating.
3. **CMSIS-DSP unresolved symbols**. Linker errors like
   `undefined reference to 'arm_rfft_fast_f32'`. Root cause: CMSIS 5
   ships no prebuilt library. Fix with §6 Option A or B.
4. **`arm_math.h` not found**. The DSP headers live outside the CubeMX
   include tree; add the path explicitly (§5.2).
5. **Stray space in preprocessor symbol**. CubeIDE happily accepts
   `" ARM_MATH_CM4"` (with a leading space) but the compiler then sees
   a different macro name, so `arm_math.h` takes the wrong code path.
   Always check the *Preprocessor* list for accidental whitespace.
6. **SWV shows nothing**. Nine times out of ten the SWV *Core Clock* is
   wrong. Match it to HCLK exactly (16 MHz here).

---

## 9. Program flow at runtime

1. `HAL_Init` + `SystemClock_Config` + `MX_GPIO_Init` bring the MCU up
   at HSI 16 MHz with PD12..PD15 as push-pull outputs and PA0 as input
   (on-board User Button).
2. **Ready/waiting**: `LED_BLUE` slow-pulses (400 ms on/off) until the
   user presses the blue PA0 button. Blue LED is then cleared.
3. `shazam_data_init()` synthesises 3 s of mock PCM (four sine tones at
   100 / 300 / 600 / 1200 Hz, 8 kHz mono) into an SRAM buffer.
4. `shazam_init()` precomputes the Hanning window and initialises
   `arm_rfft_fast_instance_f32` for N=1024.
5. `shazam_process_audio()` runs the STFT across 24 000 samples. During
   the loop a "spinner" cycles one LED per frame (Green → Orange → Red
   → Blue) so the board clearly looks *busy* rather than frozen. All
   four LEDs are cleared when the loop exits. Expect 5–60 s in `-O0`.
6. `shazam_identify()` picks the song with the tallest offset spike and
   returns a `MatchResult`.
7. `printf(...)` dumps the result over ITM.
8. The `while(1)` forever-loops the result pattern on `LED_RED`:
   - song_id == 1 → 5 fast blinks (correct)
   - other song  → 2 slow blinks (wrong match)
   - no match    → 1 long pulse

---

## 10. The Shazam pipeline in detail

### 10.1 Input

- 3 seconds of mono PCM at 8 kHz = **24 000 `float` samples** in
  `[-1, +1]`. Generated at boot as the sum of four sines:
  `100, 300, 600, 1200 Hz`, amplitudes `0.8, 0.6, 0.5, 0.4`, then
  normalised.
- These tones are *rigged* to land on predictable FFT bins (see below)
  so that the mock database can be written by hand.

### 10.2 Windowing & FFT (STFT)

For each 1024-sample frame, sliding with hop = 512 (50% overlap):

1. **Hanning window** `w[n] = 0.5 * (1 - cos(2πn / (N-1)))`.
   Precomputed once; applied with `arm_mult_f32`.
2. **Real FFT** via `arm_rfft_fast_f32`. Produces 512 complex bins
   (bins `0..N/2-1`). Resolution = `8000 / 1024 ≈ 7.81 Hz / bin`.
3. **Magnitude** `|X[k]|` via `arm_cmplx_mag_f32`.

Why N = 1024? 128 ms window — classic Shazam-style trade-off between
time and frequency resolution. 1024 is also a power of two natively
supported by `arm_rfft_fast_f32` with no zero-padding.

### 10.3 Constellation map (peak picking)

Energy in music is dominated by bass, so a naïve global argmax picks
the same low bin every frame. We split the spectrum into **4 log-ish
frequency bands** and keep the loudest bin per band per frame, provided
it is at least `CONSTELLATION_THRESHOLD × frame_max` (default 10%).

Band edges (in bins):
```
Band 0: 3..25   (~23 Hz   – 195 Hz , bass)
Band 1: 25..50  (~195 Hz  – 390 Hz , low-mid)
Band 2: 50..100 (~390 Hz  – 780 Hz , mid)
Band 3: 100..256(~780 Hz  – 2000 Hz, upper)
```

Each peak becomes a `ConstellationPoint { freq_bin, frame_index }`. All
points for the whole clip are stored in a single 256-element array.

### 10.4 Combinatorial hashing

Walk the constellation list. Each point is an **anchor**; pair it with
the next `TARGET_FAN_OUT = 5` points (the "target zone"). Each
anchor/target pair produces one **hash**:

```
(anchor_freq, target_freq, delta_time_ms)  ↦  hash
absolute_time_of_anchor_ms                 ↦  live timestamp
```

`delta_time` is the time between anchor and target in ms. With hop =
512 @ 8 kHz each frame is 64 ms, so consecutive pairs have dt = 64, the
next dt = 128, etc. Triplets like `(13, 38, 64)` are very repeatable —
that's the whole point.

### 10.5 Database matching

The "DB" is a 50-entry `const AudioHash[]` in Flash (`song_database`,
`shazam_data.c`). A real system would use a hash table keyed on
`(anchor, target, dt)`; we do a linear scan because 50 entries is
trivial.

For each live hash that matches a DB hash, compute:

```
offset = db_absolute_time - live_absolute_time   // both in ms
```

A **correct** match produces the SAME offset repeatedly (because the
anchor/target *pattern* is shifted in time by a constant amount between
live and DB). A **spurious** match produces random offsets that don't
cluster.

We increment a 2D histogram `offset_histogram[song_id][offset_ms]`.

### 10.6 Identification (argmax over histograms)

- For each tracked song (`MAX_TRACKED_SONGS = 4`), find the tallest
  histogram bin.
- The song with the globally tallest bin wins. Its bin index is the
  `offset_ms` field of `MatchResult`; its bin height is `match_count`;
  `confidence = match_count / total_matches`.

If no hash matched anything (`total_matches == 0`) we return an all-zero
`MatchResult` — that's the "long pulse" LED pattern.

### 10.7 Expected output

```
shazam: song=1 offset=1000ms votes=21 confidence=0.525
```

LED: **five fast blinks** repeated every ~2.5 s.

---

## 11. Memory footprint

Static-only; no heap is used.

```
fft_input[1024]            4096 B
fft_output[1024]           4096 B
magnitude[512]             2048 B
hanning_window[1024]       4096 B
constellation[256]         1536 B   (8 B per point)
offset_histogram[4][500]   4000 B
mock_audio[24000]         96000 B   (largest; move to const Flash if tight)
song_database[50]           800 B   (const, lives in Flash)
-----------------------------------------
total SRAM                ~116 KB  (of 192 KB on F407)
```

---

## 12. Tuning knobs

All in `shazam.h` / `shazam.c`:

| Macro                       | Meaning                                   | Default |
|-----------------------------|-------------------------------------------|---------|
| `FFT_SIZE`                  | STFT window, power of 2                   | 1024    |
| `AUDIO_SAMPLE_RATE`         | PCM rate                                  | 8000    |
| `NUM_FREQ_BANDS`            | Log bands for peak picking                | 4       |
| `band_edges[]`              | Bin boundaries between bands              | see src |
| `CONSTELLATION_THRESHOLD`   | Min peak / frame_max ratio                | 0.1     |
| `MAX_CONSTELLATION_PTS`     | Cap on stored peaks                       | 256     |
| `TARGET_FAN_OUT`            | Targets per anchor                        | 5       |
| `MAX_OFFSET_HISTOGRAM`      | Widest offset tracked (ms)                | 500     |
| `MAX_TRACKED_SONGS`         | Songs kept in histogram                   | 4       |
| `DB_HASH_COUNT`             | Entries in the mock DB                    | 50      |

---

## 13. Flashing

- **CubeIDE**: *Run → Debug As → STM32 C/C++ Application*. Uses the
  ST-Link GDB server behind the scenes.
- **VS Code / command line**: `STM32_Programmer_CLI -c port=SWD
  -w stm32_blinky.elf -rst`.

Either way, after flashing you should see:
1. 10 fast blinks (startup)
2. ~5–60 s LED off (pipeline running)
3. Forever after: the result pattern.

---

## 14. Where to go next

- Wire up I2S MEMS mic → DMA → circular buffer → feed
  `shazam_process_audio` in windowed chunks for real-time capture.
- Move `mock_audio[]` to Flash by pre-rendering the PCM and linking it
  in from a `.bin` file.
- Swap the linear DB scan for a hash table over `(anchor, target, dt)`
  once the DB grows past ~1 k entries.
- Port to CMSIS-DSP fixed-point (`q15` FFT) for cores without FPU.
