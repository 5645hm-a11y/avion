# AVION Evaluation & Commercial SDK License

Version 1.1, 4 November 2026 (1.0: 4 November 2026; 1.1 adds section 1A, free non-commercial redistribution)

Copyright © 2026 Alexandre Heymann. All rights reserved.

This license applies to the AVION media engine and its software development kit as distributed by the author, including
`AVION-0.1-Evaluation.zip`, `avion.dll`, `avion.lib`, the `c_api.h` header, the benchmark tools and results, the sample programs,
the Python package (`avion-*.whl`), the NuGet package (`Avion.Sdk.*.nupkg`), and their documentation (together, the
"Software"). By downloading, installing or using the Software you accept this license. If you do not accept it, do not use the
Software.

## 1. Free evaluation license

Subject to the terms below, the author grants you a worldwide, royalty-free, non-exclusive, non-transferable, revocable license to
install and use the Software, and to modify and run the sample programs, for the following purposes only:

1. **Evaluation**: assessing the Software's suitability for your products or projects.
2. **Benchmarking**: measuring the Software and comparing it with other software, including publishing the results, provided the
   publication identifies the version of the Software and the conditions of the measurement.
3. **Academic research**: research and teaching at universities and non-profit research institutions, including publishing papers
   that describe results obtained with the Software.
4. **Non-commercial prototyping**: building and showing prototypes, demonstrations and proofs of concept that are not sold, licensed,
   offered as a paid service, or used in production.
5. **Non-commercial, academic and personal projects**: using the Software in a project that is not sold, licensed for a fee,
   offered as a paid service, supported by advertising or otherwise provided in the course of a business, including open-source
   and hobby applications used by their authors and by others free of charge.

## 1A. Free non-commercial redistribution

You may redistribute the unmodified prebuilt binaries and packages of the Software (`avion.dll`, `avion.lib`, `c_api.h`, the Python
package and the NuGet package) as part of a project described in section 1, item 3 or 5 (non-commercial, academic or personal),
including inside its installer or archive, provided that:

1. this license accompanies them, and every copyright, license and attribution notice in them is kept;
2. the project's own documentation or "about" information states that it uses AVION and that AVION is licensed under this license;
3. the project is not sold, licensed for a fee, offered as a paid service, supported by advertising, or otherwise provided in the
   course of a business; and
4. the restrictions of section 3 continue to apply to every recipient, including the prohibition on reverse engineering.

Recipients receive the Software under this license directly from the author. A project that becomes commercial needs a commercial
license from that point on (section 2).

## 2. What requires a commercial license

The following are not granted by this license and require a separate written commercial license from the author:

1. **Production deployment**: any use of the Software in a product, service, device, or internal system that is operated for its
   intended purpose rather than for evaluation, including use by end users, customers or employees in their ordinary work, other
   than in a project allowed by section 1, item 5.
2. **Commercial redistribution**: distributing the Software, or any part of it (including `avion.dll` or the packages), inside or
   alongside a product, service, device, installer, container image, or SDK that is sold, licensed, offered for a fee, supported
   by advertising, or otherwise provided in the course of a business.
3. **Source code access**: the engine's source code is not part of this license. Access to it, and any right to modify the engine
   itself, is available only under a commercial license.

To obtain a commercial license, contact the author through the project's website (https://5645hm-a11y.github.io/avion/) or its
repository (https://github.com/5645hm-a11y/avion).

## 3. Restrictions

Except as expressly allowed above (including section 1A), you may not: (a) sell, rent, lease, sublicense or otherwise transfer the Software; (b) reverse
engineer, decompile or disassemble the binaries, except to the extent applicable law expressly permits it notwithstanding this
limitation; (c) remove or alter any copyright, license or attribution notice; (d) use the Software to build a product that
competes with it by reproducing its interfaces or behaviour from its binaries; (e) represent that you are the author of the
Software or that the author endorses your product.

You may share the unmodified evaluation bundle and packages with others for the purposes of section 1, provided this license
accompanies them.

## 4. Third-party material

The Software depends only on components of the Windows operating system. Sample media used in the showcase (Big Buck Bunny,
© Blender Foundation, CC BY 3.0) is subject to its own license.

## 5. Feedback

If you give the author suggestions or feedback about the Software, the author may use them without obligation to you.

## 6. Ownership

The Software is licensed, not sold. The author keeps all rights, title and interest in the Software, including all intellectual
property rights, except the limited rights granted to you in section 1.

## 7. Termination

This license ends automatically if you breach it. The author may also end the evaluation license for any user with 30 days'
notice (copies already redistributed under section 1A before that notice may continue to be used under this version), or publish a newer version of this license that applies to later releases. When the license ends, you must stop using
the Software and delete your copies. Sections 3, 5, 6, 8 and 9 survive termination.

## 8. No warranty

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. THE SOFTWARE IS EVALUATION SOFTWARE AND IS NOT INTENDED FOR
SAFETY-CRITICAL USE, INCLUDING VEHICLE CONTROL, MEDICAL DIAGNOSIS OR ANY USE WHERE FAILURE COULD CAUSE INJURY OR DAMAGE. TELEMETRY
SUCH AS PULSE RATE, TIME TO CONTACT OR VIBRATION IS AN ESTIMATE FROM VIDEO AND MUST NOT BE RELIED ON AS A MEASUREMENT.

## 9. Limitation of liability

TO THE MAXIMUM EXTENT PERMITTED BY LAW, IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR ITS USE. WHERE LIABILITY CANNOT
BE EXCLUDED, IT IS LIMITED TO THE AMOUNT YOU PAID FOR THE SOFTWARE UNDER THIS LICENSE (ZERO FOR THE FREE EVALUATION LICENSE).

## 10. General

This license is the entire agreement between you and the author about the Software under the free evaluation license; a
commercial license, when granted, governs the uses it covers. If a provision is held unenforceable, the rest remains in effect.

---

**In short** (not a substitute for the text above): try it, measure it, research with it and prototype with it for free; use it
and ship the unmodified binaries in non-commercial, academic and personal projects for free, with this license and its notices;
ask for a commercial license before you ship it in anything commercial, sell it, or want its source.
