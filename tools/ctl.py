# small EAC remote: ctl.py move2 | cmd <id> | close <hwnd> | initmenu <index> | tops
import ctypes, ctypes.wintypes as w, sys
u=ctypes.windll.user32
def main_hwnd():
    return u.FindWindowW("erstes", None)
def monitors():
    res=[]
    P=ctypes.WINFUNCTYPE(w.BOOL,w.HMONITOR,w.HDC,ctypes.POINTER(w.RECT),w.LPARAM)
    def cb(m,d,r,l):
        res.append((r.contents.left,r.contents.top,r.contents.right,r.contents.bottom)); return True
    u.EnumDisplayMonitors(0,0,P(cb),0); return res
a=sys.argv[1]; h=main_hwnd()
if a=='move2':
    mons=monitors(); prim=[m for m in mons if m[0]==0 and m[1]==0]
    other=[m for m in mons if m not in prim] or mons
    m=other[0]; u.ShowWindow(h,9)  # restore
    u.SetWindowPos(h,0,m[0]+40,m[1]+40,1500,900,0x14)  # NOZORDER|NOACTIVATE
    print("moved to",m, "monitors:",mons)
elif a=='cmd':
    u.PostMessageW(h,0x111,int(sys.argv[2]),0); print("posted",sys.argv[2])
elif a=='close':
    u.PostMessageW(int(sys.argv[2],16),0x10,0,0)
elif a=='initmenu':
    sub=u.GetSubMenu(u.GetMenu(h),int(sys.argv[2]))
    u.SendMessageW(h,0x117,sub,int(sys.argv[2]))
    for i in range(u.GetMenuItemCount(sub)):
        b=ctypes.create_unicode_buffer(128); u.GetMenuStringW(sub,i,b,128,0x400)
        st=u.GetMenuState(sub,i,0x400); print(u.GetMenuItemID(sub,i), b.value, "checked" if st&8 else "")
elif a=='tops':
    pid=w.DWORD(); u.GetWindowThreadProcessId(h,ctypes.byref(pid))
    P=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    def cb(x,_):
        p=w.DWORD(); u.GetWindowThreadProcessId(x,ctypes.byref(p))
        if p.value==pid.value and u.IsWindowVisible(x):
            t=ctypes.create_unicode_buffer(200); u.GetWindowTextW(x,t,200)
            c=ctypes.create_unicode_buffer(64); u.GetClassNameW(x,c,64); print(hex(x),c.value,t.value)
        return True
    u.EnumWindows(P(cb),0)
