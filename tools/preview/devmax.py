"""Songate for PC, dev only: a maximised window must end where Windows' taskbar begins (the screen's work area), however it came
to be maximised: started so, maximised by Windows, back from full screen. python tools/preview/devmax.py . Nothing of it ships."""
import ctypes
import sys
import time
from ctypes import wintypes

import pcdrive

u = ctypes.windll.user32
u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
u.GetWindow.restype = wintypes.HWND
u.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]


class MI(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('mon', wintypes.RECT), ('work', wintypes.RECT), ('flags', wintypes.DWORD)]


def state(pc, h, what):
    time.sleep(1.2)
    st = pc.stats()
    time.sleep(0.4)
    st = pc.stats()
    wr, cr = wintypes.RECT(), wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(wr))
    u.GetClientRect(h, ctypes.byref(cr))
    u.MonitorFromWindow.restype = wintypes.HMONITOR
    mi = MI()
    mi.cb = ctypes.sizeof(MI)
    u.GetMonitorInfoW(u.MonitorFromWindow(h, 2), ctypes.byref(mi))
    work_h = mi.work.bottom - mi.work.top
    # the window itself no larger than the work area (one that covers the screen is taken for a full-screen app: the taskbar goes
    # behind it), and the taskbar in front of it
    covers = wr.bottom > mi.work.bottom or wr.top < mi.work.top or wr.left < mi.work.left or wr.right > mi.work.right
    u.FindWindowW.restype = wintypes.HWND
    tray = u.FindWindowW('Shell_TrayWnd', None)
    above, z = False, u.GetWindow(h, 3)   # GW_HWNDPREV: what is in front of the app's window
    while z:
        if z == tray:
            above = True
            break
        z = u.GetWindow(z, 3)
    ok = cr.bottom == work_h and st['ph'] == work_h and not covers and above
    what += f' (covers the taskbar: {covers}, taskbar in front: {above})'
    print(f"{what}: window {wr.right - wr.left}x{wr.bottom - wr.top}, client {cr.right}x{cr.bottom}, drawn {st['pw']}x{st['ph']}, "
          f"work area {mi.work.right - mi.work.left}x{work_h}, zoomed {bool(u.IsZoomed(h))}: {'right' if ok else 'WRONG'}")
    return ok


def main():
    good = True
    pc = pcdrive.PC(window='60 60 1280 720 1')   # started maximised, as the user's
    try:
        pc.wait(lambda s: s.w > 0, 40)
        time.sleep(5)
        h = pc.hwnd()
        good &= state(pc, h, 'started maximised')
        u.ShowWindow(h, 9)
        good &= not state(pc, h, 'restored') or True
        u.ShowWindow(h, 3)
        good &= state(pc, h, 'maximised by Windows')
        pc.clear_notices()
        pc.ctl(f'pad {1 << 11}')   # the Canvas: always the whole screen (F11 cannot be pressed from here)
        time.sleep(1.5)
        wr = wintypes.RECT()
        u.GetWindowRect(h, ctypes.byref(wr))
        st = pc.stats()
        full = (wr.right - wr.left, wr.bottom - wr.top) == (u.GetSystemMetrics(0), u.GetSystemMetrics(1)) and st['ph'] == u.GetSystemMetrics(1)
        print(f"full screen (the Canvas): window {wr.right - wr.left}x{wr.bottom - wr.top}, drawn {st['pw']}x{st['ph']}: {'right' if full else 'WRONG'}")
        good &= full
        pc.ctl(f'pad {1 << 11}')
        good &= state(pc, h, 'back from full screen')
    finally:
        pc.quit()
    print('ALL GOOD' if good else 'FAULTS')
    sys.exit(0 if good else 1)


main()
