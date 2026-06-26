# AMDE v1.0 — Adaptive Mechanical Debounce Engine

> **Stop double-click ghosts for good.** AMDE runs silently in your system tray, learns your mouse's bounce pattern per-device, and filters phantom clicks with a statistically grounded algorithm — not a dumb fixed delay.

---

## Why AMDE instead of the other tools?

| Feature | AMDE v3 | Most GitHub tools | AutoHotkey scripts |
|---|:---:|:---:|:---:|
| Adaptive threshold (learns your mouse) | ✅ | ❌ | ❌ |
| Per-device profile (KVM, USB hub) | ✅ | ❌ | ❌ |
| Real Bayesian posterior (Beta conjugate) | ✅ | ❌ | ❌ |
| Dual-threshold hysteresis | ✅ | rare | ❌ |
| Drag detection (no false positives) | ✅ | ❌ | ❌ |
| XBUTTON1 + XBUTTON2 support | ✅ | partial | partial |
| Live bounce chart | ✅ | ❌ | ❌ |
| Persists learning across reboots | ✅ | ❌ | ❌ |
| Single `.exe`, no runtime deps | ✅ | varies | requires AHK |
| Thread-safe (SRWLock, double-check) | ✅ | ❌ | N/A |

---

## How it works

### 1. P95 Quantile threshold (QuickSelect)

AMDE keeps a ring buffer of the last **128** inter-click intervals from each button.
Every 4 bounce events it computes the **95th percentile** using an iterative QuickSelect
with median-of-three pivot — O(n) average, zero allocation.

The P95 value becomes the base debounce window. This means:
- If your switch bounces at 8–18 ms, the threshold settles around 18–20 ms automatically.
- If your switch is healthy, the threshold stays near the hard floor (default 35 ms).

### 2. Beta(α, β) conjugate Bayesian posterior

Unlike tools that use a fixed delay or a crude EMA labelled "Bayesian", AMDE maintains
a **true Beta distribution** posterior over the probability that any given click is a bounce.

```
Prior:  Beta(α₀=1, β₀=9)  →  P(bounce) ≈ 10% initially

On bounce event:  α ← λ·α + 1,  β ← λ·β      (λ=0.98 forgetting factor)
On clean click:   α ← λ·α,      β ← λ·β + 1

Posterior mean:   P(bounce) = α / (α + β)
```

The posterior mean drives a **Bayesian extension** on top of the P95 threshold:

```
extension     = P(bounce) × MAX_BAYES_EXTENSION (20 ms)
threshold_exit  = P95 + extension
threshold_enter = threshold_exit + hysteresis_band
```

A mouse that has been bouncing a lot gets a wider debounce window automatically.
A healthy mouse stays lean.

### 3. Dual-threshold hysteresis

Two thresholds prevent oscillation:

```
threshold_enter > threshold_exit

Outside filter zone: only enter if delta < threshold_enter  (high bar)
Inside  filter zone: only exit  if delta > threshold_exit   (low bar)
```

The gap between them is the **dead band** — a region where the filter neither
enters nor exits, preventing rapid state toggling.

### 4. Drag detection

Once a button-down is recorded, any `WM_MOUSEMOVE` that exceeds 5 px² displacement
marks that button as `STATE_DRAGGING`. Drag releases are **never filtered**, ensuring
drag-and-drop operations are unaffected.

### 5. Per-device LRU registry

Each physical device (identified by its Raw Input device name / VID+PID path) gets its own
profile. Up to 32 profiles are kept in an LRU cache. When a device reconnects (KVM switch,
USB hub replug), its learned profile is re-mapped by device string.

---

## Build

### MSVC (recommended)

```bat
cl /O2 /W4 /DUNICODE /D_UNICODE /I include ^
   src\amde_main.c src\amde_engine.c src\amde_gui.c ^
   /link /SUBSYSTEM:WINDOWS ^
   user32.lib shell32.lib advapi32.lib comctl32.lib
```

### MinGW / MSYS2

```bash
gcc -O2 -Wall -DUNICODE -D_UNICODE -mwindows -I include \
    src/amde_main.c src/amde_engine.c src/amde_gui.c \
    -luser32 -lshell32 -ladvapi32 -lcomctl32 -lcomdlg32 \
    -o amde.exe
```

No external dependencies. No CMake. One command.

---

## Project layout

```
amde_v3/
├── include/
│   ├── amde_core.h      — shared types, constants, globals declaration
│   ├── amde_engine.h    — engine API + inline Bayesian helpers
│   └── amde_gui.h       — GUI API + chart buffer
└── src/
    ├── amde_main.c      — wWinMain, single-instance, message loop
    ├── amde_engine.c    — hook, QuickSelect, Bayesian, persistence, LRU registry
    └── amde_gui.c       — tray, config window, live GDI chart
```

---

## Usage

1. Build and run `amde.exe` — a shield icon appears in the system tray.
2. AMDE starts filtering immediately with conservative defaults.
3. **Double-click the tray icon** to open the config window:
   - Adjust **Min threshold floor** (20–80 ms)
   - Adjust **Hysteresis band** (1–10 ms)
   - Watch the **live chart**: red bars = bounces filtered, green bars = clean clicks, yellow line = current threshold
   - Hit **Reset all profiles** to clear learned data
4. Profiles are saved to `HKCU\Software\AMDE` on exit and reloaded on next launch.

---

## Algorithm parameters

| Parameter | Default | Range | Effect |
|---|---|---|---|
| `DEFAULT_MIN_THRESHOLD_US` | 35 ms | 20–80 ms | Hard floor — threshold never goes below this |
| `MAX_BAYES_EXTENSION_US` | 20 ms | — | Max Bayesian extension above P95 |
| `HYSTERESIS_BAND_US` | 2.5 ms | 1–10 ms | Dead band between enter/exit thresholds |
| `BOUNCE_HISTORY_SIZE` | 128 | — | Ring buffer depth for P95 computation |
| `QUANTILE_UPDATE_PERIOD` | 4 | — | Recompute P95 every N bounce events |
| `BAYES_ALPHA_INIT` | 1.0 | — | Beta prior pseudo-count (bounces) |
| `BAYES_BETA_INIT` | 9.0 | — | Beta prior pseudo-count (clean clicks) |

---

## Contributing

PRs welcome. Priority areas:
- [ ] Auto-start at login (Task Scheduler / Run key)
- [ ] Installer (NSIS or WiX)
- [ ] Per-application profile override
- [ ] Dark/light theme for config window
- [ ] Export/import profile JSON

---

## License

MIT
