# Region allowlist generator

`gen-region-allowlist.py` writes `../region_allowlist.inc`, the `aws`-partition region names the
client accepts. The list is generated rather than typed, so it cannot quietly
gain a name nobody published; `--check` fails when the checked-in list differs from the one this
data generates, and an offline C++ test runs it. It runs on POSIX systems only: write mode calls
`os.fchmod`, which Python lacks on Windows before 3.13.

## Vendored endpoint data

`endpoints-aws.json` is the `aws` partition of the AWS SDK for C++'s published `endpoints.json`:
its `partition`, `dnsSuffix`, `regionRegex` and `regions`, sorted and pretty-printed, beside an
`extractedFrom` field naming the tag and the published file's SHA-256. It is an extract rather than
the published file, so that a review sees the data it changes. Under Apache-2.0 section 4(b) it is
a modified file: no kept value was changed, but everything else in the published file was dropped
(the other seven partitions, the aws partition's `defaults`, `partitionName` and per-service
endpoints, and the file's `version`), the
`extractedFrom` field was added, and the result was re-serialised with sorted keys. The banner the
generator writes into the list records that the names came through this extract.

It is vendored rather than downloaded because the test that runs `--check` has no network.

- Source: https://raw.githubusercontent.com/aws/aws-sdk-cpp/1.11.922/tools/code-generation/endpoints/endpoints.json
- Repository: https://github.com/aws/aws-sdk-cpp, tag `1.11.922` (commit
  `c6a8bace2080dbf3be133e4ff2ef0dbdd84ed3c5`)
- Version: aws-sdk-cpp 1.11.922
- SHA-256 of the published file: `ba0ede6b9090b721c9def5535f539d536e40cfba738949ae6ea60218c54b54fc`
- Size of the published file: 1,251,661 bytes
- SHA-256 of the extract: `25162e065bf100267744f449ebe93107a9502bee8a8db734016b72cb87fb55a2`

Re-deriving the extract needs the published file and so the network. No test does it end to end:
an offline test renders small published-format data the way `--extract` does. The recipe below is
the check to run by hand when bumping the pin, or to answer "is this really that file", and its
`cmp` covers the `extractedFrom` field too:

```
curl -sS --fail --proto '=https' --max-filesize 4000000 \
  https://raw.githubusercontent.com/aws/aws-sdk-cpp/1.11.922/tools/code-generation/endpoints/endpoints.json \
  | python3 -I gen-region-allowlist.py --extract - | cmp - endpoints-aws.json
```

`--max-filesize` allows about three times the published file's size (curl 8.4.0 or later also
enforces it when the server sends no length), and without `-L` a redirect is not followed.
`--extract` refuses data whose SHA-256 is not the one above, so a corrupted or swapped download
fails rather than being extracted, and it writes the pinned tag and that SHA-256 into the extract's
`extractedFrom` field. Generating the list, and `--check`, refuse an extract whose
`extractedFrom` is not the pin, so a pin bumped without re-deriving the extract fails offline.
They also refuse a vendored extract whose SHA-256 is not the generator's `EXTRACT_SHA256`, the
extract's digest above, so a hand edit that leaves `extractedFrom` alone fails offline too. The
field names the file the extract was cut from; what shows that the data matches that file is still
this record and the re-derivation above.

## Licence

The AWS SDK for C++ is licensed under the Apache License 2.0.

- `LICENSE.aws-sdk-cpp` is that licence, copied unmodified from the same tag
  (https://raw.githubusercontent.com/aws/aws-sdk-cpp/1.11.922/LICENSE, SHA-256
  `b40930bbcf80744c86c46a12bc9da056641d722716c378f5659b9e555ef833e1`).
- `NOTICE.aws-sdk-cpp` is the SDK's `NOTICE.txt` from the same tag
  (https://raw.githubusercontent.com/aws/aws-sdk-cpp/1.11.922/NOTICE.txt, SHA-256
  `8f43c4888a1e14f48c102b3f5556d581d02ea3afc9cbe31a2b714dae9383ef36`), carried unmodified as
  Apache-2.0 section 4(d) asks. Its last line points at the SDK's `LICENSE.txt`, which is not
  vendored. At this tag (https://raw.githubusercontent.com/aws/aws-sdk-cpp/1.11.922/LICENSE.txt, 65
  lines, SHA-256 `d721f7df04e3f82ff104de91a734c4e2ea40e8e4bb2f89dc4d4c7dd6ebbecb40`) that file is
  the Apache-2.0 terms reflowed to one line per paragraph, without the licence's URL and with
  section 4's items numbered rather than lettered, followed by a "Note: Other license terms may
  apply..." paragraph. The note puts the SDK's JSON processing code and Android build logic under
  the MIT License and its XML processing code under the licence at www.grinninglizard.com, and gives
  one MIT-style permission text with the JSON License's added "The Software shall be used for Good,
  not Evil." None of that code is in the extract.

