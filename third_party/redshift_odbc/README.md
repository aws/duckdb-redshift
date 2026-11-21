# The embedded Amazon Redshift ODBC driver

The extension embeds AWS's official Amazon Redshift ODBC driver release and loads it at run time:
on Linux and macOS byte for byte, and on Windows the MSI's files after the lock's reviewed rename
and patch rows, each file checked before and after. Nothing here is built from source, and the
repository holds no driver binary or package. This directory holds what decides which release that
is and what it may do:

| Path | What it is |
|---|---|
| `driver.lock` | The release the build embeds: version, per platform the package and the driver inside it, the notices. |
| `LICENSE`, `NOTICE`, `THIRD_PARTY_LICENSES` | The driver's notices, unchanged; installed beside the driver. |
| `signing/` | The certificates verification pins, per platform; it prints each one's DER SHA-256 on every run. |
| `policy/` | What a driver may export, need and read, reviewed by people. |
| `tools/` | `check-lock.sh`, `fetch-driver.sh`, `verify-release.sh`; helpers in `tools/lib/` (`driver_info.py` writes `driver_info.h`). |

## The lock

`driver.lock` is text, one row per line; `#` starts a comment.

```text
format 1
version 2.3.1.0
status development | release
<platform>  driver  <file name>  <sha256 of the embedded bytes>  <package URL>  <package sha256>  <path in the package>
*           notice  <LICENSE | NOTICE | THIRD_PARTY_LICENSES>  <sha256>  <source>
```

- Platforms are `linux_amd64`, `linux_arm64`, `osx_amd64`, `osx_arm64` and, in `format 2`,
  `windows_amd64`. A platform without a driver row has no driver: `fetch-driver.sh` fails for it,
  and `verify-release.sh` prints no row for a platform it cannot verify.
- `format 2` keeps every format 1 row and adds the Windows set, several files from one MSI: a
  `driver` row and a `library` row per DLL, each `<name> <sha256> <source sha256> <MSI URL> <MSI
  sha256> <extract path>`, where the hash is of the renamed file, the source hash of the file as
  the MSI holds it, and the extract path writes a space as `%20`; a `prerequisite` row per VC++
  runtime DLL with its minimum file version; and the rename's `rename <name> <embedded name>` and
  `patch <name> <offset> <original hex> <new hex>` rows, with, once each, the `name-seed <sha256>`
  the names derive from and the renamed set's `set-hash <sha256>`, which the fetched set must give.
  Each field has one spelling, which `check-lock.sh` enforces.
- A package URL must be under `https://s3.amazonaws.com/redshift-downloads/drivers/odbc/<version>/`,
  the official path of the locked version. On Linux and macOS the driver row's hash is that of the
  package member, embedded unchanged; on Windows each row's first hash is that of the renamed file
  the build embeds, and its second that of the MSI's file.
- A notice's source is the URL of the driver's public repository at the version's release tag,
  where verification takes every notice from. That copy ships: a package may hold each notice once,
  beside the driver, only as a regular file with exactly the tag's bytes, and any other member named
  like a notice fails verification. The Windows MSI's own copies, `LICENSE.txt`, `NOTICE.txt` and
  `THIRD_PARTY_LICENSES.txt`, stay in the MSI and must have the tag's bytes too.
- `tools/check-lock.sh` enforces every rule (version pattern, hash and URL grammar, unique rows,
  file names and paths that are safe to write into generated source, and macOS rows that differ in
  nothing but their platform, since both name the one universal library) and prints the lock in the
  normalised form the other tools read. They read a lock only through it.

There are two kinds of lock:

- **Generated.** Written from the rows `verify-release.sh` prints for each platform. Only a
  generated lock, or an unchanged copy of one, is `status release`.
- **Checked in.** `driver.lock`, for developer builds and the tests. It changes only by reviewed
  change, either as a **development lock** (`status development`, which a release below 2.3.0.0
  must be, and which can never be published) or as an unchanged copy of a generated lock.

The **driver set** of a platform is its driver plus the three notices; on Windows, its renamed
DLLs, under their generated names ("The Windows rename" below), plus the notices. Its manifest is
one line per file, `<sha256>  <size>  <name>`, sorted by name; the **set hash** is the SHA-256 of
the manifest. It names the private directory the loader writes the set to, so builds that embed
the same bytes share one copy and builds with other bytes never collide.
`tools/lib/driver_info.py`, the fetch's last step, writes it, with every file's hash, into the
`driver_info.h` the build embeds from.

