#!/usr/bin/env python3
"""
tools/scripts/ch592f.py — CH592F CMake build helper
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import stat
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

from build_report import UsageRow, fmt_bytes, render_usage_report
from common import (
    PROJECT_ROOT,
    colorize as _c,
    die,
    find_cmake,
    find_ninja,
    info,
    ok,
    parse_cmake_cache_var as _parse_cmake_cache_var,
    run as _run,
    run_and_capture as _run_and_capture,
    sep,
    use_color,
    warn,
)
from versioning import (
    ch592_base_stem_for_keyboard,
    ch592_filename_for_keyboard,
    ch592_iap_filename_for_keyboard,
    emit_c_header,
    normalize_keyboard_name,
)


FIRMWARE_DIR = PROJECT_ROOT / "firmware" / "CH592F"
IAP_DIR = FIRMWARE_DIR / "bootloader"
JUMPIAP_DIR = FIRMWARE_DIR / "jump_iap"
LINKER_SCRIPT = FIRMWARE_DIR / "SDK" / "Ld" / "Link.ld"
DEFAULT_KEYBOARD = "5KEY"
DEFAULT_PROFILE = "release"


def _parse_size_row_from_output(text: str) -> Optional[tuple[int, int, int]]:
    m = re.search(r'^\s*(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+[0-9a-fA-F]+\s+\S+\.elf\s*$',
                  text, re.MULTILINE)
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))


def _read_memory_regions_from_linker(linker_script: Path) -> dict[str, tuple[int, int]]:
    out: dict[str, tuple[int, int]] = {}
    if not linker_script.is_file():
        return out

    unit_mul = {"B": 1, "K": 1024, "M": 1024 * 1024}
    mem_re = re.compile(
        r'^\s*(\w+)\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*LENGTH\s*=\s*([0-9]+(?:\.[0-9]+)?)\s*([BbKkMm]?)'
    )
    try:
        for line in linker_script.read_text(errors="ignore").splitlines():
            m = mem_re.match(line)
            if not m:
                continue
            name, origin_s, num, unit = m.groups()
            origin = int(origin_s, 0)
            length = int(float(num) * unit_mul.get((unit or "B").upper(), 1))
            out[name.upper()] = (origin, length)
    except Exception:
        return {}
    return out


def _read_memory_lengths_from_linker(linker_script: Path) -> dict[str, int]:
    return {name: length for name, (_, length) in _read_memory_regions_from_linker(linker_script).items()}


def _size_row_from_artifact(build_dir: Path) -> Optional[tuple[int, int, int]]:
    elf_file = build_dir / "CH592F.elf"
    cache_file = build_dir / "CMakeCache.txt"
    if not elf_file.is_file() or not cache_file.is_file():
        return None

    cross_size = _parse_cmake_cache_var(cache_file, "CROSS_SIZE")
    if not cross_size:
        return None

    try:
        res = subprocess.run(
            [cross_size, "--format=berkeley", str(elf_file)],
            capture_output=True, text=True, check=True, timeout=3
        )
    except Exception:
        return None

    return _parse_size_row_from_output(res.stdout)


def _artifact_region_usage_from_objdump(build_dir: Path) -> Optional[dict[str, int]]:
    elf_file = build_dir / "CH592F.elf"
    cache_file = build_dir / "CMakeCache.txt"
    if not elf_file.is_file() or not cache_file.is_file():
        return None

    cross_objdump = _parse_cmake_cache_var(cache_file, "CROSS_OBJDUMP")
    if not cross_objdump:
        return None

    regions = _read_memory_regions_from_linker(LINKER_SCRIPT)
    if "FLASH" not in regions or "RAM" not in regions:
        return None

    flash_origin, flash_len = regions["FLASH"]
    ram_origin, ram_len = regions["RAM"]
    flash_end = flash_origin + flash_len
    ram_end = ram_origin + ram_len

    try:
        res = subprocess.run(
            [cross_objdump, "-h", str(elf_file)],
            capture_output=True, text=True, check=True, timeout=3
        )
    except Exception:
        return None

    flash_used = 0
    ram_used = 0
    pending: Optional[tuple[int, int, int]] = None

    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 7 and parts[0].isdigit():
            try:
                size = int(parts[2], 16)
                vma = int(parts[3], 16)
                lma = int(parts[4], 16)
                pending = (size, vma, lma)
            except ValueError:
                pending = None
            continue

        if pending is None:
            continue

        if "ALLOC" in line:
            size, vma, lma = pending
            if size > 0:
                if (("LOAD" in line) or ("CONTENTS" in line)) and (flash_origin <= lma < flash_end):
                    flash_used += size
                if ram_origin <= vma < ram_end:
                    ram_used += size
            pending = None
        elif line.strip():
            pending = None

    return {"FLASH": flash_used, "RAM": ram_used}


def _normalize_keyboard(value: str) -> str:
    keyboard = normalize_keyboard_name(value)
    if keyboard == "BASIC":
        die("CH592F does not support BASIC keyboard.")
    return keyboard


def _normalize_profile(value: str) -> str:
    profile = value.strip().lower()
    if profile not in {"release", "debug", "sleep-test"}:
        die(f"Unsupported profile: {value}. Expected release, debug, or sleep-test.")
    return profile


def preset_for(keyboard: str, profile: str) -> str:
    normalized_profile = _normalize_profile(profile)
    keyboard_name = _normalize_keyboard(keyboard).lower()
    if normalized_profile == "sleep-test":
        return f"debug-{keyboard_name}-sleep-test"
    return f"{normalized_profile}-{keyboard_name}"


def build_dir_for(keyboard: str, profile: str) -> Path:
    return FIRMWARE_DIR / "build" / preset_for(keyboard, profile)


def raw_artifact_paths(build_dir: Path) -> dict[str, Path]:
    return {
        "elf": build_dir / "CH592F.elf",
        "bin": build_dir / "CH592F.bin",
        "hex": build_dir / "CH592F.hex",
        "map": build_dir / "CH592F.map",
    }


def artifact_paths(build_dir: Path, keyboard: str) -> dict[str, Path]:
    """All possible artifact paths (app + full + iap)."""
    base = ch592_base_stem_for_keyboard(keyboard)
    iap_hex = build_dir / ch592_iap_filename_for_keyboard(keyboard, "hex")
    iap_bin = build_dir / ch592_iap_filename_for_keyboard(keyboard, "bin")
    return {
        "elf": build_dir / ch592_filename_for_keyboard(keyboard, "elf"),
        "bin": build_dir / ch592_filename_for_keyboard(keyboard, "bin"),
        "hex": build_dir / ch592_filename_for_keyboard(keyboard, "hex"),
        "map": build_dir / ch592_filename_for_keyboard(keyboard, "map"),
        "full_hex": build_dir / f"{base}-full.hex",
        "full_bin": build_dir / f"{base}-full.bin",
        "iap_hex": iap_hex,
        "iap_bin": iap_bin,
        "bootloader_hex": iap_hex,
        "bootloader_bin": iap_bin,
    }


def user_facing_artifact_paths(build_dir: Path, keyboard: str) -> dict[str, Path]:
    """Only the artifacts that matter to the user (for build-full output)."""
    a = artifact_paths(build_dir, keyboard)
    return {
        "full_hex": a["full_hex"],
        "full_bin": a["full_bin"],
        "iap_hex": a["iap_hex"],
        "iap_bin": a["iap_bin"],
        "app_bin": a["bin"],
        "app_hex": a["hex"],
    }


def _directory_is_writable(path: Path) -> bool:
    if not path.exists():
        probe = path.parent
        while not probe.exists() and probe != probe.parent:
            probe = probe.parent
        return probe.exists() and os.access(probe, os.W_OK)
    mode = path.stat().st_mode
    return path.is_dir() and bool(mode & stat.S_IWUSR) and os.access(path, os.W_OK)


def _ensure_writable_build_dir(build_dir: Path, action: str) -> None:
    if _directory_is_writable(build_dir):
        return

    owner_uid = build_dir.stat().st_uid if build_dir.exists() else None
    owner_text = f"uid {owner_uid}" if owner_uid is not None else "unknown owner"
    try:
        import pwd

        if owner_uid is not None:
            owner_text = pwd.getpwuid(owner_uid).pw_name
    except Exception:
        pass

    build_root = FIRMWARE_DIR / "build"
    die(
        f"Cannot {action}: build directory is not writable: {build_dir}\n"
        f"Owner: {owner_text}\n"
        "This usually happens after running the console launcher with sudo and leaving root-owned CH592F build artifacts behind.\n"
        f"Fix it with:\n  sudo chown -R $USER '{build_root}'\n"
        f"Or remove '{build_root}' and rebuild without sudo."
    )


def export_named_artifacts(build_dir: Path, keyboard: str) -> dict[str, Path]:
    raw = raw_artifact_paths(build_dir)
    exported = artifact_paths(build_dir, keyboard)
    for name in ("bin", "hex", "elf", "map"):
        src = raw[name]
        dst = exported[name]
        if src.is_file() and src != dst:
            try:
                shutil.copy2(src, dst)
            except PermissionError:
                _ensure_writable_build_dir(build_dir, f"export {dst.name}")
                raise
    return exported


def _build_env() -> dict[str, str]:
    env = os.environ.copy()
    ninja = find_ninja()
    if ninja:
        ninja_bin = str(ninja.parent.resolve())
        path_value = env.get("PATH", "")
        path_parts = path_value.split(os.pathsep) if path_value else []
        if ninja_bin not in path_parts:
            env["PATH"] = os.pathsep.join([ninja_bin, *path_parts]) if path_parts else ninja_bin
    return env


def _generator_configure_args() -> list[str]:
    """Return CMake generator flags (Ninja + CMAKE_MAKE_PROGRAM on Windows)."""
    args = ["-G", "Ninja"]
    if platform.system() == "Windows":
        ninja = find_ninja()
        if not ninja:
            die(
                "Ninja not found. Install Ninja or set NINJA_PATH to ninja.exe.\n"
                "CH592F builds use the Ninja generator."
            )
        args.append(f"-DCMAKE_MAKE_PROGRAM={ninja}")
    return args


def _ninja_build_files_ready(build_dir: Path) -> bool:
    return (build_dir / "CMakeCache.txt").is_file() and (build_dir / "build.ninja").is_file()


def cmake_configure_args(cmake: Path, keyboard: str, profile: str, build_dir: Path) -> list[str]:
    build_type = {
        "release": "MinSizeRel",
        "debug": "Debug",
        "sleep-test": "Debug",
    }[profile]
    args = [
        str(cmake),
        "-S", str(FIRMWARE_DIR),
        "-B", str(build_dir),
        *_generator_configure_args(),
        f"-DCMAKE_TOOLCHAIN_FILE={FIRMWARE_DIR / 'cmake' / 'toolchain-ch59x.cmake'}",
        f"-DKEYBOARD={keyboard}",
        f"-DKBD_MODEL={keyboard}",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]
    if profile == "sleep-test":
        args.append("-DKBD_SLEEP_TEST_MODE=ON")
    return args


def run(cmd: list[str], cwd: Optional[Path] = None) -> None:
    _run(cmd, cwd=cwd, env=_build_env())


def run_and_capture(cmd: list[str], cwd: Optional[Path] = None) -> tuple[list[str], float]:
    return _run_and_capture(cmd, cwd=cwd, env=_build_env())


def _build_report(lines: list[str], preset: str, build_dir: Path, elapsed: float) -> None:
    mem_re = re.compile(r'(\w+):\s+(\d+)\s+B\s+([\d.]+)\s+(KB|B)\s+([\d.]+)%')
    size_re = re.compile(r'^\s*(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+[0-9a-fA-F]+\s+\S+\.elf\s*$')

    regions = []
    size_row = None
    report_source = "linker"
    for line in lines:
        m = mem_re.search(line)
        if m:
            name, used_b, tot_v, tot_u, pct = m.groups()
            used = int(used_b)
            total = int(float(tot_v) * 1024) if tot_u == 'KB' else int(float(tot_v))
            regions.append((name, used, total, total - used, float(pct)))
        m2 = size_re.match(line)
        if m2:
            size_row = (int(m2.group(1)), int(m2.group(2)), int(m2.group(3)))

    if size_row is None:
        size_row = _size_row_from_artifact(build_dir)
        report_source = "cached-elf"

    if not regions and size_row is not None:
        mem = _read_memory_lengths_from_linker(LINKER_SCRIPT)
        flash_total = mem.get("FLASH")
        ram_total = mem.get("RAM")
        text, data, bss = size_row
        usage = _artifact_region_usage_from_objdump(build_dir)
        if usage:
            report_source = "cached-elf+objdump"
        flash_used = usage.get("FLASH") if usage else (text + data)
        ram_used = usage.get("RAM") if usage else (data + bss)
        if flash_total and flash_used is not None:
            flash_pct = (flash_used / flash_total * 100) if flash_total else 0.0
            regions.append(("FLASH", flash_used, flash_total, flash_total - flash_used, flash_pct))
        if ram_total and ram_used is not None:
            ram_pct = (ram_used / ram_total * 100) if ram_total else 0.0
            regions.append(("RAM", ram_used, ram_total, ram_total - ram_used, ram_pct))

    if not regions and size_row is None:
        return

    bin_file = raw_artifact_paths(build_dir)["bin"]
    bin_sz = bin_file.stat().st_size if bin_file.is_file() else None

    detail = ""
    if size_row is not None or bin_sz is not None:
        if size_row:
            text, data, bss = size_row
            detail += f".text {_c('36', str(text))}   "
            detail += f".data {_c('33', str(data))}   "
            detail += f".bss  {_c('35', str(bss))}"
        if bin_sz is not None:
            detail += f"   .bin {_c('32;1', fmt_bytes(bin_sz))}"

    render_usage_report(
        title="◆ CH592F Memory Report",
        subtitle=f"preset: {preset}  ·  built in {elapsed:.1f}s",
        rows=[UsageRow(name, used, total, free, pct) for name, used, total, free, pct in regions],
        detail_line=detail,
        colorize=_c,
        use_color=use_color(),
    )


def configure(keyboard: str, profile: str) -> Path:
    cmake = find_cmake()
    if not cmake:
        die("CMake not found. Install CMake or add it to PATH.")
    keyboard = _normalize_keyboard(keyboard)
    profile = _normalize_profile(profile)
    build_dir = build_dir_for(keyboard, profile)
    build_dir.mkdir(parents=True, exist_ok=True)
    _ensure_writable_build_dir(build_dir, "configure CH592F build outputs")
    sep()
    info(f"Configuring CH592F ({keyboard}, {profile})")
    run(
        cmake_configure_args(cmake, keyboard, profile, build_dir),
        cwd=PROJECT_ROOT,
    )
    ok(f"Configure complete → {build_dir}")
    return build_dir


def build(keyboard: str, profile: str) -> Path:
    cmake = find_cmake()
    if not cmake:
        die("CMake not found. Install CMake or add it to PATH.")

    keyboard = _normalize_keyboard(keyboard)
    profile = _normalize_profile(profile)
    build_dir = build_dir_for(keyboard, profile)
    _ensure_writable_build_dir(build_dir, "build CH592F artifacts")
    cache_file = build_dir / "CMakeCache.txt"
    cached_keyboard = _parse_cmake_cache_var(cache_file, "KEYBOARD")
    cached_model = _parse_cmake_cache_var(cache_file, "KBD_MODEL")
    if (
        not _ninja_build_files_ready(build_dir)
        or (cached_keyboard and _normalize_keyboard(cached_keyboard) != keyboard)
        or (cached_model and _normalize_keyboard(cached_model) != keyboard)
    ):
        configure(keyboard, profile)

    sep()
    info(f"Building CH592F ({keyboard}, {profile})")
    lines, elapsed = run_and_capture([str(cmake), "--build", str(build_dir)], cwd=PROJECT_ROOT)
    _build_report(lines, preset_for(keyboard, profile), build_dir, elapsed)

    export_named_artifacts(build_dir, keyboard)
    # Only show .hex and .bin (user-facing app artifacts)
    arts = artifact_paths(build_dir, keyboard)
    for key in ("hex", "bin"):
        path = arts[key]
        if path.is_file():
            ok(f"APP {key.upper()} → {path.parent}{os.sep}{_c('32;1', path.name)}")

    return build_dir


def rebuild(keyboard: str, profile: str) -> Path:
    keyboard = _normalize_keyboard(keyboard)
    profile = _normalize_profile(profile)
    build_dir = build_dir_for(keyboard, profile)
    if build_dir.is_dir():
        sep()
        info(f"Removing {build_dir}")
        shutil.rmtree(build_dir)
    configure(keyboard, profile)
    return build(keyboard, profile)


def clean(keyboard: str, profile: str) -> None:
    build_dir = build_dir_for(keyboard, profile)
    sep()
    if build_dir.is_dir():
        shutil.rmtree(build_dir)
        ok(f"Removed {build_dir}")
    else:
        warn(f"Build directory does not exist: {build_dir}")


def status(keyboard: str, profile: str) -> None:
    build_dir = build_dir_for(keyboard, profile)
    cache_file = build_dir / "CMakeCache.txt"

    sep()
    info(f"Keyboard: {keyboard}")
    info(f"Profile: {profile}")
    info(f"CMake: {find_cmake() if find_cmake() else 'not found'}")
    info(f"Build directory: {build_dir}")
    info(f"Configured: {'yes' if cache_file.is_file() else 'no'}")
    raw_artifacts = raw_artifact_paths(build_dir)
    if raw_artifacts["bin"].is_file() or raw_artifacts["hex"].is_file():
        export_named_artifacts(build_dir, keyboard)
    for name, path in artifact_paths(build_dir, keyboard).items():
        if path.is_file():
            info(f"{name.upper()}: {path} ({path.stat().st_size} B)")
        else:
            info(f"{name.upper()}: missing")


def show_artifact(keyboard: str, profile: str, artifact_type: str) -> int:
    build_dir = build_dir_for(keyboard, profile)
    raw = raw_artifact_paths(build_dir)
    if raw["bin"].is_file() or raw["hex"].is_file():
        export_named_artifacts(build_dir, keyboard)
    artifacts = artifact_paths(build_dir, keyboard)

    if artifact_type == "all":
        sep()
        for name, path in artifacts.items():
            print(f"{name}: {path}")
        return 0

    path = artifacts[artifact_type]
    print(path)
    return 0 if path.is_file() else 1


def emit_size_json(keyboard: str, profile: str, out: Path) -> int:
    """Write build size metrics to a JSON file for CI consumption."""
    build_dir = build_dir_for(keyboard, profile)
    mem = _read_memory_lengths_from_linker(LINKER_SCRIPT)
    usage = _artifact_region_usage_from_objdump(build_dir)
    if not usage:
        size_row = _size_row_from_artifact(build_dir)
        if size_row:
            text, data, bss = size_row
            usage = {"FLASH": text + data, "RAM": data + bss}

    if not usage:
        warn(f"Cannot determine size for {keyboard} ({profile})")
        return 1

    result: dict = {"chip": "CH592F", "keyboard": keyboard, "regions": {}}
    for region in ("FLASH", "RAM"):
        used = usage.get(region)
        total = mem.get(region)
        if used is not None and total:
            result["regions"][region] = {"used": used, "total": total}

    bin_file = raw_artifact_paths(build_dir)["bin"]
    if bin_file.is_file():
        result["bin_size"] = bin_file.stat().st_size

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2))
    ok(f"Size JSON → {out}")
    return 0


# ── IAP build ────────────────────────────────────────────────────────────────


IAP_BUILD_DIR = IAP_DIR / "build"
JUMPIAP_BUILD_DIR = JUMPIAP_DIR / "build"


def bootloader_configure() -> Path:
    cmake = find_cmake()
    if not cmake:
        die("CMake not found. Install CMake or add it to PATH.")

    build_dir = IAP_BUILD_DIR
    build_dir.mkdir(parents=True, exist_ok=True)
    sep()
    info("Configuring CH592F IAP app")
    run([
        str(cmake),
        "-S", str(IAP_DIR),
        "-B", str(build_dir),
        *_generator_configure_args(),
        f"-DCMAKE_TOOLCHAIN_FILE={FIRMWARE_DIR / 'cmake' / 'toolchain-ch59x.cmake'}",
        "-DCMAKE_BUILD_TYPE=MinSizeRel",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ], cwd=PROJECT_ROOT)
    ok(f"IAP configure complete → {build_dir}")
    return build_dir


def bootloader_build() -> Path:
    cmake = find_cmake()
    if not cmake:
        die("CMake not found. Install CMake or add it to PATH.")

    build_dir = IAP_BUILD_DIR
    cache_file = build_dir / "CMakeCache.txt"
    if not _ninja_build_files_ready(build_dir):
        bootloader_configure()

    sep()
    info("Building CH592F IAP app")
    lines, elapsed = run_and_capture([str(cmake), "--build", str(build_dir)], cwd=PROJECT_ROOT)

    bin_path = build_dir / "CH592F_IAP.bin"
    hex_path = build_dir / "CH592F_IAP.hex"
    for artifact in (bin_path, hex_path):
        if artifact.is_file():
            ok(f"{artifact.suffix.upper().lstrip('.')} → {artifact.parent}{os.sep}{_c('32;1', artifact.name)}  ({artifact.stat().st_size} B)")

    return build_dir


def bootloader_clean() -> None:
    build_dir = IAP_BUILD_DIR
    sep()
    if build_dir.is_dir():
        shutil.rmtree(build_dir)
        ok(f"Removed {build_dir}")
    else:
        warn(f"Build directory does not exist: {build_dir}")


def bootloader_hex_path() -> Path:
    return IAP_BUILD_DIR / "CH592F_IAP.hex"


def jumpiap_configure() -> Path:
    cmake = find_cmake()
    if not cmake:
        die("CMake not found. Install CMake or add it to PATH.")

    build_dir = JUMPIAP_BUILD_DIR
    build_dir.mkdir(parents=True, exist_ok=True)
    sep()
    info("Configuring CH592F JumpIAP stub")
    run([
        str(cmake),
        "-S", str(JUMPIAP_DIR),
        "-B", str(build_dir),
        *_generator_configure_args(),
        f"-DCMAKE_TOOLCHAIN_FILE={FIRMWARE_DIR / 'cmake' / 'toolchain-ch59x.cmake'}",
        "-DCMAKE_BUILD_TYPE=MinSizeRel",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ], cwd=PROJECT_ROOT)
    ok(f"JumpIAP configure complete → {build_dir}")
    return build_dir


def jumpiap_build() -> Path:
    cmake = find_cmake()
    if not cmake:
        die("CMake not found. Install CMake or add it to PATH.")

    build_dir = JUMPIAP_BUILD_DIR
    cache_file = build_dir / "CMakeCache.txt"
    if not _ninja_build_files_ready(build_dir):
        jumpiap_configure()

    sep()
    info("Building CH592F JumpIAP stub")
    lines, elapsed = run_and_capture([str(cmake), "--build", str(build_dir)], cwd=PROJECT_ROOT)

    bin_path = build_dir / "CH592F_JumpIAP.bin"
    hex_path = build_dir / "CH592F_JumpIAP.hex"
    for artifact in (bin_path, hex_path):
        if artifact.is_file():
            ok(f"{artifact.suffix.upper().lstrip('.')} → {artifact.parent}{os.sep}{_c('32;1', artifact.name)}  ({artifact.stat().st_size} B)")

    return build_dir


def jumpiap_clean() -> None:
    build_dir = JUMPIAP_BUILD_DIR
    sep()
    if build_dir.is_dir():
        shutil.rmtree(build_dir)
        ok(f"Removed {build_dir}")
    else:
        warn(f"Build directory does not exist: {build_dir}")


def jumpiap_hex_path() -> Path:
    return JUMPIAP_BUILD_DIR / "CH592F_JumpIAP.hex"


# ── Intel HEX merge ──────────────────────────────────────────────────────


def _parse_ihex(path: Path) -> dict[int, int]:
    """Parse an Intel HEX file and return a dict mapping address -> byte."""
    memory: dict[int, int] = {}
    base_addr = 0

    for line in path.read_text().splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue

        byte_count = int(line[1:3], 16)
        address = int(line[3:7], 16)
        record_type = int(line[7:9], 16)
        data = bytes.fromhex(line[9:9 + byte_count * 2])

        if record_type == 0x00:  # Data
            for i, b in enumerate(data):
                memory[base_addr + address + i] = b
        elif record_type == 0x02:  # Extended Segment Address
            base_addr = (data[0] << 8 | data[1]) << 4
        elif record_type == 0x04:  # Extended Linear Address
            base_addr = (data[0] << 8 | data[1]) << 16
        elif record_type == 0x01:  # EOF
            break

    return memory


def _write_ihex(
    memory: dict[int, int],
    path: Path,
    bytes_per_line: int = 16,
    fill_gaps_with: int | None = None,
) -> None:
    """Write a flat memory dict to an Intel HEX file.

    When ``fill_gaps_with`` is set, unwritten holes between the first and last
    address are emitted as explicit bytes. WCHISP's HEX→BIN conversion is more
    reliable with a dense ISP image than with a sparse multi-segment HEX.
    """
    if not memory:
        die("Cannot write empty Intel HEX file.")

    if fill_gaps_with is not None:
        start = min(memory)
        end = max(memory)
        fill = fill_gaps_with & 0xFF
        addresses = list(range(start, end + 1))
        read_byte = lambda addr: memory.get(addr, fill)
    else:
        addresses = sorted(memory.keys())
        read_byte = lambda addr: memory[addr]

    lines: list[str] = []
    current_ela = -1  # current Extended Linear Address (upper 16 bits)

    i = 0
    while i < len(addresses):
        addr = addresses[i]
        ela = addr >> 16

        # Emit Extended Linear Address record if needed
        if ela != current_ela:
            current_ela = ela
            data = bytes([(ela >> 8) & 0xFF, ela & 0xFF])
            checksum = (-(0x02 + 0x00 + 0x00 + 0x04 + data[0] + data[1])) & 0xFF
            lines.append(f":02000004{data.hex().upper()}{checksum:02X}")

        # Collect contiguous bytes up to bytes_per_line
        run_start = addr
        run_data: list[int] = []
        while (
            i < len(addresses)
            and addresses[i] == run_start + len(run_data)
            and len(run_data) < bytes_per_line
            and (addresses[i] >> 16) == current_ela
        ):
            run_data.append(read_byte(addresses[i]))
            i += 1

        # Emit Data record
        low_addr = run_start & 0xFFFF
        byte_count = len(run_data)
        data_bytes = bytes(run_data)
        s = byte_count + (low_addr >> 8) + (low_addr & 0xFF) + 0x00
        s += sum(data_bytes)
        checksum = (-s) & 0xFF
        lines.append(f":{byte_count:02X}{low_addr:04X}00{data_bytes.hex().upper()}{checksum:02X}")

    # EOF record
    lines.append(":00000001FF")

    path.write_text("\n".join(lines) + "\n")


def merge_hex(
    bootloader_hex: Path,
    app_hex: Path,
    output_hex: Path,
    *,
    fill_gaps_with: int | None = None,
) -> Path:
    """Merge bootloader and application Intel HEX files into a single HEX."""
    if not bootloader_hex.is_file():
        die(f"Bootloader HEX not found: {bootloader_hex}")
    if not app_hex.is_file():
        die(f"Application HEX not found: {app_hex}")

    boot_mem = _parse_ihex(bootloader_hex)
    app_mem = _parse_ihex(app_hex)

    # Check for overlap
    overlap = set(boot_mem.keys()) & set(app_mem.keys())
    if overlap:
        overlap_start = min(overlap)
        overlap_end = max(overlap)
        warn(f"Address overlap detected: 0x{overlap_start:05X}-0x{overlap_end:05X} ({len(overlap)} bytes)")

    # Merge: app overwrites bootloader in case of overlap
    merged = {**boot_mem, **app_mem}

    output_hex.parent.mkdir(parents=True, exist_ok=True)
    _write_ihex(merged, output_hex, fill_gaps_with=fill_gaps_with)

    boot_range = (min(boot_mem), max(boot_mem)) if boot_mem else (0, 0)
    app_range = (min(app_mem), max(app_mem)) if app_mem else (0, 0)
    ok(
        f"Merged HEX → {output_hex.name}  "
        f"(boot 0x{boot_range[0]:05X}-0x{boot_range[1]:05X}, "
        f"app 0x{app_range[0]:05X}-0x{app_range[1]:05X}, "
        f"total {len(merged)} bytes)"
    )
    return output_hex


def _hex_to_bin(hex_path: Path, bin_path: Path) -> None:
    """Convert an Intel HEX file to a raw binary file."""
    memory = _parse_ihex(hex_path)
    if not memory:
        die(f"Empty HEX file: {hex_path}")
    addresses = sorted(memory.keys())
    start = addresses[0]
    end = addresses[-1] + 1
    data = bytearray(b'\xFF' * (end - start))
    for addr, byte in memory.items():
        data[addr - start] = byte
    bin_path.write_bytes(data)


def build_full(keyboard: str, profile: str, build_number: int | None = None) -> Path:
    """Build JumpIAP + app + IAP app, then merge into a single -full.hex for ISP."""
    keyboard = _normalize_keyboard(keyboard)
    profile = _normalize_profile(profile)
    if build_number is not None:
        os.environ["BK_BUILD_NUMBER"] = str(build_number)

    # 1. Build JumpIAP stub + high-flash IAP app
    jumpiap_build()
    bootloader_build()

    # 2. Build app
    # The generated header is timestamp-based in CMake, so changing only the
    # environment would otherwise leave a cached dev/older version embedded.
    app_build_dir = build_dir_for(keyboard, profile)
    emit_c_header(
        "CH592F",
        app_build_dir / "generated" / "bk_version_config.h",
        build_number,
    )
    app_build_dir = build(keyboard, profile)

    # 3. Merge hex
    jump_hex = jumpiap_hex_path()
    iap_hex = bootloader_hex_path()
    arts = artifact_paths(app_build_dir, keyboard)
    app_hex = arts["hex"]
    stage_hex = app_build_dir / ".merge-stage.hex"

    sep()
    info("Merging JumpIAP + app + IAP → full HEX")
    merge_hex(jump_hex, app_hex, stage_hex)
    merge_hex(stage_hex, iap_hex, arts["full_hex"], fill_gaps_with=0xFF)
    if stage_hex.is_file():
        stage_hex.unlink()

    # 4. Generate full .bin from merged hex
    _hex_to_bin(arts["full_hex"], arts["full_bin"])

    # 5. Copy IAP artifacts with keyboard-prefixed names
    boot_build = IAP_BUILD_DIR
    for suffix in ("hex", "bin"):
        src = boot_build / f"CH592F_IAP.{suffix}"
        dst = arts[f"iap_{suffix}"]
        if src.is_file():
            shutil.copy2(src, dst)

    # 6. Print user-facing artifacts
    sep()
    info("Build artifacts:")
    for label, key in [
        ("FULL HEX (ISP)", "full_hex"),
        ("FULL BIN (ISP)", "full_bin"),
        ("IAP HEX", "iap_hex"),
        ("IAP BIN", "iap_bin"),
        ("APP HEX (OTA)", "hex"),
        ("APP BIN (OTA)", "bin"),
    ]:
        path = arts[key]
        if path.is_file():
            ok(f"{label:16s} → {_c('32;1', path.name)}  ({path.stat().st_size:,} B)")

    return app_build_dir


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="tools/scripts/ch592f.py",
        description="CH592F CMake build helper",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    for name in ("status", "configure", "build", "rebuild", "clean"):
        p = sub.add_parser(name, help=f"{name.capitalize()} CH592F firmware")
        p.add_argument("-k", "--keyboard", default=DEFAULT_KEYBOARD, help="5KEY / KNOB")
        p.add_argument("--profile", default=DEFAULT_PROFILE, help="release / debug")

    p_artifact = sub.add_parser("artifact", help="Print artifact path")
    p_artifact.add_argument("-k", "--keyboard", default=DEFAULT_KEYBOARD, help="5KEY / KNOB")
    p_artifact.add_argument("--profile", default=DEFAULT_PROFILE, help="release / debug")
    p_artifact.add_argument(
        "-t",
        "--type",
        default="all",
        choices=("all", "elf", "bin", "hex", "map", "full_hex", "full_bin", "iap_hex", "iap_bin", "bootloader_hex", "bootloader_bin"),
        help="Artifact type to print",
    )

    p_size = sub.add_parser("size-json", help="Emit build size report as JSON")
    p_size.add_argument("-k", "--keyboard", default=DEFAULT_KEYBOARD, help="5KEY / KNOB")
    p_size.add_argument("--profile", default=DEFAULT_PROFILE, help="release / debug")
    p_size.add_argument("-o", "--out", required=True, type=Path, help="Output JSON path")

    # IAP commands
    sub.add_parser("bootloader-build", help="Build high-flash IAP app (compat alias)")
    sub.add_parser("bootloader-clean", help="Clean high-flash IAP build directory")
    sub.add_parser("jumpiap-build", help="Build JumpIAP stub (4KB @ 0x00000)")
    sub.add_parser("jumpiap-clean", help="Clean JumpIAP build directory")

    # Full build (JumpIAP + app + IAP merged)
    p_full = sub.add_parser("build-full", help="Build JumpIAP + app + IAP and merge into full HEX for ISP")
    p_full.add_argument("-k", "--keyboard", default=DEFAULT_KEYBOARD, help="5KEY / KNOB")
    p_full.add_argument("--profile", default=DEFAULT_PROFILE,
                        help="release / debug / sleep-test")
    p_full.add_argument("--build-number", type=int, default=None, metavar="PATCH",
                        help="Pin the firmware patch version (CI use; skips GitHub API lookup)")

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    keyboard = getattr(args, "keyboard", DEFAULT_KEYBOARD)
    profile = getattr(args, "profile", DEFAULT_PROFILE)

    try:
        if args.command == "status":
            status(keyboard, profile)
            return 0
        if args.command == "configure":
            configure(keyboard, profile)
            return 0
        if args.command == "build":
            build(keyboard, profile)
            return 0
        if args.command == "rebuild":
            rebuild(keyboard, profile)
            return 0
        if args.command == "clean":
            clean(keyboard, profile)
            return 0
        if args.command == "size-json":
            return emit_size_json(keyboard, profile, args.out)
        if args.command == "artifact":
            return show_artifact(keyboard, profile, args.type)
        if args.command == "bootloader-build":
            bootloader_build()
            return 0
        if args.command == "bootloader-clean":
            bootloader_clean()
            return 0
        if args.command == "jumpiap-build":
            jumpiap_build()
            return 0
        if args.command == "jumpiap-clean":
            jumpiap_clean()
            return 0
        if args.command == "build-full":
            build_full(keyboard, profile, getattr(args, "build_number", None))
            return 0
        parser.error(f"Unsupported command: {args.command}")
    except subprocess.CalledProcessError as exc:
        die(f"Command failed (exit {exc.returncode})")
    except KeyboardInterrupt:
        print()
        warn("Interrupted.")
        return 130

    return 0


if __name__ == "__main__":
    sys.exit(main())
