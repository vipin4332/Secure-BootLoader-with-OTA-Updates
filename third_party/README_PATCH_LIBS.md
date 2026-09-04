# HPatchLite + tinyuz (vendored decoder subset)

The bootloader links the HPatchLite **decoder** and the **tinyuz** decompressor for
delta OTA:

- [`hpatch_lite/`](hpatch_lite/) — MIT, from
  [sisong/HDiffPatch](https://github.com/sisong/HDiffPatch) `libHDiffPatch/HPatchLite/`
- [`tinyuz/`](tinyuz/) — MIT, from
  [sisong/tinyuz](https://github.com/sisong/tinyuz) `decompress/` (decoder only)

Supported patch profiles on device:

| Compression | `hpi_compressType` | Host `make_delta.py` |
|-------------|-------------------|----------------------|
| None        | `hpi_compressType_no` | default |
| tinyuz      | `hpi_compressType_tuz` | `--compress tuz` (`hdiffi -c-tuz`) |

Host-side **`hdiffi`** from [sisong/HPatchLite](https://github.com/sisong/HPatchLite)
must be on `PATH` (or set `HDIFFI`) to generate patches via
[`tools/make_delta.py`](../tools/make_delta.py).

The bootloader build checks that `bootloader.bin` stays within the 64 KB partition
(see root `Makefile`).
