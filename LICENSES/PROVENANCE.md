# License and notice provenance

The root LICENSE (GNU GPL version 3) is copied verbatim from
Project8Recomp/project8-sw at commit
3393f2615deb7e8ac11ce51d2cd0e9defded5669.

NOTICE is based on the notice from that same Project8 commit. Its project
descriptions and other non-license prose were adapted for F.E.A.R. NX. The
embedded third-party license texts and their required copyright notices were
retained verbatim. The notice credits Rodrigo Ferreira's Project8 Switch port
and renderer integration, records the pinned Project8 source commit, and
preserves the renderer lineage to StevensND's nfsmw-nx.

ReXGlue-BSD-3-Clause.txt is copied from the pinned ReXGlue SDK source tree
(commit c94f5ebdcb3c9d1a460ca48e04f9758448f8d518).
Mesa-MIT.txt is copied from the Mesa source tree used by the NVK stack.
These separate license texts retain the SDK and Mesa terms.

The deko3d branch additionally stages the ReXGlue NX UI/provider sources from
nx-mod/rexglue-sdk-nx at 7d19603450ca25eafcf05e1fdb7c9f77c044c852, UAM from
centollOS/SwitchWakerHD at 2d147aaaa27b1ad0f9b390eff515609af9fe24a3, and
SPIRV-Cross at f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7. The setup script
records these pins and keeps the downloaded trees outside Git.

UAM.txt and SPIRV-Cross-Apache-2.0.txt are copied verbatim from those prepared
dependency trees. UAM's file includes the terms for its Mesa-derived files.
Deko3d itself is supplied by the devkitPro toolchain and retains its upstream
license. The final binary package must include the notices of its linked
dependencies in addition to this repository's source license.

The app-owned deko3d renderer preserves the Project8/NFSMW renderer lineage.
Further cache, geometry, image-operation and CPU-adapter work was informed by
VasilkovND/dantes-inferno-nx and StevensND/nfsmw-nx's deko3d implementation.
The source headers retain applicable notices; see the branch README credits.
