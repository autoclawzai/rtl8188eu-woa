import pefile,collections,sys
pe=pefile.PE('rtwlanu.sys')
print('machine',hex(pe.FILE_HEADER.Machine),'subsys',pe.OPTIONAL_HEADER.Subsystem,'ep',hex(pe.OPTIONAL_HEADER.AddressOfEntryPoint),'size',pe.OPTIONAL_HEADER.SizeOfImage)
print('ver',[ (e.Key if hasattr(e,'Key') else 0) for e in []])
tot=0
by={}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    n=d.dll.decode(); by[n]=[i.name.decode() if i.name else 'ord%d'%i.ordinal for i in d.imports]; tot+=len(by[n])
print('TOTAL imports',tot)
for k,v in by.items(): print(k,len(v))
open('imports.txt','w').write('\n'.join(f'{k}!{x}' for k,v in by.items() for x in v))
for k in by:
    if 'wdf' in k.lower() or 'WDF' in ' '.join(by[k]): print('WDF-ish', k)
print([s.Name.rstrip(b'\0').decode()+':'+hex(s.Misc_VirtualSize) for s in pe.sections])
