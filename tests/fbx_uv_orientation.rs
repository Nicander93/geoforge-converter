use std::{
    env, fs,
    path::PathBuf,
    process::Command,
    time::{SystemTime, UNIX_EPOCH},
};

#[test]
fn embedded_fbx_texture_uses_gltf_uv_orientation() {
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let input = repo.join("thirdparty/ufbx/data/blender_293_embedded_textures_7400_binary.fbx");
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let output = env::temp_dir().join(format!("geoforge-fbx-uv-{}-{unique}", std::process::id()));

    let mut command = Command::new(env!("CARGO_BIN_EXE__3dtile"));
    command.args(["-f", "fbx", "-i"]);
    command.arg(&input).arg("-o").arg(&output);
    let windows_dependencies = repo.join("vcpkg_installed/x64-windows/bin");
    if windows_dependencies.is_dir() {
        let path = env::join_paths(
            std::iter::once(windows_dependencies)
                .chain(env::split_paths(&env::var_os("PATH").unwrap_or_default())),
        )
        .unwrap();
        command.env("PATH", path);
    }
    let result = command.output().expect("run converter");
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );

    let tile = fs::read(output.join("tile_0.b3dm")).expect("read converted tile");
    assert_eq!(&tile[..4], b"b3dm");
    let glb_offset = 28
        + [12, 16, 20, 24]
            .into_iter()
            .map(|offset| u32_at(&tile, offset) as usize)
            .sum::<usize>();
    assert_eq!(&tile[glb_offset..glb_offset + 4], b"glTF");
    let json_length = u32_at(&tile, glb_offset + 12) as usize;
    assert_eq!(&tile[glb_offset + 16..glb_offset + 20], b"JSON");
    let gltf: serde_json::Value =
        serde_json::from_slice(&tile[glb_offset + 20..glb_offset + 20 + json_length]).unwrap();
    assert_eq!(gltf["images"][0]["mimeType"], "image/png");

    let accessor_index = gltf["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"]
        .as_u64()
        .unwrap() as usize;
    let accessor = &gltf["accessors"][accessor_index];
    let view_index = accessor["bufferView"].as_u64().unwrap() as usize;
    let view = &gltf["bufferViews"][view_index];
    let binary_offset = glb_offset + 20 + json_length + 8;
    let uv_offset = binary_offset
        + view["byteOffset"].as_u64().unwrap() as usize
        + accessor["byteOffset"].as_u64().unwrap_or(0) as usize;

    // The source FBX starts with UVs (0.375, 0), (0.625, 0),
    // (0.625, 0.25). glTF stores the image with its top row first.
    let expected = [(0.375, 1.0), (0.625, 1.0), (0.625, 0.75)];
    for (index, (u, v)) in expected.into_iter().enumerate() {
        assert!((f32_at(&tile, uv_offset + index * 8) - u).abs() < 1e-6);
        assert!((f32_at(&tile, uv_offset + index * 8 + 4) - v).abs() < 1e-6);
    }

    fs::remove_dir_all(output).expect("remove this test's temporary output");
}

#[test]
fn obj_texture_uses_gltf_uv_orientation() {
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let input = repo.join("tests/fixtures/texture-root.obj");
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let output = env::temp_dir().join(format!("geoforge-obj-uv-{}-{unique}", std::process::id()));

    let mut command = Command::new(env!("CARGO_BIN_EXE__3dtile"));
    command.args(["-f", "obj", "-i"]);
    command.arg(&input).arg("-o").arg(&output);
    let windows_dependencies = repo.join("vcpkg_installed/x64-windows/bin");
    if windows_dependencies.is_dir() {
        let path = env::join_paths(
            std::iter::once(windows_dependencies)
                .chain(env::split_paths(&env::var_os("PATH").unwrap_or_default())),
        )
        .unwrap();
        command.env("PATH", path);
    }
    let result = command.output().expect("run converter");
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );

    let tile = fs::read(output.join("tile_0.b3dm")).expect("read converted tile");
    let glb_offset = 28
        + [12, 16, 20, 24]
            .into_iter()
            .map(|offset| u32_at(&tile, offset) as usize)
            .sum::<usize>();
    let json_length = u32_at(&tile, glb_offset + 12) as usize;
    let gltf: serde_json::Value =
        serde_json::from_slice(&tile[glb_offset + 20..glb_offset + 20 + json_length]).unwrap();
    let accessor_index = gltf["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"]
        .as_u64()
        .unwrap() as usize;
    let accessor = &gltf["accessors"][accessor_index];
    let view = &gltf["bufferViews"][accessor["bufferView"].as_u64().unwrap() as usize];
    let uv_offset = glb_offset
        + 20
        + json_length
        + 8
        + view["byteOffset"].as_u64().unwrap_or(0) as usize
        + accessor["byteOffset"].as_u64().unwrap_or(0) as usize;

    for (index, (u, v)) in [(0.0, 1.0), (1.0, 1.0), (0.0, 0.0)].into_iter().enumerate() {
        assert!((f32_at(&tile, uv_offset + index * 8) - u).abs() < 1e-6);
        assert!((f32_at(&tile, uv_offset + index * 8 + 4) - v).abs() < 1e-6);
    }

    fs::remove_dir_all(output).expect("remove this test's temporary output");
}

fn u32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn f32_at(bytes: &[u8], offset: usize) -> f32 {
    f32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}
