// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_small_index_upload.h"
#include "deko/deko_draw_upload_cache.h"
#include "deko/deko_vertex_range.h"
#include "deko/deko_index_write.h"
#include "deko/deko_index_batch.h"
#include "deko/deko_vertex_write.h"
#include "deko/deko_shared_geometry.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

namespace {
using namespace fear_nx::deko;
#define CHECK(x) do { if (!(x)) { std::cerr << __func__ << ':' << __LINE__ << ": " #x "\n"; return false; } } while (false)

struct Heap {
  std::vector<uint8_t> bytes;
  uint32_t used=0, limit=0, writes=0, written=0;
  explicit Heap(uint32_t size):bytes(size,0xCD),limit(size){}
  SmallIndexUpload::Upload Allocate(uint32_t size,uint32_t alignment) {
    uint32_t first=(used+alignment-1)&~(alignment-1);
    if(first>limit||size>limit-first)return {};
    used=first+size;return {bytes.data()+first,uint64_t(0x100000)+first};
  }
  void Write(uint8_t* target,const uint8_t* source,uint32_t size) {
    ++writes;written+=size;std::memcpy(target,source,size);
  }
};

bool CapturesSourceAndPacksOdd16BitPayloads() {
  Heap heap(8192);
  SmallIndexUpload batch;
  auto allocate=[&](auto n,auto a){return heap.Allocate(n,a);};
  auto write=[&](auto* d,const auto* s,auto n){heap.Write(d,s,n);};
  std::vector<uint8_t> input{1,2,3,4,5,6}; // three 16-bit indices
  std::vector<SmallIndexUpload::Upload> views;
  for(int i=0;i<100;++i)views.push_back(batch.Append(input,allocate,write));
  input.assign(100000,0x77); // release/overwrite conversion storage before drain
  CHECK(heap.writes==0&&batch.pending());
  CHECK(heap.bytes[0]==0xCD);
  batch.Flush(write);
  CHECK(heap.writes==1&&heap.used==4096&&!batch.pending());
  for(auto a:views) {
    CHECK(a&&a.gpu%16==0);
    for(int i=0;i<6;++i)CHECK(a.cpu[i]==i+1);
  }
  CHECK(heap.bytes[4096]==0xCD); // no write outside owned chunk
  batch.Flush(write);CHECK(heap.writes==1);
  return true;
}

bool CapacityFallbackAndPendingWorkAreNotLost() {
  Heap heap(5000);SmallIndexUpload batch;
  auto allocate=[&](auto n,auto a){return heap.Allocate(n,a);};
  auto write=[&](auto* d,const auto* s,auto n){heap.Write(d,s,n);};
  std::vector<uint8_t> source(2048,0xAB);
  auto a=batch.Append(source,allocate,write);
  auto b=batch.Append(source,allocate,write);
  CHECK(a&&b&&a.cpu!=b.cpu&&heap.writes==0);
  CHECK(!batch.Append(std::span<const uint8_t>(source).first(6),allocate,write));
  CHECK(heap.writes==1&&!batch.pending());
  CHECK(std::equal(source.begin(),source.end(),a.cpu));
  CHECK(std::equal(source.begin(),source.end(),b.cpu));
  auto exact=heap.Allocate(6,16);CHECK(exact); // the real renderer's fallback
  heap.Write(exact.cpu,source.data(),6);
  CHECK(heap.used==4102);
  CHECK(!batch.Append({},allocate,write));
  source.resize(2049);
  CHECK(!batch.Append(source,allocate,write));
  CHECK(heap.used==4102&&heap.writes==2);
  return true;
}

bool ConversionCacheResetDoesNotInvalidatePendingBytes() {
  WorkUploadCache<uint64_t,2> conversions{4,16384};
  const std::array<uint64_t,2> key{1,2};
  const std::vector<uint8_t> source(320,0x33),converted(640,0x49);
  CHECK(conversions.Insert(key,source,123,converted));
  std::span<const uint8_t> payload;
  CHECK(conversions.Find(key,source,&payload));
  Heap heap(8192);SmallIndexUpload batch;
  auto write=[&](auto* d,const auto* s,auto n){heap.Write(d,s,n);};
  const auto a=batch.Append(payload,[&](auto n,auto align){return heap.Allocate(n,align);},write);
  CHECK(a);
  conversions.Clear();
  CHECK(conversions.Insert(key,source,456,std::vector<uint8_t>(640,0xEE)));
  batch.Flush(write);
  CHECK(std::equal(converted.begin(),converted.end(),a.cpu));
  return true;
}

bool MultipleWorksKeepSubmittedDataImmutable() {
  Heap heap(4u<<20);SmallIndexUpload batch;
  auto allocate=[&](auto n,auto a){return heap.Allocate(n,a);};
  auto write=[&](auto* d,const auto* s,auto n){heap.Write(d,s,n);};
  struct Draw {SmallIndexUpload::Upload upload;std::vector<uint8_t> expected;};
  std::vector<Draw> draws;
  uint32_t random=0x9A473C21;
  for(uint32_t work=0;work<12;++work) {
    const auto submitted=heap.bytes;
    const auto submitted_end=heap.used;
    for(uint32_t i=0;i<128;++i) {
      random=random*1664525+1013904223;
      std::vector<uint8_t> source(2+2*(random%1024));
      for(auto& b:source){random=random*1664525+1013904223;b=uint8_t(random>>24);}
      auto a=batch.Append(source,allocate,write);CHECK(a);
      draws.push_back({a,source});
      // Other uploads interleave with the reserved chunk, without being
      // overwritten when that chunk is flushed later.
      const auto guard=heap.Allocate(16,16);CHECK(guard);
      std::memset(guard.cpu,0x5A,16);
      draws.push_back({guard,std::vector<uint8_t>(16,0x5A)});
    }
    // Renderer submission/retry/KickRecorder all drain here before Begin.
    batch.Flush(write);CHECK(!batch.pending());
    CHECK(std::equal(submitted.begin(),submitted.begin()+submitted_end,heap.bytes.begin()));
    for(const auto& draw:draws)CHECK(std::equal(draw.expected.begin(),draw.expected.end(),draw.upload.cpu));
  }
  CHECK(heap.writes<12*128/2);
  std::cout << "Synthetic mixed uploads: 1536 payloads -> " << heap.writes << " physical writes\n";
  return true;
}
}

