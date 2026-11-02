# enumerate all windows (top-level + children) of a process
import ctypes, ctypes.wintypes as w, sys
u=ctypes.windll.user32
pid=int(sys.argv[1])
EnumProc=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
def info(h):
    c=ctypes.create_unicode_buffer(256);u.GetClassNameW(h,c,256)
    t=ctypes.create_unicode_buffer(256);u.GetWindowTextW(h,t,256)
    st=u.GetWindowLongW(h,-16)&0xffffffff; cid=u.GetWindowLongW(h,-12)
    r=w.RECT();u.GetWindowRect(h,ctypes.byref(r))
    return f"{h:#x} {c.value!r} id={cid} st={st:#010x} vis={u.IsWindowVisible(h)} {r.right-r.left}x{r.bottom-r.top} {t.value[:50]!r}"
def kids(h,d):
    res=[]
    def cb(ch,_):
        if u.GetParent(ch)==h or d==0: res.append(ch)
        return True
    u.EnumChildWindows(h,EnumProc(cb),0)
    for ch in res:
        if u.GetParent(ch)==h:
            print("  "*d+info(ch)); kids(ch,d+1)
tops=[]
def tcb(h,_):
    p=w.DWORD();u.GetWindowThreadProcessId(h,ctypes.byref(p))
    if p.value==pid: tops.append(h)
    return True
u.EnumWindows(EnumProc(tcb),0)
for h in tops:
    if not u.IsWindowVisible(h) and '--all' not in sys.argv: continue
    print(info(h)); kids(h,1)
