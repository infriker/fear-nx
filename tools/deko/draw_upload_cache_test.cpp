// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_draw_upload_cache.h"
#include "deko/deko_vertex_cache.h"
#include "deko/deko_command_cache.h"
#include <algorithm>
#include <iostream>

namespace {
using namespace fear_nx::deko;
#define CHECK(x) do { if (!(x)) { std::cerr << __func__ << ':' << __LINE__ << ": " #x "\n"; return false; } } while (false)

bool ExactBytesCollisionsAndWorkLifetime() {
  WorkUploadCache<uint32_t,2> cache(4,64);
  std::array<uint8_t,16> a{}, b{};
  b[7]=1;
  const std::array<uint64_t,2> key{17,16}; // Deliberate hash collision.
  CHECK(cache.Insert(key,a,256));
  CHECK(cache.Find(key,a) && *cache.Find(key,a)==256);
  CHECK(!cache.Find(key,b));
  CHECK(!cache.Find(key,std::span(a).first(8)));
  // A source write invalidates the hit even if a supplied hash collides.
  a[7]=1;
  CHECK(!cache.Find(key,a));
  CHECK(cache.Insert(key,b,512));
  CHECK(*cache.Find(key,b)==512 && cache.shadow_bytes()==16);
  b[0]=2;
  CHECK(!cache.Find(key,b)); // Shadow owns its bytes, not a pointer to b.
  cache.Clear();
  CHECK(!cache.Find(key,a) && cache.size()==0 && cache.shadow_bytes()==0);
  CHECK(cache.Insert(key,a,768) && *cache.Find(key,a)==768);
  return true;
}

bool BoundedMetadataAndLargeUncachedFallback() {
  WorkUploadCache<uint32_t,2> cache(2,12);
  const std::array<uint8_t,4> a{1,2,3,4};
  const std::array<uint8_t,8> b{};
  CHECK(cache.Insert({1,4},a,1));
  CHECK(cache.Insert({2,8},b,2));
  CHECK(!cache.Insert({3,4},a,3));
  CHECK(!cache.Insert({1,4},b,4)); // Would exceed the byte cap.
  CHECK(*cache.Find({1,4},a)==1 && *cache.Find({2,8},b)==2);
  CHECK(cache.Insert({2,8},a,5)); // Replacement releases the old shadow size.
  CHECK(cache.shadow_bytes()==8 && cache.size()==2);
  cache.Clear();
  CHECK(cache.Insert({7,0},{},7)); // Generated indices have no source bytes.
  CHECK(*cache.Find({7,0},{})==7);
  return true;
}

bool AllIndexSemanticsParticipateInIdentity() {
  IndexUploadDescription original;
  original.count=4;original.base=8;original.minimum=2;original.maximum=200;
  original.restart=0xFFFF;
  const auto key=IndexUploadKey(original,17);
  for (int change=0;change<12;++change) {
    auto d=original;
    switch(change) {
      case 0: ++d.count;break;
      case 1: ++d.base;break;
      case 2: ++d.minimum;break;
      case 3: ++d.maximum;break;
      case 4: ++d.restart;break;
      case 5: ++d.endian;break;
      case 6: d.generated=true;break;
      case 7: d.index32=true;break;
      case 8: d.fan=true;break;
      case 9: d.loop=true;break;
      case 10: d.reset=true;break;
      case 11: d.output16=true;break;
    }
    CHECK(IndexUploadKey(d,17)!=key);
  }
  CHECK(IndexUploadKey(original,18)!=key);
  return true;
}

bool ConversionRestartBoundsAndExpansion() {
  const std::array<uint32_t,4> raw{0,65535,2,4};
  IndexUploadDescription d;
  d.count=4;d.base=10;d.minimum=12;d.maximum=13;d.reset=true;d.restart=65535;
  std::vector<uint32_t> output(4);
  IndexBounds bounds;
  CHECK(BuildIndexUpload(d,[&](uint32_t i){return raw[i];},output,bounds));
  CHECK((output==std::vector<uint32_t>{0,UINT32_MAX,2,4}));
  CHECK(bounds.valid() && bounds.min()==12 && bounds.max()==13);
  CHECK(!BuildIndexUpload(d,[&](uint32_t i){return raw[i];},std::span(output).first(3),bounds));
  const std::array<uint32_t,4> vertices{2,5,7,9};
  d={};d.count=4;d.fan=true;
  output.resize(size_t(IndexUploadCount(d)));
  CHECK(BuildIndexUpload(d,[&](uint32_t i){return vertices[i];},output,bounds));
  CHECK((output==std::vector<uint32_t>{2,5,7,2,7,9}));
  CHECK(bounds.min()==2 && bounds.max()==9);
  d.fan=false;d.loop=true;
  output.resize(size_t(IndexUploadCount(d)));
  CHECK(BuildIndexUpload(d,[&](uint32_t i){return vertices[i];},output,bounds));
  CHECK((output==std::vector<uint32_t>{2,5,7,9,2}));
  d={};d.count=4;d.generated=true;d.loop=true;d.base=0xFFFFFE;
  output.resize(size_t(IndexUploadCount(d)));
  CHECK(BuildIndexUpload(d,[](uint32_t i){return i;},output,bounds));
  CHECK((output==std::vector<uint32_t>{0,1,2,3,0}));
  CHECK(bounds.min()==0 && bounds.max()==0xFFFFFF);
  d.count=UINT32_MAX;
  CHECK(IndexUploadCount(d)==uint64_t(UINT32_MAX)+1);
  d.loop=false;d.fan=true;
  CHECK(IndexUploadCount(d)==(uint64_t(UINT32_MAX)-2)*3);
  d.count=2;
  CHECK(IndexUploadCount(d)==0);
  d={};d.count=4;d.reset=true;d.restart=2;
  output.resize(4);
  CHECK(BuildIndexUpload(d,[](uint32_t){return 2u;},output,bounds));
  CHECK(!bounds.valid() && std::all_of(output.begin(),output.end(),[](auto i){return i==UINT32_MAX;}));
  return true;
}

bool RepeatedIndexDrawsAndMutableInputs() {
  struct Copy { uint32_t offset; IndexBounds bounds; };
  WorkUploadCache<Copy,4> cache(64,65536);
  IndexUploadDescription d;d.count=4;
  std::array<uint32_t,4> source{4,7,10,20};
  std::vector<uint32_t> gpu; // Simulated immutable upload address space.
  uint32_t builds=0,hits=0;
  auto draw=[&]() {
    auto bytes=std::span(reinterpret_cast<const uint8_t*>(source.data()),sizeof(source));
    const auto key=IndexUploadKey(d,1); // Also exercise a colliding input digest.
    if (const auto* hit=cache.Find(key,bytes)) { ++hits;return *hit; }
    std::vector<uint32_t> staging(size_t(IndexUploadCount(d)));
    Copy result{uint32_t(gpu.size()),{}};
    if (!BuildIndexUpload(d,[&](uint32_t i){return source[i];},staging,result.bounds)) std::abort();
    gpu.insert(gpu.end(),staging.begin(),staging.end());
    if (!cache.Insert(key,bytes,result)) std::abort();
    ++builds;return result;
  };
  const auto first=draw();
  for (int i=0;i<1000;++i) CHECK(draw().offset==first.offset);
  CHECK(builds==1 && hits==1000);
  source[2]=99;
  const auto changed=draw();
  CHECK(changed.offset!=first.offset && changed.bounds.max()==99);
  CHECK(gpu[first.offset+2]==10 && gpu[changed.offset+2]==99);
  ++d.base;
  CHECK(draw().bounds.max()==100); // Bounds key must include shader index base.
  cache.Clear();
  CHECK(draw().offset!=changed.offset);
  std::cout << "Repeated index draws: 1001 requests, 1 conversion/upload before mutation.\n";
  return true;
}

bool AlternatingConstantsAndImmutableCopies() {
  WorkUploadCache<uint32_t,2> cache(16,65536);
  std::array<uint8_t,160> a{},b{};
  a[0]=11;b[0]=22;
  std::vector<uint8_t> gpu;
  uint32_t copies=0;
  auto upload=[&](std::span<const uint8_t> source,uint64_t hash) {
    WorkUploadCache<uint32_t,2>::Key key{hash,source.size()};
    if (auto found=cache.Find(key,source)) return *found;
    const uint32_t offset=uint32_t(gpu.size());
    gpu.resize(gpu.size()+256,0);
    std::copy(source.begin(),source.end(),gpu.begin()+offset);
    if (!cache.Insert(key,source,offset)) std::abort();
    ++copies;return offset;
  };
  const auto first=upload(a,1),second=upload(b,2);
  for (int i=0;i<1000;++i) CHECK(upload(i%2?std::span(a):std::span(b),i%2?1:2)==(i%2?first:second));
  CHECK(copies==2 && gpu[first]==11 && gpu[second]==22);
  a[0]=33;
  const auto changed=upload(a,1);
  CHECK(changed!=first && gpu[first]==11 && gpu[changed]==33);
  CHECK(std::all_of(gpu.begin()+changed+160,gpu.end(),[](auto b){return b==0;}));
  cache.Clear();
  CHECK(upload(a,1)!=changed);
  std::cout << "Alternating constants: 1002 requests, 2 uploads before mutation.\n";
  return true;
}

bool PooledStorageAndCpuPayloadLifetime() {
  WorkUploadCache<IndexBounds,4> cpu(16,65536);
  WorkUploadCache<uint32_t,4> gpu(16,65536);
  IndexUploadDescription d;d.count=4;
  std::array<uint32_t,4> raw{4,7,12,20}, converted{};
  auto source=std::span(reinterpret_cast<const uint8_t*>(raw.data()),sizeof(raw));
  auto key=IndexUploadKey(d,17);
  IndexBounds bounds;
  CHECK(BuildIndexUpload(d,[&](uint32_t i){return raw[i];},converted,bounds));
  const auto output=std::span(reinterpret_cast<const uint8_t*>(converted.data()),sizeof(converted));
  CHECK(cpu.Insert(key,source,bounds,output));
  const void* storage=cpu.storage_identity();
  for(uint32_t work=1;work<=1000;++work) {
    gpu.Clear();
    CHECK(!gpu.Find(key,source));
    std::span<const uint8_t> bytes,snapshot;
    const auto* cached=cpu.Find(key,source,&bytes,&snapshot);
    CHECK(cached && cached->max()==20 && bytes.size()==sizeof(converted));
    CHECK(!std::memcmp(bytes.data(),converted.data(),bytes.size()));
    CHECK(gpu.Insert(key,snapshot,work));
    CHECK(*gpu.Find(key,source)==work);
  }
  // Exact comparison must reject a same-address write, even with a colliding
  // supplied digest. The old CPU payload must still contain its original data.
  raw[2]=99;
  CHECK(!cpu.Find(key,source));
  raw[2]=12;
  CHECK(cpu.Find(key,source));
  for(uint32_t epoch=0;epoch<100;++epoch) {
    cpu.Clear();CHECK(!cpu.Find(key,source));
    CHECK(cpu.Insert(key,source,bounds,output));
    CHECK(cpu.storage_identity()==storage);
  }
  CHECK(cpu.shadow_bytes()==sizeof(raw)+sizeof(converted));
  WorkUploadCache<int,1> full(1024,8192);
  std::array<uint8_t,8> input{};
  for(uint64_t epoch=0;epoch<25;++epoch) {
    full.Clear();
    for(uint64_t i=0;i<1024;++i) CHECK(full.Insert({i},input,int(i)));
    CHECK(!full.Insert({1025},input,1));
    for(uint64_t i=0;i<1024;++i) CHECK(*full.Find({i},input)==int(i));
  }
  std::cout << "CPU conversion survives 1000 GPU works; pooled resets retain storage.\n";
  return true;
}

bool RegisterVersionsAndPackingLayouts() {
  GenerationUpload<uint32_t> upload;
  CHECK(!upload.Find(0,0));
  upload.Store(2,8,256);
  CHECK(*upload.Find(2,8)==256);
  CHECK(!upload.Find(3,8) && !upload.Find(2,9));
  upload.Store(3,8,512);CHECK(*upload.Find(3,8)==512);

  ConstantPackingPlan plan;
  std::array<ConstantCopyRange,3> ranges{{{4,0,4},{8,4,4},{0,16,4}}};
  CHECK(plan.Configure(16,32,ranges));
  CHECK(plan.range_count()==2);
  std::array<uint8_t,16> source{};
  for(uint32_t i=0;i<source.size();++i)source[i]=uint8_t(i+1);
  CHECK(plan.Update(source));
  const auto revision=plan.revision();
  const auto bytes=plan.bytes();
  CHECK(!std::memcmp(bytes.data(),source.data()+4,8));
  CHECK(!std::memcmp(bytes.data()+16,source.data(),4));
  CHECK(std::all_of(bytes.begin()+8,bytes.begin()+16,[](auto b){return b==0;}));
  CHECK(std::all_of(bytes.begin()+20,bytes.end(),[](auto b){return b==0;}));
  CHECK(!plan.Update(source) && plan.revision()==revision);
  source[6]=99;CHECK(plan.Update(source));CHECK(plan.bytes()[2]==99);
  CHECK(plan.revision()==revision+1);
  CHECK(!plan.Update(std::span(source).first(8)));
  const std::array<ConstantCopyRange,1> bad{{{15,0,4}}};
  CHECK(!plan.Configure(16,32,bad));
  const std::array<ConstantCopyRange,1> bad_output{{{0,30,4}}};
  CHECK(!plan.Configure(16,32,bad_output));
  // Different shader layouts must not share packed data solely by generation.
  ConstantPackingPlan other;
  const std::array<ConstantCopyRange,1> other_layout{{{8,0,4}}};
  CHECK(other.Configure(16,16,other_layout) && other.Update(source));
  CHECK(other.bytes()[0]==source[8] && plan.bytes()[0]==source[4]);
  return true;
}
bool OrderedConstantUpdatesAndWorkReset() {
  ConstantDeltaShadow shadow;
  std::vector<uint8_t> source(801,1);
  CHECK(!shadow.Ready(0,source.size()));
  CHECK(shadow.Initialize(0,source));
  std::vector<uint8_t> gpu(shadow.bytes().begin(),shadow.bytes().end());
  CHECK(gpu.size()==1024 && gpu[801]==0);
  struct Command { uint32_t offset; std::vector<uint8_t> data; };
  std::vector<Command> queued;
  auto emit=[&](uint32_t offset,std::span<const uint8_t> data) {
    queued.push_back({offset,{data.begin(),data.end()}});
  };
  CHECK(shadow.Update(source,emit).commands==0);
  source[16]=4;source[32]=5;source[800]=6;
  const auto changes=shadow.Update(source,emit);
  CHECK(changes.commands==2 && changes.bytes==48);
  CHECK(queued[0].offset==16 && queued[0].data.size()==32);
  CHECK(queued[1].offset==800 && queued[1].data.size()==16);
  CHECK(queued[1].data[1]==0);
  CHECK(gpu[16]==1 && gpu[800]==1); // Earlier draw sees old constants.
  for(const auto& c:queued)std::copy(c.data.begin(),c.data.end(),gpu.begin()+c.offset);
  CHECK(!std::memcmp(gpu.data(),source.data(),source.size()));
  CHECK(gpu[801]==0); // Later draw sees the changed values and zero padding.
  source[16]=7;
  queued.clear();shadow.Update(source,emit);
  CHECK(gpu[16]==4 && queued[0].data[0]==7);
  CHECK(!shadow.Ready(1,source.size()) && !shadow.Ready(0,802));
  CHECK(shadow.Initialize(1,source) && shadow.Ready(1,source.size()));
  CHECK(shadow.bytes()[16]==7 && shadow.bytes()[801]==0);

  // Arbitrary packing sizes, maximum UBO, and the deko3d push-packet limit.
  for(size_t size: {size_t(1),size_t(15),size_t(16),size_t(17),size_t(256),size_t(4096),size_t(65536)}) {
    source.assign(size,0);CHECK(shadow.Initialize(3,source));
    gpu.assign(shadow.bytes().begin(),shadow.bytes().end());
    uint32_t seed=17;
    for(unsigned draw=0;draw<32;++draw) {
      for(size_t i=0;i<size;++i) {seed=seed*1664525u+1013904223u;if(draw==0 || (seed&31)==0)source[i]=uint8_t(seed>>24);}
      queued.clear();shadow.Update(source,emit);
      for(const auto& c:queued) {
        CHECK(c.offset%16==0 && c.data.size()%16==0 && c.data.size()<=ConstantDeltaShadow::kMaxPushBytes);
        CHECK(c.offset+c.data.size()<=gpu.size());
        std::copy(c.data.begin(),c.data.end(),gpu.begin()+c.offset);
      }
      CHECK(!std::memcmp(gpu.data(),source.data(),size));
      CHECK(std::all_of(gpu.begin()+size,gpu.end(),[](auto x){return x==0;}));
    }
  }
  source.resize(65537);CHECK(!shadow.Initialize(4,source));
  std::cout << "Ordered constant deltas preserve earlier draws, padding, work lifetimes and packet limits.\n";
  return true;
}

bool CoalescedConstantsPreserveGapsAndDrawOrder() {
  ConstantDeltaShadow shadow;
  std::vector<uint8_t> source(4096,17);
  CHECK(shadow.Initialize(1,source));
  std::vector<uint8_t> gpu(shadow.bytes().begin(),shadow.bytes().end());
  struct Push { uint32_t offset;std::vector<uint8_t> bytes; };
  std::vector<Push> pushes;
  auto emit=[&](uint32_t offset,std::span<const uint8_t> bytes) {
    pushes.push_back({offset,{bytes.begin(),bytes.end()}});
  };
  for(uint32_t i=0;i<4096;i+=64)source[i]=31;
  const auto result=shadow.Update(source,emit);
  CHECK(result.commands==1 && result.bytes==4048); // 64 formerly separate ranges.
  CHECK(gpu[0]==17 && gpu[4032]==17); // Earlier queued draw remains intact.
  for(const auto& push:pushes)std::copy(push.bytes.begin(),push.bytes.end(),gpu.begin()+push.offset);
  CHECK(gpu==source); // Includes every unchanged register in the merged gaps.
  CHECK(shadow.Update(source,emit).commands==0);
  pushes.clear();source[0]=42;source[96]=43;source[4095]=44;
  CHECK(shadow.Update(source,emit).commands==3); // 80-byte gap is not merged.
  for(const auto& push:pushes)std::copy(push.bytes.begin(),push.bytes.end(),gpu.begin()+push.offset);
  CHECK(gpu==source);
  std::cout << "Sparse constant update: 64 ranges become 1 push, preserving gaps and prior draws.\n";
  return true;
}

bool CommandStateSurvivesHelpersAndStageTransitions() {
  using S=CommandStateCache;
  S cache;
  using Key=std::array<uint64_t,4>;
  std::array<Key,S::Count> desired{},hardware{};
  for(size_t i=0;i<desired.size();++i)desired[i]={i+1,17,32,63};
  uint32_t emitted=0;
  auto draw=[&](bool enabled=true) {
    for(size_t i=0;i<desired.size();++i)
      if(cache.Update(S::Slot(i),desired[i],enabled)){hardware[i]=desired[i];++emitted;}
    return hardware==desired;
  };
  CHECK(draw() && emitted==S::Count);
  for(unsigned i=0;i<1000;++i)CHECK(draw());
  CHECK(emitted==S::Count);
  // All fields of a key matter: e.g. changed UBO extent at the same GPU address,
  // stencil masks, viewport/scissor extents and render-target depth identity.
  for(size_t slot=0;slot<desired.size();++slot)
    for(size_t field=0;field<4;++field) {
      ++desired[slot][field];const auto before=emitted;
      CHECK(draw() && emitted==before+1);
    }
  const Key shared_address{256,32,512,64};
  desired[S::UniformVS]=desired[S::UniformPS]=desired[S::UniformGS]=shared_address;
  CHECK(draw());
  desired[S::Shaders][2]=0; // Disable geometry stage; later re-enable it.
  desired[S::UniformGS]={};CHECK(draw());
  desired[S::Shaders][2]=21;desired[S::UniformGS]=shared_address;CHECK(draw());
  // Helpers, present, image replacement at the same address, and work rotation
  // can all change actual hardware state without changing guest registers.
  for(int helper=0;helper<5;++helper) {
    for(auto& key:hardware)key.fill(0);
    cache.Invalidate();const auto before=emitted;
    CHECK(draw() && emitted==before+S::Count);
  }
  const auto before=emitted;
  CHECK(draw(false) && draw(false) && emitted==before+2*S::Count);
  CHECK(draw() && emitted==before+2*S::Count); // Toggle back to cached state.
  std::array<uint8_t,512> unretained{};
  CHECK(cache.Update(S::Shaders,unretained));
  CHECK(cache.Update(S::Shaders,unretained));
  CHECK(cache.Update(S::Shaders,desired[S::Shaders]));
  std::cout << "State replay: 1000 identical draws skipped; stage changes/helpers/work rotations restore exact state.\n";
  return true;
}

bool DrawBarriersKeepUploadAndTransferVisibility() {
  DrawBarrierPolicy policy;
  using A=DrawBarrierPolicy::Action;
  uint32_t cpu_version=0,visible_version=0,attachment=0,resolved=0,full=0,data=0;
  auto full_barrier=[&] {policy.FullBarrier();visible_version=cpu_version;++full;};
  auto upload=[&] {++cpu_version;policy.CpuUpload();};
  auto draw=[&](bool optimized=true) {
    const auto action=policy.BeforeDraw(optimized);
    if(action==A::Full)full_barrier();
    else if(action==A::InvalidateData){visible_version=cpu_version;++data;}
    if(visible_version!=cpu_version)return false;
    attachment+=visible_version;return true;
  };
  policy.BeginWork();upload();CHECK(draw() && full==1 && data==0);
  for(int i=0;i<100;++i)CHECK(draw());
  CHECK(full==1 && data==0 && attachment==101);
  upload();CHECK(draw() && full==1 && data==1 && attachment==103);
  // Graphics -> transfer -> sampled-image draw keeps both helper barriers.
  full_barrier();resolved=attachment;full_barrier();
  CHECK(draw() && full==3 && data==1 && resolved==103);
  // Compute/clear helpers likewise retain synchronization, and uploads after
  // a helper still need a fresh data invalidation before their consumer.
  full_barrier();resolved=0;full_barrier();upload();
  CHECK(draw() && full==5 && data==2);
  policy.BeginWork();CHECK(draw() && full==6);
  CHECK(draw(false) && draw(false) && full==8);
  CHECK(draw() && full==8);
  std::cout << "Draw synchronization: uploads visible, transfer barriers retained, redundant full drains omitted.\n";
  return true;
}

bool PersistentIndicesRequireExactCpuIdentity() {
  struct Conversion { uint64_t identity; };
  WorkUploadCache<Conversion,1> cpu(8,4096);
  PersistentVertexCache gpu_cache(4*PersistentVertexCache::kPageBytes);
  std::array<std::vector<uint8_t>,4> slots;
  for(auto& s:slots)s.resize(gpu_cache.capacity_bytes());
  std::array<uint8_t,128> source{};
  uint64_t identity=0,hits=0;
  for(uint64_t work=0;work<100;++work) {
    gpu_cache.BeginWork(work,4); // Simulates waiting on this slot's fence.
    if(work==20)source[17]=99; // Same key/hash, different bytes.
    if(work==50)cpu.Clear(); // A reset must not recycle the old GPU identity.
    const auto* verified=cpu.Find({1},source);
    if(!verified) {CHECK(cpu.Insert({1},source,{++identity},source));verified=cpu.Find({1},source);}
    CHECK(verified);
    auto resident=gpu_cache.FindOrAllocate(0,uint32_t(source.size()),verified->identity);
    if(!resident.valid)continue; // First-observation admission uses transient upload.
    auto& storage=slots[work%4];
    if(!resident.hit)std::copy(source.begin(),source.end(),storage.begin()+resident.offset);
    else ++hits;
    CHECK(!std::memcmp(storage.data()+resident.offset,source.data(),source.size()));
    auto pinned=gpu_cache.FindOrAllocate(0,uint32_t(source.size()),verified->identity);
    CHECK(pinned.hit && pinned.offset==resident.offset);
  }
  CHECK(identity==3 && hits>65);
  gpu_cache.BeginWork(100,4);gpu_cache.DiscardActiveSlot();
  CHECK(!gpu_cache.FindOrAllocate(0,uint32_t(source.size()),identity).valid);
  std::cout << "GPU index residency survives four slots without reusing mutated/reset CPU identities.\n";
  return true;
}
} // namespace

