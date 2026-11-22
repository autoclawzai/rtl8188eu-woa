import pefile,struct,collections,json
from capstone import *
pe=pefile.PE('rtwlanu.sys'); d=pe.get_memory_mapped_image()
imgb=pe.OPTIONAL_HEADER.ImageBase
pd=[s for s in pe.sections if s.Name.startswith(b'.pdata')][0]
b=pd.get_data(); n=pd.Misc_VirtualSize//12
funcs=[struct.unpack_from('<III',b,i*12) for i in range(n)]
print('functions(pdata)',len(funcs),flush=True)
# SEH handlers: unwind info flags
seh=0
for s,e,u in funcs:
    fl=d[u]>>3
    if fl&3: seh+=1
print('funcs with EH/UHANDLER',seh,flush=True)
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
mn=collections.Counter(); tot=0; undec=0
cat=collections.Counter()
for s,e,u in funcs:
    code=bytes(d[s:e]); off=0
    while off<len(code):
        got=False
        for i in md.disasm(code[off:],s+off):
            got=True; tot+=1; m=i.mnemonic; mn[m]+=1
            pf=i.bytes[0]==0xf0
            if m.startswith('lock') or pf: cat['lock']+=1
            if m in('xchg','cmpxchg','xadd') : cat['atomic_rmw']+=1
            if m in('mfence','lfence','sfence','pause'): cat['fence']+=1
            if m=='call' and i.op_str.startswith(('r','qword ptr [r')) : cat['call_indirect']+=1
            if m=='jmp' and ('[' in i.op_str or i.op_str.startswith('r')): cat['jmp_indirect']+=1
            if m in('rdmsr','wrmsr','cli','sti','hlt','in','out','rdtsc','cpuid','lgdt','invlpg','swapgs','int','syscall'): cat['priv_'+m]+=1
            if 'cr' in i.op_str and m=='mov' and any(c in i.op_str for c in ('cr0','cr2','cr3','cr4','cr8')): cat['mov_cr']+=1
            if 'gs:' in i.op_str: cat['gs_access']+=1
            if m.startswith(('v',)) : cat['avx']+=1
            if m.startswith(('movaps','movups','movdq','pxor','paddd','pshuf','xmm')) or 'xmm' in i.op_str: cat['sse']+=1
            if m.startswith('rep') or m.startswith(('movs','stos')) : cat['string']+=1
            off=i.address-s+i.size
        if not got or off<len(code):
            if off<len(code): undec+=1; off+=1
        if got and off>=len(code): break
print('instrs',tot,'undecodable bytes',undec,flush=True)
json.dump({'tot':tot,'undec':undec,'cat':cat,'top':mn.most_common(400),'nfunc':len(funcs),'seh':seh},open('mix.json','w'))
print(dict(cat)); print(mn.most_common(40))