This follows `third_party/unixodbc/README.md`, which records the same things for the vendored
unixODBC headers. The data sits here rather than under `third_party/` because only the ODBC client
reads it, while `third_party/` holds the vendored files of the driver interface, the driver loader
and the build.

## Which SDK version, and why this one

The pinned version is meant to be the one the driver bundles.

The driver's changelog for 2.3.1 names only Windows' SDK upgrade (to 1.11.929), and states no SDK
version for the other platforms. For one platform there is stronger evidence than a source
statement. The public 2.3.1.0 Linux x86_64 driver library carries the string `1.11.922` beside
`aws-sdk-cpp`, in the dependency table the driver fills from
`AWS_SDK_VERSION_STRING` at `src/odbc/rsodbc/rsversion.cpp` and logs when the first environment
handle is allocated (`src/odbc/rsodbc/rsutil.c`, `initODBC`), and no other
`1.11.x` string appears in it.

There is one pin. Check it against each platform's driver when the driver lock changes; the
driver's changelog shows Windows-only SDK upgrades, so the platforms can differ. The pin follows
Linux x86_64, whose library carries the version string (above): a difference there means bumping the
pin, the data and the digests ("Bumping the pin" below), and a difference on another platform does
not.

For 2.3.1.0 the Linux libraries (x86_64 and arm64) and the macOS library carry `1.11.922`, and the
Windows `rsodbc.dll` carries `1.11.929`; 2.3.0.0's Linux library carried `1.11.911`.

The region names this file yields do not actually move with every SDK release: `endpoints.json` is
byte-for-byte identical at tags 1.11.903, 1.11.911 and 1.11.922, so the digest above does not on its
own tell those tags apart.

## Bumping the pin

1. Set `SDK_TAG` and `PUBLISHED_ENDPOINTS_SHA256` in `gen-region-allowlist.py` to the new tag and
   the SHA-256 of `tools/code-generation/endpoints/endpoints.json` at it. Until step 2, the
   generator and `--check` refuse the old extract, whose `extractedFrom` names the old pin, and
   until step 3 they refuse the new one, whose SHA-256 is not yet `EXTRACT_SHA256`.
2. Re-derive the extract from the new tag, which `--extract` records in it, then re-copy `LICENSE`
   and `NOTICE.txt` from that tag over `LICENSE.aws-sdk-cpp` and `NOTICE.aws-sdk-cpp`. The extract
   goes to a new file that `mktemp` makes, so no symlink is written through; it replaces the old
   one only when `--extract` takes the download, and is removed otherwise. `>|` writes that file
   even with noclobber set (bash `set -C`, zsh `NO_CLOBBER`):

   ```
   new="$(mktemp endpoints-aws.json.XXXXXX)"
   curl -sS --fail --proto '=https' --max-filesize 4000000 \
     https://raw.githubusercontent.com/aws/aws-sdk-cpp/<new tag>/tools/code-generation/endpoints/endpoints.json \
     | python3 -I gen-region-allowlist.py --extract - >| "$new" \
     && chmod 644 "$new" && mv "$new" endpoints-aws.json || { rm -f "$new"; false; }
   ```
3. Update this file: the tag in every link and in the recipe, which then checks the new extract;
   the commit; the version; the published file's size, and the `--max-filesize` bound in both
   recipes if it is no longer about three times that; the five SHA-256 digests (the published
   file, the extract, `LICENSE`, `NOTICE.txt` and `LICENSE.txt`), with the extract's also set as
   `EXTRACT_SHA256` in `gen-region-allowlist.py` (`sha256sum endpoints-aws.json` gives it); the
   description of `LICENSE.txt` and of what the extract dropped, if either changed, with the
   comments above `PARTITION` and `EXTRACT_KEYS` in `gen-region-allowlist.py`; and the section
   "Which SDK version, and why this one".
4. Run `python3 -I gen-region-allowlist.py` and review the change to `../region_allowlist.inc`. A
   region that disappears stops working for customers who were using it, so an unexpected removal is
   a reason to stop rather than to commit.
5. If the `aws` partition's `regionRegex` or another partition's region names changed, update
   `PUBLISHED_REGION_REGEX` in `test/client/odbc/region_allowlist_test.cpp` or
   `OtherPartitionRegions()` in `test/client/odbc/region_validation_test.cpp`, which copy them from
   the data.

No test names the pinned tag or digest: the tests read both from the generated list's banner.
