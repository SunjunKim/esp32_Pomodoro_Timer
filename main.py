# To Run:
# python3 -m venv .venv
# source .venv/bin/activate
# pip install -r requirements.txt
# python main.py

from __future__ import annotations

from array import array
import ctypes
import ctypes.util
import math
import shutil
import subprocess
import sys
from dataclasses import dataclass
from enum import Enum

import pygame


LOGICAL_W = 240
LOGICAL_H = 280
SCALE = 1.0 # desktop scale factor


class SessionType(Enum):
    WORK = "Work"
    REST = "Rest"


class TimerState(Enum):
    STOPPED = "stopped"
    RUNNING = "running"
    PAUSED = "paused"


class UIMode(Enum):
    TIMER = "timer"
    SET = "set"


@dataclass
class Alarm333:
    active: bool = False
    tone_on: bool = False
    beeps_done_in_group: int = 0
    groups_done: int = 0
    next_ms: int = 0


def clamp(v: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, v))


def lv_checkbox_indicator_rect(row: pygame.Rect, size: int) -> pygame.Rect:
    """Logical rect for an LVGL-style lv_checkbox indicator (left side of a row)."""
    return pygame.Rect(row.left, row.centery - size // 2, size, size)


def draw_lv_checkbox_indicator(surf: pygame.Surface, rect: pygame.Rect, checked: bool) -> None:
    """Draw lv_checkbox PART_INDICATOR: bordered box; when checked, a tick like default LVGL theme."""
    border_rgb = (224, 228, 236)
    tick_rgb = (240, 243, 250)
    pygame.draw.rect(surf, border_rgb, rect, width=1, border_radius=3)
    if not checked:
        return
    x, y, w, h = rect.x, rect.y, rect.w, rect.h
    pad = max(3, w // 5)
    pts = (
        (x + pad, y + h // 2 - 1),
        (x + w // 2 - 1, y + h - pad),
        (x + w - pad, y + pad + 1),
    )
    pygame.draw.lines(surf, tick_rgb, False, pts, width=2)


def set_window_always_on_top(enabled: bool) -> None:
    """Raise or lower the pygame window in the stacking order (best-effort per OS)."""
    if not pygame.display.get_init():
        return
    try:
        wm = pygame.display.get_wm_info()
    except pygame.error:
        return

    if sys.platform == "win32":
        hwnd = wm.get("window")
        if hwnd is None:
            return
        HWND_TOPMOST = -1
        HWND_NOTOPMOST = -2
        SWP_NOSIZE = 0x0001
        SWP_NOMOVE = 0x0002
        SWP_NOACTIVATE = 0x0010
        flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
        try:
            from ctypes import wintypes

            user32 = ctypes.WinDLL("user32", use_last_error=True)
            user32.SetWindowPos.argtypes = [
                wintypes.HWND,
                wintypes.HWND,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_uint,
            ]
            user32.SetWindowPos.restype = wintypes.BOOL
            z = HWND_TOPMOST if enabled else HWND_NOTOPMOST
            user32.SetWindowPos(hwnd, z, 0, 0, 0, 0, flags)
        except Exception:
            pass
        return

    if sys.platform == "darwin":
        capsule = wm.get("window")
        if capsule is None:
            return
        try:
            PyCapsule_GetPointer = ctypes.pythonapi.PyCapsule_GetPointer
            PyCapsule_GetPointer.restype = ctypes.c_void_p
            PyCapsule_GetPointer.argtypes = [ctypes.py_object, ctypes.c_char_p]
            nswin = PyCapsule_GetPointer(capsule, b"window")
        except Exception:
            return
        if not nswin:
            return
        try:
            objc_lib = ctypes.cdll.LoadLibrary(ctypes.util.find_library("objc"))
            objc_lib.sel_registerName.restype = ctypes.c_void_p
            objc_lib.sel_registerName.argtypes = [ctypes.c_char_p]
            sel = objc_lib.sel_registerName(b"setLevel:")
            # NSFloatingWindowLevel (3) vs NSNormalWindowLevel (0)
            level = 3 if enabled else 0
            msg_send = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long)(
                ("objc_msgSend", objc_lib)
            )
            msg_send(nswin, sel, level)
        except Exception:
            pass
        return

    if sys.platform.startswith("linux") and shutil.which("wmctrl"):
        wid = wm.get("window")
        if not isinstance(wid, int):
            return
        op = "add" if enabled else "remove"
        try:
            subprocess.run(
                ["wmctrl", "-i", "-r", str(wid), "-b", f"{op},above"],
                capture_output=True,
                timeout=2,
                check=False,
            )
        except Exception:
            pass


def format_mmss(remaining_seconds: int) -> str:
    if remaining_seconds >= 0:
        mm = remaining_seconds // 60
        ss = remaining_seconds % 60
        return f"{mm:02d}:{ss:02d}"
    neg = -remaining_seconds
    mm = neg // 60
    ss = neg % 60
    return f"-{mm:02d}:{ss:02d}"


def make_tone(
    freq_hz: float,
    duration_ms: int,
    volume: float = 0.25,
    sample_rate: int = 44100,
) -> pygame.mixer.Sound:
    n = int(sample_rate * (duration_ms / 1000.0))
    if n <= 0:
        n = 1

    # Interleaved stereo int16 samples: L, R, L, R, ...
    samples = array("h")
    amp = int(32767.0 * float(volume))
    two_pi_f = 2.0 * math.pi * float(freq_hz)
    inv_sr = 1.0 / float(sample_rate)

    for i in range(n):
        v = int(math.sin(two_pi_f * (i * inv_sr)) * amp)
        samples.append(v)
        samples.append(v)

    return pygame.mixer.Sound(buffer=samples.tobytes())


def make_tomato_icon_surface(size: int = 128) -> pygame.Surface:
    """
    Create a small tomato icon surface for the window/app icon.
    This avoids external asset loading issues in PyInstaller builds.
    """
    surf = pygame.Surface((size, size), pygame.SRCALPHA)
    surf.fill((0, 0, 0, 0))

    cx = size // 2
    body_r = int(size * 0.34)
    body_cy = int(size * 0.58)

    red = (229, 57, 53, 255)
    dark = (198, 40, 40, 255)
    green = (46, 125, 50, 255)
    green2 = (27, 94, 32, 255)

    # Body (slightly flattened)
    for i in range(-4, 5):
        rr = body_r + (abs(i) // 2)
        y = body_cy + i
        pygame.draw.circle(surf, red, (cx, y), rr)

    # Lower shade
    pygame.draw.ellipse(
        surf,
        dark,
        pygame.Rect(int(size * 0.18), int(size * 0.68), int(size * 0.64), int(size * 0.22)),
    )
    pygame.draw.ellipse(
        surf,
        red,
        pygame.Rect(int(size * 0.18), int(size * 0.66), int(size * 0.64), int(size * 0.20)),
    )

    # Highlight
    hl = pygame.Surface((size, size), pygame.SRCALPHA)
    pygame.draw.ellipse(
        hl,
        (255, 255, 255, 70),
        pygame.Rect(int(size * 0.30), int(size * 0.30), int(size * 0.20), int(size * 0.42)),
    )
    surf.blit(hl, (0, 0))

    # Stem
    stem = pygame.Rect(int(size * 0.47), int(size * 0.14), int(size * 0.06), int(size * 0.22))
    pygame.draw.rect(surf, green2, stem, border_radius=max(2, size // 32))

    # Calyx (simple 5-point star)
    center = (cx, int(size * 0.33))
    outer = int(size * 0.16)
    inner = int(size * 0.07)
    pts: list[tuple[int, int]] = []
    for k in range(10):
        ang = math.radians(-90 + k * 36)
        rr = outer if (k % 2 == 0) else inner
        pts.append((center[0] + int(rr * math.cos(ang)), center[1] + int(rr * math.sin(ang))))
    pygame.draw.polygon(surf, green, pts)
    pygame.draw.polygon(surf, green2, pts, width=max(1, size // 64))

    return surf


class UIButton:
    def __init__(
        self,
        rect: pygame.Rect,
        label: str,
        on_click,
        *,
        bg=(60, 60, 60),
        bg_pressed=(40, 40, 40),
        fg=(255, 255, 255),
        radius: int = 10,
        font: pygame.font.Font | None = None,
        visible: bool = True,
    ):
        self.rect = rect
        self.label = label
        self.on_click = on_click
        self.bg = bg
        self.bg_pressed = bg_pressed
        self.fg = fg
        self.radius = radius
        self.font = font
        self.visible = visible
        self._pressed = False

    def handle_event(self, e: pygame.event.Event) -> bool:
        if not self.visible:
            return False
        if e.type == pygame.MOUSEBUTTONDOWN and e.button == 1:
            if self.rect.collidepoint(e.pos):
                self._pressed = True
                return True
        if e.type == pygame.MOUSEBUTTONUP and e.button == 1:
            was_pressed = self._pressed
            self._pressed = False
            if was_pressed and self.rect.collidepoint(e.pos):
                self.on_click()
                return True
        return False

    def draw(self, surf: pygame.Surface):
        if not self.visible:
            return
        color = self.bg_pressed if self._pressed else self.bg
        pygame.draw.rect(surf, color, self.rect, border_radius=self.radius)
        if self.label:
            font = self.font
            if font is None:
                font = pygame.font.SysFont(None, 28)
            text = font.render(self.label, True, self.fg)
            tr = text.get_rect(center=self.rect.center)
            surf.blit(text, tr)


class PomodoroApp:
    def __init__(self):
        self.work_minutes = 25
        self.rest_minutes = 5
        self.session_type = SessionType.WORK
        self.timer_state = TimerState.STOPPED
        self.ui_mode = UIMode.TIMER
        self.always_on_top = False
        self.remaining_seconds = self.work_minutes * 60
        self.last_second_tick_ms = 0

        self.alarm = Alarm333()
        self.tone_until_ms = 0

        self._click_sound: pygame.mixer.Sound | None = None
        self._alarm_beep: pygame.mixer.Sound | None = None
        self._welcome: list[pygame.mixer.Sound] = []

        # Set runtime window icon (prevents default placeholder icon).
        try:
            pygame.display.set_icon(make_tomato_icon_surface(128))
        except Exception:
            pass

        self.screen = pygame.display.set_mode((LOGICAL_W * SCALE, LOGICAL_H * SCALE))
        pygame.display.set_caption("Pomodoro Timer (Pygame)")
        self.canvas = pygame.Surface((LOGICAL_W, LOGICAL_H)).convert_alpha()

        self.font_session = pygame.font.SysFont(None, 32)
        self.font_time = pygame.font.SysFont(None, 52)
        self.font_btn = pygame.font.SysFont(None, 34)
        self.font_small = pygame.font.SysFont(None, 20)

        self._build_sounds()
        self._play_welcome()
        self._build_ui()
        self._sync_labels()

    def _build_sounds(self):
        try:
            pygame.mixer.init(frequency=44100, size=-16, channels=2, buffer=512)
            self._click_sound = make_tone(1047, 42, volume=0.18)
            self._alarm_beep = make_tone(1760, 90, volume=0.22)
            for hz in (1047, 1319, 1568, 2093):
                self._welcome.append(make_tone(hz, 45, volume=0.16))
        except Exception:
            self._click_sound = None
            self._alarm_beep = None
            self._welcome = []

    def _play_welcome(self):
        if not self._welcome:
            return
        for s in self._welcome:
            s.play()
            pygame.time.delay(60)

    def _ui_click(self):
        if self._click_sound:
            self._click_sound.play()

    def _clear_buzzer(self):
        self.tone_until_ms = 0
        self.alarm = Alarm333()

    def _start_alarm_333(self, now_ms: int):
        self.alarm.active = True
        self.alarm.tone_on = False
        self.alarm.beeps_done_in_group = 0
        self.alarm.groups_done = 0
        self.alarm.next_ms = now_ms

    def _reset_remaining_for_current_session(self):
        mins = self.work_minutes if self.session_type == SessionType.WORK else self.rest_minutes
        self.remaining_seconds = int(mins) * 60

    def _do_restart(self):
        self._reset_remaining_for_current_session()
        self.timer_state = TimerState.RUNNING
        self.last_second_tick_ms = pygame.time.get_ticks()
        self._clear_buzzer()
        self._sync_labels()

    def _do_pause_resume(self):
        if self.timer_state == TimerState.RUNNING:
            self.timer_state = TimerState.PAUSED
        elif self.timer_state == TimerState.PAUSED:
            self.timer_state = TimerState.RUNNING
            self.last_second_tick_ms = pygame.time.get_ticks()
        self._sync_labels()

    def _do_stop_reset(self):
        self.timer_state = TimerState.STOPPED
        self._reset_remaining_for_current_session()
        self._clear_buzzer()
        self._sync_labels()

    def _do_next_session(self):
        self.session_type = SessionType.REST if self.session_type == SessionType.WORK else SessionType.WORK
        self._reset_remaining_for_current_session()
        self.timer_state = TimerState.RUNNING
        self.last_second_tick_ms = pygame.time.get_ticks()
        self._clear_buzzer()
        self._sync_labels()

    def _toggle_session_stopped(self):
        if self.timer_state == TimerState.RUNNING:
            return
        self.session_type = SessionType.REST if self.session_type == SessionType.WORK else SessionType.WORK
        self.timer_state = TimerState.STOPPED
        self._reset_remaining_for_current_session()
        self._clear_buzzer()
        self._ui_click()
        self._sync_labels()

    def _adjust_minutes(self, work: bool, delta: int):
        if self.ui_mode != UIMode.SET:
            return
        if work:
            self.work_minutes = clamp(self.work_minutes + delta, 1, 99)
        else:
            self.rest_minutes = clamp(self.rest_minutes + delta, 1, 99)
        if self.timer_state == TimerState.STOPPED:
            self._reset_remaining_for_current_session()
        self._sync_labels()

    def _toggle_ui_mode(self):
        self.ui_mode = UIMode.SET if self.ui_mode == UIMode.TIMER else UIMode.TIMER
        self._ui_click()
        self._sync_labels()

    def _toggle_always_on_top(self):
        self.always_on_top = not self.always_on_top
        set_window_always_on_top(self.always_on_top)
        self._ui_click()

    def _build_ui(self):
        bottom_section_h = LOGICAL_H // 4
        bottom_section_y = LOGICAL_H - bottom_section_h
        half_w = LOGICAL_W // 2

        self.rect_bar = pygame.Rect(6, 40, 228, 100)  # matches LVGL size; centered-ish with padding
        self.rect_gear = pygame.Rect(6, 6, 32, 32)
        self.rect_aot = pygame.Rect(
            self.rect_gear.right + 4,
            6,
            LOGICAL_W - (self.rect_gear.right + 4) - 6,
            32,
        )
        self.rect_cb_aot = lv_checkbox_indicator_rect(self.rect_aot, 16)
        self._aot_label_x = self.rect_cb_aot.right + 6

        self.btn_left = UIButton(
            pygame.Rect(0, bottom_section_y, half_w, bottom_section_h),
            "Start",
            self._on_btn_left,
            bg=(31, 168, 85),
            bg_pressed=(21, 128, 61),
            radius=0,
            font=self.font_btn,
        )
        self.btn_right = UIButton(
            pygame.Rect(half_w, bottom_section_y, LOGICAL_W - half_w, bottom_section_h),
            "Reset",
            self._on_btn_right,
            bg=(211, 47, 47),
            bg_pressed=(168, 37, 37),
            radius=0,
            font=self.font_btn,
        )
        self.btn_gear = UIButton(
            self.rect_gear,
            "⚙",
            self._toggle_ui_mode,
            bg=(27, 34, 48),
            bg_pressed=(17, 22, 32),
            radius=8,
            font=pygame.font.SysFont(None, 28),
        )

        # Set screen buttons (top half = Work, bottom half = Rest)
        row_h = LOGICAL_H // 2
        btn_w, btn_h = 76, 58
        pad_x, pad_y = 10, 6

        self.btn_work_minus = UIButton(
            pygame.Rect(pad_x, row_h - btn_h - pad_y, btn_w, btn_h),
            "-",
            lambda: self._adjust_minutes(True, -1),
            bg=(27, 34, 48),
            bg_pressed=(17, 22, 32),
            radius=10,
            font=pygame.font.SysFont(None, 44),
        )
        self.btn_work_plus = UIButton(
            pygame.Rect(LOGICAL_W - pad_x - btn_w, row_h - btn_h - pad_y, btn_w, btn_h),
            "+",
            lambda: self._adjust_minutes(True, +1),
            bg=(27, 34, 48),
            bg_pressed=(17, 22, 32),
            radius=10,
            font=pygame.font.SysFont(None, 44),
        )
        self.btn_rest_minus = UIButton(
            pygame.Rect(pad_x, LOGICAL_H - btn_h - pad_y, btn_w, btn_h),
            "-",
            lambda: self._adjust_minutes(False, -1),
            bg=(27, 34, 48),
            bg_pressed=(17, 22, 32),
            radius=10,
            font=pygame.font.SysFont(None, 44),
        )
        self.btn_rest_plus = UIButton(
            pygame.Rect(LOGICAL_W - pad_x - btn_w, LOGICAL_H - btn_h - pad_y, btn_w, btn_h),
            "+",
            lambda: self._adjust_minutes(False, +1),
            bg=(27, 34, 48),
            bg_pressed=(17, 22, 32),
            radius=10,
            font=pygame.font.SysFont(None, 44),
        )

    def _on_btn_left(self):
        if self.ui_mode != UIMode.TIMER:
            return
        if self.timer_state == TimerState.STOPPED:
            self._do_restart()
        elif self.remaining_seconds > 0:
            self._do_pause_resume()
        else:
            self._do_next_session()
        self._ui_click()

    def _on_btn_right(self):
        if self.ui_mode != UIMode.TIMER:
            return
        self._do_stop_reset()
        self._ui_click()

    def _sync_labels(self):
        # Left button label logic matches Arduino update_ui()
        stopped = self.timer_state == TimerState.STOPPED
        positive = self.remaining_seconds > 0
        if stopped:
            self.btn_left.label = "Start"
        elif positive:
            self.btn_left.label = "Resume" if self.timer_state == TimerState.PAUSED else "Pause"
        else:
            self.btn_left.label = "Next"
        self.btn_right.label = "Reset"

    def _tick_timer(self, now_ms: int):
        if self.timer_state != TimerState.RUNNING:
            return
        if (now_ms - self.last_second_tick_ms) < 1000:
            return
        # keep Arduino-like cadence by stepping exactly 1s per 1000ms slice
        while (now_ms - self.last_second_tick_ms) >= 1000:
            self.last_second_tick_ms += 1000
            before = self.remaining_seconds
            self.remaining_seconds -= 1
            if before > 0 and self.remaining_seconds == 0:
                self._start_alarm_333(now_ms=self.last_second_tick_ms)
        self._sync_labels()

    def _poll_alarm(self, now_ms: int):
        if not self.alarm.active:
            return
        if now_ms < self.alarm.next_ms:
            return

        # Arduino constants:
        # kAlarmShortBeepMs = 90
        # kAlarmShortGapMs = 110
        # kAlarmGroupGapMs = 260
        # kAlarmBeepsPerGroup = 3
        # kAlarmGroupCount = 3
        if not self.alarm.tone_on:
            if self._alarm_beep:
                self._alarm_beep.play()
            self.alarm.tone_on = True
            self.alarm.next_ms = now_ms + 90
            return

        # tone off moment (we just wait; sound sample will have ended)
        self.alarm.tone_on = False
        self.alarm.beeps_done_in_group += 1

        if self.alarm.beeps_done_in_group < 3:
            self.alarm.next_ms = now_ms + 110
            return

        self.alarm.beeps_done_in_group = 0
        self.alarm.groups_done += 1
        if self.alarm.groups_done < 3:
            self.alarm.next_ms = now_ms + 260
        else:
            self.alarm.active = False
            self.alarm.next_ms = 0

    def _handle_event(self, e: pygame.event.Event):
        if e.type == pygame.QUIT:
            raise SystemExit

        # Scale mouse coords down to logical canvas coords
        if e.type in (pygame.MOUSEBUTTONDOWN, pygame.MOUSEBUTTONUP, pygame.MOUSEMOTION):
            mx, my = e.pos
            e.pos = (mx // SCALE, my // SCALE)

        # Gear always active
        if self.btn_gear.handle_event(e):
            return

        if e.type == pygame.MOUSEBUTTONUP and e.button == 1:
            if self.rect_aot.collidepoint(e.pos):
                self._toggle_always_on_top()
                return

        if self.ui_mode == UIMode.TIMER:
            if self.btn_left.handle_event(e):
                return
            if self.btn_right.handle_event(e):
                return
            if e.type == pygame.MOUSEBUTTONUP and e.button == 1:
                if self.rect_bar.collidepoint(e.pos):
                    self._toggle_session_stopped()
                    return
        else:
            if self.btn_work_minus.handle_event(e):
                self._ui_click()
                return
            if self.btn_work_plus.handle_event(e):
                self._ui_click()
                return
            if self.btn_rest_minus.handle_event(e):
                self._ui_click()
                return
            if self.btn_rest_plus.handle_event(e):
                self._ui_click()
                return

    def _draw_timer_screen(self, surf: pygame.Surface):
        # Progress bar background + border
        pygame.draw.rect(surf, (27, 34, 48), self.rect_bar, border_radius=16)
        pygame.draw.rect(surf, (61, 74, 99), self.rect_bar, width=3, border_radius=16)

        total_seconds = (self.work_minutes if self.session_type == SessionType.WORK else self.rest_minutes) * 60
        progress_permille = 0
        if total_seconds > 0:
            if self.session_type == SessionType.WORK:
                if self.remaining_seconds > 0:
                    progress_permille = int((self.remaining_seconds * 1000) / total_seconds)
            else:
                elapsed = total_seconds - self.remaining_seconds
                elapsed = clamp(elapsed, 0, total_seconds)
                progress_permille = int((elapsed * 1000) / total_seconds)

        progress_permille = clamp(progress_permille, 0, 1000)
        frac = progress_permille / 1000.0
        fill_w = int(self.rect_bar.width * frac)
        fill_rect = pygame.Rect(self.rect_bar.left, self.rect_bar.top, fill_w, self.rect_bar.height)
        fill_color = (255, 59, 48) if self.session_type == SessionType.WORK else (34, 197, 94)
        if fill_w > 0:
            pygame.draw.rect(surf, fill_color, fill_rect, border_radius=16)
            # cover right-side rounding when partially filled
            if fill_w < self.rect_bar.width:
                pygame.draw.rect(
                    surf,
                    fill_color,
                    pygame.Rect(self.rect_bar.left, self.rect_bar.top, fill_w, self.rect_bar.height),
                    border_radius=16,
                )

        # Session label centered on bar
        session_text = self.font_session.render(self.session_type.value, True, (242, 242, 242))
        surf.blit(session_text, session_text.get_rect(center=self.rect_bar.center))

        # Time label above bottom buttons
        bottom_section_h = LOGICAL_H // 4
        time_str = format_mmss(self.remaining_seconds)
        time_text = self.font_time.render(time_str, True, (255, 255, 255))
        surf.blit(time_text, time_text.get_rect(midbottom=(LOGICAL_W // 2, LOGICAL_H - bottom_section_h - 6)))

        # Buttons
        self.btn_left.draw(surf)
        self.btn_right.draw(surf)

    def _draw_set_screen(self, surf: pygame.Surface):
        row_h = LOGICAL_H // 2

        # Work row (top half)
        work_title = self.font_small.render("Work", True, (255, 255, 255))
        surf.blit(work_title, work_title.get_rect(midright=(LOGICAL_W // 2 - 8, 52)))
        work_val = self.font_session.render(f"{self.work_minutes} min", True, (255, 255, 255))
        surf.blit(work_val, work_val.get_rect(midleft=(LOGICAL_W // 2 + 8, 52)))
        self.btn_work_minus.draw(surf)
        self.btn_work_plus.draw(surf)

        # Rest row (bottom half)
        rest_title = self.font_small.render("Rest", True, (255, 255, 255))
        surf.blit(rest_title, rest_title.get_rect(midright=(LOGICAL_W // 2 - 8, row_h + 52)))
        rest_val = self.font_session.render(f"{self.rest_minutes} min", True, (255, 255, 255))
        surf.blit(rest_val, rest_val.get_rect(midleft=(LOGICAL_W // 2 + 8, row_h + 52)))
        self.btn_rest_minus.draw(surf)
        self.btn_rest_plus.draw(surf)

    def draw(self):
        self.canvas.fill((16, 19, 26))

        if self.ui_mode == UIMode.TIMER:
            self._draw_timer_screen(self.canvas)
        else:
            self._draw_set_screen(self.canvas)

        self.btn_gear.draw(self.canvas)

        draw_lv_checkbox_indicator(self.canvas, self.rect_cb_aot, self.always_on_top)
        aot_surf = self.font_small.render("always-on-top", True, (200, 200, 200))
        self.canvas.blit(
            aot_surf,
            aot_surf.get_rect(midleft=(self._aot_label_x, self.rect_aot.centery)),
        )

        scaled = pygame.transform.scale(self.canvas, self.screen.get_size())
        self.screen.blit(scaled, (0, 0))
        pygame.display.flip()

    def run(self):
        clock = pygame.time.Clock()
        while True:
            now_ms = pygame.time.get_ticks()
            for e in pygame.event.get():
                self._handle_event(e)

            self._tick_timer(now_ms)
            self._poll_alarm(now_ms)
            self.draw()
            clock.tick(60)


def main():
    pygame.init()
    try:
        PomodoroApp().run()
    except SystemExit:
        pass
    finally:
        pygame.quit()


if __name__ == "__main__":
    main()