bool Index16MatchesExistingPath() {
  using namespace fear_nx::deko;
  IndexUploadDescription d;d.count=65536;
  std::vector<uint16_t> narrow(d.count);
  std::vector<uint32_t> wide(d.count);
  for(uint32_t endian=0;endian<2;++endian)for(uint32_t clamp=0;clamp<2;++clamp) {
    d.endian=endian;d.minimum=clamp?17:0;d.maximum=clamp?65000:IndexBounds::kIndexMask;
    auto read=[&](uint32_t i){return endian?((i&255)<<8)|(i>>8):i;};
    IndexBounds a,b;
    CHECK(BuildIndexUpload16(d,read,narrow,a));CHECK(BuildIndexUpload(d,read,wide,b));
    CHECK(a.valid()==b.valid() && a.min()==b.min() && a.max()==b.max());
    for(uint32_t i=0;i<d.count;++i)CHECK(narrow[i]==wide[i]);
    CHECK(narrow[65535]==65535); // Not a restart when restart is disabled.
  }
  for(unsigned reason=0;reason<7;++reason) {
    auto bad=d;
    switch(reason){case 0:bad.generated=true;break;case 1:bad.index32=true;break;
      case 2:bad.fan=true;break;case 3:bad.loop=true;break;case 4:bad.reset=true;break;
      case 5:bad.base=1;break;case 6:bad.endian=2;break;}
    IndexBounds bounds;CHECK(!CanUseIndex16(bad));
    CHECK(!BuildIndexUpload16(bad,[](uint32_t i){return i;},narrow,bounds));
  }
  IndexBounds bounds;
  CHECK(!BuildIndexUpload16(d,[](uint32_t){return 65536u;},narrow,bounds));
  WorkUploadCache<unsigned,4> cache(8,1<<20);
  const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(wide.data()),wide.size()*4);
  d.output16=false;const auto key32=IndexUploadKey(d,1);CHECK(cache.Insert(key32,bytes,32));
  d.output16=true;const auto key16=IndexUploadKey(d,1);CHECK(!cache.Find(key16,bytes));
  CHECK(cache.Insert(key16,bytes,16));CHECK(*cache.Find(key32,bytes)==32 && *cache.Find(key16,bytes)==16);
  return true;
}

