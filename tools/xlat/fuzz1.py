import sys,struct,subprocess,random,os,itertools
sys.path.insert(0,'.')
import xlat
from unicorn import *
from unicorn.x86_const import *
from unicorn.arm64_const import *
def asm_x86(text):
    open('t.s','w').write('.intel_syntax noprefix\n.text\n'+text+'\n')
    subprocess.run(['llvm-mc','-triple=x86_64','-filetype=obj','t.s','-o','t.o'],check=True,capture_output=True)
    subprocess.run(['llvm-objcopy','-O','binary','-j','.text','t.o','t.bin'],check=True)
    return open('t.bin','rb').read()
def asm_arm(lines):
    open('a.s','w').write('\n'.join(lines)+'\n')
    r=subprocess.run(['clang','-target','aarch64-linux-gnu','-c','a.s','-o','a.o'],capture_output=True,text=True)
    if r.returncode: raise Exception(r.stderr[:300])
    subprocess.run(['llvm-objcopy','-O','binary','-j','.text','a.o','a.bin'],check=True)
    return open('a.bin','rb').read()
PRODUCERS=['add eax, ecx','add rax, rcx','sub eax, ecx','sub rax, rcx','and eax, ecx','or rax, rcx','test rax, rax','cmp eax, ecx','cmp rax, rcx','xor eax, ecx']
Xs=['bt dword ptr [rdi+8], 5','bt dword ptr [rdi+4], 31','bt ax, 8','bt cx, 15','bt word ptr [rdi+2], 3','bt qword ptr [rdi], 63','sbb al, al','sbb dx, dx','sbb cl, cl','cmove ax, dx','cmovne cx, ax','cmovb ax, cx','cmovg dx, bx','shr al, cl','shr dx, cl','shl al, cl','sar dx, cl','sar al, cl','shl dx, cl','shl eax, 3\nsete bl','shr rax, 2\nsete bl','shl eax, 1\nsets bl','movd xmm0, ecx\nmovd eax, xmm0','movq xmm1, rcx\nmovq rax, xmm1','movsd xmm0, qword ptr [rdi]\nmovsd qword ptr [rdi+8], xmm0','movups xmm2, xmmword ptr [rdi]\npextrw eax, xmm2, 4','movups xmm2, xmmword ptr [rdi]\npsrldq xmm2, 8\nmovups xmmword ptr [rdi+16], xmm2','lock add dword ptr [rdi+8], ecx','lock sub qword ptr [rdi], rcx','lock inc dword ptr [rdi]','lock dec qword ptr [rdi+8]','lock or dword ptr [rdi], ecx','lock and dword ptr [rdi], 0xff0f','lock xor qword ptr [rdi], rcx','lock xadd dword ptr [rdi], ecx','lock xadd qword ptr [rdi+8], rcx','lock cmpxchg dword ptr [rdi], ecx','lock cmpxchg qword ptr [rdi+8], rcx','xchg dword ptr [rdi], ecx','mov ecx, 5\nrep stosd','mov ecx, 7\nrep stosb','mov ecx, 5\nrep movsq','mov ecx, 9\nrep movsb','mov ecx, 0\nrep stosq','adc eax, ecx','adc rax, rcx','sbb eax, ecx','sbb rax, rcx','sbb eax, eax','adc qword ptr [rdi+8], rcx','sbb dword ptr [rdi+4], ecx',
'mul ecx','mul rcx','imul ecx','imul rcx','xor edx, edx\ndiv ecx','cdq\nidiv ecx','xor edx, edx\ndiv rcx','cqo\nidiv rcx',
'bts eax, 5','btr rax, 40','btc eax, 31','bts eax, ecx','btr rax, rcx','btc rax, rcx','bts dword ptr [rdi+4], 3','bt eax, 7','bt rax, rcx','bt eax, ecx',
'movabs rax, 0x123456789abcdef0','rol eax, 5','ror rax, 9','rol rax, 63','bswap eax','bswap rax','cdqe','cdq','cqo',
'shl eax, 3','shr rax, 7','sar eax, 31','sar rax, cl','shl eax, cl','shr eax, cl','shl al, 2','shr ax, 3','sar al, 1',
'inc eax','dec rax','neg eax','neg rax','not rax','inc byte ptr [rdi]','dec word ptr [rdi+2]','neg dword ptr [rdi]','inc cl','dec dx',
'cmovne eax, ecx','cmovl rax, rcx','cmovb rax, rcx','cmovae eax, ecx','cmovg rax, rcx','movsx rax, cl','movsxd rax, ecx','movzx eax, cl','movzx eax, cx',
'lea eax, [rax+rcx*4+7]','lea rax, [rcx+rcx*2]','and cl, 0x7f','or dl, 1','add al, cl','sub cx, dx','cmp al, 5','test cl, cl','add byte ptr [rdi], cl','sub word ptr [rdi+2], 7','xor byte ptr [rdi+1], 0xff']
CONS='mov r12d,0\nmov r13d,0\nmov r14d,0\nmov r15d,0\nmov r10d,0\nmov r11d,0\n'
# NOTE flags consumers inserted after X
CONS_AFTER='sete r12b\nsetb r13b\nsetl r14b\nsetle r15b\nsets r10b\nseta r11b\nret\n'
MAP=[(UC_X86_REG_RAX,UC_ARM64_REG_X0),(UC_X86_REG_RCX,UC_ARM64_REG_X1),(UC_X86_REG_RDX,UC_ARM64_REG_X2),(UC_X86_REG_RBX,UC_ARM64_REG_X3),(UC_X86_REG_RSP,UC_ARM64_REG_X4),(UC_X86_REG_RBP,UC_ARM64_REG_X5),(UC_X86_REG_RSI,UC_ARM64_REG_X6),(UC_X86_REG_RDI,UC_ARM64_REG_X7)]
for i in range(8,16): MAP.append((getattr(sys.modules['unicorn.x86_const'],'UC_X86_REG_R%d'%i),getattr(sys.modules['unicorn.arm64_const'],'UC_ARM64_REG_X%d'%i)))
EDGE=[0,1,2,3,0x7f,0x80,0xff,0x7fff,0x8000,0xffff,0x7fffffff,0x80000000,0xffffffff,0x100000000,0x7fffffffffffffff,0x8000000000000000,0xffffffffffffffff,0x123456789abcdef0]
DATA=0x20000000; STK=0x30000000; RET=0x7ff00000; X86C=0x10000000; ARMC=0x40000000
stats={'pass':0,'fail':0,'unsup':0}; unsup={}; fails=[]
rng=random.Random(1)
for X in (sys.argv[1:] and [x.replace('\\n','\n') for x in sys.argv[1:]] or Xs):
    for P in (PRODUCERS if True else []):
        # skip pairs that obviously can't matter? run all
        okk=False
        for cs in ('sete r12b\nsetb r13b\nsetl r14b\nsetle r15b\nsets r10b\nseta r11b\n','sete r12b\nsetl r14b\nsetle r15b\nsets r10b\n','setb r13b\n',''):
            text=CONS+P+'\n'+X+'\n'+cs+'ret\n'
            try:
                xc=asm_x86(text); lines=xlat.translate_code(xc,0x1000); okk=True; break
            except xlat.Unsup as e: last=e
            except Exception as e: last=e
        if not okk:
            e=last
            stats['unsup']+=1; k=X.split('\n')[-1].split(',')[0]+' | '+str(e)[:30]; unsup[k]=unsup.get(k,0)+1; continue
        try:
            pass
        except xlat.Unsup as e:
            stats['unsup']+=1; k=X.split('\n')[-1].split(',')[0]+' | '+str(e)[:30]; unsup[k]=unsup.get(k,0)+1; continue
        except Exception as e:
            stats['unsup']+=1; unsup['ERR '+X.split()[0]+str(e)[:40]]=unsup.get('ERR '+X.split()[0],0)+1; continue
        ac=asm_arm(lines)
        for trial in range(25):
            regs={}
            for xr,_ in MAP: regs[xr]=rng.choice(EDGE) if rng.random()<0.8 else rng.getrandbits(64)
            regs[UC_X86_REG_RDI]=DATA+0x100; regs[UC_X86_REG_RSI]=DATA+0x400; regs[UC_X86_REG_RSP]=STK-8
            if rng.random()<0.5: regs[UC_X86_REG_RCX]|=1
            mem=bytes(rng.getrandbits(8) for _ in range(0x1000))
            sp=STK
            u=Uc(UC_ARCH_X86,UC_MODE_64); u.mem_map(X86C,0x1000); u.mem_write(X86C,xc); u.mem_map(DATA,0x1000); u.mem_write(DATA,mem)
            u.mem_map(STK-0x1000,0x2000); u.mem_map(RET&~0xfff,0x1000)
            for xr,_ in MAP: u.reg_write(xr,regs[xr])
            u.reg_write(UC_X86_REG_RSP,sp-8); u.mem_write(sp-8,struct.pack('<Q',RET))
            try: u.emu_start(X86C,RET,count=200)
            except UcError: continue   # e.g. divide error: skip
            if u.reg_read(UC_X86_REG_RIP)!=RET: continue
            a=Uc(UC_ARCH_ARM64,UC_MODE_ARM); a.mem_map(ARMC,0x4000); a.mem_write(ARMC,ac); a.mem_map(DATA,0x1000); a.mem_write(DATA,mem)
            a.mem_map(STK-0x1000,0x2000); a.mem_map(RET&~0xfff,0x1000); a.mem_write(RET,b'\x1f\x20\x03\xd5')
            for xr,ar in MAP: a.reg_write(ar,regs[xr])
            a.mem_write(sp-8,struct.pack('<Q',RET))
            try: a.emu_start(ARMC,RET,count=2000)
            except UcError as ex: stats['fail']+=1; fails.append((P,X,'armerr',str(ex))); break
            diff=[(xr,hex(u.reg_read(xr)),hex(a.reg_read(ar))) for xr,ar in MAP if u.reg_read(xr)!=a.reg_read(ar)]
            if bytes(u.mem_read(DATA,0x1000))!=bytes(a.mem_read(DATA,0x1000)): diff.append('mem')
            if diff: stats['fail']+=1; fails.append((P,X.replace('\n','; '),diff[:3],{hex(k):hex(v) for k,v in list(regs.items())[:3]})); break
            else: stats['pass']+=1
print(stats); print('unsupported:',sorted(unsup.items(),key=lambda x:-x[1])[:25])
seen=set()
for f in fails:
    k=(f[0],f[1])
    if k in seen: continue
    seen.add(k)
    if len(seen)<=14: print(f)
print('distinct failing (producer,instr) pairs:',len(seen), 'distinct instrs:',len({f[1] for f in fails}))
