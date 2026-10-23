// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_texture_checks.h"
#include "deko/deko_validation_cache.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
using namespace fear_nx::deko;
#define CHECK(x) do {if(!(x)){std::cerr<<__LINE__<<": "<<#x<<'\n';std::exit(1);}}while(false)

void SampleBoundaries() {
  const size_t page=size_t(sysconf(_SC_PAGESIZE)), bytes=page*80;
  auto* p=static_cast<uint8_t*>(mmap(nullptr,bytes+page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  CHECK(p!=MAP_FAILED);CHECK(!mprotect(p+bytes,page,PROT_NONE));
  for(size_t i=0;i<bytes;++i)p[i]=uint8_t(i*31+(i>>8));
  // Every length, including page boundaries and tails, ends at a protected page.
  for(size_t n=0;n<=bytes;++n) {
    auto* start=p+bytes-n;uint64_t reference=77,read=0;
    for(size_t off=0;off<n;off+=4096)if((off/4096)%8==0 || off+4096>=n) {
      auto count=std::min<size_t>(4096,n-off);
      reference=XXH3_64bits_withSeed(start+off,count,reference);read+=count;
    }
    CHECK(TextureChecks::Sample({start,n},77)==reference);
    CHECK(TextureChecks::SampleBytes(n)==read);
  }
  munmap(p,bytes+page);
}

void TexturePolicy() {
  std::vector<uint8_t> base(65536,3),mips(65536,7);
  TextureChecks cache;TextureChecks::State s;
  auto check=[&](uint64_t f){return cache.Check(s,f,0x10000,0,base,mips);};
  auto first=check(0);CHECK(first.bytes==base.size()+mips.size());
  const auto h=XXH3_64bits_withSeed(mips.data(),mips.size(),XXH3_64bits(base.data(),base.size()));
  CHECK(first.hash==h);
  CHECK(!check(0).bytes);
  for(int i=0;i<5;++i)check(s.next);
  CHECK(s.interval==32 && s.sample_valid);
  CHECK(!check(s.next-1).bytes);
  while(cache.stats().training<TextureChecks::kGuardChecks)check(s.next);
  const auto full_before=cache.stats().full_checks;
  for(int i=0;i<7;++i){auto sample=check(s.next);CHECK(sample.bytes==2*TextureChecks::SampleBytes(base.size()));}
  CHECK(cache.stats().full_checks==full_before);
  check(s.next);CHECK(cache.stats().full_checks==full_before+1);
  // A change outside sampled 4 KiB blocks is caught by the recurring full guard.
  mips[4096+9]^=123;
  for(int i=0;i<7;++i)CHECK(check(s.next).hash==h);
  auto missed=check(s.next);CHECK(missed.guard_miss && missed.hash!=h);
  CHECK(cache.stats().excluded_bases==1 && cache.stats().training==0);
  const auto full_after=cache.stats().full_checks;
  for(int i=0;i<1000;++i)CHECK(!check(s.last).bytes);
  CHECK(cache.stats().full_checks==full_after);
  // Excluded addresses retain frame/interval scheduling, but never sample again.
  auto samples=cache.stats().sample_checks;
  for(int i=0;i<8;++i)check(s.next);
  CHECK(cache.stats().sample_checks==samples && s.interval==32);
  mips[0]^=1;
  auto exact=cache.Check(s,s.last,0x10000,0,base,mips,false);
  CHECK(exact.bytes==base.size()+mips.size());
  CHECK(exact.hash==XXH3_64bits_withSeed(mips.data(),mips.size(),XXH3_64bits(base.data(),base.size())));
  TextureChecks::State recreated;CHECK(cache.Check(recreated,s.last,0x10000,0,base,mips).bytes==2*base.size());
}

void GuardLimitAndBudget() {
  std::vector<uint8_t> data(65536,5);
  TextureChecks cache;
  uint64_t frame=0;
  for(uint32_t i=0;i<5;++i) {
    TextureChecks::State s;auto addr=0x10000+i*0x10000;
    auto run=[&](){auto result=cache.Check(s,frame,addr,0,data,{});frame=s.next;return result;};
    for(int n=0;n<4;++n)run();
    CHECK(s.interval>=8 && s.sample_valid);
    data[4096+11]^=uint8_t(i+1);
    CHECK(run().guard_miss);
  }
  CHECK(cache.stats().excluded_bases==4 && cache.stats().guard_misses==5 && cache.stats().sampling_disabled);
  // Budget is deliberately soft: fresh data always wins, and an oversized first
  // request progresses. A stable victim must progress after eight deferrals.
  std::vector<uint8_t> big(TextureChecks::kBudget+4096,11);
  TextureChecks limited;TextureChecks::State stable;
  for(uint64_t f=0;stable.interval<8;f=stable.next)limited.Check(stable,f,0x20000,0,data,{});
  for(int i=0;i<9;++i) {
    uint64_t f=stable.next;TextureChecks::State fresh;
    CHECK(limited.Check(fresh,f,0x40000,0,big,{}).bytes==big.size());
    auto result=limited.Check(stable,f,0x20000,0,data,{});
    CHECK((i<8 && result.bytes==0) || (i==8 && result.bytes>0));
  }
  CHECK(limited.stats().budget_deferrals==8);
}

void GeometryEpochs() {
  std::vector<uint8_t> data(4096,5);
  ValidationCache c;c.SetEnabled(true);c.SetGeometryEpochChecks(true);c.BeginFrame(1);
  c.Hash(data.data(),0x10000,data.size());
  data[2*64+5]^=1;c.Revalidate();
  CHECK(c.Hash(data.data(),0x10000,data.size()).guard_miss);
  auto strict=c.Hash(data.data(),0x10000,data.size());CHECK(strict.bytes_hashed==data.size());
  for(int i=0;i<2000;++i)CHECK(!c.Hash(data.data(),0x10000,data.size()).bytes_hashed);
  // Mutation is intentionally not visible until a real epoch boundary.
  data[99]^=1;CHECK(c.Hash(data.data(),0x10000,data.size()).hash==strict.hash);
  c.Revalidate();auto changed=c.Hash(data.data(),0x10000,data.size());
  CHECK(changed.hash!=strict.hash && changed.hash==XXH3_64bits(data.data(),data.size()));
  CHECK(c.Hash(data.data()+64,0x10040,2048,7).bytes_hashed==2048);
  CHECK(!c.Hash(data.data()+64,0x10040,2048,7).bytes_hashed);
  c.HardInvalidate();CHECK(c.Hash(data.data(),0x10000,data.size()).bytes_hashed==data.size());
  c.SetEnabled(false);CHECK(c.Hash(data.data(),0x10000,data.size()).bytes_hashed==data.size());
  CHECK(c.Hash(data.data(),0x10000,data.size()).bytes_hashed==data.size());
  ValidationCache dynamic;dynamic.SetEnabled(true);dynamic.SetGeometryEpochChecks(true);dynamic.BeginFrame(10);
  dynamic.Hash(data.data(),0x90000,data.size());
  for(int i=0;i<2;++i){data[0]^=1;dynamic.Revalidate();dynamic.Hash(data.data(),0x90000,data.size());}
  for(int i=0;i<100;++i) {
    data[i]^=3;auto fresh=dynamic.Hash(data.data(),0x90000,data.size());
    CHECK(fresh.bypass_cache && fresh.unhashed_geometry && fresh.volatile_geometry && !fresh.bytes_hashed);
    CHECK(!(!fresh.unhashed_geometry && (!fresh.bypass_cache||fresh.volatile_geometry)));
  }
  dynamic.BeginFrame(74);auto reseed=dynamic.Hash(data.data(),0x90000,data.size());
  CHECK(!reseed.bypass_cache && reseed.bytes_hashed==data.size());
}
int main(){SampleBoundaries();TexturePolicy();GuardLimitAndBudget();GeometryEpochs();std::cout<<"NFSMW texture/geometry scheduling regressions passed\n";}