bool IndexHintsRequireExactBytesAndLiveCacheEntries() {
  using namespace fear_nx::deko;
  IndexValidationMemo memo;
  WorkUploadCache<uint32_t,4> local(16,32768),cpu(16,32768);
  std::vector<uint8_t> source(2048,0x31);
  IndexUploadDescription d;d.count=512;d.index32=true;d.compact16=true;
  uint32_t hashes=0,conversions=0;
  auto lookup=[&] {
    std::array<uint64_t,4> key{};
    const uint32_t* found=nullptr;
    const bool reused=memo.TryReuse(source,[&](uint64_t hash) {
      key=IndexUploadKey(d,hash);found=local.Find(key,source);
      if(!found)found=cpu.Find(key,source);
      return found!=nullptr;
    });
    if(!reused) {
      ++hashes;
      // Deliberate collision: no pointer, epoch or hash can establish equality.
      key=IndexUploadKey(d,1);
      found=cpu.Find(key,source);
      if(!found){cpu.Insert(key,source,++conversions);found=cpu.Find(key,source);}
    }
    if(!found)return uint32_t(0);
    const auto result=*found;
    local.Insert(key,source,result);
    memo.Remember(source,key[0]);
    return result;
  };
  CHECK(lookup()==1&&hashes==1);
  for(int i=0;i<1000;++i)CHECK(lookup()==1);
  CHECK(hashes==1&&memo.stats().hits==1000);
  // A last-byte write inside the same work must invalidate bounds/payloads.
  source.back()^=1;CHECK(lookup()==2&&hashes==2);
  local.Clear();CHECK(lookup()==2&&hashes==2); // verified cross-work CPU snapshot
  cpu.Clear();local.Clear();CHECK(lookup()==3&&hashes==3); // stale hint is no lease
  source[0]^=1;CHECK(lookup()==4&&hashes==4);
  d.base=7;CHECK(lookup()==5&&hashes==5); // source alone doesn't fix semantics
  CHECK(memo.stats().rejected==4);
  std::cout<<"Index hints: 1000 repeated reads reuse one hash; same-work writes, forced collisions and resets require exact validation.\n";
  return true;
}

