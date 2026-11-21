# Platforms and the embedded driver

## Platforms

Each release is built for DuckDB v2.0.1 and qualified on the five platforms below before it is
published.

| Platform | Hosts | Prerequisites |
|---|---|---|
| `linux_amd64` | Linux on x86_64 | glibc 2.34 or later, a `libstdc++` with `GLIBCXX_3.4.30` (GCC 12's or later), and `libkeyutils.so.1` (the `keyutils-libs` or `libkeyutils1` package) |
| `linux_arm64` | Linux on Arm64 | The same as `linux_amd64` |
| `osx_arm64` | macOS on Apple silicon | macOS 11.7 or later |
| `osx_amd64` | macOS on Intel | macOS 11.7 or later; qualified on Apple silicon, under Rosetta 2 |
| `windows_amd64` | Windows on x64, tested on Windows Server 2022 | Microsoft's latest Visual C++ v14 x64 redistributable, version 14.44 or later; [Windows](#windows) has the rest |

On Linux, Ubuntu 22.04 and later and Amazon Linux 2023 from release 2023.7.20250331 meet these; Red
Hat Enterprise Linux 8 and 9 and their rebuilds do not (8 has an older glibc, 9 an older
`libstdc++`), though DuckDB itself runs there.

On macOS, a host program built with the hardened runtime loads the embedded driver only if it has
the `com.apple.security.cs.disable-library-validation` entitlement.

DuckDB's other platforms (Linux with musl, `windows_amd64_mingw`, `windows_arm64` and WebAssembly)
have no Amazon Redshift ODBC driver, so the extension is not built for them.

## The embedded driver

Each binary contains the official Amazon Redshift ODBC driver, with its notices: the release that
`third_party/redshift_odbc/driver.lock` records, which the debug log's `Redshift driver loaded` line
names ([Logging](troubleshooting.md#logging)). The driver changes only with a release of the
extension: a newer driver comes as a new release, which you take as
[Installing](install.md#taking-a-new-release) describes. The extension loads only the driver it
embeds, never one installed on the host, and does not use the host's ODBC configuration. Once
loaded, the driver stays loaded until the process ends.

[third_party/redshift_odbc/](../third_party/redshift_odbc/) holds the driver's licence and notices,
and `driver.lock` there records the release the build embeds and the SHA-256 of each of its files.

## Where the driver is written

On the first connect in a process, the extension writes the driver's files into
`.redshift-driver/<set hash>/` in the first usable directory of:

1. `REDSHIFT_DRIVER_DIR`, when it is set. It is then the only one, and must be an absolute path to
   an existing directory, with no symbolic link and no `..`.
2. The directory that holds the extension file.
3. The per-user cache: `$XDG_CACHE_HOME/duckdb-redshift`, or `$HOME/.cache/duckdb-redshift`, on
   Linux; `$HOME/Library/Caches/duckdb-redshift` on macOS (none for root); and
   `%LOCALAPPDATA%\duckdb-redshift` on Windows. It is created, readable by you alone, when missing.

On Linux and macOS, a directory is usable when no other user can change it or any directory above
it: each must be owned by you or root and writable by no one else, except that a root-owned
directory with the sticky bit, such as `/tmp`, may lie above it (but not be it). A directory on a
file system mounted `noexec` is skipped. Windows has its own rules ([Windows](#windows)). The
extension checks the files against the SHA-256s it was built with before every load, and writes them
again if they differ. A later process, or a later release, finds its own set by its hash. The set
holds the driver's three notice files, `LICENSE`, `NOTICE` and `THIRD_PARTY_LICENSES`, beside its
libraries.

When no directory is usable, connecting fails with an error that names each directory it tried, by
its role, and the rule it broke; set `REDSHIFT_DRIVER_DIR` to one only you can write.

`REDSHIFT_DRIVER_DIR` also serves hosts with application control, or a read-only extension
directory: an administrator can provide a directory that already holds this release's
`.redshift-driver/<set hash>/` with its files, read-only, and the extension then loads the files
from there without writing.

[Files on disk](security.md#files-on-disk) says where to install the extension, what a network home
directory needs and what `LD_LIBRARY_PATH` must not name.

## Windows

`windows_amd64` builds are tested on **Windows Server 2022**, on x64. Windows 10 and 11 are not
supported.

### The Visual C++ runtime

Install Microsoft's latest Visual C++ v14 x64 redistributable
([Microsoft's downloads](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist);
Windows may list an installed one as Visual C++ 2015-2022). The driver needs `VCRUNTIME140.dll` and
`VCRUNTIME140_1.dll` at version 14.44.0.0 or later, the minimum this release's
`third_party/redshift_odbc/driver.lock` records.

While the copy the process would use is missing or older, connecting fails with an error that names
the file, the version it needs and the copy it found; install the redistributable, restart the
program and connect again.

A program that loads its own copy of the runtime, as python.org Python does from beside
`python.exe`, keeps using that copy whatever the redistributable installs: when the error names the
copy the program loaded, update the program, or the runtime it ships, instead.

### Where the driver and its trust file are written

On the first connect the extension writes the driver's files, its DLLs under generated names that no
other copy of the same libraries in the process can clash with, into `.redshift-driver\<set hash>\`
in the first usable folder of: `REDSHIFT_DRIVER_DIR` when it is set (then the only one), the folder
that holds the extension file, and `%LOCALAPPDATA%\duckdb-redshift`. A folder is usable when it is
on a local drive, owned by you, SYSTEM or Administrators and writable by no other account; the
extension does not create the one `REDSHIFT_DRIVER_DIR` names.

It exports the roots the Windows certificate store trusts for server authentication, less those its
`Disallowed` store holds, to a trust file in a `trust` folder beside `.redshift-driver`, or, where
that folder cannot hold it, as for a read-only `REDSHIFT_DRIVER_DIR`, in
`%LOCALAPPDATA%\duckdb-redshift\trust\`.

The host's Windows certificate store must hold the Amazon root the Redshift endpoints chain to, as
supported Windows Server images have it. A host without it, such as one that is offline or has
automatic root update turned off, fails to connect until the root is in the store.

### App Control for Business

The extension file and the driver's DLLs are not Authenticode-signed, apart from Microsoft's
`msvcr100.dll`, so a host that enforces an App Control policy has to allow them by hash, or by path
for a `REDSHIFT_DRIVER_DIR` an administrator provides.

The per-file list is this release's `third_party/redshift_odbc/driver.lock`: each `windows_amd64`
`driver` and `library` row names a DLL of the driver's installer, and its first SHA-256 is that of
the file the extension writes, under the name that DLL's `rename` row gives, into the folder its
`set-hash` row names (the second SHA-256 is the installer's own file, the same when the rename
changed none of its bytes). The folder's three notice files, `LICENSE`, `NOTICE` and
`THIRD_PARTY_LICENSES`, are the lock's `*` `notice` rows, each with the SHA-256 of the file as
written.

App Control's hash rules take a file's Authenticode hash, not its SHA-256, so make them from files
you have checked:

1. Connect once, on this host with the policy in audit mode or on another host, so that `LOAD`
   checks the extension file's signature and the connect writes the driver's files.
2. Check that every file in that folder has a SHA-256 the lock lists (`Get-FileHash`).
3. Create the rules for that folder, for example with
   `New-CIPolicy -Level Hash -UserPEs -ScanPath <folder> -FilePath <policy file>`.
4. Create the rule for the extension file on its own, for example with
   `New-CIPolicyRule -Level Hash -DriverFilePath <extension file>`, not with a scan of the folder
   that holds it, which would allow every other extension installed there.
5. Merge them into the policy.

A release with another driver writes another folder, which needs rules of its own.

To allow the driver's files by path instead, an administrator provides a `REDSHIFT_DRIVER_DIR` that
already holds this release's `.redshift-driver\<set hash>\` with its files, all owned by
Administrators or SYSTEM and writable by no other account; the extension then loads the files from
there without writing. The extension file still needs its hash rule, made as above, unless it is
installed in an extension directory that only administrators can write, which a path rule can allow
as well.

Smart App Control in enforcement mode is not supported: it has not been tested with the
extension, whose driver files are unsigned.

### Kerberos

Kerberos and GSS authentication are not supported, on any platform. The driver's Windows files
include MIT Kerberos for Windows libraries, which the driver links and the extension never uses;
they do not load the libraries of an installed MIT Kerberos for Windows.

While a Kerberos variable is set in the process's environment (`KRB5_CONFIG`, `KRB5CCNAME`,
`KRB5_TRACE`, `KRB5_KTNAME`, `KRB5_CLIENT_KTNAME`, `KRB5_KDC_PROFILE`, `KRB5RCACHEDIR`,
`KRB5RCACHENAME`, `KRB5RCACHETYPE` or `GSS_MECH_CONFIG`), the extension does not load the driver,
and connecting fails with an error that names the variable: unset it for the DuckDB process. A
program that removes the variable only from the Windows environment block, as .NET's
`Environment.SetEnvironmentVariable` does, leaves it in the C runtime's copy the driver reads, so
connecting is still refused: unset it before the program starts. The same holds for every
[variable that stops a connect](security.md#environment-variables).

### A user profile whose path is not ASCII

The driver opens files through the process's ANSI code page, so on Windows the extension uses no
folder, and no trust file, whose path holds a character outside ASCII. On such a profile the default
folders are refused, and connecting fails with an error that points to `REDSHIFT_DRIVER_DIR`. Set it
to an existing folder you can write; that one setting moves both the driver and the trust file. The
folder's path must be:

- ASCII, in its long form (not an 8.3 short name);
- free of `}`, junctions and links, and of components that end in a dot or a space;
- short enough that every path the driver uses in it stays within 259 characters and the trust
  file's within 255 bytes (with this release, a folder path of at most 156 characters).

Make it directly under `C:\`, for example `C:\rsdriver`. There it inherits write access for other
accounts (Users or Authenticated Users, by Windows version), so it is refused until you disable its
inheritance, remove the inherited entries and give your own account Full control: in its
Properties, Security, Advanced, or in Command Prompt with:

```bat
icacls C:\rsdriver /inheritance:r /grant:r "%USERDOMAIN%\%USERNAME%:(OI)(CI)F"
```

In PowerShell, write the account as `${env:USERDOMAIN}\${env:USERNAME}`. Where that name does not
resolve, as for a Microsoft Entra ID account, give the account by its SID with the same switches,
with `<SID>` from `whoami /user`:

```bat
icacls C:\rsdriver /inheritance:r /grant:r "*<SID>:(OI)(CI)F"
```

Another drive's root often lets Authenticated Users modify it, and a folder under it is then refused
whatever its own entries. Only a read-only `REDSHIFT_DRIVER_DIR` an administrator provides, on such
a profile, also needs `SSL_CERT_FILE`, set to the ASCII path of a PEM CA bundle.

### An installed Redshift ODBC driver with logging on

When the Amazon Redshift ODBC driver is installed on the host and its `LogLevel` or `Trace` value
under `HKEY_LOCAL_MACHINE\SOFTWARE\Amazon\Amazon Redshift ODBC Driver (x64)\Driver` turns its
logging on, the extension refuses to load its own driver, which would read those values too, until
an administrator turns that logging off. A value counts as off only when it is absent, or a string
(`REG_SZ`) that is empty or `0`.