bool VertexBatchPreservesRebasedFetchesAndPriorWork() {
  constexpr uint32_t capacity=4u<<20;
  Heap heap(capacity);SmallVertexUpload batch;
  uint64_t minimum=0;
  auto allocate=[&](uint32_t bytes,uint32_t align)->SmallVertexUpload::Upload {
    if(minimum>0x100000+heap.used)heap.used=uint32_t(minimum-0x100000);
    const auto a=heap.Allocate(bytes,align);return {a.cpu,a.gpu};
  };
  auto write=[&](auto* d,const auto* s,auto n){heap.Write(d,s,n);};
  std::vector<uint32_t> source(65536/4);
  for(uint32_t i=0;i<source.size();++i)source[i]=i*13+7;
  auto bytes=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(source.data()),source.size()*4);
  std::vector<SmallVertexUpload::Upload> views;
  for(int i=0;i<2;++i)views.push_back(batch.Append(bytes,allocate,write));
  CHECK(heap.writes==0);
  batch.Flush(write);CHECK(heap.writes==1);
  const auto old=heap.bytes;
  const auto old_used=heap.used;
  minimum=0x100000+(1u<<20);
  auto high=batch.Append(bytes,allocate,write,minimum);CHECK(high&&high.gpu>=minimum);
  // A later higher minimum must not return an earlier chunk's negative base.
  minimum+=256u<<10;
  auto higher=batch.Append(bytes,allocate,write,minimum);CHECK(higher&&higher.gpu>=minimum);
  source.assign(source.size(),0); // payload was captured, never deferred guest reads
  batch.Flush(write);CHECK(heap.writes==3);
  CHECK(std::equal(old.begin(),old.begin()+old_used,heap.bytes.begin()));
  views.push_back(high);views.push_back(higher);
  for(auto a:views) {
    uint32_t fetch_base;
    const uint32_t start=uint32_t(a.gpu-0x100000);
    VertexWindow window{start,65536};
    CHECK(RebaseVertexWindow(window,start,capacity,fetch_base));
    for(uint32_t i=0;i<65536/4;++i) {
      uint32_t value;std::memcpy(&value,heap.bytes.data()+fetch_base+start+i*4,4);
      CHECK(value==i*13+7);
    }
  }
  // Wrong allocator/minimum is rejected; exact-size caller fallback remains.
  SmallVertexUpload rejected;
  CHECK(!rejected.Append(bytes,[](auto,auto){return SmallVertexUpload::Upload{reinterpret_cast<uint8_t*>(uintptr_t(16)),16};},write,32));
  CHECK(!rejected.pending());
  std::cout<<"Vertex batches preserve full 64 KiB windows, high SSBO bases, immutable earlier work and guest source mutation.\n";
  return true;
}

