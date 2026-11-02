# open Metadata options, select a provider (index), click "Show options"; prints new window handle
import ctypes, ctypes.wintypes as w, sys, time
u=ctypes.windll.user32
idx=int(sys.argv[1]) if len(sys.argv)>1 else 1
main=u.FindWindowW("erstes",None)
u.PostMessageW(main,0x111,543,0); time.sleep(2)
dlg=u.FindWindowW("#32770","Metadata options")
P=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM); found={}
def cb(h,_):
    c=ctypes.create_unicode_buffer(64);u.GetClassNameW(h,c,64)
    t=ctypes.create_unicode_buffer(128);u.GetWindowTextW(h,t,128)
    if c.value=='ComboBox' and u.IsWindowVisible(h): found['cb']=h
    if t.value.startswith('Show options'): found['btn']=h
    return True
u.EnumChildWindows(dlg,P(cb),0)
cbh=found['cb']; u.SendMessageW(cbh,0x14E,idx,0)
u.SendMessageW(u.GetParent(cbh),0x111,(1<<16)|u.GetDlgCtrlID(cbh),cbh)
u.PostMessageW(found['btn'],0xF5,0,0); time.sleep(3)
print(hex(dlg))
