import pefile,struct,sys,subprocess,random,os
from capstone import *
from capstone.x86 import *
pe=pefile.PE('rtwlanu.sys'); d=pe.get_memory_mapped_image(); IB=pe.OPTIONAL_HEADER.ImageBase
pd=[s for s in pe.sections if s.Name.startswith(b'.pdata')][0]; b=pd.get_data()
funcs=[struct.unpack_from('<III',b,i*12) for i in range(pd.Misc_VirtualSize//12)]
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
R64={'rax':0,'rcx':1,'rdx':2,'rbx':3,'rsp':4,'rbp':5,'rsi':6,'rdi':7,'r8':8,'r9':9,'r10':10,'r11':11,'r12':12,'r13':13,'r14':14,'r15':15}
R32={'esp':'rsp','eax':'rax','ecx':'rcx','edx':'rdx','ebx':'rbx','ebp':'rbp','esi':'rsi','edi':'rdi'}
for i in range(8,16): R32['r%dd'%i]='r%d'%i
IAT={}
for _d in pe.DIRECTORY_ENTRY_IMPORT:
    for _i in _d.imports: IAT[_i.address]=(_d.dll.decode().split('.')[0].lower()+'_'+(_i.name.decode() if _i.name else 'ord%d'%_i.ordinal))
_t=[x for x in pe.sections if x.Name.startswith(b'.text')][0]
class _R:
    def __contains__(self,v): return IB+_t.VirtualAddress<=v<IB+_t.VirtualAddress+_t.Misc_VirtualSize
FSTART=_R()
STATS={'call_int':0,'call_imp':0,'call_ind':0,'tail':0}
class Unsup(Exception): pass
CUR=[None]
R8={'al':'rax','cl':'rcx','dl':'rdx','bl':'rbx','spl':'rsp','bpl':'rbp','sil':'rsi','dil':'rdi'}
for _i in range(8,16): R8['r%db'%_i]='r%d'%_i
R16={'ax':'rax','cx':'rcx','dx':'rdx','bx':'rbx','sp':'rsp','bp':'rbp','si':'rsi','di':'rdi'}
for _i in range(8,16): R16['r%dw'%_i]='r%d'%_i
def full(name):
    for t in(R64,):
        if name in t: return name
    for t in(R32,R8,R16):
        if name in t: return t[name]
    raise Unsup('reg '+name)
def reg(name,size):
    if size==8 and name in R64: return 'x%d'%R64[name]
    if size==4 and name in R32: return 'w%d'%R64[R32[name]]
    raise Unsup('reg '+name)
SUBC={'e':'eq','ne':'ne','b':'lo','ae':'hs','be':'ls','a':'hi','l':'lt','ge':'ge','le':'le','g':'gt','s':'mi','ns':'pl'}
def cond(cc,kind):
    if kind in('sub',):
        if cc in SUBC: return SUBC[cc]
    if kind=='add':
        if cc in('e','ne','s','ns','l','ge','le','g'): return SUBC[cc]
        if cc=='b': return 'hs'
        if cc=='ae': return 'lo'
    if kind=='logic':
        if cc in('e','ne','s','ns','l','ge','le','g'): return SUBC[cc]
        if cc=='be': return 'eq'
        if cc=='a': return 'ne'
        if cc=='ae': return 'al'
    if kind=='bt':
        if cc=='b': return 'hs'
        if cc=='ae': return 'lo'
    if kind=='incdec':
        if cc in('e','ne','s','ns','l','ge','le','g'): return SUBC[cc]
    raise Unsup('cond %s after %s'%(cc,kind))
def movimm(r,v,sz):
    v&=(1<<64)-1
    if sz==4: v&=0xffffffff
    out=['mov %s, #%d'%(r,0)]
    out=[]; first=True
    for sh in range(0,64,16):
        part=(v>>sh)&0xffff
        if part or (sh==0 and v==0):
            out.append(('movz %s, #%d, lsl #%d' if first else 'movk %s, #%d, lsl #%d')%(r,part,sh)); first=False
    return out
def addr(op):
    m=op.mem
    if m.segment!=0: raise Unsup('mem seg')
    out=[]
    if m.base==X86_REG_RIP:
        return movimm('x16',CUR[0].address+CUR[0].size+m.disp,8)
    out+=movimm('x16',m.disp,8)
    if m.base: out.append('add x16, x16, %s'%reg(md.reg_name(m.base),8))
    if m.index:
        sc={1:0,2:1,4:2,8:3}[m.scale]
        out.append('add x16, x16, %s, lsl #%d'%(reg(md.reg_name(m.index),8),sc))
    return out
def rd(op,sz,tmp):
    """read operand; for sz 1/2 returns value left-aligned in a w-reg (bits 31..), sz 4/8 native"""
    n=tmp[1:]; out=[]
    if sz in(4,8):
        if op.type==X86_OP_REG: return [],reg(md.reg_name(op.reg),sz)
        if op.type==X86_OP_IMM: return movimm(tmp if sz==8 else 'w'+n,op.imm,sz),(tmp if sz==8 else 'w'+n)
        out+=addr(op); out.append('ldr %s%s, [x16]'%('x' if sz==8 else 'w',n)); return out,('x' if sz==8 else 'w')+n
    sh=32-8*sz
    if op.type==X86_OP_IMM: return movimm('w'+n,(op.imm&((1<<(8*sz))-1))<<sh,4),'w'+n
    if op.type==X86_OP_REG:
        r=R64[full(md.reg_name(op.reg))]; out.append('ubfiz w%s, w%d, #%d, #%d'%(n,r,sh,8*sz)); return out,'w'+n
    out+=addr(op); out.append(('ldrb' if sz==1 else 'ldrh')+' w%s, [x16]'%n); out.append('lsl w%s, w%s, #%d'%(n,n,sh)); return out,'w'+n
def wr(op,sz,valreg):
    out=[]
    if sz in(4,8):
        if op.type==X86_OP_REG: out.append('mov %s, %s'%(reg(md.reg_name(op.reg),sz),valreg))
        else: out+=addr(op); out.append('str %s, [x16]'%valreg)
        return out
    sh=32-8*sz; out.append('lsr %s, %s, #%d'%(valreg,valreg,sh))
    if op.type==X86_OP_REG: out.append('bfi x%d, x%s, #0, #%d'%(R64[full(md.reg_name(op.reg))],valreg[1:],8*sz))
    else: out+=addr(op); out.append(('strb' if sz==1 else 'strh')+' %s, [x16]'%valreg)
    return out
def src(op,sz,tmp='x17'):
    # returns (pre-instrs, regname)
    t=tmp if sz==8 else 'w'+tmp[1:]
    if op.type==X86_OP_REG: return [],reg(md.reg_name(op.reg),sz)
    if op.type==X86_OP_IMM:
        v=op.imm
        return movimm(tmp,v,sz) if True else [],t
    if op.type==X86_OP_MEM:
        pre=addr(op); sfx={8:'x',4:'w'}[sz]
        pre.append('ldr %s%s, [x16]'%(sfx,tmp[1:])); return pre,t
    raise Unsup('op')
def translate(fs,fe):
    code=bytes(d[fs:fe]); ins=list(md.disasm(code,IB+fs))
    if sum(i.size for i in ins)!=len(code): raise Unsup('decode')
    addrs={i.address for i in ins}
    out=[]; kind=None
    for i in ins:
        CUR[0]=i
        out.append('L%x:'%i.address)
        m=i.mnemonic; ops=i.operands
        if m.startswith('rep') or i.prefix[0] in(0xf0,): raise Unsup('prefix')
        if m=='nop' or m.startswith('nop') or m=='int3': continue
        if m=='ret' and not ops: out+=['ldr x16, [x4], #8','br x16']; continue
        if m=='push' and ops[0].type==X86_OP_REG and ops[0].size==8: out.append('str %s, [x4, #-8]!'%reg(md.reg_name(ops[0].reg),8)); continue
        if m=='pop' and ops[0].type==X86_OP_REG and ops[0].size==8: out.append('ldr %s, [x4], #8'%reg(md.reg_name(ops[0].reg),8)); continue
        if m=='call':
            o=ops[0]; rl='R%x'%i.address
            push=['adr x16, %s'%rl,'str x16, [x4, #-8]!']
            if o.type==X86_OP_IMM and o.imm in FSTART: out+=push+['b F%x'%(o.imm-IB)]; STATS['call_int']+=1
            elif o.type==X86_OP_MEM and o.mem.base==X86_REG_RIP and (i.address+i.size+o.mem.disp) in IAT:
                out+=push+['b imp_%s'%IAT[i.address+i.size+o.mem.disp]]; STATS['call_imp']+=1
            elif o.type in(X86_OP_REG,X86_OP_MEM):
                p,v=rd(o,8,'x17'); out+=p+['mov x17, %s'%v]+push+['b __indirect_x17']; STATS['call_ind']+=1
            else: raise Unsup('call target')
            out.append(rl+':'); continue
        if m=='jmp' and ops[0].type==X86_OP_IMM and ops[0].imm in FSTART and ops[0].imm not in addrs:
            out.append('b F%x'%(ops[0].imm-IB)); STATS['tail']+=1; continue
        if m=='jmp' or m[0]=='j':
            if ops[0].type!=X86_OP_IMM or ops[0].imm not in addrs: raise Unsup('branch target')
            tgt='L%x'%ops[0].imm
            if m=='jmp': out.append('b '+tgt)
            else:
                c=cond(m[1:],kind)
                if c=='al': out.append('b '+tgt)
                else: out.append('b.%s %s'%(c,tgt))
            continue
        sz=ops[0].size if ops else 0
        if m=='mov':
            if sz not in(1,2,4,8): raise Unsup('size')
            if sz in(4,8) and ops[0].type==X86_OP_REG and ops[1].type==X86_OP_IMM:
                out+=movimm(reg(md.reg_name(ops[0].reg),sz),ops[1].imm,sz)
            else:
                p,v=rd(ops[1],sz,'x17'); out+=p; out+=wr(ops[0],sz,v if ops[1].type!=X86_OP_REG or sz in(1,2) else v)
        elif m=='lea':
            d0=reg(md.reg_name(ops[0].reg),sz); out+=addr(ops[1]); out.append('mov %s, %s'%(d0,'x16' if sz==8 else 'w16'))
        elif m=='movzx':
            if ops[0].size not in(2,4,8): raise Unsup('movzx dst')
            d0=reg(md.reg_name(ops[0].reg),ops[0].size) if ops[0].size!=2 else None
            if d0 is None: raise Unsup('movzx dst16')
            ss=ops[1].size
            if ops[1].type==X86_OP_MEM: out+=addr(ops[1]); out.append(('ldrb' if ss==1 else 'ldrh')+' %s, [x16]'%('w'+d0[1:]))
            else:
                sr=R64[full(md.reg_name(ops[1].reg))]; out.append('ubfx w%s, w%d, #0, #%d'%(d0[1:],sr,8*ss))
        elif m in('add','sub','and','or','xor','cmp','test'):
            if ops[0].size!=ops[1].size and ops[1].type!=X86_OP_IMM: raise Unsup('opsize')
            if m=='xor' and ops[0].type==X86_OP_REG and ops[1].type==X86_OP_REG and ops[1].reg==ops[0].reg and sz in(4,8):
                d0=reg(md.reg_name(ops[0].reg),sz); out+=['mov %s, #0'%d0,'tst %s, %s'%(d0,d0)]; kind='logic'; continue
            if sz not in(1,2,4,8): raise Unsup('size')
            wk=m not in('cmp','test')
            pa,a_=rd(ops[0],sz,'x17'); pb,b_=rd(ops[1],sz,'x18'); out+=pa+pb
            res=a_
            if sz in(1,2) or ops[0].type!=X86_OP_REG: res=('x17' if sz==8 else 'w17')
            ins={'add':'adds','sub':'subs','and':'ands','xor':'eor','or':'orr','cmp':'cmp','test':'tst'}[m]
            if m in('cmp','test'): out.append('%s %s, %s'%(ins,a_,b_))
            else: out.append('%s %s, %s, %s'%(ins,res,a_,b_))
            if m in('or','xor'): out.append('tst %s, %s'%(res,res))
            kind={'add':'add','sub':'sub','cmp':'sub'}.get(m,'logic')
            if wk and (res!=a_ or sz in(1,2)): out+=wr(ops[0],sz,res)
            elif wk and ops[0].type==X86_OP_MEM: out+=wr(ops[0],sz,res)
        elif m in('movsx','movsxd') and ops[0].type==X86_OP_REG:
            d0=reg(md.reg_name(ops[0].reg),ops[0].size); ss=ops[1].size
            if ops[1].type==X86_OP_MEM:
                out+=addr(ops[1]); out.append({1:'ldrsb',2:'ldrsh',4:'ldrsw'}[ss]+' %s, [x16]'%d0)
            else:
                sr=R64[full(md.reg_name(ops[1].reg))]; out.append({1:'sxtb',2:'sxth',4:'sxtw'}[ss]+' %s, w%d'%(d0,sr))
        elif m=='neg' and ops[0].type==X86_OP_REG and sz in(4,8):
            d0=reg(md.reg_name(ops[0].reg),sz); out.append('negs %s, %s'%(d0,d0)); kind='sub'
        elif m=='imul' and ops[0].type==X86_OP_REG and sz in(4,8):
            d0=reg(md.reg_name(ops[0].reg),sz)
            if len(ops)==2: p,sx=rd(ops[1],sz,'x17'); out+=p; out.append('mul %s, %s, %s'%(d0,d0,sx))
            else:
                p,sx=rd(ops[1],sz,'x17'); q,sy=rd(ops[2],sz,'x18'); out+=p+q; out.append('mul %s, %s, %s'%(d0,sx,sy))
            kind=None
        elif m.startswith('cmov') and ops[0].type==X86_OP_REG and sz in(4,8):
            d0=reg(md.reg_name(ops[0].reg),sz); p,v=rd(ops[1],sz,'x17'); out+=p; c=cond(m[4:],kind)
            out.append('csel %s, %s, %s, %s'%(d0,v,d0,c if c!='al' else 'al'))
        elif m.startswith('set') and ops[0].size==1:
            c=cond(m[3:],kind); out.append('cset w17, %s'%c); out+=wr(ops[0],1,'w17') if False else []
            if ops[0].type==X86_OP_REG: out.append('bfi x%d, x17, #0, #8'%R64[full(md.reg_name(ops[0].reg))])
            else: out+=addr(ops[0])+['strb w17, [x16]']
        elif m=='lfence': out.append('dsb ld')
        elif m in('inc','dec','neg','not') and sz in(1,2,4,8):
            pa,a_=rd(ops[0],sz,'x17'); out+=pa
            res=('x17' if sz==8 else 'w17') if (sz in(1,2) or ops[0].type!=X86_OP_REG) else a_
            if m=='not':
                out.append('mvn %s, %s'%(res,a_))
            elif m=='neg': out.append('negs %s, %s'%(res,a_)); kind='sub'
            else:
                if sz in(4,8): one='#1'
                else: out+=movimm('w18',1<<(32-8*sz),4); one='w18'
                out.append(('adds %s, %s, %s' if m=='inc' else 'subs %s, %s, %s')%(res,a_,one)); kind='incdec'
            if res!=a_ or sz in(1,2) or ops[0].type==X86_OP_MEM: out+=wr(ops[0],sz,res)
        elif m in('shl','shr','sar','sal') and len(ops)==2 and sz in(1,2,4,8) and (ops[1].type==X86_OP_IMM or (ops[1].type==X86_OP_REG and md.reg_name(ops[1].reg)=='cl' and sz in(4,8))):
            pa,a_=rd(ops[0],sz,'x17'); out+=pa
            res=('x17' if sz==8 else 'w17') if (sz in(1,2) or ops[0].type!=X86_OP_REG) else a_
            ins_={'shl':'lsl','sal':'lsl','shr':'lsr','sar':'asr'}[m]
            if ops[1].type==X86_OP_IMM:
                n=ops[1].imm&(63 if sz==8 else 31)
                if sz in(1,2) and n>=8*sz: raise Unsup('shift big')
                if n==0: raise Unsup('shift0')
                if sz in(1,2) and m in('shr','sar'):
                    # operand is left-aligned; logical/arith shift right by n works directly
                    pass
                out.append('%s %s, %s, #%d'%(ins_,res,a_,n))
            else: out.append('%s %s, %s, %s'%(ins_,res,a_,'x1' if sz==8 else 'w1'))
            kind=None
            if res!=a_ or sz in(1,2) or ops[0].type==X86_OP_MEM: out+=wr(ops[0],sz,res)
        elif m=='bt' and sz in(4,8) and ops[0].type!=X86_OP_MEM:
            pa,a_=rd(ops[0],sz,'x17'); out+=pa
            if ops[1].type==X86_OP_IMM: out.append('ubfx %s, %s, #%d, #1'%('x17' if sz==8 else 'w17',a_,ops[1].imm&(63 if sz==8 else 31)))
            else:
                n=reg(md.reg_name(ops[1].reg),sz); out+=['lsr %s, %s, %s'%('x17' if sz==8 else 'w17',a_,n),'and %s, %s, #1'%('x17' if sz==8 else 'w17','x17' if sz==8 else 'w17')]
            out.append('cmp w17, #1'); kind='bt'
        elif m in('movups','movaps','movdqu','movdqa') and ops[0].size==16:
            if ops[0].type==X86_OP_REG and ops[1].type==X86_OP_MEM: out+=addr(ops[1])+['ldr q%d, [x16]'%(ops[0].reg-X86_REG_XMM0)]
            elif ops[0].type==X86_OP_MEM and ops[1].type==X86_OP_REG: out+=addr(ops[0])+['str q%d, [x16]'%(ops[1].reg-X86_REG_XMM0)]
            elif ops[0].type==X86_OP_REG and ops[1].type==X86_OP_REG: out.append('mov v%d.16b, v%d.16b'%(ops[0].reg-X86_REG_XMM0,ops[1].reg-X86_REG_XMM0))
            else: raise Unsup('sse mov')
        elif m in('xorps','pxor') and ops[0].type==X86_OP_REG and ops[1].type==X86_OP_REG:
            a0=ops[0].reg-X86_REG_XMM0; a1=ops[1].reg-X86_REG_XMM0
            out.append('eor v%d.16b, v%d.16b, v%d.16b'%(a0,a0,a1))
        elif False:
            d0=reg(md.reg_name(ops[0].reg),sz); n=ops[1].imm&(63 if sz==8 else 31)
            if n==0: raise Unsup('shift0')
            out.append({'shl':'lsl','shr':'lsr','sar':'asr'}[m]+' %s, %s, #%d'%(d0,d0,n)); kind=None
        elif False:
            d0=reg(md.reg_name(ops[0].reg),sz)
            out.append(('adds %s, %s, #1' if m=='inc' else 'subs %s, %s, #1')%(d0,d0)); kind='incdec'
        else: raise Unsup('mnemonic '+m)
    return out
def _free(PD,t):
    import bisect
    k=bisect.bisect_right(PD,(t,1<<62))-1
    return not(k>=0 and PD[k][0]<t<PD[k][1])
if __name__=='__main__':
    res={}; reasons={}
    import bisect
    PD=sorted((s,e) for s,e,u in funcs)
    class _P:
        def __iter__(self_): return iter(PD)
    starts=set(s for s,e,u in funcs)
    tx=_t.VirtualAddress; te=tx+_t.Misc_VirtualSize
    for s0,e0,u0 in funcs:
        for i in md.disasm(bytes(d[s0:e0]),IB+s0):
            if i.mnemonic in('call','jmp') and i.operands and i.operands[0].type==X86_OP_IMM:
                t=i.operands[0].imm-IB
                if tx<=t<te and _free(PD,t): starts.add(t)
    # also scan for callers inside discovered leaves (one round)
    ss=sorted(starts); ext={}
    for k,a0 in enumerate(ss):
        nxt=ss[k+1] if k+1<len(ss) else te
        pdend={s:e for s,e,u in funcs}.get(a0)
        e1=pdend if pdend else nxt
        while e1>a0 and d[e1-1] in(0xcc,0x90,0x00): e1-=1
        ext[a0]=e1
    cand=[(a0,e1) for a0,e1 in ext.items() if 8<=e1-a0<=400]
    for s,e in cand:
        try: res[s]=translate(s,e)
        except Unsup as x: reasons[str(x).split(' ')[0]+' '+(str(x).split(' ')[1] if ' ' in str(x) else '')]=reasons.get(str(x).split(' ')[0]+' '+(str(x).split(' ')[1] if ' ' in str(x) else ''),0)+1
        except Exception as x: reasons['ERR '+type(x).__name__]=reasons.get('ERR '+type(x).__name__,0)+1
    print('candidates(8-400B funcs)',len(cand),'translated',len(res))
    print(sorted(reasons.items(),key=lambda x:-x[1])[:12])
    with open('poc.s','w') as f:
        for s,ls in res.items():
            f.write('F%x:\n'%s); f.write('\n'.join(ls)+'\n')
    open('poc_funcs.txt','w').write('\n'.join('%x %x'%(s,e) for s,e in cand if s in res))
