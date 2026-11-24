import sys,struct,subprocess,random
sys.path.insert(0,'.')
import xlat, abi
from fuzz1_helpers import asm_x86, asm_arm_file
from unicorn import *
from unicorn.x86_const import *
from unicorn.arm64_const import *
M64=(1<<64)-1
# guest function f: calls pointer in rcx-saved via rax with 9 args; then returns rax + rbx + rsi + rdi + r12..r15 checksum
F_CALL='''
push rbx
push rsi
push rdi
push r12
push r13
push r14
push r15
sub rsp, 0x68
mov rax, rcx
mov rbx, 0x1111
mov rsi, 0x2222
mov rdi, 0x3333
mov r12, 0x4444
mov r13, 0x5555
mov r14, 0x6666
mov r15, 0x7777
mov ecx, 1
mov edx, 2
mov r8d, 3
mov r9d, 4
mov qword ptr [rsp+0x20], 5
mov qword ptr [rsp+0x28], 6
mov qword ptr [rsp+0x30], 7
mov qword ptr [rsp+0x38], 8
mov qword ptr [rsp+0x40], 9
call rax
add rax, rbx
add rax, rsi
add rax, rdi
add rax, r12
add rax, r13
add rax, r14
add rax, r15
add rsp, 0x68
pop r15
pop r14
pop r13
pop r12
pop rdi
pop rsi
pop rbx
ret
'''
# guest callee g(a1..a9) = a1 + 2*a2 + 3*a3 + 4*a4 + 5*a5 + 6*a6 + 7*a7 + 8*a8 + 9*a9  (Win x64 ABI)
F_G='''
mov rax, rcx
lea rax, [rax+rdx*2]
lea r10, [r8+r8*2]
add rax, r10
lea r10, [r9*4]
add rax, r10
mov r10, qword ptr [rsp+0x28]
imul r10, r10, 5
add rax, r10
mov r10, qword ptr [rsp+0x30]
imul r10, r10, 6
add rax, r10
mov r10, qword ptr [rsp+0x38]
imul r10, r10, 7
add rax, r10
mov r10, qword ptr [rsp+0x40]
imul r10, r10, 8
add rax, r10
mov r10, qword ptr [rsp+0x48]
imul r10, r10, 9
add rax, r10
ret
'''
fc=asm_x86(F_CALL); fg=asm_x86(F_G)
lf=xlat.translate_code(fc,0x1000); lg=xlat.translate_code(fg,0x2000)
mock='''
.text
.balign 4
native_sum:   // AAPCS64: x0..x7 args, 9th arg at [sp]; clobbers caller-saved + would clobber guest nonvolatiles
    mov x9, #2
    madd x0, x1, x9, x0
    mov x9, #3
    madd x0, x2, x9, x0
    mov x9, #4
    madd x0, x3, x9, x0
    mov x9, #5
    madd x0, x4, x9, x0
    mov x9, #6
    madd x0, x5, x9, x0
    mov x9, #7
    madd x0, x6, x9, x0
    mov x9, #8
    madd x0, x7, x9, x0
    ldr x10, [sp]
    mov x9, #9
    madd x0, x10, x9, x0
    movz x3, #0xdead
    mov x5, x3
    mov x6, x3
    mov x7, x3
    mov x12, x3
    mov x13, x3
    mov x14, x3
    mov x15, x3
    mov x8, x3
    movi v6.16b, #0xff
    movi v9.16b, #0xff
    ret
// native test driver: calls ent_F2000 with 9 args, checks callee-saved x19..x28 + sp preserved, returns x0
native_caller:
    stp x29, x30, [sp, #-112]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    mov x19, #0x19
    mov x20, #0x20
    mov x21, #0x21
    mov x22, #0x22
    mov x23, #0x23
    mov x24, #0x24
    mov x25, #0x25
    mov x26, #0x26
    mov x27, #0x27
    mov x28, #0x28
    mov x0, #1
    mov x1, #2
    mov x2, #3
    mov x3, #4
    mov x4, #5
    mov x5, #6
    mov x6, #7
    mov x7, #8
    mov x9, #9
    mov x29, sp
    sub sp, sp, #16
    str x9, [sp]
    bl ent_F2000
    add sp, sp, #16
    // checksum callee-saved into x0 delta: if all preserved leave x0
    cmp x19, #0x19
    b.ne bad
    cmp x20, #0x20
    b.ne bad
    cmp x21, #0x21
    b.ne bad
    cmp x22, #0x22
    b.ne bad
    cmp x23, #0x23
    b.ne bad
    cmp x24, #0x24
    b.ne bad
    cmp x25, #0x25
    b.ne bad
    cmp x26, #0x26
    b.ne bad
    cmp x27, #0x27
    b.ne bad
    cmp x28, #0x28
    b.ne bad
    mov x9, sp
    cmp x9, x29
    b.ne bad
    b done
bad:
    movz x0, #0xbad
done:
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #112
    ret
// glue to call guest f with the native target pointer: guest entry convention; args: x1 = target
call_guest_f:
    // set up guest stack: x4 = x17-based region
    ret
'''
# We call guest f directly: guest convention. harness pre-sets x4 (rsp) with return address RET and rcx=target.
asm=['.text']+['Ff1000_dummy:']
body='.text\nF1000:\n'+'\n'.join(lf)+'\nF2000:\n'+'\n'.join(lg)+'\n'
open('abi_all.s','w').write(abi.gen([],[0x2000])+body+mock)
r=subprocess.run(['clang','-target','aarch64-linux-gnu','-c','abi_all.s','-o','abi_all.o'],capture_output=True,text=True)
print(r.stderr[:1500]); r.check_returncode()
subprocess.run(['llvm-objcopy','-O','binary','-j','.text','abi_all.o','abi_all.bin'],check=True)
code=open('abi_all.bin','rb').read()
syms={}
for l in subprocess.check_output(['llvm-nm','abi_all.o']).decode().split('\n'):
    p=l.split()
    if len(p)==3: syms[p[2]]=int(p[0],16)
