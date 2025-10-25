#!/bin/bash
cd "$(dirname "$0")"

python -m venv .venv_gpu
source .venv_gpu/bin/activate

pip install ultralytics

rm -r runs/

python train_model.py