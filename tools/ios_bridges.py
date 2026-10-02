#!/usr/bin/env python3
"""Generate typed guest-to-Darwin bridges; translate pointers, never GL offsets."""
import re
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/ios/host'
# arm64 and arm64_32 pass the first eight integer or pointer arguments in registers; past them
# the stack layouts differ (a pointer is 4 bytes in the guest, 8 in the host), so a host_* import's
# bridge would read garbage. Floats go in the vector registers under both ABIs.
INTEGER_REGISTER_COUNT=8
FLOAT_SCALARS={'float','double'}

def split_parameter(param):
    """(type text, name) of one C parameter; the name is None when the parameter is unnamed"""
    param=param.strip()
    m=re.match(r'(.*?)(\w+)\s*$',param)
    ptype,pname=m.group(1).strip(),m.group(2)
    if not ptype:  # unnamed
        ptype,pname=param,None
    return ptype,pname

def declarations(path,pattern):
    text=re.sub(r'/\*.*?\*/','',path.read_text(),flags=re.S)
    text=re.sub(r'__attribute__\s*\(\(.*?\)\)','',text)
    return re.findall(r'^([\w \t*]+?)\b('+pattern+r')\s*\(([^;]*?)\)\s*;',text,re.M|re.S)

def bridge(ret,name,params):
    """the host side of one guest import declared as `ret name(params)`: (the import's name, the
    C source of its bridge function ios_bridge_<name>). Pointers arrive as 64-bit zero-extended
    guest addresses and are translated with host_pointer; other arguments pass through. A
    host_gpu_x import calls the GPU backend's gpu_x (gpu_gl.c compiled into the host); a posix_x
    declaration imports as hostposix_x. Raises ValueError for more than INTEGER_REGISTER_COUNT
    integer or pointer arguments."""
    ret=' '.join(ret.split());params=' '.join(params.split())
    decl=[];args=[];count=0
    for index,param in enumerate([] if params in ('void','') else params.split(',')):
        kind,_=split_parameter(param);arg=f'a{index}'
        if '*' in kind:
            count+=1
            decl.append(f'uint64_t {arg}')
            opaque=name in ('posix_directory_next','posix_directory_close') and index==0
            args.append(f'({kind})'+(f'(uintptr_t){arg}' if opaque else f'host_pointer({arg})'))
        else:
            if kind.replace('const','').strip() not in FLOAT_SCALARS:count+=1
            decl.append(f'{kind} {arg}');args.append(arg)
    # host_* only, as the spec says: posix_socket_select has nine (its ninth is an int, 4
    # bytes on both ABIs, which is why it works); the hazard is a pointer past the eighth
    if name.startswith('host_') and count>INTEGER_REGISTER_COUNT:
        raise ValueError(f'{name}: {count} integer or pointer arguments; host imports take at most '
                         f'{INTEGER_REGISTER_COUNT} (past them arm64_32 and arm64 lay out the stack differently)')
    if name.startswith('posix_'):imported,callee='hostposix_'+name[6:],name
    elif name.startswith('host_gpu_'):imported,callee=name,name[len('host_'):]
    else:imported,callee=name,name
    return_type='uint32_t' if '*' in ret else ret
    call=f'{callee}({", ".join(args)})'
    if '*' in ret:call=f'(uint32_t)(uintptr_t){call}'
    source=(f'static {return_type} ios_bridge_{name}({", ".join(decl) or "void"}) {{\n'
            +('    ' if ret=='void' else '    return ')+call+';\n}')
    return imported,source

