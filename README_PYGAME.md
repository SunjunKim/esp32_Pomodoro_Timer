# Pomodoro Timer (Python + Pygame)

Desktop reimplementation of the Arduino LVGL Pomodoro UI.

## What matches the Arduino behavior

- **Two modes/screens**: Timer + Set
- **Same Timer logic**
  - Left bottom button: **Start** / **Pause** / **Resume** / **Next**
  - Right bottom button: **Reset**
  - Tap progress bar (when not running): toggles **Work/Rest** and resets
  - Timer continues past zero into **negative time**, but alarm triggers only when hitting **00:00**
- **Mode toggle**: Arduino “mode button” is replaced by a **gear button (top-left)**
- **No power features**: no pins, no backlight dimming, no sleep/off

## Run

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python main.py
```

## Controls

- **Mouse/touch**
  - Click the **gear** to toggle Timer/Set
  - Timer mode:
    - Click left/right bottom buttons
    - Click the progress bar area to toggle Work/Rest (only when paused/stopped)

