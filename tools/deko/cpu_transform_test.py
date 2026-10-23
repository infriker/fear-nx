import importlib.util
from pathlib import Path
path=Path(__file__).resolve().parents[1]/'switch/prepare_cpu_adapter.py'
spec=importlib.util.spec_from_file_location('adapter',path);adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)
body="""DEFINE_REX_FUNC(sub_test) {
REX_ENTER_GLOBAL_LOCK();
// lwarx r9,0,r8
ctx.reserved.u32 = *(uint32_t*)REX_RAW_ADDR(ea);
// cmpw cr6,r9,r30
ctx.cr6.compare<int32_t>(ctx.r9.s32, ctx.r30.s32, ctx.xer);
// bne cr6,0x100
if (!ctx.cr6.eq) goto loc_100;
// stwcx. r10,0,r8
ctx.cr0.eq = __sync_bool_compare_and_swap(p, old, next);
REX_LEAVE_GLOBAL_LOCK();
return;
loc_100:
// stwcx. r9,0,r8
ctx.cr0.eq = __sync_bool_compare_and_swap(p, old, old);
REX_LEAVE_GLOBAL_LOCK();
ctx.fpscr.enableFlushMode();
}
"""
assert adapter.prove_atomic_regions(body)
for changed in (
 body.replace('// stwcx. r9,0,r8','// stw r9,0(r8)'),
 body.replace('REX_LEAVE_GLOBAL_LOCK();','return;',1),
 body.replace('loc_100:', 'loc_200:'),
 body.replace('// cmpw cr6,r9,r30','// bl 0x123'),
 body.replace('loc_100:\n','loc_100:\nREX_STORE_U32(0, 0);\n'),
 body.replace('loc_100:\n','loc_100:\nREX_ENTER_GLOBAL_LOCK();\n')
):assert not adapter.prove_atomic_regions(changed)
result,counts,functions=adapter.transform(body)
assert counts['atomic_regions']==1 and counts['flush_sites']==1 and functions==['sub_test']
assert '__atomic_load_n' in result and 'fear_nx::cpu::SetFlush(ctx.fpscr, true, false)' in result
bad=body.replace('// cmpw cr6,r9,r30','// bl 0x123')
result,counts,_=adapter.transform(bad)
assert counts['preserved_lock_functions']==1 and 'REX_ENTER_GLOBAL_LOCK' in result
print('CPU translation proof tests passed')
