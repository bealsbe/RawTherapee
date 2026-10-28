#!/usr/bin/env python3
"""
RawTherapee segmentation backend.

RawTherapee starts this script as a child process and talks to it over
stdin/stdout, one JSON object per line. Images and masks are exchanged as
files (binary PPM in, binary PGM out) so that no large payload goes through
the pipe.

Requests
--------
  {"cmd": "info"}
      -> {"ok": true, "models": [{"id", "name", "description", "available", "reason", "hidden"}],
          "default": "sam2", "device": "auto"|"cuda"|"cpu", "python": "3.12"}

  {"cmd": "segment", "id": 7, "model": "sam2", "image": "/tmp/in.ppm",
   "mask": "/tmp/out.pgm", "target": "subject"|"sky"|"background"|"object",
   "prompt": "optional noun phrase", "point": [nx, ny] | null,
   "threshold": 0.5, "refine": false}
      -> {"ok": true, "id": 7, "instances": 2, "elapsed": 0.31}
      -> {"ok": false, "id": 7, "error": "..."}

  {"cmd": "quit"}

The mask is written with the same size as the input image, 0..255, where
255 means "fully selected". Masks are soft probabilities, never hard labels,
so that RawTherapee's threshold and feathering work on them. The process
exits when stdin is closed.

"refine": true asks for the mask edges to be refined with a matting model
(ViTMatte): a trimap is built from the soft mask and the alpha is re-estimated
in the unknown band. It costs ~0.1 s on a GPU and is off by default.

"available" tells whether a model can run now; "reason" explains why not
(gated weights not granted, package missing) or carries a short note when it
can. Models with "hidden": true are usable by id but not offered in the GUI.

Adding a model: subclass ModelAdapter, implement available() and
segment(), and add the class to MODEL_CLASSES. Model code is imported
lazily so that startup stays fast and optional dependencies are only
needed when a model is actually used.
"""

import argparse
import hashlib
import json
import os
import sys
import threading
import time
import traceback

import numpy as np
from PIL import Image

SAM3_REPO = os.environ.get("RT_SAM3_REPO", "facebook/sam3")
# SAM 3 switch. While it is off (the default) the sam3 / sam3-official providers are reported
# unavailable with a static reason, are never the default, and no request of any kind is made to
# Hugging Face for the gated facebook/sam3 weights (no auth_check, no config download). Set
# RT_ENABLE_SAM3=1 to turn the SAM 3 code paths back on once access has been granted.
SAM3_ENABLED = os.environ.get("RT_ENABLE_SAM3", "").strip() == "1"
SAM3_DISABLED_REASON = "SAM 3 is disabled in this build until access to the gated weights is granted (set RT_ENABLE_SAM3=1 to enable)"
SAM2_REPO = os.environ.get("RT_SAM2_REPO", "facebook/sam2.1-hiera-large")
SEGFORMER_REPO = os.environ.get("RT_SEGFORMER_REPO", "nvidia/segformer-b2-finetuned-ade-512-512")
EOMT_REPO = os.environ.get("RT_EOMT_REPO", "tue-mps/ade20k_semantic_eomt_large_512")
# Semantic (ADE20K class) model behind the class targets: "eomt" (MIT, DINOv2 backbone, better) or
# "segformer" (smaller and faster on CPU, but NVIDIA's weights are licensed for research use only).
SEMANTIC_MODEL = os.environ.get("RT_SEMANTIC_MODEL", "eomt").strip().lower()
# BiRefNet (dichotomous image segmentation, MIT) for subject / background. "auto" picks
# BiRefNet_dynamic (220 M parameters) on a GPU and BiRefNet_lite (44 M) on the CPU.
BIREFNET_REPO = os.environ.get("RT_BIREFNET_REPO", "auto")
BIREFNET_GPU_REPO = "ZhengPeng7/BiRefNet_dynamic"
BIREFNET_CPU_REPO = "ZhengPeng7/BiRefNet_lite"
VITMATTE_REPO = os.environ.get("RT_VITMATTE_REPO", "hustvl/vitmatte-small-composition-1k")
# Width of the trimap's unknown band around the mask edge, as a fraction of the image diagonal.
MATTING_BAND = float(os.environ.get("RT_MATTING_BAND", "0.02"))
HF_ACCESS_TIMEOUT = float(os.environ.get("RT_HF_ACCESS_TIMEOUT", "4"))

DEFAULT_PROMPTS = {
    "subject": "person",
    "sky": "sky",
    "background": "person",   # segmented, then inverted
    "object": "",
}

# ADE20K classes (as named in the SegFormer configs) used for the class targets.
SUBJECT_PRIMARY = ["person", "animal"]
SUBJECT_SECONDARY = ["car", "bus", "truck", "van", "boat", "ship", "airplane", "bicycle", "minibike"]
STUFF_CLASSES = ["wall", "building", "sky", "floor", "tree", "ceiling", "road", "grass", "sidewalk", "earth",
                 "mountain", "field", "water", "sea", "sand", "river", "hill", "path", "land", "dirt track", "rock",
                 "fence", "railing", "windowpane", "door", "curtain", "column", "stairs", "stairway", "step",
                 "bridge", "pier", "skyscraper", "house", "tower", "runway", "swimming pool", "lake", "waterfall",
                 "palm", "plant", "rug", "ceiling", "awning", "canopy", "grandstand", "escalator", "bannister",
                 "fireplace", "hovel", "tent", "pole", "streetlight", "bulletin board"]

# Words users are likely to type in the Concept field, mapped to ADE20K classes.
CONCEPT_SYNONYMS = {
    "people": ["person"], "man": ["person"], "woman": ["person"], "child": ["person"], "human": ["person"],
    "face": ["person"], "portrait": ["person"], "model": ["person"],
    "dog": ["animal"], "cat": ["animal"], "horse": ["animal"], "bird": ["animal"], "cow": ["animal"],
    "sheep": ["animal"], "pet": ["animal"], "wildlife": ["animal"], "animals": ["animal"],
    "vehicle": ["car", "bus", "truck", "van"], "cars": ["car"], "motorbike": ["minibike"], "motorcycle": ["minibike"],
    "bike": ["bicycle"], "plane": ["airplane"], "aircraft": ["airplane"], "boats": ["boat", "ship"],
    "clouds": ["sky"], "cloud": ["sky"], "heaven": ["sky"],
    "ocean": ["sea"], "waves": ["sea"], "pond": ["lake"], "stream": ["river"],
    "trees": ["tree"], "foliage": ["tree", "plant", "grass", "palm"], "vegetation": ["tree", "plant", "grass", "palm"],
    "leaves": ["tree", "plant"], "flowers": ["flower"], "lawn": ["grass"],
    "ground": ["earth", "land", "floor", "road", "sidewalk", "path", "sand", "field"], "dirt": ["earth", "dirt track"],
    "mountains": ["mountain"], "hills": ["hill"], "rocks": ["rock"], "stone": ["rock"],
    "buildings": ["building", "house", "skyscraper"], "architecture": ["building", "house", "skyscraper", "tower"],
    "walls": ["wall"], "street": ["road", "sidewalk"], "snow": ["earth", "land", "field", "mountain"],
    "food": ["food", "plate"], "statue": ["sculpture"], "toy": ["plaything"], "sign": ["signboard"],
    "window": ["windowpane"], "tv": ["television receiver"], "television": ["television receiver"],
    "screen": ["screen", "monitor", "crt screen"],
}


