#!/bin/bash
cd "$(dirname "$0")"

python -m venv .venv_gpu
source .venv_gpu/bin/activate

pip install ultralytics "onnx>=1.12.0" "onnxslim>=0.1.71" onnxruntime-gpu

#rm -r runs/

python train_model.py