def main():
    # the GL half's helpers, imported here so that importing this module (tools/test_ios_bridges.py)
    # neither needs them nor runs the generator
    from guest_gl_stubs import gles_functions, prototypes, FLOAT_TYPES, WIDE_TYPES
    OUT.mkdir(parents=True,exist_ok=True)
    lines=['/* Generated typed bridges. */','#include "ios_host.h"','#include "guest_host.h"','#include "posix.h"',
           '#include <SDL3/SDL.h>','#include <GLES3/gl32.h>','#include <GLES2/gl2ext.h>','#include <string.h>']
    table=[]
    for header,pattern in [(ROOT/'port/runtime/guest/runtime/guest_host.h',r'host_\w+'),(ROOT/'port/linux/src/posix.h',r'posix_\w+')]:
        for ret,name,params in declarations(header,pattern):
            imported,source=bridge(ret,name,params)
            lines.append(source)
            table.append((imported,'ios_bridge_'+name))

    gl=ROOT/'build/ios/gl_include'
    protos=prototypes(str(gl/'GLES3/gl32.h'),str(gl/'GLES2/gl2ext.h'))
    for name in gles_functions(str(ROOT/'port/linux/src/gl.h')):
        if name=='glGetString':continue
        ret,params=protos[name]
        plist=[] if params=='void' else [split_parameter(p) for p in params.split(',')]
        decl=[];args=[];integer_index=0
        for index,(kind,_) in enumerate(plist):
            arg=f'a{index}';pointer='*' in kind;base=kind.replace('const','').strip()
            if base in FLOAT_TYPES and not pointer:
                decl.append(f'{kind} {arg}');args.append(arg);continue
            on_stack=integer_index>=8;integer_index+=1
            if pointer:
                decl.append(f'uint64_t {arg}')
                offset=name in ('glVertexAttribPointer','glVertexAttribIPointer','glDrawElements','glDrawElementsBaseVertex') and index==len(plist)-1
                # BaseVertex's final parameter is an integer; its indices are #3.
                offset=offset or (name=='glDrawElementsBaseVertex' and index==3)
                args.append(f'({kind})'+(f'(uintptr_t){arg}' if offset else f'host_pointer({arg})'))
            elif base in WIDE_TYPES or on_stack:
                decl.append(f'long long {arg}');args.append(f'({kind}){arg}')
            else:decl.append(f'{kind} {arg}');args.append(arg)
        bridge_name='ios_bridge_'+name
        lines.append(f'static {ret} {bridge_name}({", ".join(decl) or "void"}) {{')
        signature=', '.join(t for t,_ in plist) or 'void'
        lines.append(f'    typedef {ret} (GL_APIENTRY *Function)({signature});')
        lines.append(f'    static Function function; if(!function) function=(Function)SDL_GL_GetProcAddress("{name}");')
        lines.append(f'    if(!function) host_fatal("OpenGL ES entry point unavailable: {name}");')
        if name=='glBindFramebuffer':
            lines.append('    if(!a1) a1=host_ios_default_framebuffer();')
        if name=='glShaderSource':
            lines.extend(['    const uint64_t *raw=host_pointer(a2); const GLchar *strings[16];',
                          '    if(a1<0 || a1>16) host_fatal("invalid shader string count");',
                          '    for(int i=0;i<a1;i++) strings[i]=host_pointer(raw[i]);'])
            args[2]='strings'
        lines.append(('    ' if ret=='void' else '    return ')+f'function({", ".join(args)});\n'+'}')
        table.append(('hostgl_'+name,bridge_name))
    lines.extend(['void *host_resolve_import(const char *name) {',
                  '    static const struct {const char *name; void *function;} table[]={'])
    lines.extend(f'        {{"{name}",(void *){fn}}},' for name,fn in table)
    lines.extend(['    };','    for(unsigned i=0;i<sizeof(table)/sizeof(table[0]);i++) if(!strcmp(name,table[i].name))return table[i].function;',
                  '    return NULL;','}'])
    (OUT/'bridges.c').write_text('\n'.join(lines)+'\n')
    # Use Linux numbers explicitly: Darwin SYS_* constants have different values.
    (OUT/'guest_syscall_numbers.h').write_text((ROOT/'port/runtime/guest/libc/arch/arm64_32/bits/syscall.h.in').read_text())

if __name__=='__main__':main()