bool IndexWritesRespectSourceAndOwnedBounds() {
  // ASan sees the source's true allocation end, including unaligned inputs.
  for(uint32_t size:{0u,1u,2u,6u,15u,16u,17u,63u,64u,65u,2048u,65535u,65536u,65537u})
  for(uint32_t source_offset:{0u,1u,7u,15u})for(uint32_t destination_offset:{0u,3u})
  for(bool padded:{false,true}) {
    const uint32_t owned=padded?((size+15)&~15u):size;
    std::vector<uint8_t> source(size+source_offset);
    for(uint32_t i=0;i<size;++i)source[source_offset+i]=uint8_t(i*13+7);
    std::vector<uint8_t> destination(owned+destination_offset+32,0xCD);
    auto* out=destination.data()+16+destination_offset;
    const auto written=WriteIndexBytes(out,size?source.data()+source_offset:nullptr,size,owned);
    CHECK(written==owned);
    for(uint32_t i=0;i<size;++i)CHECK(out[i]==uint8_t(i*13+7));
    for(uint32_t i=size;i<written;++i)CHECK(out[i]==0);
    for(uint32_t i=0;i<16+destination_offset;++i)CHECK(destination[i]==0xCD);
    for(size_t i=16+destination_offset+written;i<destination.size();++i)CHECK(destination[i]==0xCD);
  }
  return true;
}

bool ResidentBatchCapturesOnlyAdjacentOwnedBlocks() {
  Heap heap(3*ResidentIndexUploadBatch::kCapacity+256);ResidentIndexUploadBatch batch;
  auto write=[&](uint8_t* dst,const uint8_t* src,uint32_t bytes){
    ++heap.writes;heap.written+=WriteIndexBytes(dst,src,bytes,bytes);
  };
  std::vector<uint8_t> source{1,2,3,4,5,6};
  for(uint32_t i=0;i<100;++i)CHECK(batch.Append(heap.bytes.data()+64*i,0x100000+64*i,source,64,write));
  source.assign(10000,0xEE);
  CHECK(heap.writes==0&&batch.pending());
  // A gap is an older live cache entry: it must never be padded over.
  CHECK(batch.Append(heap.bytes.data()+6464,0x100000+6464,std::span<const uint8_t>(source).first(6),64,write));
  CHECK(heap.writes==1&&heap.written==6400);
  for(uint32_t i=0;i<100;++i) {
    for(uint32_t b=0;b<6;++b)CHECK(heap.bytes[64*i+b]==b+1);
    for(uint32_t b=6;b<64;++b)CHECK(heap.bytes[64*i+b]==0);
  }
  for(uint32_t i=6400;i<6464;++i)CHECK(heap.bytes[i]==0xCD);
  // CPU-contiguous aliases with non-contiguous GPU addresses cannot merge.
  CHECK(batch.Append(heap.bytes.data()+6528,0x200000,std::span<const uint8_t>(source).first(6),64,write));
  CHECK(heap.writes==2);
  std::vector<uint8_t> too_large(ResidentIndexUploadBatch::kCapacity+1,0x42);
  CHECK(!batch.Append(heap.bytes.data()+6592,0x200040,too_large,uint32_t(too_large.size()),write));
  CHECK(!batch.Append(heap.bytes.data()+6592,0x200040,std::span<const uint8_t>(source).first(6),5,write));
  CHECK(batch.pending());batch.Flush(write);CHECK(heap.writes==3&&!batch.pending());
  batch.Flush(write);CHECK(heap.writes==3);
  std::vector<uint8_t> full(ResidentIndexUploadBatch::kCapacity,0x35);
  CHECK(batch.Append(heap.bytes.data()+65536,0x300000,full,uint32_t(full.size()),write));
  CHECK(batch.Append(heap.bytes.data()+131072,0x310000,std::span<const uint8_t>(source).first(6),64,write));
  CHECK(heap.writes==4);batch.Flush(write);CHECK(heap.writes==5);
  CHECK(std::equal(full.begin(),full.end(),heap.bytes.begin()+65536));
  CHECK(heap.bytes[131072+64]==0xCD);
  std::cout<<"Resident snapshots: 100 adjacent admissions -> one write; gaps, GPU aliases, exact capacity and invalid fallback preserved.\n";
  return true;
}

