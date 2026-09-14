# Upstream pin

| Field | Value |
| --- | --- |
| Upstream | https://github.com/fanvanzh/3dtiles |
| GeoForge source snapshot | `Nicander93/3dtiles` path `engines/3dtiles-converter` at commit `bdb8b7fd31c1eaa7ef65ad9f3419c6ab8c56c149` |
| Validation | HK LandsD `11-NW-10B` 5×4 OSGB converted via lineage runtime (`winner1/3dtiles:1.0` / same CLI contract); TopRebuild consumed output |
| Platform | windows-x64 Release |

Do not treat `fanvanzh/3dtiles` tag `v0.4` (2021) as this release baseline.

## CLI contract (product)

```text
_3dtile.exe -f osgb -i <INPUT> -o <OUTPUT> [-c <CONFIG>] [-v]
```

Top-level rebuild is **not** part of this converter; GeoForge uses `top_rebuild` separately.
