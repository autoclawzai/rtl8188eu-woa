"""ABI bridge generator: guest (translated x64 convention) <-> native ARM64 Windows ABI.
Guest regs: rax x0, rcx x1, rdx x2, rbx x3, rsp x4, rbp x5, rsi x6, rdi x7, r8..r15 x8..x15. Scratch: x16-x17, x19-x21.
Guest call: return address pushed at [x4]; shadow space 32B; arg5+ at [x4+0x28].
"""
THUNK = r'''
.text
.balign 4
// native call thunk. x17 = native target. Guest return address at [x4].
thunk_common:
    mov x22, sp
    mov x23, x4
    ldr x24, [x4]
    sub x16, x4, #0x400
    and x16, x16, #~0xf
    mov sp, x16
    stp x3, x5, [sp, #0]
    stp x6, x7, [sp, #16]
    stp x12, x13, [sp, #32]
    stp x14, x15, [sp, #48]
    stp q6, q7, [sp, #64]
    stp q8, q9, [sp, #96]
    stp q10, q11, [sp, #128]
    stp q12, q13, [sp, #160]
    stp q14, q15, [sp, #192]
    mov x0, x1
    mov x1, x2
    mov x2, x8
    mov x3, x9
    ldp x4, x5, [x23, #0x28]
    ldp x6, x7, [x23, #0x38]
    sub sp, sp, #64
    ldp x19, x20, [x23, #0x48]
    stp x19, x20, [sp, #0]
    ldp x19, x20, [x23, #0x58]
    stp x19, x20, [sp, #16]
    ldp x19, x20, [x23, #0x68]
    stp x19, x20, [sp, #32]
    ldp x19, x20, [x23, #0x78]
    stp x19, x20, [sp, #48]
    blr x17
    add sp, sp, #64
    ldp x3, x5, [sp, #0]
    ldp x6, x7, [sp, #16]
    ldp x12, x13, [sp, #32]
    ldp x14, x15, [sp, #48]
    ldp q6, q7, [sp, #64]
    ldp q8, q9, [sp, #96]
    ldp q10, q11, [sp, #128]
    ldp q12, q13, [sp, #160]
    ldp q14, q15, [sp, #192]
    add x4, x23, #8
    mov sp, x22
    br x24

__indirect_x17:
    b thunk_common
'''
def import_thunk(name):
    return f'''
.balign 4
imp_{name}:
    adrp x17, iat_{name}
    ldr x17, [x17, :lo12:iat_{name}]
    b thunk_common
'''
def iat_slot(name):
    return f'.data\n.balign 8\niat_{name}: .quad 0\n.text\n'
def entry_stub(rva):
    n=f'F{rva:x}'
    return f'''
.balign 4
ent_{n}:
    stp x29, x30, [sp, #-96]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    sub x17, sp, #0x108
    stp x4, x5, [x17, #0x28]
    stp x6, x7, [x17, #0x38]
    ldp x19, x20, [sp, #96]
    stp x19, x20, [x17, #0x48]
    ldp x19, x20, [sp, #112]
    stp x19, x20, [x17, #0x58]
    ldp x19, x20, [sp, #128]
    stp x19, x20, [x17, #0x68]
    ldp x19, x20, [sp, #144]
    stp x19, x20, [x17, #0x78]
    mov x8, x2
    mov x9, x3
    mov x2, x1
    mov x1, x0
    adr x16, ent_ret_{n}
    str x16, [x17]
    mov x4, x17
    b {n}
ent_ret_{n}:
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #96
    ret
'''
def gen(imports, entries):
    out=[THUNK]
    for n in imports: out.append(iat_slot(n)); out.append(import_thunk(n))
    for r in entries: out.append(entry_stub(r))
    return '\n'.join(out)