template <typename Batch> bool ResidentBatchPreservesFencePinnedCacheAndCpuReset() {
  Heap heap(512);Batch batch;
  SharedGeometryCache gpu_cache(2,256,64,128);
  WorkUploadCache<uint64_t,2,16> cpu_cache(8,4096);
  auto write=[&](uint8_t* dst,const uint8_t* src,uint32_t bytes){
    ++heap.writes;heap.written+=WriteIndexBytes(dst,src,bytes,bytes);
  };
  const std::vector<uint8_t> source{9,8,7},payload{1,2,3,4,5,6};
  gpu_cache.BeginWork(0,0);
  std::vector<uint32_t> offsets;
  for(uint64_t id=1;id<=4;++id) {
    CHECK(cpu_cache.Insert({id,0},source,id,payload));
    std::span<const uint8_t> converted;
    CHECK(cpu_cache.Find({id,0},source,&converted));
    CHECK(!gpu_cache.FindOrAllocate(0,6,id).valid); // admission still requires observation
    const auto allocation=gpu_cache.FindOrAllocate(0,6,id);
    CHECK(allocation.valid&&!allocation.hit);
    CHECK(batch.Append(heap.bytes.data()+allocation.offset,0x100000+allocation.offset,converted,64,write));
    offsets.push_back(allocation.offset);
    const auto pending_hit=gpu_cache.FindOrAllocate(0,6,id);
    CHECK(pending_hit.hit&&pending_hit.offset==allocation.offset); // no second write
  }
  cpu_cache.Clear();
  CHECK(cpu_cache.Insert({1,0},source,99,std::vector<uint8_t>(6,0xEE)));
  // Pending work is pinned, so cache pressure cannot recycle its destinations.
  CHECK(!gpu_cache.FindOrAllocate(0,6,5).valid);
  CHECK(!gpu_cache.FindOrAllocate(0,6,5).valid);
  batch.Flush(write);CHECK(!batch.pending()&&heap.writes==2);
  for(auto offset:offsets)CHECK(std::equal(payload.begin(),payload.end(),heap.bytes.begin()+offset));
  const auto submitted=heap.bytes;
  gpu_cache.BeginWork(1,0);
  CHECK(gpu_cache.FindOrAllocate(0,6,1).hit);
  CHECK(!gpu_cache.FindOrAllocate(0,6,5).valid);
  CHECK(heap.bytes==submitted);
  // Work 0 has completed, but work 1 still reads entry 1.
  gpu_cache.BeginWork(2,1);
  const auto fresh=gpu_cache.FindOrAllocate(0,6,5);
  CHECK(fresh.valid&&!fresh.hit&&fresh.offset!=offsets[0]);
  const std::vector<uint8_t> replacement(6,0x71);
  CHECK(batch.Append(heap.bytes.data()+fresh.offset,0x100000+fresh.offset,replacement,64,write));
  batch.Flush(write);
  CHECK(std::equal(payload.begin(),payload.end(),heap.bytes.begin()+offsets[0]));
  CHECK(std::equal(replacement.begin(),replacement.end(),heap.bytes.begin()+fresh.offset));
  for(uint32_t i=192;i<320;++i)CHECK(heap.bytes[i]==0xCD); // inter-slot gap
  return true;
}