ARMC=0x40000000; STK=0x30000000; RET=0x7ff00000
def run_arm(entry,regs,count=5000):
    a=Uc(UC_ARCH_ARM64,UC_MODE_ARM); a.mem_map(ARMC,(len(code)+0xfff)&~0xfff+0x1000); a.mem_write(ARMC,code)
    a.mem_map(STK-0x10000,0x20000); a.mem_map(RET&~0xfff,0x1000); a.mem_write(RET,b'\x1f\x20\x03\xd5')
    # enable FP
    a.reg_write(UC_ARM64_REG_CPACR_EL1,0x300000)
    for k,v in regs.items(): a.reg_write(k,v)
    a.emu_start(ARMC+entry,RET,count=count); return a
# ---- Test 1: guest f -> native_sum via thunk
sp=STK-0x100
a=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
a.mem_map(ARMC,0x100000); a.mem_write(ARMC,code); a.mem_map(STK-0x10000,0x20000); a.mem_map(RET&~0xfff,0x1000); a.mem_write(RET,b'\x1f\x20\x03\xd5')
a.reg_write(UC_ARM64_REG_CPACR_EL1,0x300000)
a.mem_write(sp-8,struct.pack('<Q',RET)); a.reg_write(UC_ARM64_REG_X4,sp-8)
a.reg_write(UC_ARM64_REG_X1,ARMC+syms['native_sum'])
a.reg_write(UC_ARM64_REG_SP,STK-0x8000)
a.emu_start(ARMC+syms['F1000'],RET,count=20000)
got=a.reg_read(UC_ARM64_REG_X0)
exp=sum(i*i for i in range(1,10))+0x1111+0x2222+0x3333+0x4444+0x5555+0x6666+0x7777
print('T1 guest->native 9 args + nonvolatile restore:', hex(got), 'expected', hex(exp), 'PASS' if got==exp else 'FAIL')
# ---- Test 2: native -> guest g via entry stub
b=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
b.mem_map(ARMC,0x100000); b.mem_write(ARMC,code); b.mem_map(STK-0x10000,0x20000); b.mem_map(RET&~0xfff,0x1000); b.mem_write(RET,b'\x1f\x20\x03\xd5')
b.reg_write(UC_ARM64_REG_CPACR_EL1,0x300000); b.reg_write(UC_ARM64_REG_SP,STK-0x8000); b.reg_write(UC_ARM64_REG_X30,RET)
b.emu_start(ARMC+syms['native_caller'],RET,count=20000)
g2=b.reg_read(UC_ARM64_REG_X0); print('T2 native->guest via entry stub (9 args, callee-saved preserved):',hex(g2),'expected',hex(sum(i*i for i in range(1,10))),'PASS' if g2==sum(i*i for i in range(1,10)) else 'FAIL')