bool Compact16PreservesRawIndicesBoundsAndPolicy() {
  using namespace fear_nx::deko;
  for(bool generated:{false,true})for(uint32_t topology=0;topology<3;++topology)
  for(uint32_t base:{0u,7u,0xFFFFFFF9u}) {
    IndexUploadDescription d;d.count=64;d.index32=!generated;d.generated=generated;
    d.fan=topology==1;d.loop=topology==2;d.base=base;d.minimum=3;d.maximum=500;
    const auto n=IndexUploadCount(d);
    std::vector<uint32_t> wide(n);
    std::vector<uint16_t> narrow(n,0x1234);
    IndexBounds bounds;
    CHECK(BuildIndexUpload(d,[&](uint32_t i){return generated?i:((i==63)?65535:i*17);},wide,bounds));
    const auto old_min=bounds.min(),old_max=bounds.max();
    CHECK(PackIndexUpload16(d,wide,narrow));
    CHECK(bounds.min()==old_min&&bounds.max()==old_max);
    for(size_t i=0;i<n;++i) {
      CHECK(wide[i]==narrow[i]);
      // Original raw indices are identical; shader base/mask/clamp give the
      // same index even when base wraps or exceeds the 16-bit domain.
      const auto transform=[&](uint32_t raw){return std::clamp(uint32_t((uint64_t(raw)+base)&IndexBounds::kIndexMask),d.minimum,d.maximum);};
      CHECK(transform(wide[i])==transform(narrow[i]));
    }
    const auto before=narrow;
    wide.back()=65536;CHECK(!PackIndexUpload16(d,wide,narrow));CHECK(narrow==before);
    wide.back()=0x1000001;CHECK(!PackIndexUpload16(d,wide,narrow)); // small after 24-bit wrap is insufficient
    wide.back()=65535;d.reset=true;CHECK(!PackIndexUpload16(d,wide,narrow));CHECK(narrow==before);
  }
  IndexUploadDescription d;d.count=3;d.index32=true;
  std::vector<uint32_t> indices{0,65535,17};std::vector<uint16_t> narrow(3);
  CHECK(PackIndexUpload16(d,indices,narrow)&&narrow[1]==65535);
  d.compact16=false;auto old=IndexUploadKey(d,99);
  d.compact16=true;auto packed=IndexUploadKey(d,99);CHECK(old!=packed);
  struct Cached { bool narrow;uint64_t identity; };
  WorkUploadCache<Cached,4> cache(4,1024);
  auto source=[&]{return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(indices.data()),indices.size()*4);};
  CHECK(cache.Insert(packed,source(),{true,1}));
  indices[1]=65536;CHECK(!cache.Find(packed,source()));
  CHECK(cache.Insert(packed,source(),{false,2}));CHECK(!cache.Find(packed,source())->narrow);
  indices[1]=65535;CHECK(!cache.Find(packed,source()));
  CHECK(cache.Insert(packed,source(),{true,3}));CHECK(cache.Find(packed,source())->identity==3);
  std::cout<<"Compact16 preserves raw values, base/wrap/clamp, generated topology and width transitions; restart/high values retain Uint32.\n";
  return true;
}

