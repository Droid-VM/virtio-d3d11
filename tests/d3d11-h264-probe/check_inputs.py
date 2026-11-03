"""Offline strict fixture check; no compiler, guest or driver required."""
import base64, hashlib, json
from pathlib import Path
ROOT = Path(__file__).resolve().parent
class Bits:
    def __init__(self, nal):
        self.d = nal[1:].replace(b'\0\0\3', b'\0\0'); self.p = 0
    def u(self, n):
        assert self.p+n <= len(self.d)*8
        v=0
        for _ in range(n):
            v=(v<<1)|((self.d[self.p//8]>>(7-self.p%8))&1); self.p+=1
        return v
    def ue(self):
        z=0
        while not self.u(1):
            z+=1; assert z<=30
        return (1<<z)-1+self.u(z)
    def se(self):
        v=self.ue(); return (v+1)//2 if v&1 else -v//2
j=json.loads((ROOT/'fixture.json').read_text())
for name in ('red','blue'):
    a=(ROOT/(name+'.h264')).read_bytes()
    assert a==base64.b64decode(j[name])
    import re
    n=[x for x in re.split(b'\x00\x00\x00?\x01',a) if x]
    assert [x[0]&31 for x in n]==[7,8,5]
    s=Bits(n[0]); assert [s.u(8),s.u(8),s.u(8),s.ue()]==[66,192,10,0]
    logfn=s.ue(); poc=s.ue(); assert poc==2
    refs=s.ue(); gaps=s.u(1); w=s.ue(); h=s.ue(); progressive=s.u(1); direct=s.u(1); crop=s.u(1)
    assert (w,h,progressive,crop,gaps)==(5,5,1,0,0)
    p=Bits(n[1]); assert [p.ue(),p.ue(),p.u(1),p.u(1),p.ue(),p.ue(),p.ue(),p.u(1),p.u(2)]==[0]*9
    qp,qs,chroma=p.se(),p.se(),p.se(); deblock,constrained,redundant=p.u(1),p.u(1),p.u(1)
    assert p.u(1)==1
    while p.p<len(p.d)*8: assert p.u(1)==0
    v=Bits(n[2]); first,typ,pps=v.ue(),v.ue(),v.ue(); frame=v.u(logfn+4); idr=v.ue()
    no_output,longterm=v.u(1),v.u(1); delta=v.se(); disable=v.ue()
    alpha,beta=(v.se(),v.se()) if disable!=1 else (0,0)
    assert (first,typ,pps,frame,no_output,longterm)==(0,7,0,0,0,0)
    print(name, 'sha256='+hashlib.sha256(a).hexdigest(), 'bytes='+str(len(a)),
          dict(logfn=logfn,poc=poc,refs=refs,direct=direct,qp=qp,qs=qs,chroma=chroma,
               deblock=deblock,constrained=constrained,redundant=redundant,idr=idr,
               delta=delta,disable=disable,alpha=alpha,beta=beta))
source=(ROOT/'probe.cpp').read_text()
for label,nal_index in [('expected_sps',0),('expected_pps',1),('red_slice',2)]:
    payload=re.search(r'\b'+label+r'=\{([^}]+)\}',source).group(1)
    embedded=bytes(int(v.strip(),0) for v in payload.split(','))
    red_nals=[x for x in re.split(b'\x00\x00\x00?\x01',(ROOT/'red.h264').read_bytes()) if x]
    assert embedded==red_nals[nal_index], label
    if label=='red_slice':
        blue=bytearray(embedded)
        for i,v in re.findall(r'blue_slice\[(\d+)\]=(0x[0-9a-f]+)',source): blue[int(i)]=int(v,16)
        assert blue==n[2]
assert not re.search(r'DeviceIoControl|vm_flush\s*\(|vm_send\s*\(',source)
print('PASS fixed SPS/PPS/slice contract and embedded bytes; this does not compile or run D3D11')
