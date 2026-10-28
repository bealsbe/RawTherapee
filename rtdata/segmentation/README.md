# AI segmentation backend for the Masks tool

The Subject / Sky / Background / Object components of the Masks tool get
their masks from an external Python process, `rt_segment_server.py`, which
RawTherapee starts on demand. Keeping the models out of process means
RawTherapee itself needs no PyTorch or ONNX dependency, and models can be
added or swapped without touching C++ code.

## Models

| id             | Model                                            | Prompts                                                        | Weights (licence)                                                       |
|----------------|--------------------------------------------------|----------------------------------------------------------------|-------------------------------------------------------------------------|
| `birefnet`     | BiRefNet + SAM 2.1 + EoMT (default without SAM 3)| clicked point (Object); ADE20K class name as Concept           | `ZhengPeng7/BiRefNet_dynamic` (GPU) / `ZhengPeng7/BiRefNet_lite` (CPU) (MIT), `facebook/sam2.1-hiera-large` (Apache-2.0), `tue-mps/ade20k_semantic_eomt_large_512` (MIT) |
| `sam2`         | SAM 2.1 + EoMT (ADE20K) via Transformers         | clicked point (Object); ADE20K class name as Concept           | `facebook/sam2.1-hiera-large` (Apache-2.0), `tue-mps/ade20k_semantic_eomt_large_512` (MIT) |
| `semantic`     | EoMT (ADE20K) alone                              | ADE20K class name as Concept; Object = class under the click   | `tue-mps/ade20k_semantic_eomt_large_512` (MIT)                          |
| `sam3`         | SAM 3 via Hugging Face Transformers              | free text concept, box around the clicked point                | `facebook/sam3` (gated, request access; custom SAM licence)             |
| `sam3-official`| SAM 3 via the `sam3` package                     | same                                                           | `facebook/sam3` (gated)                                                 |
| `dummy-python` | heuristics, no ML (hidden; tests only)           | none                                                           | none                                                                    |

Edge refinement (any provider, any target): `hustvl/vitmatte-small-composition-1k`
(ViTMatte, Apache-2.0, 26 M parameters), see below.

How the targets are computed by `birefnet`, the default on machines without
SAM 3 access:

* **Subject**: BiRefNet, a dichotomous ("salient foreground") segmentation
  model: no class knowledge needed, soft edges, works for people, animals,
  products, anything that stands out. When a Concept is typed, the subject is
  the ADE20K classes of that concept instead (EoMT, refined by SAM 2.1).
* **Background**: 1 - Subject.
* **Object**: SAM 2.1 prompted with the clicked point (best scored candidate).
* **Sky**: EoMT's sky probability, gated by the SAM 2.1 regions grown from the
  confident sky pixels along the top edge. A semantic model alone can take a
  flat bright surface for sky, SAM alone takes any region at the top for sky;
  the product keeps what both agree on.

The `sam2` provider is the same without BiRefNet: its Subject is the Concept's
classes if given, otherwise confident people/animals, then vehicles, each
sharpened by SAM 2.1 (box + point per component), otherwise the most salient
SAM 2.1 region(s) from a grid of point prompts. Use it when BiRefNet picks the
wrong thing or cannot be installed.

All masks are soft probabilities (class maps upsampled bilinearly, SAM logits
through a sharpened sigmoid, BiRefNet / ViTMatte alphas), so the Threshold
slider and feathering work on them.

### Edge refinement (matting)

With *Refine edges* enabled on an AI component (`"refine": true` in the
protocol), the mask edges are re-estimated by ViTMatte: a trimap keeps the
confident foreground and background and leaves a band of about 2% of the image
diagonal around the edge (`RT_MATTING_BAND`) plus every uncertain pixel for the
model; the result is a soft alpha matte that follows hair, fur and foliage.
It costs about 0.1 s on a GPU (a few seconds on a CPU) per mask and is off by
default.

### BiRefNet and `trust_remote_code`

BiRefNet's weights are plain safetensors, but the model class is not part of
Transformers: the author's modeling code (`birefnet.py`, `BiRefNet_config.py`)
is downloaded from the Hugging Face repo and executed, which is what
`trust_remote_code=True` means. Only the `ZhengPeng7/BiRefNet*` repositories are
loaded this way; pin a revision with `RT_BIREFNET_REPO=ZhengPeng7/BiRefNet_dynamic`
plus the Hugging Face cache if you want to freeze the code. The code needs the
`einops`, `kornia` and `timm` packages; without them (or offline without
cached weights) the `birefnet` provider is listed greyed out with the reason
and `sam2` becomes the default.

### Semantic model