bool AlignedConversionPayloadsKeepExactValidationAndBudget() {
  using namespace fear_nx::deko;
  WorkUploadCache<uint64_t,2,16> cache(64,8192);
  std::vector<uint8_t> source,payload;
  for(uint64_t id=1;id<=31;++id) {
    source.assign(id,uint8_t(id));payload.assign(id*2,uint8_t(id+1));
    CHECK(cache.Insert({id,9},source,id,payload));
  }
  for(uint64_t id=1;id<=31;++id) {
    source.assign(id,uint8_t(id));payload.assign(id*2,uint8_t(id+1));
    std::span<const uint8_t> converted,verified;
    const auto* found=cache.Find({id,9},source,&converted,&verified);
    CHECK(found&&*found==id&&converted.size()==payload.size());
    CHECK(reinterpret_cast<uintptr_t>(converted.data())%16==0);
    CHECK(reinterpret_cast<uintptr_t>(verified.data())%16==0);
    CHECK(std::equal(payload.begin(),payload.end(),converted.begin()));
    source.back()^=1;CHECK(!cache.Find({id,9},source));
    // Same-key replacement of odd-sized sources must realign the payload.
    CHECK(cache.Insert({id,9},source,id+100,payload));
    CHECK(cache.Find({id,9},source,&converted)&&reinterpret_cast<uintptr_t>(converted.data())%16==0);
  }
  const auto storage=cache.storage_identity();cache.Clear();
  CHECK(cache.Insert({1,9},source,99,payload)&&cache.storage_identity()==storage);
  // 1 byte source + 15 pad + 2 payload, then 14 alignment gap + 18 = 50.
  WorkUploadCache<uint64_t,2,16> bounded(4,49);
  source.assign(1,1);payload.assign(2,2);
  CHECK(bounded.Insert({1,0},source,1,payload));
  CHECK(!bounded.Insert({2,0},source,2,payload));
  CHECK(bounded.Find({1,0},source));
  bounded.Clear();CHECK(bounded.Insert({2,0},source,2,payload));
  WorkUploadCache<uint64_t,2,16> exact(4,50);
  CHECK(exact.Insert({1,0},source,1,payload)&&exact.Insert({2,0},source,2,payload));
  CHECK(exact.shadow_bytes()==36&&exact.capacity_bytes()==50);
  WorkUploadCache<uint64_t,2> ordinary(4,6);
  CHECK(ordinary.Insert({1,0},source,1,payload)&&ordinary.Insert({2,0},source,2,payload));
  std::cout<<"Aligned CPU conversions: exact odd-sized sources, payloads, replacement/reset and fixed arena boundaries preserved.\n";
  return true;
}

int main() {
  if (!(ExactBytesCollisionsAndWorkLifetime() && BoundedMetadataAndLargeUncachedFallback() &&
        Index16MatchesExistingPath() && IndexHintsRequireExactBytesAndLiveCacheEntries() && Compact16PreservesRawIndicesBoundsAndPolicy() &&
        AllIndexSemanticsParticipateInIdentity() && ConversionRestartBoundsAndExpansion() &&
        RepeatedIndexDrawsAndMutableInputs() && AlternatingConstantsAndImmutableCopies() &&
        PooledStorageAndCpuPayloadLifetime() && AlignedConversionPayloadsKeepExactValidationAndBudget() && RegisterVersionsAndPackingLayouts() &&
        OrderedConstantUpdatesAndWorkReset() && PersistentIndicesRequireExactCpuIdentity() &&
        CoalescedConstantsPreserveGapsAndDrawOrder() &&
        CommandStateSurvivesHelpersAndStageTransitions() && DrawBarriersKeepUploadAndTransferVisibility())) return 1;
  std::cout << "Passed draw upload reuse, conversion, mutation and lifetime tests.\n";
}