## What verification proves

`tools/verify-release.sh <version> <platform>` verifies a release before a lock can name it; every
platform runs on Linux. It trusts only `signing/` and,
on Linux, the system trust store: no option and no environment variable changes the certificates or
the policy, and openssl and the Python helpers run with the variables that could add a trusted
certificate or load other OpenSSL configuration unset (`tools/lib/common.sh`, `no_trust_variables`).
What a host can set for any command it runs (`PATH`, `LD_PRELOAD`, `BASH_ENV`, `TMPDIR`) reaches
these tools too, so run them with a clean environment and a private temporary directory. It needs
OpenSSL 1.1.0 or later.

The chain is checked at the current time, so a release whose signing certificate has expired is not
verified again: it is reused from the recorded lock and the retained package instead (a driver
rollback). `RS_DRIVER_MIRROR`, a directory of previously downloaded release files, and
`RS_DRIVER_LOCK`, the lock that recorded them, make a run read those bytes instead of downloading
when that lock names the version and platform asked for: the retained package must then have the
lock's SHA-256, the rules are the same, and a rollback can therefore regenerate the lock with the
public URL unreachable. With the variable set but no such lock, the run says so and downloads, so it
can be set for every run. The retained files are named by the package's SHA-256:
`<sha256>`, `<sha256>-<notice>` and, for Linux, `<sha256>-signature.bin` and
`<sha256>-certificate.pem` (the driver mirror; the tool tests' cache uses the same layout). Neither
variable changes what is trusted. On Linux, in order:

1. **Before any download.** The version must be one the lock accepts; openssl must be 1.1.0 or
   later and python3, `readelf` and `od` on PATH, with curl too unless the release files come
   from `RS_DRIVER_MIRROR`; the DigiCert Trusted G4 Code Signing
   RSA4096 SHA384 2021 CA1 certificate in `signing/linux/` must be one PEM certificate, and its
   DER SHA-256 (`46011ede1c147eb2bc731a539b7c047b7ee93e48b9d3c3ba710ce132bbdfac6b` for the 2.3.0.0
   and 2.3.1.0 signer) is printed with the result;
   `policy/linux/package-path.txt` must hold a path and a file name the lock accepts; the interface
   header, whose entry points step 7 checks, must be readable; the version must have a release-tag
   rule for the notices (`<a>.<b>.<c>.0`); and the host must have a system CA bundle
   (`trust-store`).
2. **Download.** The package, its detached signature and its certificate, from the official path,
   over HTTPS only, with no redirects, bounded in size and, by curl's own limits, in time, into a
   new directory, through the same helper `fetch-driver.sh` uses; or, with `RS_DRIVER_MIRROR`, the
   retained copies, the package checked against the recorded lock's SHA-256 before it is read.
3. **Chain.** The certificate must chain to DigiCert Trusted Root G4 in the system trust store
   through the pinned intermediate, and through nothing else: the chain is exactly the certificate,
   the pinned intermediate (the only certificate openssl is given besides the trust store, and the
   one at depth 1, whose signature on the certificate openssl has verified) and a root from the
   store. The publisher is proven by chain and identity, not by the certificate's fingerprint,
   because AWS renews it every year; a self-signed certificate, or one from any other CA, with
   AWS's name fails.
4. **Identity.** The certificate must carry the Code Signing usage and, in its subject, exactly
   `jurisdictionC=US`, `jurisdictionST=Delaware`, `serialNumber=4152954`,
   `O=Amazon Web Services, Inc.` and `OU=AWS Redshift`, the extended-validation identity of AWS's
   own Redshift signer.
5. **Signature and package.** The detached signature must verify over the package with the
   certificate's key. The signature covers only the bytes, so the signed package's RPM header must
   then name the driver package, the requested version and the architecture.
6. **Extraction and notices.** The driver member at `policy/linux/package-path.txt` is taken out
   as the one regular file at its path, after the whole archive is checked (no absolute or `..`
   member, no link, device or FIFO at the path, one member there), into a new directory; the
   payload is read with python3 (`tools/lib/rpm.py`), so no `rpm2cpio` is needed. Nothing else is
   written, and nothing from the package is executed. Each notice comes from the release tag (or
   the retained copies) and must equal the checked-in copy, so a notice change stops the run until
   a reviewed change updates the copy. The tag's copy ships: a notice the package holds beside the
   driver must be a regular file with the tag's bytes, and any other member named like a notice fails
   the run, named, before the tag's notices are fetched.
7. **Loading policy and environment names.** Under `policy/linux/`: the exports equal
   `expected-exports.txt`, which must hold every entry point the extension calls, and each listed
   name must be defined once in the default version (`name@@V` or unversioned), as a non-absolute
   function `dlsym` finds: not a data object, a `HIDDEN`, `INTERNAL`, zero-valued or `SHN_ABS`
   function, or only a hidden `name@V`. Before those, the `.dynamic` section readelf reads is the
   table the loader reads at `PT_DYNAMIC`'s address, of type `DYNAMIC`, through the `PT_LOAD` that
   maps it, and ends with `DT_NULL`; and each `PT_LOAD` starts at or above the page-rounded end of
   the one before it, so they are in address order (glibc sizes its reservation from the last
   `PT_LOAD`) and share no page (`dynamic-view`). One `.dynsym` row in readelf's
   ten-field `-SW` form must have type `DYNSYM`, and its link must select an exact-name `.dynstr`
   row in that form, of type `STRTAB`, and no other section may be named exactly `.dynstr` (`readelf
   -tW`), as `readelf -d` and `-V` read names from such a `STRTAB`. `readelf -V` may list at most
   one version-definition section, named `.gnu.version_d`, and must list exactly one version-needs
   section, named `.gnu.version_r`; each corresponding ten-field row has the expected
   type and links to that `.dynstr`. These rows are matched by readelf's `-SW` Name column, where a
   name padded with spaces (`.dynsym `) reads the same; only the `.dynstr` row's exact name is
   pinned, by `readelf -tW`. No name in the selected `.dynstr` may hold a space, a control
   character or a literal `@` (`strings`); every needed library is in `allowed-needed.txt`; readelf
   lists one file of version needs for each `DT_VERNEEDNUM`, at least one when `DT_VERNEED` is set,
   and one name for each need in the `vn_aux`/`vn_next`/`vna_next` chain read from
   `.gnu.version_r`'s section-header offset, a section of at most 1 MiB, up to 4096 files and 65536
   needs. Every glibc version among those names is numeric and at most `glibc-floor.txt`. A crafted
   section table can hide, add or shift sections that `readelf -SW` does not list as the loader sees
   them, and `readelf --dyn-syms` prints the symbols of every `DYNSYM`-typed section, while
   `strings` scans only the matched `.dynsym`'s string table. Readelf also resolves `DT_NEEDED` and
   `DT_SONAME` names through `.dynstr`, not the loader's `DT_STRTAB`, and the walk does not map
   `DT_VERNEED` back to the section offset. For the signed, locked file, these remain accepted
   residuals. The driver may have no RPATH, RUNPATH or text relocations; it must have a
   non-executable stack, no debug sections, and the platform's ELF machine, with program headers
   glibc and readelf count alike (`e_phentsize` 56, `e_phnum` not `PN_XNUM`). Every dynamic entry's
   tag must be one that cannot change which library loads, so `DT_AUDIT`, `DT_DEPAUDIT`,
   `DT_AUXILIARY`, `DT_FILTER` and any tag the rule does not list fail (`dynamic`), as do the
   `DT_FLAGS_1` bits `DF_1_GLOBAL`, `DF_1_LOADFLTR` and `DF_1_PIE` (`flags`). That tag list is in
   `tools/lib/elf_policy.sh` rather than a policy file, because it is the ABI's
   and not this driver's; a new tag is a reviewed code edit. Then every name in the driver that
   could be an environment variable, a `PG` name or one under a prefix in `environment-prefixes.txt`,
   must be classified in `refused-environment.txt`, `known-environment.txt` or
   `ignored-strings.txt`, and every row there that the scan would report must still be found in the
   driver.
8. **Lock rows.** Only then are the platform's rows printed, on stdout and nowhere else, the
   driver's hash being that of the extracted member and the package URL always the official one.
   Notice rows apply to every platform and are identical for all of them; the generated lock is
   assembled from every platform's rows, with each notice row once, and notices whose hashes differ
   are refused.

Verification does not check revocation (no CRL or OCSP): a certificate DigiCert or Apple has revoked
still passes the chain and signature rules. This is an accepted risk. The mitigation is that every
lock is reviewed, a version's first with the signer's status checked by hand, and the build embeds
only the bytes the lock pins, so a package re-signed under the same version fails the fetch until a
reviewed lock change takes it. A lock change must not alter a hash of a version whose lock was
already published.
Once a lock is published, it pins every file's SHA-256, and a rollback regenerates it from the
retained copies, or reuses it unchanged once its signer has expired.

On macOS (`osx_amd64` or `osx_arm64`; `tools/lib/verify_macos.sh`), one universal package holds
one universal library, which both macOS platforms embed unchanged. Either platform verifies that
package once and prints both macOS rows, which differ only in their platform, and the lock checker
refuses macOS rows that differ in anything else. The chain is anchored at the Apple root and
Developer ID intermediate pinned in `signing/macos/`, never at a system trust store, and no macOS
host or LLVM tool is needed. The tools read only the forms 2.3.0.0 and 2.3.1.0 use and refuse every other as
drift, which a reviewed change adds. In order:

1. **Before any download.** As on Linux, but python3 and curl are the tools;
   the run prints both pinned certificates' DER SHA-256 (`signing/macos/AppleRootCA.pem`, Apple
   Root CA from https://www.apple.com/appleca/AppleIncRootCertificate.cer, `b0b1730e...f024`, and
   `DeveloperIDG2CA.pem`, Developer ID Certification Authority (G2) from
   https://www.apple.com/certificateauthority/DeveloperIDG2CA.cer, `f16cd3c5...df3a`, each byte for
   byte the certificate in the 2.3.0.0 package's own signature);
   `policy/macos/package-path.txt` must name a file inside a component's `Payload`; and
   `policy/macos/signer.txt` must be printable ASCII with exactly one `checksum`, `installer` and
   `team` row.
2. **Download.** The universal package from the official path, or its retained copy, as on Linux.
3. **Package signature.** The package is a xar archive whose header names sha1: its table of
   contents names a stored checksum, which must lie inside the file, use `signer.txt`'s algorithm
   (sha1) and equal the checksum of the table of contents. Every file the table of contents
   describes may decode to at most 64 MiB, and all to 128 MiB together, checked before anything is
   decompressed. The package must have one RSA and one CMS signature, naming one certificate, which
   must chain through the pinned intermediate to the pinned root and nothing else, be a Developer ID
   Installer certificate that is not a CA, with only the critical extensions that role has, and be
   `Developer ID Installer: AMZN Mobile LLC` of team `94KV3E626L`, all before either signature is
   parsed; then the RSA signature, and only then the CMS signature, must verify over the stored
   checksum, and the component payload must match the checksums the signed table of contents
   records. So the signature covers every byte taken out. Last, the signed `PackageInfo` beside that
   payload must name the driver, `com.amazon.redshift.odbc2x64`, and the version asked for, as the
   RPM header must on Linux, since the signer signs other products and versions too. 2.3.0.0's and
   2.3.1.0's signatures are SHA-1, which some hosts' crypto policies refuse (RHEL 9 and its rebuilds, by
   default); on such a host the signature rule fails and says so, and verification needs a host
   whose OpenSSL allows SHA-1, such as Amazon Linux 2023, Ubuntu or `manylinux_2_28`.
4. **Extraction and notices.** The library at `package-path.txt` is taken out of the payload, a
   gzip-compressed cpio archive in the odc format, with the same member checks as on Linux; the
   notices are checked as on Linux. Every payload read, the search for notices included, must use
   `signer.txt`'s algorithm. The package signature has authenticated every byte of the library,
   which the lock then pins by SHA-256, so the library's own code signature is not checked again.
5. **Loading policy and environment names.** The library must hold exactly an `x86_64` and an
   `arm64` slice, each a 64-bit dynamic library. For each slice, under `policy/macos/`: every load
   command is one the tool recognises, and none is a form it does not read (chained fixups, a
   separate export trie, `LC_VERSION_MIN_MACOSX`, `LC_DYLD_INFO` rather than `LC_DYLD_INFO_ONLY`, a
   library load form other than `LC_LOAD_DYLIB`, a weak import, or an export that is not regular,
   weak, or weak thread-local); two-level namespaces and exactly one `LC_DYLD_INFO_ONLY`, and every
   bind and lazy bind going, by its library ordinal, to one of the slice's own libraries, never by
   flat or weak lookup or to the host application, with the bind opcodes read to their first done
   opcode, where dyld stops; every library an `LC_LOAD_DYLIB` loads in `allowed-load-dylibs.txt`; no
   `LC_RPATH`; a minimum macOS no newer than `min-os.txt`'s driver row; and the exports equal to
   `expected-exports.txt`, which holds every entry point the extension calls for both slices, each
   with a leading underscore, which the rows leave out, so each row is exactly one name. Weak binds
   name no library: dyld resolves them by name across the process's images, the host's included. So
   a name the slice binds by weak lookup, compared exactly as the slice spells it, must be one it
   exports (as `expected-exports.txt` pins) or one it also binds by ordinal to one of its own
   libraries, and any other fails (`weak-binds`). In 2.3.1.0 that holds for all of them: 2,211 names
   in `x86_64` and 1,293 in `arm64`. Weak definitions still coalesce by name, as for any C++
   library, in both directions: an accepted residual, which the driver's macOS tests check. Then the
   environment-name rule as on Linux, on each slice as its own platform (`osx_amd64` for `x86_64`,
   `osx_arm64` for `arm64`).
6. **Lock rows.** Both macOS driver rows, which name the universal package and library, and the
   notice rows, as on Linux.

A failure names its rule (`version`, `tools`, `openssl`, `certificate`, `policy`, `trust-store`,
`download`, `mirror`, `chain`, `eku`, `subject`, `signature`, `package`, `extract`, `notices`,
`machine`, `dynamic-view`, `strings`, `exports`, `entry-points`, `needed`, `search-path`, `textrel`,
`dynamic`, `flags`, `stack`, `debug`, `glibc`, `environment`; on macOS also `algorithm`,
`checksum-location`, `checksum`, `usage`, `payload`, `product`, `slices`, `load-commands`,
`namespace`, `weak-binds`, `linked-libraries` and `min-os`, a slice's failure led by the slice; on
Windows also `cfb`, `digest`, `signer`, `files`, `cab`, `set`, `image`, `resources`, `imports`,
`prerequisites`, `delay-imports`, `bound-imports`, `forwarders`, `load-flags` and `runtime-names`)
and prints the difference; the steps above are numbered as in `tools/lib/verify_linux.sh`,
`tools/lib/verify_macos.sh` and `tools/lib/verify_windows.sh`.

## Reviewing a policy change

Verification fails when a new driver no longer fits a policy file in the ways step 7 (Linux), step 5
(macOS) or steps 5 and 6 (Windows) check, and the fix is always a reviewed edit to a policy file,
never a flag. What the scans cannot see (a variable outside the prefixes, a name built at run time,
a file opened by a computed name, a UTF-16 or mixed-case name in a Windows file, a bare
`LoadLibrary` name that no file, import or row names, as libpq's `LoadLibraryA("wship6")`, and an
embedded side-by-side manifest's `dependentAssembly` or `<file>` redirect) is found by a review of
the driver's source on every driver update. Read each diff for what it lets the new driver do:

- `expected-exports.txt`: a removed name the extension calls fails by itself; an added one is new
  driver surface.
- `allowed-needed.txt`: a new library is a new host requirement; `host-prerequisites.txt` lists the
  ones outside the base system that customers must install.
- `glibc-floor.txt`: raising it drops older Linux hosts; it follows DuckDB's own floor.
- `package-path.txt`: a new path for the driver in the package.
- `refused-environment.txt`: variables the extension will refuse to connect with, each with the
  earliest phase the driver reads it (`load`, `handle`, `connect`); the loader refuses the `load`
  ones before the driver is loaded, and the client refuses every one before each connect.
  Removing a name, or moving it to a later phase, needs a review of the driver's source behind it.
  A row the driver no longer holds fails unless its comment says why it may be absent.
  `test/client/driver/verify-env_test.sh` pins the list's exact rows and phases (47 pairs for
  2.3.0.0 and 2.3.1.0), so a reviewed change edits that pin too.
- `known-environment.txt`: variables left to the driver: with the connection keyword that
  overrides each (the client refuses to connect when its connection string lacks that
  keyword), or `-` and a reason: either why a variable is left live on purpose, or, marked
  `claim:`, a claim about what the driver does with it, which is to be checked live against the
  driver. Moving a name here from the refused list, or adding a `claim:` row, is the change to read
  most closely. The same suite pins the keyword rows and the rows outside the scanned prefixes,
  which the scan cannot check, so a change to either edits its pin too.
- `ignored-strings.txt`: strings under a scanned prefix that are not variables. A new row must be
  shown not to be one; a variable never goes here.
- `# only in <platform>,...` on a refused, known or ignored row: a name only some platforms'
  drivers hold. It need not be found on the others; a known or ignored row so marked also fails
  where it is found on a platform it leaves out. A refused name is refused on every platform
  whatever its mark. Adding a platform to a known row's list leaves that name live there.
- `macos/expected-exports.txt`: as on Linux, per slice where the slices differ; the macOS library
  also exports its bundled libraries' symbols, so it is long, and a diff is read for new `SQL`
  names and for libraries that come or go. `python3 -I tools/lib/macho.py exports <library>`
  prints the new driver's rows, which replace the old ones below the header. A row also lets the
  driver bind its name by weak lookup (`weak-binds`), so an added row is read as that too.
- `macos/allowed-load-dylibs.txt`: the system libraries and frameworks the library may load; a new
  row is a new host dependency.
- `macos/min-os.txt`: DuckDB's macOS floor and the newest minimum the driver may need; raising the
  driver row drops older macOS hosts, and a row above DuckDB's is a reviewed decision. 2.3.1.0, as
  2.3.0.0, needs macOS 11.7, a host requirement `docs/platforms.md` states (macOS 11 is out of support, and
  11.7 is its last minor release).
- `macos/signer.txt`: the installer name, the team and the one checksum algorithm. A change is a
  new publisher identity or package format, confirmed with the driver's publisher first.
- `windows/product.txt` and `windows/msi-files.txt`: the MSI's ProductName, and every file it holds
  by File key, long name and directory, marked `driver`, `library` or `leave`; `python3 -I
  tools/lib/msi.py record <msi>` prints the rows. An added file is new MSI content to read; a file
  newly marked `library` is a new DLL in the set, renamed and embedded with it.
- `windows/kept-names.txt`: the strings in the set's DLLs that equal a member's bare name and are
  not a load of it, each keyed by the file's SHA-256 and offset with the reason a reviewer read; the
  rename leaves them unchanged. A new row is a string the reviewer has traced to no module lookup.
- `environment-prefixes.txt`: the prefixes the scan reads besides `PG`. Removing or narrowing one
  takes every refused and known name under it out of the drift check: a new variable under it then
  needs no classification, and a listed row under it is no longer checked against the driver. Read
  such a diff as dropping those names from review.
- `config-files.txt`: the configuration files the driver and its bundled libraries can read, where
  they look by default, their kind (`ini` for an INI file the driver reads its settings and logging
  from, `other` for the rest), and the variables that override or move them. A new row is a new file
  the driver reads; a removed row needs a trace or a source reading behind it. Every variable a row
  names is refused or known, and a file under `~/`, `./` or a world-writable directory is the one
  to read most closely. The trace in `test/client/driver/driver_config_files_test.cpp` fails when
  the load and the closed-port smoke open a configuration path no row lists. Where `ptrace` is
  refused, or cannot read the traced threads' registers, it warns and stops instead, or fails with
  `REDSHIFT_TEST_REQUIRE_HOST_LIBRARIES=1`; on a host without `libkeyutils.so.1`, where the driver
  cannot load, it stops as every real-driver case does. A location with a backslash is a Windows
  row, a `key` row a registry key; the Linux trace leaves them out and the driver suite checks their
  form.
- `windows/allowed-imports.txt`, `windows/expected-exports.txt`, `windows/runtime-names.txt`: per
  file of the Windows set, the DLLs it imports, the driver's exports, and the DLL names it holds for
  `LoadLibrary`. Each must equal what the file holds, so a new import or run-time name is a new DLL
  the set loads, read as closely as a new `allowed-needed.txt` row; a new VC++ run-time import also
  needs its `host-prerequisites.txt` row; delay imports and the DLLs that forwarders name are listed
  with the imports. `python3 -I tools/lib/windows_policy.py rows <set-dir> rsodbc.dll policy
  <kind>`, the kind being `allowed-imports`, `expected-exports` or `runtime-names`, prints a set's
  rows, which replace the old ones below the header.
- `host-prerequisites.txt`: the libraries outside the base system that customers must install,
  per operating system; the loader names the missing one when the driver cannot load. A row
  follows every new `allowed-needed.txt` entry outside the base system.

The scan only sees literal names. On every driver update the source of the driver and of each
library it bundles is also reviewed for every environment variable it reads, and the lists follow
that review.

## The Windows rename

Windows matches a DLL import against the modules already loaded by base name, so the Windows set's
DLLs are renamed. Three tools in `tools/lib/` do it, in this order:

- `rename_pin.py pin <set-dir> rsodbc.dll`, at pin time, where `<set-dir>` holds the DLLs
  `msi.py check` extracts from the MSI, under the MSI's names, and the lock's three notices
  (`LICENSE`, `NOTICE`, `THIRD_PARTY_LICENSES`), since the seed and set hash cover every file:
  derives each DLL's generated name from the
  name seed (the source manifest and the transform's identity) and its original base name, rewrites
  the imports of set members with delvewheel's routine, rewrites the self-names and the unshipped
  DLLs' names (`policy/windows/unshipped-names.txt`) in place, leaves a member's bare base name
  that is no self-lookup where `policy/windows/kept-names.txt` keeps it (krb5_64 and wshelp64 are
  the only self-names), and prints the `name-seed`,
  `set-hash`, `driver`, `library`, `rename` and `patch` rows. `rename_pin.py names <set-dir>` prints
  the names alone. delvewheel and pefile are pinned by SHA-256 and PyPI URL in
  `tools/lib/rename-requirements.txt` and used nowhere else; `rename_pin.py fetch-tools` downloads
  them into `build/rename-tools/` with `download.sh`, whatever `RS_DRIVER_MIRROR` says, and never
  through pip. On Linux, `make test_driver` runs it first when the lock has `rename` rows. The
  pin imports a private copy of the checked bytes.
- `rename_apply.py <rows> <set-dir> <out-dir>`, which the build runs (`fetch-driver.sh`) when it
  embeds the Windows set: checks each original's SHA-256, the original bytes at each patch and the
  result's SHA-256. It uses no third-party code.
- `rename_check.py <rows> <original-dir> <renamed-dir>`, the check run before a lock names the
  set: written apart from the transform, it confirms each renamed file differs from its original
  only at the places its rename and patch rows record, and, with a search of its own, ignoring
  case, in ASCII and UTF-16LE at every offset, that no member's file name, self-name or unshipped
  DLL's name is left outside the places a DLL may name one. `<original-dir>` holds what the pin's
  `<set-dir>` held, the MSI's files and the three notices, and the checker then recomputes the
  name seed from them and from the transform's identity in its own tree, apart from the pin's
  code: a `name-seed` row it does not give fails, though every generated name derives from it.

The transform's identity holds each `tools/lib/rename-requirements.txt` line's name, version and
SHA-256 (delvewheel's and pefile's), the `VERSION` of `rename_pin.py` and `rename_apply.py`, and
each rewritten string's place and original bytes, so a change to any of them gives every DLL a new
name.

## When verification fails

- `certificate`: `signing/` does not hold one readable PEM certificate, or the certificate
  verification read is not one it can read. Restore it; a new intermediate or root is a reviewed
  change to that file, and its new DER SHA-256 is in the run's output.
  On macOS it can also be a package's signing certificate that openssl cannot read.
- `download`: the official path did not serve the file; nothing is fetched from anywhere else.
- `mirror`: the retained copies are in use and one is missing, not a regular file, too large, or
  not the package the recorded lock names. A mirror no recorded lock goes with is not a failure:
  the run says so and downloads.
- `chain`, `eku`, `subject`, `signature`: the release is not proven AWS's. Do not lock it; a
  changed signer is confirmed with the driver's publisher and then reviewed. On macOS a
  `signature` failure can also be the host's crypto policy refusing SHA-1 signatures (RHEL 9's
  does by default), as the message says: verify on a host whose OpenSSL allows them, without
  changing the host's policy.
- `openssl`: the host's openssl is older than 1.1.0 or does not run; use a working 1.1.0 or later.
- `tools`: python3, `readelf`, `od` or curl is missing; install it.
- `trust-store`: the host has no system CA bundle; install the distribution's CA certificates
  (`ca-certificates`). This is the host, not the release.
- `version`: the version asked for is not four numbers; there are no aliases such as `latest`.
- `policy`: a policy file is missing or malformed; fix it in a reviewed change.
- `package`: the signed package is another version or architecture than the one asked for; do not
  lock it.
- `extract`: the package does not have the expected shape at `package-path.txt`, or its payload is
  not a gzip-compressed cpio archive (on macOS, in the odc format).
- On macOS, `algorithm`, `checksum-location`, `checksum`, `certificate`, `usage` and `payload`
  (with `chain`, `eku`, `subject` and `signature`, as on Linux): the package is not proven AWS's, as for `chain`; do not lock it. `algorithm` can also
  mean `signer.txt`'s algorithm is one `tools/lib/xar.py` does not compute. `load-commands` naming a
  form the tool does not read means the driver moved to it (chained fixups come with a macOS 12
  minimum): a reviewed change adds its rules.
- `product`: the signed package is another product or version than the one asked for, as
  `package` on Linux; do not lock it.
- `notices`: update the checked-in notice in a reviewed change, after checking the new text, and
  the driver's list of components in the root `THIRD_PARTY_LICENSES` with it. A notice the package
  holds out of place, or with other bytes than the tag's, is not locked: the tag's copy ships, so
  read why the package differs before any reviewed change to the rule.
- A policy rule: read the difference, decide whether the new driver is acceptable, and change the
  policy file in a reviewed change with the reason. For `dynamic` and `flags`, whose lists are in
  `tools/lib/elf_policy.sh`, that reviewed change is to the script.
- On Windows, `imports`, `prerequisites`, `delay-imports`, `bound-imports`, `forwarders`,
  `exports`, `load-flags` and `runtime-names` are policy rules, as above. A form `tools/lib/pe.py`
  does not read fails under the rule that reads it: `image` (a 32-bit or .NET image, a debug
  directory, a CodeView record that is not RSDS), `imports` (a delay-import table by virtual
  address, an import table or name outside its section), `exports` (the export directory, its
  tables, names and forwarders) or `version` (the version resource). It means the driver moved to
  that form, and a reviewed change adds its rules.
- `dynamic-view`; `machine` refusing `e_phnum` `PN_XNUM` or an `e_phentsize` other than 56;
  `strings`; `glibc` reporting a malformed, multiply defined, mislinked, incomplete or out-of-bounds
  version-needs structure; or `exports` reporting a malformed or mislinked version definition or an
  expected export that is not one non-absolute function of the default version: no policy file can
  admit it. The file is not what a linker writes, or the driver's interface changed; do not lock it,
  and raise it with the driver's publisher. A deliberate exception is a reviewed change to
  `tools/lib/elf_policy.sh`.

The tool tests (`test/client/driver/run-tool-tests.sh`) cover every rule with inputs built at test
time: test certificate authorities (one with Apple's certificate shapes), small libraries built with
the flags that break each rule, tiny Mach-O libraries and signed xar packages written by the tests,
and copies of the real drivers with one field changed. The cases that verify the real releases skip
themselves once a signing certificate is outside its validity, which is also when that version can
no longer be verified. The cases that sign or verify a macOS package (all of
`verify-macos-package_test.sh`) skip, saying so, where openssl refuses SHA-1 signatures (RHEL 9's
default policy; Amazon Linux 2023's allows them). They need the network only once, to cache the
locked release files (each Linux package, its signature and certificate; the one universal macOS
package, whose signature is inside it; the one Windows MSI, whose signature is inside it; and any
notice from the release tag) under `build/driver-package-cache/`, and none when `RS_DRIVER_MIRROR`
holds them. The two rename wheels are not release files: on Linux, `make test_driver`'s
`rename-tools.sh` step fetches them before the tool tests, from their pinned URLs whatever
`RS_DRIVER_MIRROR` holds, into `build/rename-tools/`, where an offline host puts each as
`<sha256>.whl` instead.
