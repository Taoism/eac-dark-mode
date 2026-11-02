# dump a window's menu bar (cross-process): menus.py <hwnd hex>
import ctypes, ctypes.wintypes as w, sys
u=ctypes.windll.user32
def dump(m,d):
    for i in range(u.GetMenuItemCount(m)):
        b=ctypes.create_unicode_buffer(128); u.GetMenuStringW(m,i,b,128,0x400)
        sub=u.GetSubMenu(m,i); mid=u.GetMenuItemID(m,i)
        print("  "*d+f"{mid if mid!=0xffffffff else '':>6} {b.value}")
        if sub: dump(sub,d+1)
dump(u.GetMenu(int(sys.argv[1],16)),0)
