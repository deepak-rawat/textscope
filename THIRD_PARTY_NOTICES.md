# Third-party dependencies

The application is an independent implementation. No code from klogg or
TextAnalyzer is incorporated.

| Component | Use | Upstream license |
|---|---|---|
| Qt 6 Core, Gui, Widgets, Concurrent, Core5Compat, and transitive DBus | Desktop UI and platform/encoding services | LGPL-3.0 / GPL / commercial; verify the selected distribution |
| Qt Test | Development tests | Qt licensing terms |
| PCRE2 | Native regular expressions | BSD-3-Clause; see the distribution's COPYING/LICENCE and bundled JIT notices |
| GCC runtime libraries | UCRT64 standalone Windows runtime | GPL with GCC Runtime Library Exception; verify the selected distribution |

Builds link to externally supplied dependencies. Distribution must include the
actual pinned dependencies' license files and applicable copyright notices,
including Qt third-party components. This table is not a replacement for those
files or LGPL compliance. Qt deployment scripts copy runtime libraries, not all
legal notices automatically.

The macOS deployment includes the Qt GPL/LGPL license texts, available
Homebrew dependency notices, and the installed Qt upstream SPDX documents
under `Contents/Resources`. Those upstream documents describe the Qt builds;
they are not a complete post-deployment application SBOM.

Standalone UCRT64 ZIPs include the MSYS2 installation's dependency license
directories under `share/textscope/licenses/ucrt64`. These can include notices
for installed development packages as well as deployed runtime libraries.
Audit the exact shipped DLLs, GCC runtime exception terms, Qt third-party
components and source/replacement obligations before publishing.

The locally deployed bundle also has transitive libraries from libb2, Brotli,
OpenSSL, D-Bus, double-conversion, FreeType, GLib, Graphite2, HarfBuzz, ICU,
gettext, MD4C, libpng and zstd. Dependencies vary with the Qt distribution.
Audit their exact versions, source correspondence and notices before public
redistribution; the development package is not a declaration that this audit
has been completed.

Qt LGPL distributions must preserve applicable replacement/relinking rights.
macOS signing/notarization and static builds require particular care. Consult
[Qt licensing](https://doc.qt.io/qt-6/licensing.html) before publishing binaries.

An SBOM with exact dependency versions must be generated from the release build
environment. The repository does not claim that an unversioned system-library
build has a reproducible, fully audited dependency inventory.

Behavioral research:

- [TextAnalysisTool.NET documentation](https://textanalysistool.github.io/)
- [Microsoft's published .tat example](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/mb-basic-connectivity-tat)
- [TextAnalyzer](https://github.com/jay-mao-cn/TextAnalyzer)
- [klogg](https://github.com/variar/klogg)
