# geoforge-converter

Standalone OSGB → 3D Tiles converter (`_3dtile`) for [GeoForge](https://github.com/Nicander93/3dtiles).

GeoForge products depend on **Release binaries**, not this source tree.

## Upstream

See [docs/UPSTREAM.md](./docs/UPSTREAM.md). Lineage: [fanvanzh/3dtiles](https://github.com/fanvanzh/3dtiles).

## Build (Windows x64)

Requires MSVC, CMake, and vcpkg (`VCPKG_ROOT`).

```powershell
git submodule update --init --recursive
cargo build --release
```

Packaging for GitHub Release is done by CI on tag `v*.*.*` (see `.github/workflows/release-windows.yml`).

## Runtime layout (Windows zip)

```text
converter/
├─ _3dtile.exe
├─ *.dll
├─ osgPlugins-3.6.5/
├─ gdal/
├─ proj/
├─ geoids/          # optional
└─ manifest.json
```

Directory names match `_3dtile` `setup_osg_environment()` (`OSG_LIBRARY_PATH`, `GDAL_DATA`, `PROJ_DATA`).

## License

Preserve upstream copyright notices in this tree. Product packaging must ship LICENSE files with the runtime.