The class targets use EoMT (Encoder-only Mask Transformer, DINOv2 ViT-L
backbone, ADE20K semantic checkpoint, MIT licensed, 316 M parameters, 1.3 GB
download) by default. SegFormer-B2 ADE20K (27 M parameters, 110 MB, about 3x
faster and the better choice for CPU-only machines) can be selected with
`RT_SEMANTIC_MODEL=segformer`, but note that NVIDIA's pretrained SegFormer
weights are licensed for research and evaluation use only, so it is not the
default.

`semantic` is the lightweight option without SAM (one model); its Subject is
weaker (class probabilities only) and its Sky is not SAM-gated.

The backend's default is SAM 3 when its gated weights are accessible, otherwise
`birefnet`, otherwise `sam2`, otherwise `semantic`. Each AI component in the
Masks panel has a *Provider* dropdown to pick a model explicitly; models that
exist but cannot run (gated weights not granted, package missing) are listed
greyed out with the reason. The *Concept* text field takes free text with
SAM 3 and an ADE20K class name (person, animal, car, tree, water, building,
...; a few synonyms such as "people", "dog", "ocean" are understood) with the
other providers.

Environment variables: `RT_SAM3_REPO`, `RT_SAM2_REPO` (for example
`facebook/sam2.1-hiera-small` for a faster, smaller model), `RT_EOMT_REPO`,
`RT_SEGFORMER_REPO` (for example `nvidia/segformer-b0-finetuned-ade-512-512`,
4 M parameters, for slow CPUs), `RT_BIREFNET_REPO` (`auto` = dynamic on a GPU,
lite on the CPU; or any `ZhengPeng7/BiRefNet*` repo, e.g. `BiRefNet-matting`
for alpha mattes), `RT_VITMATTE_REPO`, `RT_MATTING_BAND` (fraction of the
diagonal, default 0.02), `RT_HF_ACCESS_TIMEOUT` (seconds, default 4) bounding
the access check for weights that are not cached yet.

## Requirements

* Python 3.10+ with `numpy`, `pillow`, `torch`, `transformers` (5.x, or any
  version that provides `Sam2Model` / `EomtForUniversalSegmentation` /
  `VitMatteForImageMatting` / `Sam3Model`) and `huggingface_hub`.
* For BiRefNet: `einops`, `kornia`, `timm` (`pip install einops kornia timm`).
* A GPU is strongly recommended (CUDA or ROCm builds of PyTorch both work;
  the device is chosen automatically). CPU works but is slow.
* For SAM 3: request access at https://huggingface.co/facebook/sam3 and
  log in once with `hf auth login`. **SAM 3 is currently switched off**: the
  `sam3` / `sam3-official` entries are listed greyed out with a static reason,
  are never chosen as default, and the backend makes no request to Hugging
  Face for `facebook/sam3` at all. The single switch is `SAM3_ENABLED` in
  `rt_segment_server.py`, set from the environment variable `RT_ENABLE_SAM3=1`;
  with it on, availability is checked against the gated repo again
  (`huggingface_hub.auth_check`, short timeout, offline safe).
* Optional: `scipy` (faster connected components).

Weights are downloaded on first use into the Hugging Face cache. The "info"
request checks access to uncached weights with a short network timeout and is
offline safe: cached weights count as available, uncached ones are reported
unavailable with the reason.

## Configuration

Keys in RawTherapee's `options` file, group `[Segmentation]`:

```
[Segmentation]
Enabled=true          ; start the backend at all
Command=python3       ; interpreter (use the one that has torch/transformers)
Script=               ; empty = this bundled rt_segment_server.py
Device=auto           ; torch device: auto, cuda, cpu, mps
```

## Protocol

One JSON object per line on stdin/stdout. Images are passed as binary PPM
files, masks come back as binary PGM files (0..255) of the same size.

```
{"cmd": "info"}
{"cmd": "segment", "id": 1, "model": "birefnet", "image": "/tmp/in.ppm", "mask": "/tmp/out.pgm",
 "target": "subject", "prompt": "", "point": [0.5, 0.5], "threshold": 0.5, "refine": false}
{"cmd": "quit"}
```

The "info" reply lists every model with `available`, `reason` (why it cannot
run, or a note) and `hidden` (usable by id, not shown in the GUI), plus a
`refine` object telling whether edge refinement (ViTMatte) is available. A
segment reply carries `refined: true` when the matting ran.

Test it by hand:

```
printf '{"cmd":"info"}\n' | python3 rt_segment_server.py
```

## Adding a model

Subclass `ModelAdapter` in `rt_segment_server.py`, implement `available()`
and `segment()` (return a float HxW array in 0..1), and add the class to
`MODEL_CLASSES`. Import heavy dependencies lazily inside `load()` so that
startup stays fast. The new model appears automatically in the Provider
dropdown.
