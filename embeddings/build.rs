extern crate cbindgen;

use std::env;
use std::path::PathBuf;
use std::process::Command;

/// Extracts the git commit hash (short format) for version string generation.
///
/// First attempts to read from GIT_COMMIT_ID environment variable (set by CMake),
/// then falls back to executing git command directly. Returns "unknown" if both fail.
/// This ensures version strings work both in CMake builds and standalone cargo builds.
fn get_git_commit() -> String {
    // First try environment variable (set by CMake via build_embeddings.cmake)
    if let Ok(commit) = env::var("GIT_COMMIT_ID") {
        return commit;
    }

    // Fallback to git command for standalone cargo builds
    let output = Command::new("git")
        .args(["log", "-1", "--format=%h"])
        .current_dir(env::var("CARGO_MANIFEST_DIR").unwrap_or(".".to_string()))
        .output();

    if let Ok(output) = output {
        if output.status.success() {
            return String::from_utf8_lossy(&output.stdout).trim().to_string();
        }
    }

    // Default fallback if git is not available
    "unknown".to_string()
}

/// Extracts the git commit timestamp for version string generation.
///
/// Format: YYMMDDHH (8 digits representing year, month, day, hour of last commit).
/// First attempts to read from GIT_TIMESTAMP_ID environment variable (set by CMake),
/// then falls back to executing git command directly. Returns "00000000" if both fail.
/// This ensures version strings work both in CMake builds and standalone cargo builds.
fn get_git_timestamp() -> String {
    // First try environment variable (set by CMake via build_embeddings.cmake)
    if let Ok(timestamp) = env::var("GIT_TIMESTAMP_ID") {
        return timestamp;
    }

    // Fallback to git command - format as YYMMDDHH (matches format used by other libraries)
    let output = Command::new("git")
        .args(["log", "-1", "--date=format-local:%y%m%d%H", "--format=%cd"])
        .current_dir(env::var("CARGO_MANIFEST_DIR").unwrap_or(".".to_string()))
        .output();

    if let Ok(output) = output {
        if output.status.success() {
            let ts = String::from_utf8_lossy(&output.stdout).trim().to_string();
            // Take first 8 characters (YYMMDDHH) to match format used by columnar, secondary, knn
            if ts.len() >= 8 {
                return ts[..8].to_string();
            }
            return ts;
        }
    }

    // Default fallback if git is not available
    "00000000".to_string()
}

fn main() {
    let crate_dir = PathBuf::from(
        env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR env var is not defined"),
    );

    let config = cbindgen::Config::from_file("cbindgen.toml")
        .expect("Unable to find cbindgen.toml configuration file");

    // Generate C header file for FFI bindings
    cbindgen::generate_with_config(&crate_dir, config)
        .expect("Unable to generate bindings")
        .write_to_file(crate_dir.join("manticoresearch_text_embeddings.h"));

    // Generate version string with commit and timestamp in format: "VERSION commit@timestamp"
    // This matches the format used by other Manticore libraries (columnar, secondary, knn)
    // Example: "1.1.0 38f499e@25112313"
    let version = env::var("CARGO_PKG_VERSION").unwrap_or_else(|_| "1.1.0".to_string());
    let commit = get_git_commit();
    let timestamp = get_git_timestamp();
    let version_str = format!("{} {}@{}", version, commit, timestamp);

    // Pass version string to compile-time via cargo:rustc-env directive
    // This will be read in ffi.rs using env!() macro to create a static version string
    println!("cargo:rustc-env=EMBEDDINGS_VERSION_STR={}", version_str);
}
