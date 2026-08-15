#!/usr/bin/env python3
"""Convert the Terrain Diffusion ONNX models to float16, for the Terrain/V3 "FP16 inference" toggle.

Writes <stem>_fp16.onnx BESIDE the fp32 originals in Assets/TerrainDiffusion/, where they are shipped assets
like the models they came from: converted once, committed, and thereafter just loaded. The engine never runs
this -- rerun it by hand only if the fp32 model set changes.

They land under the repo's "*.onnx filter=lfs" rule, so committing them adds ~1.1 GB to LFS. That is the
price of the models being present in a fresh clone, matching how the fp32 set already ships.

The fp32 originals are never touched or replaced: the engine prefers a converted file when the toggle is on
and silently falls back to the fp32 one whenever one is missing, so running this is optional and reversible
(delete the *_fp16.onnx files to undo).

Why this is an offline script and not engine code: ONNX models are protobuf, the engine does not link
protobuf, and ONNX Runtime has no API to convert or to run an fp32 graph in fp16 -- the precision is a
property of the model file. onnxconverter_common already solves the fiddly parts (which ops are unsafe in
fp16, where Cast pairs go), so vendoring a hand-rolled rewriter would be a large step backwards.

Graph inputs and outputs stay float32 (keep_io_types=True); the conversion inserts Casts just inside the
boundary. That is what keeps this a drop-in: Onnx.cpp and WorldPipeline.cpp feed and read exactly the same
float buffers and never learn which precision is loaded.

WHAT THIS ACTUALLY BUYS (measured at metersPerPixel=3 on the DiffusionTest bake bench, RTX/DirectML):
    model VRAM     ~2.28 GB -> ~1.1 GB
    base_model load   2.14 s -> 1.19 s
    near cascade      4.03 s -> 4.02 s   (unchanged)
    far/coarse        1.12 s -> 1.30 s   (~15% SLOWER)
So it is a MEMORY optimisation, not a speed one: the pipeline is dispatch-bound rather than compute-bound,
so halving the FLOPs changes nothing and the boundary Casts cost the tiny coarse model a little. Convert if
you want the VRAM back for the renderer; do not expect faster terrain.

NOTE: fp16 inference is NOT bit-identical to fp32, so a given seed produces different terrain in each mode.
The coarse stage tracks fp32 almost exactly (far-cascade elevation range matched to 1 m), but error
compounds through the base/decoder sampler steps and fine detail visibly shifts. The engine treats the
toggle as a world-changing config and regenerates, exactly like a seed change.

Usage (needs onnx + onnxconverter_common, which the engine build does NOT -- keep them out of the runtime):
    python -m venv .venv
    .venv/Scripts/python -m pip install onnx onnxconverter_common
    .venv/Scripts/python Tools/convert_models_fp16.py
"""


import sys
import time
from pathlib import Path

MODELS = ["coarse_model.onnx", "decoder_model.onnx", "base_model.onnx"]


def human(n: int) -> str:
    return f"{n / (1024 * 1024):.0f} MB"


def retarget_internal_casts(model) -> int:
    """Point every internal `Cast -> float32` at float16, BEFORE the fp16 conversion runs.

    Without this the conversion produces a model ONNX Runtime refuses to load, and the reason is subtle
    enough to be worth writing down.

    These models are full of explicit upcasts (54 in coarse_model, 184 in decoder_model, every one of them
    to float32) feeding Conv/MatMul/Einsum/Mul/Div. The converter rewrites initializers and node types but
    never touches an existing Cast's `to` attribute, so each of those Casts keeps emitting float32 into an
    operator whose other input just became float16. ORT rejects that at load:

        Type parameter (T) of Optype (Einsum) bound to different types (tensor(float) and tensor(float16))

    It is worse for the Casts fed straight from a graph input: keep_io_types pins those inputs to float32,
    so `Cast -> float32` becomes a no-op that the converter DELETES, wiring the float32 graph input directly
    into an operator that is now float16. Retargeting to float16 first fixes both cases at once -- the Cast
    is no longer redundant, so it survives and does the real fp32->fp16 conversion at the boundary.

    (Blocking the offending ops instead does not work: it just moves the mismatch to whatever consumes the
    fp32 island, and blocking `Add`/`Mul` would leave nothing running in fp16.)

    Casts feeding a graph OUTPUT are left alone -- keep_io_types requires those to stay float32.
    """
    import onnx

    graph_outputs = {o.name for o in model.graph.output}
    n = 0
    for node in model.graph.node:
        if node.op_type != "Cast" or (set(node.output) & graph_outputs):
            continue
        for attr in node.attribute:
            if attr.name == "to" and attr.i == onnx.TensorProto.FLOAT:
                attr.i = onnx.TensorProto.FLOAT16
                n += 1
    return n


def convert(src: Path, dst: Path) -> bool:
    import onnx
    from onnxconverter_common import float16

    if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
        print(f"  {dst.name}: up to date ({human(dst.stat().st_size)}), skipping")
        return True

    print(f"  {src.name} ({human(src.stat().st_size)}) -> {dst.name} ...", flush=True)
    t0 = time.time()
    model = onnx.load(str(src))
    n_cast = retarget_internal_casts(model)

    # keep_io_types: the graph keeps its float32 inputs/outputs (the converter Casts just inside the
    #   boundary), so nothing in the engine has to know which precision is loaded.
    # disable_shape_infer: infer_shapes() cannot handle base_model -- it is 2 GB, at protobuf's ceiling.
    #   The conversion does not need it once the Casts above are fixed.
    # check_fp16_ready: trips because retarget_internal_casts already introduced fp16. Expected.
    converted = float16.convert_float_to_float16(
        model,
        keep_io_types=True,
        disable_shape_infer=True,
        check_fp16_ready=False,
    )
    # Any value_info the source carried describes the pre-conversion types and now contradicts the graph;
    # ORT re-infers it at load.
    del converted.graph.value_info[:]
    print(f"    retargeted {n_cast} internal Cast nodes to fp16", flush=True)
    onnx.save(converted, str(dst))
    print(f"    done in {time.time() - t0:.1f}s -> {human(dst.stat().st_size)}", flush=True)
    return True


def main() -> int:
    try:
        import onnx  # noqa: F401
        from onnxconverter_common import float16  # noqa: F401
    except ImportError:
        print(
            "error: this script needs the onnx tooling, which the engine deliberately does not depend on.\n"
            "  python -m venv .venv\n"
            "  .venv/Scripts/python -m pip install onnx onnxconverter_common\n"
            "  .venv/Scripts/python Tools/convert_models_fp16.py",
            file=sys.stderr,
        )
        return 1

    model_dir = Path(__file__).resolve().parent.parent / "Assets" / "TerrainDiffusion"
    if not model_dir.is_dir():
        print(f"error: {model_dir} not found", file=sys.stderr)
        return 1

    print(f"Converting the fp32 models in {model_dir} to fp16, in place beside them")
    for name in MODELS:
        src = model_dir / name
        if not src.exists():
            print(f"  {name}: missing, skipping", file=sys.stderr)
            continue
        dst = src.with_name(src.stem + "_fp16.onnx")
        try:
            convert(src, dst)
        except MemoryError:
            print(
                f"  {name}: out of memory. base_model is 2 GB of fp32 weights and the converter holds a\n"
                f"  second copy while it works; this needs roughly 8 GB free.",
                file=sys.stderr,
            )
            return 1

    print("\nDone. Enable Terrain/V3 -> 'FP16 inference' in the Tweaks panel.")
    print("Delete the *_fp16.onnx files to go back to fp32 only.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
