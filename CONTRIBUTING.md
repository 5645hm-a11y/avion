# Contributing to AVION

Thank you for reading the code. Issues, measurements on your hardware and bug reports are welcome. Please include the
`media_player_demo` lines the report is about (`Playback:`, `Clock:`, `Pacing:` and the `Seek` lines), the GPU, and the file's
codec and container.

## Licensing of contributions

AVION is dual-licensed ([LICENSING.md](LICENSING.md)): the GNU AGPLv3 ([LICENSE](LICENSE)), or a commercial licence. To keep
offering both, the author must be able to license every line under both. Every contribution is therefore made under the
Individual Contributor License Agreement below. **By submitting a pull request, a patch or any other contribution to this
repository, you accept that agreement for that contribution.** Please also end each commit message with this line, so the
acceptance is recorded with the change:

```
CLA: I agree to the AVION Individual Contributor License Agreement v1.0
```

You never need the agreement for a fork: under the AGPLv3 you may change and redistribute AVION freely, on its terms.

## AVION Individual Contributor License Agreement, version 1.0

This agreement is between you ("You") and Alexandre Heymann (the "Maintainer"), the author and copyright holder of AVION (the
"Project", https://github.com/5645hm-a11y/avion). It applies to every Contribution You submit to the Project.

**1. Definitions.** "Contribution" means any original work of authorship, including any change or addition to existing work,
that You intentionally submit to the Maintainer for inclusion in the Project, by a pull request, a patch, an issue attachment,
e-mail or any other form of communication sent to the Project or the Maintainer. Communication that You mark conspicuously in
writing as "Not a Contribution" is excluded.

**2. Copyright licence.** You grant the Maintainer, and anyone who receives software from the Maintainer, a perpetual,
worldwide, non-exclusive, no-charge, royalty-free, irrevocable licence to reproduce, prepare derivative works of, publicly
display, publicly perform, sublicense and distribute Your Contributions and such derivative works. This includes the right to
license them, alone or as part of the Project, under the GNU Affero General Public License version 3 only (AGPL-3.0-only) and,
at the same time, under commercial and proprietary licence terms of the Maintainer's choosing, and to change those terms in the
future.

**3. Patent licence.** You grant the Maintainer, and anyone who receives software from the Maintainer, a perpetual, worldwide,
non-exclusive, no-charge, royalty-free, irrevocable patent licence to make, have made, use, offer to sell, sell, import and
otherwise transfer the Project. This licence covers only those patent claims that You can license and that Your Contribution
necessarily infringes, alone or combined with the Project as it was when You submitted it. If anyone brings a patent claim
against You alleging that the Project or a Contribution infringes a patent, the patent licences granted to that party under
this agreement end on the date that claim is filed.

**4. You keep your copyright.** You keep all right, title and interest in Your Contributions. You may use them for any other
purpose. This agreement grants licences. It does not transfer ownership.

**5. Your representations.** You represent that:

- each Contribution is Your original creation, or You have the right to submit it under this agreement, and You identify in
  the submission any third-party material and its licence;
- You are legally entitled to grant the licences above; and
- if Your employer has rights to intellectual property that You create, You have permission to make the Contribution on its
  behalf, or Your employer has waived those rights for the Contribution.

If You become aware of a fact or circumstance that would make these representations inaccurate, You will notify the Maintainer.

**6. Moral rights.** To the extent the law allows, You waive, or agree not to assert, any moral rights in Your Contributions
against the Maintainer and the recipients of the Project.

**7. No obligation.** The Maintainer may accept, change or decline any Contribution, and is not obliged to include it in the
Project or in any release.

**8. No warranty and no support.** Unless required by law or agreed in writing, You provide Your Contributions "AS IS",
without warranties or conditions of any kind, express or implied, including warranties of title, non-infringement,
merchantability or fitness for a particular purpose. You are not expected to provide support for them.

**9. Acceptance.** You accept this agreement for a Contribution by submitting it. The `CLA:` line above, a signed copy, or a
comment in the pull request records that acceptance, but none of them is required for it.

## The rules the code must keep

[docs/ENGINE_MANIFESTO.md](docs/ENGINE_MANIFESTO.md) has the engine's laws in full. In short, every change must hold to these:

- No third-party multimedia or utility libraries: Win32, COM, Direct3D 11/12, DXGI, DirectComposition, WASAPI and Media
  Foundation's own decoders, and C++20.
- No heap allocation once the engine is `Primed` or `Running`. Memory comes from sealed `LinearMemoryArena`s. The tests run an
  allocation probe around playback, and a single allocation fails them.
- Real-time stages talk only through single-producer single-consumer rings, with their cursors on separate 64-byte lines. No
  mutexes or condition variables in those paths.
- Decoded pictures stay on the GPU. The one exception is the platform's CPU decoder fallback (manifesto, section 2E).
- MSVC x64 `/W4 /WX` and GCC `-Wall -Wextra -Werror` for the portable parts.

## Before you open a pull request

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run the suite serially: GPU playback tests compete with one another under `-j`. On Linux, `bash tools/linux/gcc.sh` builds and
runs the portable suites at `-O2` and with ASan/UBSan. Tests that need third-party footage (the LG, Costa Rica and Dolby demo
reels, Big Buck Bunny in 4K) are registered only when those files are present under `assets/test_fixtures`.
