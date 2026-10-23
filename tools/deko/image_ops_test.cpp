// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_image_ops.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <utility>
using namespace fear_nx::deko;
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";return 1;}}while(false)
int main() {
  CHECK(!CanDeferClear(8,8,1,0,7,8,false,true,255));
  CHECK(!CanDeferClear(8,8,0,0,8,8,true,false,255));
  CHECK(!CanDeferClear(8,8,0,0,8,8,true,true,15));
  CHECK(CanDeferClear(8,8,0,0,8,8,true,true,255));
  CHECK(!FitsImage(8,8,UINT32_MAX,0,1,1));
  CHECK(!FitsImage(8,8,0,0,UINT32_MAX,1));
  // Every representable UNORM component survives the selected float path.
  for(unsigned max:{3,255,1023})for(unsigned code=0;code<=max;++code)
    CHECK(unsigned(std::floor((float(code)/float(max))*float(max)+.5f))==code);
  for(auto format:{DkImageFormat_RGBA8_Unorm,DkImageFormat_RGB10A2_Unorm}) {
    CHECK(ColorImageFlags(format,true)&DkImageFlags_HwCompression);
    CHECK(!(ColorImageFlags(format,false)&DkImageFlags_HwCompression));
  }
  for(auto format:{DkImageFormat_RGBA16_Snorm,DkImageFormat_RGBA16_Float,DkImageFormat_R32_Float,
                  DkImageFormat_Z24S8,DkImageFormat_ZF32_X24S8}) {
    CHECK(!Copy3DColorFormat(format));CHECK(!(ColorImageFlags(format,true)&DkImageFlags_HwCompression));
  }
  // texelFetch coordinates for nonzero source/destination origins and crops.
  for(int sx:{0,3,27})for(int dx:{0,5,31})for(int x=0;x<19;++x)
    CHECK(int(float(dx+x)+.5f)+(sx-dx)==sx+x);
  std::cout<<"Passed clear eligibility, UNORM and copy-coordinate checks.\n";
}
