# capture a window via PrintWindow: shot.py <hwnd hex|pid> out.png
import ctypes, ctypes.wintypes as w, sys
from PIL import Image
u=ctypes.windll.user32; g=ctypes.windll.gdi32
a=sys.argv[1]
if a.startswith('0x'): h=int(a,16)
else:
    pid=int(a); found=[]
    P=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    def cb(hh,_):
        p=w.DWORD();u.GetWindowThreadProcessId(hh,ctypes.byref(p))
        if p.value==pid and u.IsWindowVisible(hh) and u.GetWindow(hh,4)==0: found.append(hh)
        return True
    u.EnumWindows(P(cb),0); h=found[0]
r=w.RECT();u.GetWindowRect(h,ctypes.byref(r));W,H=r.right-r.left,r.bottom-r.top
hdc=u.GetWindowDC(h);m=g.CreateCompatibleDC(hdc);b=g.CreateCompatibleBitmap(hdc,W,H);g.SelectObject(m,b)
u.PrintWindow(h,m,2)
class BI(ctypes.Structure):_fields_=[('s',w.DWORD),('w',w.LONG),('h',w.LONG),('p',w.WORD),('b',w.WORD),('c',w.DWORD),('x',w.DWORD*5)]
bi=BI(40,W,-H,1,32,0);buf=ctypes.create_string_buffer(W*H*4)
g.GetDIBits(m,b,0,H,buf,ctypes.byref(bi),0)
Image.frombuffer('RGB',(W,H),buf,'raw','BGRX',0,1).save(sys.argv[2])
print(hex(h),W,H)
