"""Background music for Montage with ACE-Step 1.5 (turbo). Montage runs this with the Python of the
ACE-Step checkout's env (see GeneratorConfig in src/ai/Generators.h):

    python acestep_music.py --ace-root ~/ai-studio/ace-step --model-dir ~/ai-studio/models/ace-step-1.5 \
        --caption "soft acoustic guitar, warm and calm" --seconds 15 --out music.wav

Instrumental only. The optional 5Hz language model isn't used, so the caption is taken as written.
The DiT runs in PyTorch on MPS (faster than the MLX path here and without its second copy of the
weights). Prints "PROGRESS <0-1> <note>" lines while it works and one JSON line at the end:
{"file": ..., "seconds": ...} or {"error": ...}.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path


def progress(value: float, note: str) -> None:
    print(f"PROGRESS {value:.2f} {note}", flush=True)


def fail(message: str) -> int:
    print(json.dumps({"error": message}), flush=True)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ace-root", type=Path, required=True)
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--caption", required=True)
    parser.add_argument("--seconds", type=float, default=15.0)
    parser.add_argument("--bpm", type=int, default=100)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--steps", type=int, default=8, help="turbo model: 8")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    ace_root = args.ace_root.expanduser()
    model_dir = args.model_dir.expanduser()
    for part in ("acestep-v15-turbo", "vae", "Qwen3-Embedding-0.6B"):
        if not (model_dir / part).is_dir():
            return fail(f"{model_dir / part} is missing")

    # Must be set before acestep is imported.
    os.environ.setdefault("ACESTEP_CHECKPOINTS_DIR", str(model_dir))
    os.environ.setdefault("ACESTEP_PROJECT_ROOT", str(ace_root))
    os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")
    os.environ.setdefault("HF_HUB_OFFLINE", "1")

    progress(0.02, "Loading the music model")
    from acestep.handler import AceStepHandler
    from acestep.inference import GenerationConfig, GenerationParams, generate_music
    from acestep.llm_inference import LLMHandler

    dit = AceStepHandler()
    status, ok = dit.initialize_service(
        project_root=str(ace_root), config_path="acestep-v15-turbo", device="auto", use_mlx_dit=False
    )
    if not ok:
        return fail(f"ACE-Step could not start: {status}")
    llm = LLMHandler()  # never initialized: captions are used as written

    params = GenerationParams(
        caption=args.caption,
        lyrics="[Instrumental]",
        instrumental=True,
        bpm=args.bpm,
        duration=args.seconds,
        inference_steps=args.steps,
        seed=args.seed,
        thinking=False,
        use_cot_metas=False,
        use_cot_caption=False,
        use_cot_language=False,
    )
    config = GenerationConfig(batch_size=1, use_random_seed=False, seeds=[args.seed], audio_format="wav")

    # ACE-Step reports 0.5-1.0 for generation; loading took the first half.
    def on_progress(value: float, desc: str | None = None, **_: object) -> None:
        progress(0.3 + 0.7 * max(0.0, min(1.0, float(value))), desc or "Composing")

    progress(0.3, "Composing")
    with tempfile.TemporaryDirectory() as work:
        result = generate_music(dit, llm, params, config, save_dir=work, progress=on_progress)
        if not result.success or not result.audios:
            return fail(f"ACE-Step failed: {result.error or 'no audio'}")
        args.out.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(result.audios[0]["path"], args.out)
    print(json.dumps({"file": str(args.out), "seconds": args.seconds}), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
