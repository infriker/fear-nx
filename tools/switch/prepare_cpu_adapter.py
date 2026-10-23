#!/usr/bin/env python3
"""Derive game-only translation units; never edit generated inputs or SDK files."""
import argparse, hashlib, json, re
from pathlib import Path

def write_changed(path,text):
    path.parent.mkdir(parents=True,exist_ok=True)
    if not path.exists() or path.read_text()!=text:path.write_text(text)

def prove_atomic_regions(body):
    lines=body.splitlines()
    labels={s.strip()[:-1]:i for i,s in enumerate(lines) if re.fullmatch(r'loc_[0-9A-F]+:',s.strip())}
    allowed={'lwarx','ldarx','stwcx.','stdcx.','cmpw','cmpd','cmpwi','cmplw','cmplwi','addi','add','mtmsrd','bne','beq','b'}
    enters=[i for i,s in enumerate(lines) if 'REX_ENTER_GLOBAL_LOCK();' in s]
    for start in enters:
        todo=[(start+1,False,False)];seen=set();exits=0
        while todo:
            i,load,cas=todo.pop()
            if (i,load,cas) in seen:continue
            seen.add((i,load,cas))
            if i>=len(lines):return False
            s=lines[i].strip()
            if 'REX_LEAVE_GLOBAL_LOCK();' in s:
                if not load or not cas:return False
                exits+=1;continue
            if s.startswith('//'):
                op=s[3:].split()[0]
                if op not in allowed:return False
            if 'REX_ENTER_GLOBAL_LOCK' in s or 'REX_CALL' in s or 'REX_STORE' in s or 'return' in s:return False
            if 'ctx.reserved.' in s and '= *(' in s:load=True
            if '__sync_bool_compare_and_swap' in s:cas=True
            jump=re.search(r'goto (loc_[0-9A-F]+);',s)
            if jump:
                if jump[1] not in labels:return False
                todo.append((labels[jump[1]],load,cas))
                if not s.startswith('if'):continue
            todo.append((i+1,load,cas))
        if not exits:return False
    return bool(enters)

def transform(text):
    counts={'flush_sites':0,'atomic_regions':0,'atomic_functions':0,'preserved_lock_functions':0}
    pattern=r'DEFINE_REX_FUNC\(([^)]+)\)\s*\{'
    matches=list(re.finditer(pattern,text));pieces=[];cursor=0;functions=[]
    for n,m in enumerate(matches):
        end=matches[n+1].start() if n+1<len(matches) else len(text)
        body=text[m.start():end]
        if 'REX_ENTER_GLOBAL_LOCK' in body:
            if prove_atomic_regions(body):
                counts['atomic_functions']+=1;counts['atomic_regions']+=body.count('REX_ENTER_GLOBAL_LOCK')
                functions.append(m[1])
                for a,b in [('REX_CHECK_GLOBAL_LOCK()','fear_nx::cpu::CheckInterrupts(ctx)'),
                            ('REX_ENTER_GLOBAL_LOCK()','fear_nx::cpu::EnterInterrupts()'),
                            ('REX_LEAVE_GLOBAL_LOCK()','fear_nx::cpu::LeaveInterrupts()')]:body=body.replace(a,b)
                body=re.sub(r'\*\((uint(?:32|64)_t)\*\)REX_RAW_ADDR\(ea\)',r'__atomic_load_n(reinterpret_cast<\1*>(REX_RAW_ADDR(ea)), __ATOMIC_SEQ_CST)',body)
            else:counts['preserved_lock_functions']+=1
        pieces.extend([text[cursor:m.start()],body]);cursor=end
    pieces.append(text[cursor:]);text=''.join(pieces)
    def flush(m):
        counts['flush_sites']+=1
        return 'fear_nx::cpu::SetFlush(ctx.fpscr, '+('true' if m[1].startswith('enable') else 'false')+', '+('true' if m[1].endswith('Unconditional') else 'false')+')'
    text=re.sub(r'ctx\.fpscr\.(enableFlushMode(?:Unconditional)?|disableFlushMode(?:Unconditional)?)\(\)',flush,text)
    return text,counts,functions

def main():
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();manifest={};total={}
    for path in sorted(a.source.glob('fear_xbox360_recomp.*.cpp')):
        raw=path.read_text();text,counters,functions=transform(raw)
        assert counters['flush_sites'] or counters['atomic_regions'] or raw==text
        write_changed(a.output/path.name,text)
        manifest[path.name]={'input_sha256':hashlib.sha256(raw.encode()).hexdigest(),'output_sha256':hashlib.sha256(text.encode()).hexdigest(),'counters':counters,'atomic_functions':functions}
        for k,v in counters.items():total[k]=total.get(k,0)+v
    assert total['atomic_regions']==89 and total['atomic_functions']==59 and total['preserved_lock_functions']==0,total
    write_changed(a.output/'manifest.json',json.dumps({'totals':total,'files':manifest},indent=2)+'\n')
    print('F.E.A.R. game-only CPU adapter:',total)
if __name__=='__main__':main()