bool VertexWritesPreserveRawBytesAndLargeTails() {
  for(uint32_t size:{4u,12u,64u,65532u,65536u,65540u,131072u,(1u<<20)+12})
  for(uint32_t source_offset:{0u,4u,12u})for(bool padded:{false,true}) {
    // The source allocation ends exactly at the final guest word. Every byte
    // pattern is retained; no CPU endian swap may be added to this SSBO path.
    std::vector<uint8_t> source(source_offset+size);
    for(uint32_t i=0;i<size;++i)source[source_offset+i]=uint8_t(i*37+i/256);
    const uint32_t owned=padded?((size+15)&~15u):size;
    std::vector<uint8_t> destination(owned+32,0xCD);
    CHECK(WriteVertexBytes(destination.data()+16,source.data()+source_offset,size,owned)==owned);
    CHECK(std::equal(source.begin()+source_offset,source.end(),destination.begin()+16));
    for(uint32_t i=size;i<owned;++i)CHECK(destination[16+i]==0);
    for(uint32_t i=0;i<16;++i)CHECK(destination[i]==0xCD&&destination[16+owned+i]==0xCD);
  }
  return true;
}

bool ResidentVertexBatchesPreserveWindowsAndFallbacks() {
  constexpr uint32_t stride=1u<<20,capacity=4*stride;
  Heap heap(capacity);ResidentVertexUploadBatch batch;
  SharedGeometryCache cache(4,stride,4096,(512u<<10)-4096);
  cache.BeginWork(0,0);
  auto write=[&](uint8_t* dst,const uint8_t* src,uint32_t bytes){
    ++heap.writes;heap.written+=WriteVertexBytes(dst,src,bytes,bytes);
  };
  std::vector<uint8_t> source(65536);
  for(uint32_t i=0;i<source.size();++i)source[i]=uint8_t(i*17+3);
  const auto expected=source;
  const VertexWindow window{128u<<10,65536};
  std::vector<uint32_t> offsets;
  for(uint64_t identity=1;identity<=5;++identity) {
    CHECK(!cache.FindOrAllocate(0,65536,identity,window.first_byte).valid);
    const auto allocation=cache.FindOrAllocate(0,65536,identity,window.first_byte);
    CHECK(allocation.valid&&!allocation.hit&&allocation.offset>=window.first_byte);
    CHECK(batch.Append(heap.bytes.data()+allocation.offset,0x100000+allocation.offset,source,65536,write));
    offsets.push_back(allocation.offset);
    CHECK(cache.FindOrAllocate(0,65536,identity,window.first_byte).hit);
  }
  CHECK(heap.writes==1&&heap.written==ResidentVertexUploadBatch::kCapacity);
  std::fill(source.begin(),source.end(),0xEE);
  // Larger blocks bypass capture without dropping the pending small block.
  std::vector<uint8_t> large(131076,0x39);
  const uint32_t large_owned=(uint32_t(large.size())+15)&~15u;
  const uint32_t large_offset=3*stride;
  CHECK(!batch.Append(heap.bytes.data()+large_offset,0x100000+large_offset,large,large_owned,write));
  CHECK(batch.pending());
  CHECK(WriteVertexBytes(heap.bytes.data()+large_offset,large.data(),uint32_t(large.size()),large_owned)==large_owned);
  batch.Flush(write);CHECK(heap.writes==2&&!batch.pending());
  CHECK(std::equal(large.begin(),large.end(),heap.bytes.begin()+large_offset));
  CHECK(heap.bytes[large_offset+large_owned]==0xCD);
  for(auto offset:offsets) {
    uint32_t base;
    CHECK(RebaseVertexWindow(window,offset,capacity,base));
    CHECK(std::equal(expected.begin(),expected.end(),heap.bytes.begin()+base+window.first_byte));
  }
  const auto submitted=heap.bytes;
  cache.BeginWork(1,0);
  for(uint64_t identity=1;identity<=5;++identity)CHECK(cache.FindOrAllocate(0,65536,identity,window.first_byte).hit);
  CHECK(heap.bytes==submitted&&heap.writes==2);
  // Insertions separated by an unrelated live region must not merge over it.
  CHECK(batch.Append(heap.bytes.data()+stride,0x100000+stride,expected,65536,write));
  CHECK(batch.Append(heap.bytes.data()+stride+65536+64,0x100000+stride+65536+64,expected,65536,write));
  CHECK(heap.writes==3);batch.Flush(write);CHECK(heap.writes==4);
  for(uint32_t i=stride+65536;i<stride+65536+64;++i)CHECK(heap.bytes[i]==0xCD);
  std::cout<<"Resident vertex batches: 5 x 64 KiB -> 2 writes; high fetch bases, source mutation, cache hits, large direct fallback and gaps preserved.\n";
  return true;
}

