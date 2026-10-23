// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_image_ops.h"
#include <uam_api.h>
#include <iostream>
#include <cstring>
int main(){
  uam::init(true);
  const auto vs=uam::compile(uam::Stage::Vertex,fear_nx::deko::kImageCopyVertex);
  const auto fs=uam::compile(uam::Stage::Fragment,fear_nx::deko::kImageCopyFragment);
  uam::shutdown();
  for(const auto* result:{&vs,&fs}) {
    if(!result->ok || result->dksh.size()<24 || std::memcmp(result->dksh.data(),"DKSH",4)) {
      std::cerr<<result->log;return 1;
    }
  }
  std::cout<<"Both production image-copy shaders compiled with pinned UAM: "
           <<vs.dksh.size()<<" and "<<fs.dksh.size()<<" bytes.\n";
}