def log(msg):
    sys.stderr.write("[rt-segment] %s\n" % msg)
    sys.stderr.flush()


_device_cache = {}


def pick_device(requested):
    """
    Resolves "auto" to cuda / mps / cpu. Called when a model is first loaded, not
    at startup: initialising the GPU (torch.cuda.is_available) takes seconds on
    ROCm and the "info" request must stay fast.
    """
    if requested and requested != "auto":
        return requested

    if "auto" in _device_cache:
        return _device_cache["auto"]

    device = "cpu"

    try:
        import torch

        if torch.cuda.is_available():
            device = "cuda"
        elif hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
            device = "mps"
    except Exception:
        pass

    log("using device %s" % device)
    _device_cache["auto"] = device
    return device


# ---------------------------------------------------------------------------
# Dependency checks. Importing torch and transformers takes seconds, so the
# availability checks only look at installed package versions; the real import
# happens when a model is first used.

def _version_tuple(text):
    parts = []

    for piece in text.split("."):
        digits = ""

        for ch in piece:
            if ch.isdigit():
                digits += ch
            else:
                break

        if not digits:
            break

        parts.append(int(digits))

    return tuple(parts)


def check_transformers(feature, min_version, extra_packages=()):
    """(True, "") when torch and transformers >= min_version (and extra_packages) are installed, else (False, reason)."""
    import importlib.metadata
    import importlib.util

    for pkg in ("torch", "numpy") + tuple(extra_packages):
        if importlib.util.find_spec(pkg) is None:
            return False, "%s is not installed (needed for %s; pip install %s)" % (pkg, feature, " ".join(extra_packages) or pkg)

    try:
        version = importlib.metadata.version("transformers")
    except importlib.metadata.PackageNotFoundError:
        return False, "transformers is not installed (needed for %s)" % feature

    if _version_tuple(version) < _version_tuple(min_version):
        return False, "transformers %s is too old for %s (%s or newer is needed)" % (version, feature, min_version)

    return True, ""


# ---------------------------------------------------------------------------
# Hugging Face access checks (offline safe, bounded time)

_access_cache = {}


def check_repo_access(repo):
    """
    (True, "") when the weights of `repo` can be loaded: either they are in the
    local cache or the hub confirms access (gated repos need an approved
    request plus `hf auth login`). Otherwise (False, reason). Never blocks
    longer than HF_ACCESS_TIMEOUT seconds and never raises.
    """
    if repo in _access_cache:
        return _access_cache[repo]

    result = _check_repo_access(repo)
    _access_cache[repo] = result
    return result


def _check_repo_access(repo):
    try:
        from huggingface_hub import auth_check, constants, try_to_load_from_cache
    except Exception as e:
        return False, "huggingface_hub is not installed (%s)" % e

    try:
        if isinstance(try_to_load_from_cache(repo, "config.json"), str):
            return True, ""
    except Exception:
        pass

    if getattr(constants, "HF_HUB_OFFLINE", False):
        return False, "weights for %s are not in the local Hugging Face cache and HF_HUB_OFFLINE is set" % repo

    try:
        from huggingface_hub.errors import GatedRepoError, RepositoryNotFoundError
    except Exception:
        GatedRepoError = RepositoryNotFoundError = ()

    outcome = {}

    def worker():
        try:
            auth_check(repo)
            outcome["ok"] = True
        except GatedRepoError:
            outcome["error"] = ("access to the gated repository %s has not been granted to this Hugging Face account; "
                                "request it at https://huggingface.co/%s and log in with `hf auth login`" % (repo, repo))
        except RepositoryNotFoundError:
            outcome["error"] = "repository %s does not exist or requires a login (`hf auth login`)" % repo
        except Exception as e:
            outcome["error"] = "cannot verify access to %s (%s: %s)" % (repo, type(e).__name__, str(e).splitlines()[0][:120])

    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(HF_ACCESS_TIMEOUT)

    if t.is_alive():
        return False, "weights for %s are not cached and huggingface.co did not answer within %gs" % (repo, HF_ACCESS_TIMEOUT)

    if outcome.get("ok"):
        return True, "weights are downloaded from Hugging Face (%s) on first use" % repo

    return False, outcome.get("error", "cannot verify access to %s" % repo)


# ---------------------------------------------------------------------------
# Small image helpers

def image_key(image):
    """Cheap content hash so that consecutive requests on the same image reuse model state."""
    h = hashlib.blake2b(digest_size=16)
    h.update(("%dx%d" % image.size).encode())
    h.update(np.asarray(image.resize((64, 64), Image.BILINEAR)).tobytes())
    return h.hexdigest()


def label_components(binary):
    """Connected components (4-connectivity) of a boolean array; returns (labels, count)."""
    try:
        from scipy import ndimage

        labels, count = ndimage.label(binary)
        return labels, int(count)
    except Exception:
        pass

    h, w = binary.shape
    labels = np.zeros((h, w), np.int32)
    count = 0
    stack = []

    for y0 in range(h):
        for x0 in range(w):
            if not binary[y0, x0] or labels[y0, x0]:
                continue

            count += 1
            labels[y0, x0] = count
            stack.append((y0, x0))

            while stack:
                y, x = stack.pop()

                for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                    if 0 <= ny < h and 0 <= nx < w and binary[ny, nx] and not labels[ny, nx]:
                        labels[ny, nx] = count
                        stack.append((ny, nx))

    return labels, count


def resize_map(m, w, h):
    """Bilinear upsampling of a float map to (h, w), values kept in [0, 1]."""
    if m.shape == (h, w):
        return m.astype(np.float32)

    im = Image.fromarray(np.clip(m, 0.0, 1.0).astype(np.float32), mode="F").resize((w, h), Image.BILINEAR)
    return np.clip(np.asarray(im, dtype=np.float32), 0.0, 1.0)


