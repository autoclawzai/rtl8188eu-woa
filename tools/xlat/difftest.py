import sys; sys.path.insert(0,".")
import random,struct,subprocess
sys.argv=['x']; import sys
import xlat
from unicorn import *
from unicorn.x86_const import *
from unicorn.arm64_const import *
IB=xlat.IB; d=xlat.d
subprocess.run(['llvm-objcopy','-O','binary','-j','.text','poc.o','poc.bin'],check=True)
code=open('poc.bin','rb').read()
from capstone.x86 import X86_OP_IMM
syms={}
for l in subprocess.check_output(['llvm-nm','poc.o']).decode().split('\n'):
    p=l.split()
    if len(p)==3 and p[2].startswith('F'): syms[int(p[2][1:],16)]=int(p[0],16)
# funcs list that translated and have no call (pure leaf)
funcs=[tuple(int(x,16) for x in l.split()) for l in open('poc_funcs.txt').read().split('\n') if l]
pure=[]
deps={}
fe=dict(funcs)
for s,e in funcs:
    ins=list(xlat.md.disasm(bytes(d[s:e]),IB+s)); tg=set(); good=True; locs={x.address for x in ins}
    for i in ins:
        if i.mnemonic in('call','jmp'):
            o=i.operands[0]
            if o.type!=X86_OP_IMM: good=False; continue
            if i.mnemonic=='jmp' and o.imm in locs: continue
            tg.add(o.imm-IB)
    deps[s]=(good,tg)
okset={s for s,(g,t) in deps.items() if g}
ch=True
while ch:
    ch=False
    for s in list(okset):
        if any(t not in okset for t in deps[s][1]): okset.discard(s); ch=True
pure=[(s,e) for s,e in funcs if s in okset]
print('closed translated funcs',len(pure),flush=True)
import os
if os.environ.get('ONLY'):
    only={int(x,16) for x in os.environ['ONLY'].split(',')}; pure=[p for p in pure if p[0] in only]
else:
    random.Random(7).shuffle(pure); pure=pure[:250]
MAP=[(UC_X86_REG_RAX,UC_ARM64_REG_X0),(UC_X86_REG_RCX,UC_ARM64_REG_X1),(UC_X86_REG_RDX,UC_ARM64_REG_X2),(UC_X86_REG_RBX,UC_ARM64_REG_X3),(UC_X86_REG_RSP,UC_ARM64_REG_X4),(UC_X86_REG_RBP,UC_ARM64_REG_X5),(UC_X86_REG_RSI,UC_ARM64_REG_X6),(UC_X86_REG_RDI,UC_ARM64_REG_X7)]
for i in range(8,16): MAP.append((getattr(sys.modules['unicorn.x86_const'],'UC_X86_REG_R%d'%i),getattr(sys.modules['unicorn.arm64_const'],'UC_ARM64_REG_X%d'%i)))
DATA=0x20000000; DS=0x10000; STK=0x30000000; RET=0x7ff00000; ARMC=0x40000000
img=bytes(d[:xlat.pe.OPTIONAL_HEADER.SizeOfImage])
ok=bad=skip=0; fails=[]
for s,e in pure:
    if s not in syms: continue
    for trial in range(int(os.environ.get('TR','6'))):
        rnd=random.Random(s*100+trial)
        mem=bytes(rnd.getrandbits(8) for _ in range(DS))
        regs={}
        for xr,_ in MAP:
            r=rnd.random()
            regs[xr]=(DATA+rnd.randrange(0x400,DS-0x400)) if r<0.75 else (rnd.getrandbits(64) if r<0.8 else rnd.randrange(0,300))
        # x86
        u=Uc(UC_ARCH_X86,UC_MODE_64)
        try:
            u.mem_map(IB&~0xfff, (len(img)+0xfff)&~0xfff); u.mem_write(IB,img)
        except Exception as ex: skip+=1; break
        u.mem_map(DATA,DS); u.mem_write(DATA,mem); u.mem_map(STK-0x10000,0x20000); u.mem_map(RET&~0xfff,0x1000)
        for xr,_ in MAP: u.reg_write(xr,regs[xr])
        sp=STK; u.reg_write(UC_X86_REG_RSP,sp-8); u.mem_write(sp-8,struct.pack('<Q',RET)); regs[UC_X86_REG_RSP]=sp-8
        try: u.emu_start(IB+s,RET,count=5000)
        except UcError: skip+=1; continue
        if u.reg_read(UC_X86_REG_RIP)!=RET: skip+=1; continue
        # arm
        a=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        a.mem_map(ARMC,(len(code)+0xfff)&~0xfff); a.mem_write(ARMC,code)
        a.mem_map(IB&~0xfff,(len(img)+0xfff)&~0xfff); a.mem_write(IB,img)
        a.mem_map(DATA,DS); a.mem_write(DATA,mem); a.mem_map(STK-0x10000,0x20000); a.mem_map(RET&~0xfff,0x1000)
        a.mem_write(RET,b'\x1f\x20\x03\xd5')
        for xr,ar in MAP: a.reg_write(ar,regs[xr])
        a.mem_write(sp-8,struct.pack('<Q',RET))
        try: a.emu_start(ARMC+syms[s],RET,count=20000)
        except UcError as ex: bad+=1; fails.append((hex(s),'arm err',str(ex))); continue
        diff=[]
        for xr,ar in MAP:
            if u.reg_read(xr)!=a.reg_read(ar): diff.append((xr,hex(u.reg_read(xr)),hex(a.reg_read(ar))))
        if bytes(u.mem_read(DATA,DS))!=bytes(a.mem_read(DATA,DS)): diff.append('mem')
        if diff: bad+=1; fails.append((hex(s),diff[:3]))
        else: ok+=1
print(flush=True)
print('PASS',ok,'FAIL',bad,'skipped(x86 fault/loop)',skip)
for f in fails[:8]: print(f)
