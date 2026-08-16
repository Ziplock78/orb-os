#!/usr/bin/env bash
# Cut a video clip into the baseline-JPEG frame sequence one Spy Cam feed plays back
# (src/spycam_view.cpp). Each feed ("cam") gets its own numbered prefix so multiple
# clips coexist in one folder: cam0_000.jpg.., cam1_000.jpg.., etc.
# Frame count/fps/size here MUST match the CAMS[] table in spycam_view.cpp — if you
# change one, change both.
#
#   tools/spycam_export.sh <input.mp4> <cam_index> [fps] [quality] [zoom] [grain]
#
#   zoom  0..0.4  — crop this fraction off before scaling back up (0.16 = 16% zoom in,
#                   biased to trim mostly off the TOP of frame — handy for cropping out
#                   a distracting light fixture/ceiling detail). Default 0 (no zoom).
#   grain 0..40   — added film-grain noise strength after scaling. Default 0 (none).
#
# Output goes to sdcard_stage/spycam_frames/ (frames now live on the microSD card, not
# internal flash — copy that folder onto the card as /spycam_frames/ via a card reader).
set -euo pipefail

IN="${1:?usage: spycam_export.sh <input.mp4> <cam_index> [fps] [quality] [zoom] [grain]}"
CAM="${2:?usage: spycam_export.sh <input.mp4> <cam_index> [fps] [quality] [zoom] [grain]}"
FPS="${3:-6}"
Q="${4:-6}"          # ffmpeg -q:v (2=best/largest .. 31=worst/smallest); 6 is a good JPEG-quality start
ZOOM="${5:-0}"       # e.g. 0.16
GRAIN="${6:-0}"      # e.g. 18
SIZE=466

OUT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/sdcard_stage/spycam_frames"
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR/cam${CAM}_"*.jpg

# Crop: start from the full square, then optionally trim ZOOM fraction off each side
# (90% of the trimmed margin comes off the top, so it doubles as "crop out the ceiling").
CROP="crop='min(iw,ih)*(1-${ZOOM})':'min(iw,ih)*(1-${ZOOM})':'(iw-min(iw,ih)*(1-${ZOOM}))/2':'(ih-min(iw,ih)*(1-${ZOOM}))*0.9'"
VF="fps=${FPS},${CROP},scale=${SIZE}:${SIZE}"
if [ "$GRAIN" != "0" ]; then
    VF="${VF},noise=alls=${GRAIN}:allf=t+u"   # temporal+uniform luma/chroma noise = film grain
fi

# -pix_fmt yuvj420p + baseline JPEG (ffmpeg's mjpeg encoder is baseline by default —
# TJpgDec on the device cannot decode progressive JPEGs).
ffmpeg -y -i "$IN" \
  -vf "$VF" \
  -q:v "$Q" -pix_fmt yuvj420p \
  "$OUT_DIR/cam${CAM}_%03d.jpg"

# ffmpeg's %03d starts at 1; the firmware expects 0-based names (cam0_000.jpg ...).
i=0
for f in "$OUT_DIR/cam${CAM}_"*.jpg; do
    mv "$f" "$OUT_DIR/tmp${CAM}_$(printf '%03d' "$i").jpg"
    i=$((i + 1))
done
for f in "$OUT_DIR/tmp${CAM}_"*.jpg; do
    mv "$f" "${f/tmp${CAM}_/cam${CAM}_}"
done

COUNT=$(ls "$OUT_DIR/cam${CAM}_"*.jpg | wc -l | tr -d ' ')
SIZE_TOTAL=$(du -sh "$OUT_DIR" | cut -f1)
echo "wrote $COUNT frames to $OUT_DIR/cam${CAM}_*.jpg (folder total: $SIZE_TOTAL)"
echo "update the CAMS[] entry for cam $CAM in src/spycam_view.cpp: frameCount = $COUNT"
echo "then copy $OUT_DIR onto the microSD card as /spycam_frames/ (card reader, not the USB cable)"
