#!/usr/bin/env python3
"""WP2 step 0: run native Tint (SPIR-V -> WGSL) over a SPIR-V corpus.

  python run_corpus.py --out out/wgsl out/spv [more dirs]
  python run_corpus.py --fixed src/graphics/shaders/vulkan_spirv --out ...

Default converter: rex_spirv_to_wgsl (SDK thirdparty/tint, built by build_native.bat),
i.e. the SpirvToWgsl() the wasm64 build links, with its output re-parsed by Tint's
WGSL reader + resolver (what Chrome runs). --upstream uses Dawn's own tint.exe
(`--allow-non-uniform-derivatives --validate`) instead, without the SPIR-V fix-ups.
--fixed extracts the SPIR-V words from the SDK's generated vulkan_spirv/*.h headers
(`const uint32_t name[] = { 0x07230203, ... };`) first and converts them with
--texel-buffers-as-1d.
Writes <out>/<name>.wgsl, <out>/results.json and prints acceptance + failure categories.
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import struct
import subprocess
import sys

HEADER_RE = re.compile(r"const\s+uint32_t\s+(\w+)\[\]\s*=\s*\{([^}]*)\}", re.S)


def extract_fixed(header_dir, spv_dir):
    os.makedirs(spv_dir, exist_ok=True)
    names = []
    for fn in sorted(os.listdir(header_dir)):
        if not fn.endswith(".h"):
            continue
        text = open(os.path.join(header_dir, fn), encoding="utf-8").read()
        m = HEADER_RE.search(text)
        if not m:
            print(f"no SPIR-V array in {fn}", file=sys.stderr)
            continue
        words = [int(w, 16) for w in re.findall(r"0x[0-9A-Fa-f]+", m.group(2))]
        assert words[0] == 0x07230203, fn
        path = os.path.join(spv_dir, m.group(1) + ".spv")
        with open(path, "wb") as f:
            f.write(struct.pack("<%dI" % len(words), *words))
        names.append(path)
    return names


def categorize(err):
    """First meaningful error line with identifiers / numbers normalized."""
    lines = [l.strip() for l in err.splitlines() if l.strip()]
    pick = ""
    # Prefer the specific diagnostic (validator / reader) over the generic
    # "Failed to parse SPIR-V" header.
    for l in lines:
        if "error:" in l and not l.startswith("Failed to parse"):
            pick = l
            break
    if not pick:
        for l in lines:
            if "error" in l.lower() or "failed" in l.lower():
                pick = l
                break
    if not pick and lines:
        pick = lines[0]
    pick = re.sub(r"^.*?(error|Failed)", r"\1", pick)
    pick = re.sub(r"%[\w.]+", "%id", pick)
    pick = re.sub(r"'[^']*'", "'x'", pick)
    pick = re.sub(r"\b\d+\b", "N", pick)
    return pick[:200]


def run_one(a, spv, out_dir, extra):
    base = os.path.splitext(os.path.basename(spv))[0]
    wgsl = os.path.join(out_dir, base + ".wgsl")
    if os.path.exists(wgsl):
        os.remove(wgsl)
    if a.upstream:
        cmd = [a.tint, "--format", "wgsl", "--allow-non-uniform-derivatives", "--validate",
               "-o", wgsl] + extra + [spv]
    else:
        cmd = [a.rex] + extra + [spv, wgsl]
    p = subprocess.run(cmd, capture_output=True, text=True)
    ok = p.returncode == 0 and os.path.exists(wgsl) and os.path.getsize(wgsl) > 0
    err = (p.stderr + p.stdout).strip()
    # Keep the diagnostics, drop the SPIR-V disassembly Tint prints with them.
    err = "\n".join(l for l in err.splitlines()
                    if "error" in l.lower() or "warning" in l.lower() or "note:" in l
                    or l.startswith("Failed"))
    return {"name": base, "spv": spv, "ok": ok, "rc": p.returncode,
            "category": "" if ok else categorize(err), "error": "" if ok else err[:4000],
            "wgsl_bytes": os.path.getsize(wgsl) if ok else 0,
            "spv_bytes": os.path.getsize(spv)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rex", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "out", "tint-host", "rex_spirv_to_wgsl" + (".exe" if os.name == "nt" else "")))
    ap.add_argument("--tint", default="tint")
    ap.add_argument("--upstream", action="store_true")
    ap.add_argument("--out", required=True)
    ap.add_argument("--fixed", help="SDK vulkan_spirv header dir to extract first")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--tint-arg", action="append", default=[])
    ap.add_argument("dirs", nargs="*")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    spvs = []
    if a.fixed:
        spvs += extract_fixed(a.fixed, os.path.join(a.out, "spv"))
        if not a.upstream and "--texel-buffers-as-1d" not in a.tint_arg:
            a.tint_arg.append("--texel-buffers-as-1d")
    for d in a.dirs:
        spvs += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(".spv")]
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        results = list(ex.map(lambda s: run_one(a, s, a.out, a.tint_arg), spvs))
    ok = sum(r["ok"] for r in results)
    cats = collections.Counter(r["category"] for r in results if not r["ok"])
    summary = {"total": len(results), "accepted": ok,
               "rate": (ok / len(results)) if results else 0.0,
               "categories": cats.most_common(),
               "spv_bytes": sum(r["spv_bytes"] for r in results),
               "wgsl_bytes": sum(r["wgsl_bytes"] for r in results)}
    json.dump({"summary": summary, "results": results},
              open(os.path.join(a.out, "results.json"), "w"), indent=1)
    print(f"accepted {ok}/{len(results)} ({100.0 * summary['rate']:.2f}%)  "
          f"spv {summary['spv_bytes']} B -> wgsl {summary['wgsl_bytes']} B")
    for c, n in cats.most_common():
        ex_name = next(r["name"] for r in results if not r["ok"] and r["category"] == c)
        print(f"  {n:4d}  {c}   (e.g. {ex_name})")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