bool DeferredIndicesJoinShuffledFreshBlocks() {
  struct alignas(64) Storage {std::array<uint8_t,3*65536> bytes;};
  auto heap=std::make_unique<Storage>();heap->bytes.fill(0xCD);
  DeferredIndexUploadBatch batch;ResidentIndexUploadBatch old;
  uint32_t writes=0,old_writes=0;uint64_t written=0;
  auto write=[&](auto* dst,const auto* src,uint32_t n){++writes;written+=WriteIndexBytes(dst,src,n,n);};
  std::array<uint8_t,6> source{};
  // Every destination is unique but capture order alternates between halves.
  // CPU snapshots must retain their own content until the sorted drain.
  for(uint32_t i=0;i<1024;++i) {
    const uint32_t block=(i%2)*512+i/2;
    source.fill(uint8_t(block+3));
    CHECK(batch.Append(heap->bytes.data()+block*64,0x100000+block*64,source,64,write));
    CHECK(old.Append(heap->bytes.data()+65536+block*64,0x200000+block*64,source,64,
        [&](auto*,const auto*,uint32_t){++old_writes;}));
  }
  CHECK(writes==0&&batch.pending());source.fill(0xEE);
  batch.Flush(write);old.Flush([&](auto*,const auto*,uint32_t){++old_writes;});
  CHECK(writes==1&&written==65536&&old_writes==1024);
  CHECK(batch.stats().merged==1023&&batch.stats().capacity_flushes==0);
  for(uint32_t i=0;i<1024;++i)for(uint32_t j=0;j<64;++j)
    CHECK(heap->bytes[i*64+j]==(j<6?uint8_t(i+3):0));
  for(uint32_t i=65536;i<heap->bytes.size();++i)CHECK(heap->bytes[i]==0xCD);
  // Neither a CPU gap nor a GPU discontinuity may be filled across.
  CHECK(batch.Append(heap->bytes.data()+65536,0x300000,source,64,write));
  CHECK(batch.Append(heap->bytes.data()+65664,0x300080,source,64,write));
  CHECK(batch.Append(heap->bytes.data()+65728,0x400000,source,64,write));
  CHECK(!batch.Append(heap->bytes.data()+65792,0x400040,source,5,write));
  batch.Flush(write);CHECK(writes==4);
  for(uint32_t i=65600;i<65664;++i)CHECK(heap->bytes[i]==0xCD);
  std::vector<uint8_t> full(65536,0x71),large(65537,0x37);
  CHECK(batch.Append(heap->bytes.data()+65536,0x500000,full,65536,write));
  CHECK(!batch.Append(heap->bytes.data()+131072,0x510000,large,65600,write));
  CHECK(batch.pending());
  CHECK(batch.Append(heap->bytes.data()+131072,0x510000,source,64,write));
  CHECK(writes==5&&batch.stats().capacity_flushes==1);
  batch.Flush(write);CHECK(writes==6&&!batch.pending());
  std::cout<<"Shuffled new index admissions: 1024 old writes -> one sorted write; live gaps, CPU/GPU discontinuities and capacity fallback preserved.\n";
  return true;
}

