import subprocess
def asm_x86(text):
    open('t.s','w').write('.intel_syntax noprefix\n.text\n'+text+'\n')
    subprocess.run(['llvm-mc','-triple=x86_64','-filetype=obj','t.s','-o','t.o'],check=True,capture_output=True)
    subprocess.run(['llvm-objcopy','-O','binary','-j','.text','t.o','t.bin'],check=True)
    return open('t.bin','rb').read()
def asm_arm_file(lines): pass