def smoothstep(a, b, x):
    t = np.clip((x - a) / max(b - a, 1e-6), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def component_stats(labels, count, weight):
    """Per component: area (pixels), weighted centroid, bbox and peak position of `weight`."""
    stats = []

    for i in range(1, count + 1):
        ys, xs = np.nonzero(labels == i)

        if len(ys) == 0:
            continue

        wts = weight[ys, xs]
        peak = int(np.argmax(wts))
        stats.append({
            "label": i,
            "area": len(ys),
            "cx": float(xs.mean()),
            "cy": float(ys.mean()),
            "x0": int(xs.min()), "x1": int(xs.max()),
            "y0": int(ys.min()), "y1": int(ys.max()),
            "px": int(xs[peak]), "py": int(ys[peak]),
        })

    return stats


# ---------------------------------------------------------------------------
# Shared model holders (one per process, loaded on first use)

class SemanticModel:
    """
    ADE20K semantic segmentation shared by the class targets. Produces per-class
    probabilities at a reduced resolution; class maps are upsampled bilinearly on
    demand, so results stay soft and resolution independent.
    """

    short_name = ""

    def __init__(self, device):
        self.device = device
        self.model = None
        self.cache_key = None
        self.cache_probs = None   # [C, h, w] float32, per pixel distribution over classes
        self.name_to_id = {}

    @staticmethod
    def available():
        return True, ""

    def load(self):
        raise NotImplementedError

    def compute(self, image):
        """[C, h, w] numpy float32 class probabilities (any reduced resolution)."""
        raise NotImplementedError

    def set_labels(self, id2label):
        self.name_to_id = {}

        for i, name in id2label.items():
            self.name_to_id[name.strip().lower()] = int(i)

    def probabilities(self, image, key):
        """Class probabilities [C, h, w] for the image, cached for the last image."""
        self.load()

        if key == self.cache_key and self.cache_probs is not None:
            return self.cache_probs

        probs = self.compute(image)
        self.cache_key = key
        self.cache_probs = probs
        return probs

    def class_ids(self, names):
        self.load()
        ids = []

        for n in names:
            i = self.name_to_id.get(n.strip().lower())

            if i is not None and i not in ids:
                ids.append(i)

        return ids

    def classes_for_prompt(self, prompt):
        """ADE20K classes matching a free text concept; [] if nothing matches."""
        self.load()
        text = prompt.strip().lower()

        if not text:
            return []

        names = []

        if text in self.name_to_id:
            names.append(text)

        for word in text.replace(",", " ").split():
            word = word.strip()

            if word in self.name_to_id:
                names.append(word)

            for syn in CONCEPT_SYNONYMS.get(word, []):
                names.append(syn)

            if word.endswith("s") and word[:-1] in self.name_to_id:
                names.append(word[:-1])

        return self.class_ids(names)

    def class_map(self, probs, ids):
        if not ids:
            return np.zeros(probs.shape[1:], np.float32)

        return np.clip(probs[ids].sum(axis=0), 0.0, 1.0).astype(np.float32)


class EomtSemantic(SemanticModel):
    """
    EoMT (Encoder-only Mask Transformer, CVPR 2025) with a DINOv2 ViT-L backbone,
    ADE20K semantic checkpoint: MIT licensed, 58.4 mIoU, plain attention (no custom
    kernels). The processor tiles non-square images into 512 px crops; the crops'
    soft class maps are merged back with the processor's own routine.
    """

    short_name = "EoMT"

    def __init__(self, device):
        super().__init__(device)
        self.processor = None

    @staticmethod
    def available():
        ok, reason = check_transformers("EoMT", "4.54.0")
        return (ok, reason) if not ok else check_repo_access(EOMT_REPO)

    def load(self):
        if self.model is not None:
            return

        import torch
        from transformers import AutoImageProcessor, EomtForUniversalSegmentation

        t0 = time.time()
        self.device = pick_device(self.device)
        log("loading %s on %s" % (EOMT_REPO, self.device))
        self.processor = AutoImageProcessor.from_pretrained(EOMT_REPO)
        self.model = EomtForUniversalSegmentation.from_pretrained(EOMT_REPO)
        self.model.to(self.device)
        self.model.eval()
        self.torch = torch
        self.set_labels(self.model.config.id2label)
        log("loaded in %.1f s" % (time.time() - t0))

    def compute(self, image):
        torch = self.torch
        w, h = image.size
        inputs = self.processor(images=image, return_tensors="pt").to(self.device)

        with torch.no_grad():
            outputs = self.model(**inputs)
            size = self.processor.size
            edge = int(size.get("shortest_edge") or 512) if hasattr(size, "get") else 512
            masks = torch.nn.functional.interpolate(outputs.masks_queries_logits.float(), size=(edge, edge), mode="bilinear")
            classes = outputs.class_queries_logits.float().softmax(dim=-1)[..., :-1]
            seg = torch.einsum("bqc, bqhw -> bchw", classes, masks.sigmoid())
            # Half of the model's working resolution is plenty for soft class maps and keeps memory low.
            scale = (0.5 * edge) / min(w, h)
            target = (max(int(round(h * scale)), 8), max(int(round(w * scale)), 8))
            offsets = getattr(outputs, "patch_offsets", None)

            if offsets:
                merged = self.processor.merge_image_patches(seg, offsets, [target], size)[0]
            else:
                merged = torch.nn.functional.interpolate(seg, size=target, mode="bilinear", align_corners=False)[0]

            merged = merged.clamp(0.0, 1.0)
            merged = merged / merged.sum(dim=0, keepdim=True).clamp(min=1e-6)
            return merged.cpu().numpy().astype(np.float32)


class SegformerSemantic(SemanticModel):
    """
    SegFormer ADE20K (NVIDIA): small and fast, also on CPU, but NVIDIA's pretrained
    weights are licensed for research and evaluation use only. Opt in with
    RT_SEMANTIC_MODEL=segformer.
    """

    short_name = "SegFormer"
    CANVAS = int(os.environ.get("RT_SEGFORMER_CANVAS", "1024"))

    @staticmethod
    def available():
        ok, reason = check_transformers("SegFormer", "4.12.0")
        return (ok, reason) if not ok else check_repo_access(SEGFORMER_REPO)

    def load(self):
        if self.model is not None:
            return

        import torch
        from transformers import SegformerForSemanticSegmentation

        t0 = time.time()
        self.device = pick_device(self.device)
        log("loading %s on %s" % (SEGFORMER_REPO, self.device))
        self.model = SegformerForSemanticSegmentation.from_pretrained(SEGFORMER_REPO)
        self.model.to(self.device)
        self.model.eval()
        self.torch = torch
        self.set_labels(self.model.config.id2label)
        log("loaded in %.1f s" % (time.time() - t0))

    def compute(self, image):
        torch = self.torch
        w, h = image.size
        # SegFormer was trained on 512 px crops; more than ~1024 px costs time without helping. The image
        # is letterboxed into a fixed square canvas so that the GPU sees one tensor shape (ROCm and CUDA
        # tune kernels per shape; a new shape costs seconds the first time).
        canvas = self.CANVAS
        scale = min(1.0, float(canvas) / max(w, h))
        iw = max(int(round(w * scale)), 32)
        ih = max(int(round(h * scale)), 32)
        arr = np.asarray(image.resize((iw, ih), Image.BILINEAR), dtype=np.float32) / 255.0
        mean = np.array([0.485, 0.456, 0.406], np.float32)
        std = np.array([0.229, 0.224, 0.225], np.float32)
        arr = (arr - mean) / std
        full = np.zeros((canvas, canvas, 3), np.float32)
        full[:ih, :iw] = arr
        x = torch.from_numpy(full.transpose(2, 0, 1)[None]).to(self.device)

        with torch.no_grad():
            logits = self.model(pixel_values=x).logits   # [1, C, canvas/4, canvas/4]
            logits = logits[:, :, :max(ih // 4, 1), :max(iw // 4, 1)]
            return torch.softmax(logits.float(), dim=1)[0].cpu().numpy().astype(np.float32)


SEMANTIC_CLASSES = {"eomt": EomtSemantic, "segformer": SegformerSemantic}


def semantic_class():
    return SEMANTIC_CLASSES.get(SEMANTIC_MODEL, EomtSemantic)


class Sam2Prompter:
    """SAM 2.1 (Transformers) with the image embedding cached for the last image."""

    SHARPNESS = 3.0

    def __init__(self, device):
        self.device = device
        self.model = None
        self.processor = None
        self.cache_key = None
        self.cache_embeddings = None

    @staticmethod
    def available():
        ok, reason = check_transformers("SAM 2", "4.56.0")
        return (ok, reason) if not ok else check_repo_access(SAM2_REPO)

    def load(self):
        if self.model is not None:
            return

        import torch
        from transformers import Sam2Model, Sam2Processor

        t0 = time.time()
        self.device = pick_device(self.device)
        log("loading %s on %s" % (SAM2_REPO, self.device))
        self.model = Sam2Model.from_pretrained(SAM2_REPO)
        self.model.to(self.device)
        self.model.eval()
        self.processor = Sam2Processor.from_pretrained(SAM2_REPO)
        self.torch = torch
        log("loaded in %.1f s" % (time.time() - t0))

    def predict(self, image, key, points=None, labels=None, box=None, multimask=True):
        """
        Soft masks [n, H, W] in [0, 1] (sigmoid of the upsampled logits) and their
        predicted IoU scores [n]. Points are (x, y) in pixels, box is (x0, y0, x1, y1).
        """
        self.load()
        torch = self.torch
        kwargs = {}

        if points:
            kwargs["input_points"] = [[list(map(list, points))]]
            kwargs["input_labels"] = [[list(labels or [1] * len(points))]]

        if box is not None:
            kwargs["input_boxes"] = [[list(box)]]

        inputs = self.processor(images=image, return_tensors="pt", **kwargs).to(self.device)

        with torch.no_grad():
            if key != self.cache_key or self.cache_embeddings is None:
                self.cache_embeddings = self.model.get_image_embeddings(inputs["pixel_values"])
                self.cache_key = key

            model_inputs = {k: v for k, v in inputs.items() if k not in ("pixel_values", "original_sizes")}
            outputs = self.model(image_embeddings=self.cache_embeddings, multimask_output=multimask, **model_inputs)

        logits = self.processor.post_process_masks(outputs.pred_masks.cpu(), inputs["original_sizes"], binarize=False)[0]
        logits = logits[0].float()   # [n, H, W]
        # SAM's logits are not calibrated probabilities: pixels well outside a mask still sit around -1
        # (sigmoid 0.27). Sharpening keeps a soft edge of a few pixels but makes the outside really 0.
        soft = torch.sigmoid(logits * self.SHARPNESS).numpy().astype(np.float32)
        scores = outputs.iou_scores[0, 0].detach().float().cpu().numpy()
        return soft, scores


class BiRefNetModel:
    """
    BiRefNet (Bilateral Reference Network, MIT): dichotomous image segmentation,
    i.e. a soft "salient foreground" mask without any class knowledge. The
    weights are loaded through the author's modeling code stored in the Hugging
    Face repo (trust_remote_code), which needs the einops, kornia and timm packages.
    """

    INPUT = int(os.environ.get("RT_BIREFNET_SIZE", "1024"))

    def __init__(self, device):
        self.device = device
        self.model = None
        self.repo = None
        self.cache_key = None
        self.cache_mask = None   # [H, W] float32 at the input image size

    @staticmethod
    def repo_for(device):
        if BIREFNET_REPO and BIREFNET_REPO != "auto":
            return BIREFNET_REPO

        return BIREFNET_CPU_REPO if device == "cpu" else BIREFNET_GPU_REPO

    @staticmethod
    def available(device):
        ok, reason = check_transformers("BiRefNet", "4.40.0", extra_packages=("einops", "kornia", "timm"))

        if not ok:
            return False, reason

        # The device is not resolved here (initialising the GPU costs seconds): with "auto" both
        # candidate repos are public, so access to either one stands for both.
        if BIREFNET_REPO and BIREFNET_REPO != "auto":
            candidates = [BIREFNET_REPO]
        elif device in ("auto", ""):
            candidates = [BIREFNET_GPU_REPO, BIREFNET_CPU_REPO]
        else:
            candidates = [BiRefNetModel.repo_for(device)]

        reason = ""

        for repo in candidates:
            ok, reason = check_repo_access(repo)

            if ok:
                break
        else:
            return False, reason

        note = "runs the model author's code from the Hugging Face repo %s (trust_remote_code)" % " / ".join(candidates)
        return True, note + ("; " + reason if reason else "")

    def load(self):
        if self.model is not None:
            return

        import torch
        from transformers import AutoModelForImageSegmentation

        t0 = time.time()
        self.device = pick_device(self.device)
        self.repo = self.repo_for(self.device)
        log("loading %s on %s (trust_remote_code)" % (self.repo, self.device))
        # The checkpoints are stored in fp16; load as fp32 so that CPU inference works as well.
        self.model = AutoModelForImageSegmentation.from_pretrained(self.repo, trust_remote_code=True, dtype=torch.float32)
        self.model.to(self.device)
        self.model.eval()
        self.torch = torch
        log("loaded in %.1f s" % (time.time() - t0))

    def foreground(self, image, key):
        """Soft foreground probability [H, W] in [0, 1] at the image size, cached for the last image."""
        self.load()

        if key == self.cache_key and self.cache_mask is not None:
            return self.cache_mask

        torch = self.torch
        w, h = image.size
        # Always the models' native 1024 x 1024 (BiRefNet_dynamic accepts other sizes, but one fixed
        # shape means the GPU kernels are tuned once per process instead of once per aspect ratio).
        iw = ih = self.INPUT
        arr = np.asarray(image.resize((iw, ih), Image.BILINEAR), dtype=np.float32) / 255.0
        arr = (arr - np.array([0.485, 0.456, 0.406], np.float32)) / np.array([0.229, 0.224, 0.225], np.float32)
        x = torch.from_numpy(arr.transpose(2, 0, 1)[None]).to(self.device)

        with torch.no_grad():
            preds = self.model(x)
            pred = preds[-1] if isinstance(preds, (list, tuple)) else preds
            pred = torch.sigmoid(pred.float())

            if pred.dim() == 4:
                pred = pred[0, 0]
            elif pred.dim() == 3:
                pred = pred[0]

        mask = resize_map(pred.cpu().numpy().astype(np.float32), w, h)
        self.cache_key = key
        self.cache_mask = mask
        return mask


class ViTMatteRefiner:
    """
    ViTMatte (Apache-2.0, native in Transformers): trimap based matting used to
    refine the edges of any soft mask. The trimap keeps confident foreground and
    background and leaves a band of a few percent of the diagonal around the
    edge (plus every uncertain pixel) for the model to re-estimate.
    """

    def __init__(self, device):
        self.device = device
        self.model = None
        self.processor = None

    @staticmethod
    def available():
        ok, reason = check_transformers("ViTMatte", "4.33.0")
        return (ok, reason) if not ok else check_repo_access(VITMATTE_REPO)

    def load(self):
        if self.model is not None:
            return

        import torch
        from transformers import VitMatteForImageMatting, VitMatteImageProcessor

        t0 = time.time()
        self.device = pick_device(self.device)
        log("loading %s on %s" % (VITMATTE_REPO, self.device))
        self.processor = VitMatteImageProcessor.from_pretrained(VITMATTE_REPO)
        self.model = VitMatteForImageMatting.from_pretrained(VITMATTE_REPO)
        self.model.to(self.device)
        self.model.eval()
        self.torch = torch
        log("loaded in %.1f s" % (time.time() - t0))

    CANVAS = int(os.environ.get("RT_VITMATTE_CANVAS", "1024"))

    @staticmethod
    def near_edge(binary, band_px):
        """True within band_px of the boundary of a boolean mask."""
        try:
            from scipy import ndimage

            # Distance of every pixel to the mask boundary: for foreground pixels the distance to the
            # nearest background pixel and vice versa.
            dist = np.where(binary, ndimage.distance_transform_edt(binary), ndimage.distance_transform_edt(~binary))
            return dist <= band_px
        except Exception:
            pass

        # Fallback without scipy: iterative 4-neighbour dilation of the boundary (Chebyshev band).
        edge = np.zeros_like(binary)
        edge[:-1, :] |= binary[:-1, :] != binary[1:, :]
        edge[1:, :] |= binary[:-1, :] != binary[1:, :]
        edge[:, :-1] |= binary[:, :-1] != binary[:, 1:]
        edge[:, 1:] |= binary[:, :-1] != binary[:, 1:]
        band = edge.copy()

        for _ in range(int(band_px)):
            grown = band.copy()
            grown[1:, :] |= band[:-1, :]
            grown[:-1, :] |= band[1:, :]
            grown[:, 1:] |= band[:, :-1]
            grown[:, :-1] |= band[:, 1:]
            band = grown

        return band

    def trimap(self, mask, band_px):
        """0 = background, 128 = unknown, 255 = foreground."""
        binary = mask > 0.5
        unknown = self.near_edge(binary, band_px)
        tri = np.full(mask.shape, 128, np.uint8)
        tri[np.logical_and(~unknown, mask > 0.9)] = 255
        tri[np.logical_and(~unknown, mask < 0.1)] = 0
        return tri

    def refine(self, image, mask):
        """Returns the matted alpha [H, W] in [0, 1]; confident regions of the trimap are kept as they are."""
        self.load()
        torch = self.torch
        w, h = image.size
        band_px = max(int(round(MATTING_BAND * np.hypot(w, h))), 2)
        tri = self.trimap(mask, band_px)

        if not (tri == 128).any():
            return mask

        # Fixed square canvas (background in the padding) so that the GPU sees one shape per process.
        canvas = max(self.CANVAS, ((w + 31) // 32) * 32, ((h + 31) // 32) * 32)
        padded = Image.new("RGB", (canvas, canvas))
        padded.paste(image, (0, 0))
        tri_full = np.zeros((canvas, canvas), np.uint8)
        tri_full[:h, :w] = tri
        inputs = self.processor(images=padded, trimaps=Image.fromarray(tri_full, mode="L"), return_tensors="pt").to(self.device)

        with torch.no_grad():
            alpha = self.model(**inputs).alphas[0, 0].float().cpu().numpy()

        alpha = np.clip(alpha[:h, :w], 0.0, 1.0).astype(np.float32)
        return np.where(tri == 255, 1.0, np.where(tri == 0, 0.0, alpha)).astype(np.float32)


class ModelAdapter:
    id = ""
    name = ""
    description = ""
    hidden = False

    def __init__(self, device):
        self.device = device

    def available(self):
        """Return (True, note) if the model can be used, else (False, reason)."""
        return True, ""

    def segment(self, image, target, prompt, point, threshold):
        """Return (float32 HxW array in [0, 1], instance count) for the given PIL image."""
        raise NotImplementedError


# ---------------------------------------------------------------------------
# Class targets (subject / sky / background) from ADE20K semantics, shared by
# the SegFormer and SAM 2 providers

class SemanticTargets:
    def __init__(self, segformer):
        self.seg = segformer

    def sky(self, probs):
        return self.seg.class_map(probs, self.seg.class_ids(["sky"]))

    def stuff(self, probs):
        return self.seg.class_map(probs, self.seg.class_ids(STUFF_CLASSES))

    def sky_top_points(self, probs, w, h):
        """Up to three (x, y) pixel positions on confident sky near the top edge, one per horizontal third."""
        sky = self.sky(probs)
        ph, pw = sky.shape
        rows = max(int(0.12 * ph), 1)
        pts = []

        for k in range(3):
            x0 = k * pw // 3
            x1 = max((k + 1) * pw // 3, x0 + 1)
            block = sky[:rows, x0:x1]
            ys, xs = np.nonzero(block >= 0.5)

            if len(ys) < 0.2 * block.size:
                continue

            # Center of mass of the confident sky pixels, kept away from the very edge where SAM is unreliable.
            cx = min(max((x0 + xs.mean() + 0.5) * w / pw, 0.03 * w), 0.97 * w)
            cy = min(max((ys.mean() + 0.5) * h / ph, 0.03 * h), 0.97 * h)
            pts.append((cx, cy))

        return pts

    @staticmethod
    def touching_top(low):
        """Keeps only the components of low > 0.5 that reach the top 10% of the image."""
        labels, count = label_components(low > 0.5)

        if count == 0:
            return low

        rows = max(int(0.1 * low.shape[0]), 1)
        keep = np.unique(labels[:rows])
        keep = keep[keep > 0]

        if len(keep) == 0:
            return low

        return np.where(np.isin(labels, keep), low, 0.0).astype(np.float32)

    def subject(self, probs, prompt, w, h):
        """
        Low resolution subject probability and a description of how it was found:
        the Concept classes if given, else people/animals, else vehicles, else the
        most salient "thing" (anything that is not stuff like walls, sky or ground).
        """
        seg = self.seg

        if prompt and prompt.strip():
            ids = seg.classes_for_prompt(prompt)

            if not ids:
                raise ValueError("concept '%s' is not one of the ADE20K classes this model knows (try person, animal, car, "
                                 "tree, water, building, ...); SAM 3 is needed for free text" % prompt.strip())

            return seg.class_map(probs, ids), "concept"

        area = probs.shape[1] * probs.shape[2]

        for group, how, minimum in ((SUBJECT_PRIMARY, "person/animal", 0.02), (SUBJECT_SECONDARY, "vehicle", 0.03)):
            m = seg.class_map(probs, seg.class_ids(group))
            confident = m > 0.5

            # Needs a meaningful, confident region; weak responses (a fursuit read as 55% "person") go to the
            # class-free fallbacks instead.
            if confident.sum() >= minimum * area and float(m[confident].mean()) >= 0.75:
                return m, how

        # Salient thing: not stuff, preferring large components near the image center.
        thing = 1.0 - self.stuff(probs)
        labels, count = label_components(thing > 0.5)
        best = None
        best_score = 0.0
        ph, pw = thing.shape

        for s in component_stats(labels, count, thing):
            frac = s["area"] / float(area)

            if frac < 0.005:
                continue

            d = np.hypot((s["cx"] / pw - 0.5) / 0.5, (s["cy"] / ph - 0.5) / 0.5)
            score = frac * (1.0 - 0.6 * min(d, 1.0))

            if score > best_score:
                best_score = score
                best = s

        if best is None:
            return np.zeros_like(thing), "none"

        return np.where(labels == best["label"], thing, 0.0).astype(np.float32), "salient"


# ---------------------------------------------------------------------------
# SAM 2.1 (+ SegFormer for the class targets)
#
# SAM 2 has no text prompts, so "subject" and "sky" come from ADE20K
# semantics (SegFormer); the subject is then sharpened by prompting SAM 2
# with a box and a point per semantic component. "object" uses the clicked
# point directly.

class Sam2TransformersModel(ModelAdapter):
    id = "sam2"
    name = "SAM 2.1 + %s (ADE20K)" % semantic_class().short_name
    description = ("Object: SAM 2.1 at the clicked point. Subject, sky and background: ADE20K classes from %s, "
                   "refined by SAM 2.1. Concept: an ADE20K class name (person, animal, car, tree, water, ...)."
                   % semantic_class().short_name)

    def __init__(self, device):
        super().__init__(device)
        self.sam = Sam2Prompter(device)
        self.segformer = semantic_class()(device)
        self.targets = SemanticTargets(self.segformer)

    def available(self):
        ok, reason = Sam2Prompter.available()

        if not ok:
            return False, reason

        ok2, reason2 = semantic_class().available()

        if not ok2:
            return False, reason2

        return True, reason

    def segment(self, image, target, prompt, point, threshold):
        w, h = image.size
        key = image_key(image)

        if target == "object":
            if point is None:
                point = (0.5, 0.5)

            soft, scores = self.sam.predict(image, key, points=[(point[0] * w, point[1] * h)], multimask=True)

            if soft.shape[0] == 0:
                return np.zeros((h, w), np.float32), 0

            return soft[int(np.argmax(scores))], 1

        probs = self.segformer.probabilities(image, key)

        if target == "sky":
            return self.sky(image, key, probs, w, h)

        low, how = self.targets.subject(probs, prompt, w, h)

        out = None

        if how in ("concept", "person/animal", "vehicle"):
            out, n = self.refine(image, key, low, w, h)

            if how != "concept" and (out > 0.5).mean() < 0.01:
                out = None

        if out is None:
            # No recognised class: the most salient SAM 2 region(s), scored by size, position and border contact.
            out, n = self.salient(image, key, probs, w, h)

        if target == "background":
            out = 1.0 - out

        return out, n

    def sky(self, image, key, probs, w, h):
        """
        Sky probability from SegFormer, gated by the SAM 2 regions grown from the
        confident sky pixels at the top edge. SegFormer alone takes any flat, bright
        surface for sky; SAM alone takes any region at the top for sky; the product
        keeps what both agree on.
        """
        sem = self.targets.sky(probs)
        pts = self.targets.sky_top_points(probs, w, h)

        if not pts:
            return resize_map(self.targets.touching_top(sem), w, h), (1 if (sem > 0.5).any() else 0)

        gate = np.zeros((h, w), np.float32)
        used = 0

        sem_full = resize_map(sem, w, h)

        for pt in pts:
            # Among SAM's well scored candidates take the one that is purest sky according to
            # SegFormer (sky alone rather than sky plus mountains or flat ground), largest on ties.
            soft, scores = self.sam.predict(image, key, points=[pt], multimask=True)

            if soft.shape[0] == 0 or float(scores.max()) < 0.5:
                continue

            good = [i for i in range(soft.shape[0]) if scores[i] >= scores.max() - 0.1]

            def purity(i):
                inside = soft[i] > 0.5
                return (float(sem_full[inside].mean()) if inside.any() else 0.0, float(inside.sum()))

            gate = np.maximum(gate, soft[max(good, key=purity)])
            used += 1

        if used == 0:
            return resize_map(self.targets.touching_top(sem), w, h), 1

        return np.clip(sem_full * gate, 0.0, 1.0).astype(np.float32), used

    def largest_candidate(self, image, key, pt, min_score=0.5):
        """SAM 2 at one point: the largest of the well scored candidate masks (the whole object rather than a part), or None."""
        soft, scores = self.sam.predict(image, key, points=[pt], multimask=True)

        if soft.shape[0] == 0 or float(scores.max()) < min_score:
            return None

        good = [i for i in range(soft.shape[0]) if scores[i] >= scores.max() - 0.1]
        return soft[max(good, key=lambda i: float(soft[i].sum()))]

    def salient(self, image, key, probs, w, h):
        """
        Salient object(s) without any class knowledge: SAM 2 masks from a grid of
        point prompts, scored by size (not tiny, not the whole frame), centrality,
        border contact (backgrounds touch three or more borders) and how much of
        them SegFormer takes for stuff (walls, sky, ground, vegetation).
        """
        stuff = resize_map(self.targets.stuff(probs), w, h)
        candidates = []

        for gy in range(4):
            for gx in range(5):
                best = self.largest_candidate(image, key, ((gx + 0.5) * w / 5.0, (gy + 0.5) * h / 4.0))

                if best is not None:
                    candidates.append(best)

        scored = []
        area = float(w * h)

        for soft in candidates:
            binary = soft > 0.5
            frac = binary.sum() / area

            if frac < 0.005 or frac > 0.85:
                continue

            ys, xs = np.nonzero(binary)
            d = np.hypot((xs.mean() / w - 0.5) / 0.5, (ys.mean() / h - 0.5) / 0.5)
            borders = sum(int(side.mean() > 0.3) for side in (binary[0], binary[-1], binary[:, 0], binary[:, -1]))

            if borders >= 3:
                continue

            stuffness = float((stuff * soft).sum() / max(soft.sum(), 1.0))
            value = np.sqrt(frac) * (1.0 - 0.6 * min(d, 1.0)) * (1.0 - 0.5 * stuffness) * (1.0 - 0.15 * borders)
            scored.append((value, soft, binary))

        if not scored:
            # Nothing passes the filters (e.g. a close-up filling the frame): SAM 2 around the center.
            best = self.largest_candidate(image, key, (0.5 * w, 0.55 * h), min_score=0.0)
            return (np.zeros((h, w), np.float32), 0) if best is None else (best, 1)

        scored.sort(key=lambda t: -t[0])
        chosen = []

        for value, soft, binary in scored:
            if chosen and value < 0.7 * scored[0][0]:
                break

            # Skip near duplicates and parts of what is already selected.
            overlap = False

            for _, _, cb in chosen:
                inter = np.logical_and(binary, cb).sum()

                if inter > 0.5 * min(binary.sum(), cb.sum()):
                    overlap = True
                    break

            if not overlap:
                chosen.append((value, soft, binary))

        out = np.zeros((h, w), np.float32)

        for _, soft, _ in chosen:
            out = np.maximum(out, soft)

        return out, len(chosen)

    def refine(self, image, key, low, w, h):
        """Sharpen a low resolution semantic map with SAM 2 masks prompted per component."""
        out = resize_map(low, w, h)
        labels, count = label_components(low > 0.5)
        ph, pw = low.shape
        area = ph * pw
        stats = [s for s in component_stats(labels, count, low) if s["area"] >= 0.003 * area]
        stats.sort(key=lambda s: -s["area"])
        sx = w / float(pw)
        sy = h / float(ph)
        accepted = 0

        for s in stats[:3]:
            pad = 0.03 * max(w, h)
            box = (max((s["x0"]) * sx - pad, 0.0), max((s["y0"]) * sy - pad, 0.0),
                   min((s["x1"] + 1) * sx + pad, w - 1.0), min((s["y1"] + 1) * sy + pad, h - 1.0))
            pt = ((s["px"] + 0.5) * sx, (s["py"] + 0.5) * sy)
            soft, scores = self.sam.predict(image, key, points=[pt], labels=[1], box=box, multimask=False)

            if soft.shape[0] == 0:
                continue

            cand = soft[0]
            comp = resize_map((labels == s["label"]).astype(np.float32), w, h) > 0.5
            cand_bin = cand > 0.5
            inter = np.logical_and(cand_bin, comp).sum()
            union = np.logical_or(cand_bin, comp).sum()
            iou = inter / float(max(union, 1))

            if iou < 0.4:
                continue

            # Inside the box SAM's edges win; the semantic estimate of other components is kept.
            x0, y0, x1, y1 = (int(box[0]), int(box[1]), int(np.ceil(box[2])) + 1, int(np.ceil(box[3])) + 1)
            region = out[y0:y1, x0:x1]
            others = np.where(comp[y0:y1, x0:x1], 0.0, region)
            out[y0:y1, x0:x1] = np.maximum(cand[y0:y1, x0:x1], others)
            accepted += 1

        return np.clip(out, 0.0, 1.0).astype(np.float32), (accepted if accepted else len(stats))


# ---------------------------------------------------------------------------
# BiRefNet for subject / background, everything else as the SAM 2.1 provider

class BiRefNetProviderModel(Sam2TransformersModel):
    id = "birefnet"
    name = "BiRefNet + SAM 2.1 + %s" % semantic_class().short_name
    description = ("Subject and background: BiRefNet (salient foreground, soft edges). Object: SAM 2.1 at the clicked "
                   "point. Sky and Concept subjects: ADE20K classes from %s refined by SAM 2.1."
                   % semantic_class().short_name)

    def __init__(self, device):
        super().__init__(device)
        self.birefnet = BiRefNetModel(device)

    def available(self):
        ok, reason = BiRefNetModel.available(self.device)

        if not ok:
            return False, reason

        ok2, reason2 = super().available()

        if not ok2:
            return False, reason2

        return True, reason

    def segment(self, image, target, prompt, point, threshold):
        if target in ("subject", "background") and not (prompt and prompt.strip()):
            out = self.birefnet.foreground(image, image_key(image))
            n = 1 if (out > 0.5).any() else 0

            if target == "background":
                out = 1.0 - out

            return out, n

        return super().segment(image, target, prompt, point, threshold)


# ---------------------------------------------------------------------------
# Semantic classes alone (no SAM): one model, simpler, with SegFormer also CPU friendly

class SemanticOnlyModel(ModelAdapter):
    id = "semantic"
    name = "%s ADE20K classes (no SAM)" % semantic_class().short_name
    description = ("Semantic classes only, no SAM: one model, simpler and lighter. Object: the class under the clicked "
                   "point. Concept: an ADE20K class name.")

    def __init__(self, device):
        super().__init__(device)
        self.segformer = semantic_class()(device)
        self.targets = SemanticTargets(self.segformer)

    def available(self):
        return semantic_class().available()

    def segment(self, image, target, prompt, point, threshold):
        w, h = image.size
        key = image_key(image)
        probs = self.segformer.probabilities(image, key)

        if target == "sky":
            sem = self.targets.touching_top(self.targets.sky(probs))
            return resize_map(sem, w, h), (1 if (sem > 0.5).any() else 0)

        if target == "object":
            if point is None:
                point = (0.5, 0.5)

            ph, pw = probs.shape[1:]
            px = min(max(int(point[0] * pw), 0), pw - 1)
            py = min(max(int(point[1] * ph), 0), ph - 1)
            cls = int(np.argmax(probs[:, py, px]))
            m = probs[cls]
            labels, count = label_components(m > 0.5)
            lab = labels[py, px]

            if lab > 0:
                # Keep the clicked connected region (soft edges from the probabilities around it).
                keep = labels == lab
                keep = np.logical_or.reduce([keep, np.roll(keep, 1, 0), np.roll(keep, -1, 0), np.roll(keep, 1, 1), np.roll(keep, -1, 1)])
                m = np.where(keep, m, 0.0).astype(np.float32)

            return resize_map(m, w, h), 1

        low, how = self.targets.subject(probs, prompt, w, h)
        out = resize_map(low, w, h)

        if target == "background":
            out = 1.0 - out

        return out, (0 if how == "none" else 1)


# ---------------------------------------------------------------------------
# SAM 3 through Hugging Face Transformers (Sam3Model / Sam3Processor)

class Sam3TransformersModel(ModelAdapter):
    id = "sam3"
    name = "SAM 3 (Meta, Transformers)"
    description = "Segment Anything 3: concept prompts (free text), boxes around a clicked point."
    repo = SAM3_REPO

    def __init__(self, device):
        super().__init__(device)
        self.model = None
        self.processor = None

    def available(self):
        if not SAM3_ENABLED:
            return False, SAM3_DISABLED_REASON

        ok, reason = check_transformers("SAM 3", "5.0.0")
        return (ok, reason) if not ok else check_repo_access(self.repo)

    def load(self):
        if self.model is not None:
            return

        import torch
        from transformers import Sam3Model, Sam3Processor

        t0 = time.time()
        self.device = pick_device(self.device)
        log("loading %s on %s" % (self.repo, self.device))
        self.model = Sam3Model.from_pretrained(self.repo)
        self.model.to(self.device)
        self.model.eval()
        self.processor = Sam3Processor.from_pretrained(self.repo)
        self.torch = torch
        log("loaded in %.1f s" % (time.time() - t0))

    def segment(self, image, target, prompt, point, threshold):
        self.load()
        torch = self.torch
        w, h = image.size
        text = prompt.strip() if prompt else ""
        invert = False

        if target == "background":
            invert = True

        if not text:
            text = DEFAULT_PROMPTS.get(target, "")

        kwargs = {}
        box = None

        if target == "object" and point is not None:
            # A positive box around the clicked point; the text (if any) narrows the concept.
            bw = max(0.06 * min(w, h), 8.0)
            cx, cy = point[0] * w, point[1] * h
            box = [max(cx - bw, 0.0), max(cy - bw, 0.0), min(cx + bw, w - 1.0), min(cy + bw, h - 1.0)]
            kwargs["input_boxes"] = [[box]]
            kwargs["input_boxes_labels"] = [[1]]

        if text:
            kwargs["text"] = text
        elif box is None:
            return np.zeros((h, w), np.float32), 0

        inputs = self.processor(images=image, return_tensors="pt", **kwargs).to(self.device)

        with torch.no_grad():
            outputs = self.model(**inputs)

        results = self.processor.post_process_instance_segmentation(
            outputs,
            threshold=float(threshold),
            mask_threshold=0.5,
            target_sizes=inputs.get("original_sizes").tolist(),
        )[0]

        masks = results.get("masks")
        scores = results.get("scores")
        n = 0 if masks is None else len(masks)

        if n == 0:
            out = np.zeros((h, w), np.float32)
        else:
            m = masks.detach().cpu().numpy().astype(np.float32)
            m = m.reshape(n, m.shape[-2], m.shape[-1])

            if target == "object" and point is not None:
                # Prefer the instance that contains the clicked point.
                px = min(max(int(point[0] * w), 0), w - 1)
                py = min(max(int(point[1] * h), 0), h - 1)
                hits = [i for i in range(n) if m[i, py, px] > 0.5]

                if hits:
                    best = max(hits, key=lambda i: float(scores[i]) if scores is not None else 0.0)
                    out = m[best]
                else:
                    out = m.max(axis=0)
            else:
                out = m.max(axis=0)

        if invert:
            out = 1.0 - out

        return out, n


# ---------------------------------------------------------------------------
# SAM 3 through Meta's official package (pip install -e . from facebookresearch/sam3)

class Sam3OfficialModel(ModelAdapter):
    id = "sam3-official"
    name = "SAM 3 (Meta, official package)"
    description = "Same model through the facebookresearch/sam3 package."

    def __init__(self, device):
        super().__init__(device)
        self.processor = None

    def available(self):
        import importlib.util

        if not SAM3_ENABLED:
            return False, SAM3_DISABLED_REASON

        if importlib.util.find_spec("sam3") is None:
            return False, "the sam3 package is not installed (pip install from github.com/facebookresearch/sam3)"

        return check_repo_access(SAM3_REPO)

    def load(self):
        if self.processor is not None:
            return

        from sam3.model_builder import build_sam3_image_model
        from sam3.model.sam3_image_processor import Sam3Processor

        t0 = time.time()
        self.device = pick_device(self.device)
        model = build_sam3_image_model(device=self.device)
        self.processor = Sam3Processor(model, device=self.device)
        log("sam3 official loaded in %.1f s" % (time.time() - t0))

    def segment(self, image, target, prompt, point, threshold):
        self.load()
        w, h = image.size
        text = prompt.strip() if prompt else ""
        invert = target == "background"

        if not text:
            text = DEFAULT_PROMPTS.get(target, "")

        state = self.processor.set_image(image)
        self.processor.set_confidence_threshold(float(threshold), state)

        if target == "object" and point is not None:
            bw = max(0.06 * min(w, h), 8.0)
            cx, cy = point[0] * w, point[1] * h
            self.processor.add_geometric_prompt([cx / w, cy / h, 2 * bw / w, 2 * bw / h], True, state)

        if text:
            state = self.processor.set_text_prompt(text, state)

        logits = state.get("masks_logits")
        n = 0 if logits is None else int(logits.shape[0])

        if n == 0:
            out = np.zeros((h, w), np.float32)
        else:
            m = logits.detach().float().cpu().numpy().reshape(n, h, w)
            out = m.max(axis=0)

        if invert:
            out = 1.0 - out

        return out, n


# ---------------------------------------------------------------------------
# Heuristic stand-in, for testing the pipeline without any ML dependency.
# Hidden: usable when requested by id, never offered in the GUI.

class DummyModel(ModelAdapter):
    id = "dummy-python"
    name = "Heuristic (Python placeholder)"
    description = "Position and color heuristics; no machine learning. For tests only."
    hidden = True

    def segment(self, image, target, prompt, point, threshold):
        rgb = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
        h, w, _ = rgb.shape
        lum = 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]
        ys = np.linspace(0.0, 1.0, h, dtype=np.float32)[:, None]
        xs = np.linspace(0.0, 1.0, w, dtype=np.float32)[None, :]

        blue = np.clip(rgb[..., 2] - np.maximum(rgb[..., 0], rgb[..., 1]), 0, 1)
        sky = smoothstep(0.05, 0.3, blue) * smoothstep(0.3, 0.6, lum) * (1 - smoothstep(0.45, 0.9, ys + 0 * xs))

        if target == "sky":
            out = sky
        elif target in ("subject", "background"):
            rho = np.sqrt(((xs - 0.5) / 0.5) ** 2 + ((ys - 0.55) / 0.5) ** 2)
            out = (1 - smoothstep(0.35, 0.85, rho)) * (1 - sky)

            if target == "background":
                out = 1 - out
        else:
            px, py = (point if point is not None else (0.5, 0.5))
            ref = rgb[min(int(py * h), h - 1), min(int(px * w), w - 1)]
            d = np.sqrt(((rgb - ref) ** 2).sum(axis=-1))
            similar = 1 - smoothstep(0.1, 0.35, d)
            dist = np.sqrt(((xs - px) * w) ** 2 + ((ys - py) * h) ** 2) / np.hypot(w, h)
            out = np.maximum(np.exp(-(dist ** 2) / (2 * 0.04 ** 2)), np.exp(-(dist ** 2) / (2 * 0.18 ** 2)) * similar)

        return np.clip(out, 0, 1).astype(np.float32), 1


MODEL_CLASSES = [Sam3TransformersModel, Sam3OfficialModel, BiRefNetProviderModel, Sam2TransformersModel, SemanticOnlyModel, DummyModel]
PREFERRED_MODELS = ["sam3", "birefnet", "sam2", "semantic"]
SAM3_MODEL_IDS = ("sam3", "sam3-official")


def choose_default_model(models):
    """SAM 3 when its gated weights are accessible, otherwise BiRefNet + SAM 2.1, SAM 2.1 + semantic, semantic only; "" if none works."""
    for mid in PREFERRED_MODELS:
        if mid in SAM3_MODEL_IDS and not SAM3_ENABLED:
            continue

        m = models.get(mid)

        if m is not None and not m.hidden and m.available()[0]:
            return mid

    return ""


class Server:
    def __init__(self, device):
        self.device = device
        self.models = {}
        self.refiner = ViTMatteRefiner(device)

        for cls in MODEL_CLASSES:
            self.models[cls.id] = cls(device)

    def info(self):
        models = []

        for m in self.models.values():
            ok, reason = m.available()
            models.append({"id": m.id, "name": m.name, "description": m.description, "available": bool(ok),
                           "reason": reason, "hidden": bool(m.hidden)})

        refine_ok, refine_reason = ViTMatteRefiner.available()
        return {"ok": True, "models": models, "default": choose_default_model(self.models), "device": self.device,
                "refine": {"available": bool(refine_ok), "reason": refine_reason, "model": VITMATTE_REPO},
                "python": "%d.%d" % sys.version_info[:2]}

    def segment(self, req):
        model_id = req.get("model") or choose_default_model(self.models)
        model = self.models.get(model_id)

        if model is None:
            raise ValueError("unknown model '%s'" % model_id)

        ok, reason = model.available()

        if not ok:
            raise RuntimeError(reason)

        image = Image.open(req["image"]).convert("RGB")
        point = req.get("point")

        if point is not None:
            point = (float(point[0]), float(point[1]))

        t0 = time.time()
        mask, instances = model.segment(image, req.get("target", "subject"), req.get("prompt", ""), point,
                                        req.get("threshold", 0.5))
        mask = np.clip(mask, 0.0, 1.0).astype(np.float32)
        refined = False

        if req.get("refine") and instances > 0:
            ok, reason = ViTMatteRefiner.available()

            if not ok:
                raise RuntimeError("edge refinement is not available: %s" % reason)

            t1 = time.time()
            mask = self.refiner.refine(image, mask)
            refined = True
            log("edge refinement in %.2f s" % (time.time() - t1))

        mask8 = (mask * 255.0 + 0.5).astype(np.uint8)
        Image.fromarray(mask8, mode="L").save(req["mask"], format="PPM")
        return {"ok": True, "instances": int(instances), "refined": refined, "elapsed": round(time.time() - t0, 3)}

    def run(self):
        out = sys.stdout

        for line in sys.stdin:
            line = line.strip()

            if not line:
                continue

            rid = None

            try:
                req = json.loads(line)
                rid = req.get("id")
                cmd = req.get("cmd")

                if cmd == "info":
                    resp = self.info()
                elif cmd == "segment":
                    resp = self.segment(req)
                elif cmd == "quit":
                    break
                else:
                    raise ValueError("unknown command '%s'" % cmd)
            except Exception as e:
                log(traceback.format_exc())
                resp = {"ok": False, "error": "%s: %s" % (type(e).__name__, e)}

            if rid is not None:
                resp["id"] = rid

            out.write(json.dumps(resp) + "\n")
            out.flush()


def main():
    parser = argparse.ArgumentParser(description="RawTherapee segmentation backend")
    parser.add_argument("--device", default="auto", help="torch device: auto, cuda, cpu, mps, ...")
    args = parser.parse_args()
    Server(args.device).run()


if __name__ == "__main__":
    main()
