#!/bin/bash

DATASET=/home/misha/projects/ai_driver_assistant/tmp/yolo_dataset/train
MODELS=/home/misha/projects/ai_driver_assistant/tmp/runs/detect/tl_detector3/weights
MODEL_FILE_NAME=best.onnx

docker build -t k230-converter .

docker run --rm \
    -v ${MODELS}:/models \
    -v ${DATASET}:/calib_images \
    k230-converter \
    python convert.py \
        --model /models/${MODEL_FILE_NAME} \
        --imgsz 320 \
        --calib-samples -1 \
        --calib-dir /calib_images \
        --ptq-option 1

# Parameters:
# --calib-samples: Number of calibration images to use for quantization
#   -1 = use all available images (default)
#   N  = use exactly N images (error if not enough images available)
#
# --ptq-option: Post-Training Quantization preset
#   0: uint8/uint8
#   1: int16/uint8 (recommended, default)
#   2: uint8/int16
#   3: uint8/uint8 + Kld calibration
#   4: int16/uint8 + Kld calibration
#   5: uint8/int16 + Kld calibration