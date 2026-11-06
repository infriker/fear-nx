// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_image_ops.h"
#include <uam_api.h>
#include <cstring>
#include <iostream>
int main() {
  using namespace fear_nx::deko;
  uam::init(true);
  const auto depth_ms = MultisampledShader(kDepthStencilCopyFragment);
  const char* fragments[] = {kImageCopyFragment, kSceneResolveFragment, kMultisampleCopyFragment,
                             kDepthStencilCopyFragment, depth_ms.c_str()};
  const auto check = [](const auto& shader) {
    if (shader.ok && shader.dksh.size() >= 24 && !std::memcmp(shader.dksh.data(), "DKSH", 4))
      return true;
    std::cerr << shader.log << '\n';
    return false;
  };
  bool ok = check(uam::compile(uam::Stage::Vertex, kImageCopyVertex));
  for (const char* fragment : fragments)
    ok = check(uam::compile(uam::Stage::Fragment, fragment)) && ok;
  uam::shutdown();
  if (!ok) return 1;
  std::cout << "Color, multisample and depth/stencil copy shaders compiled to DKSH.\n";
}