bool DeferredIndicesRespectDelayedGpuReads() {
  struct alignas(64) Storage {std::array<uint8_t,16384> bytes;};
  auto heap=std::make_unique<Storage>();heap->bytes.fill(0xDD);
  SharedGeometryCache cache(4,4096,256,1536);
  cache.PreferSequentialAllocations(true);
  DeferredIndexUploadBatch batch;
  struct Read {uint64_t work;uint32_t offset,bytes;uint8_t value;};
  std::vector<Read> submitted,pending;
  std::mt19937 random(20261023);
  uint32_t writes=0;
  auto intact=[&](const auto& reads) {
    for(const auto& r:reads)for(uint32_t i=0;i<r.bytes;++i)
      if(heap->bytes[r.offset+i]!=r.value)return false;
    return true;
  };
  for(uint64_t work=0;work<200;++work) {
    CHECK(!batch.pending()&&intact(submitted));
    const uint64_t complete=work>3?work-3:0;
    std::erase_if(submitted,[&](const auto& r){return r.work<complete;});
    cache.BeginWork(work,complete);
    if(work%17==16) {
      submitted.clear();cache.BeginWork(work,work);CHECK(cache.BlockRegion(work%4));
      auto offset=uint32_t(work%4)*4096+256;
      std::fill_n(heap->bytes.data()+offset,1536,0xA7);
      submitted.push_back({work,offset,1536,0xA7});continue;
    }
    for(unsigned draw=0;draw<100;++draw) {
      const uint32_t key=random()%40,bytes=2+(key%5)*62;
      const uint8_t value=uint8_t(1+key+(work/9)*41);
      auto a=cache.FindOrAllocate(key,bytes,value);
      if(!a.valid)continue;
      if(!a.hit) {
        std::vector<uint8_t> source(bytes,value);
        CHECK(batch.Append(heap->bytes.data()+a.offset,0x100000+a.offset,source,(bytes+63)&~63u,
            [&](auto* dst,const auto* src,uint32_t n){++writes;WriteIndexBytes(dst,src,n,n);}));
        std::fill(source.begin(),source.end(),0xCC); // conversion cache can be reset immediately
      }
      pending.push_back({work,a.offset,bytes,value});
      CHECK(intact(submitted));
    }
    batch.Flush([&](auto* dst,const auto* src,uint32_t n){++writes;WriteIndexBytes(dst,src,n,n);});
    CHECK(intact(submitted)&&intact(pending));
    submitted.insert(submitted.end(),pending.begin(),pending.end());pending.clear();
    for(uint32_t slot=0;slot<4;++slot) {
      for(uint32_t i=0;i<256;++i)CHECK(heap->bytes[slot*4096+i]==0xDD);
      for(uint32_t i=1792;i<4096;++i)CHECK(heap->bytes[slot*4096+i]==0xDD);
    }
  }
  CHECK(writes&&cache.stats().hits&&cache.stats().evictions&&cache.stats().capacity_misses);
  return true;
}

int main() {
  if(!(CapturesSourceAndPacksOdd16BitPayloads()&&CapacityFallbackAndPendingWorkAreNotLost()&&
       ConversionCacheResetDoesNotInvalidatePendingBytes()&&MultipleWorksKeepSubmittedDataImmutable()&&VertexBatchPreservesRebasedFetchesAndPriorWork()&&
       IndexWritesRespectSourceAndOwnedBounds()&&ResidentBatchCapturesOnlyAdjacentOwnedBlocks()&&
       ResidentBatchPreservesFencePinnedCacheAndCpuReset<ResidentIndexUploadBatch>()&&
       ResidentBatchPreservesFencePinnedCacheAndCpuReset<ResidentVertexUploadBatch>()&&
       VertexWritesPreserveRawBytesAndLargeTails()&&ResidentVertexBatchesPreserveWindowsAndFallbacks()&&
       DeferredIndicesJoinShuffledFreshBlocks()&&DeferredIndicesRespectDelayedGpuReads()))return 1;
  std::cout << "Passed bounded small-index upload, snapshot, fallback and submission lifetime checks.\n";
}
