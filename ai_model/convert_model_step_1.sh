#!/bin/bash
cd "$(dirname "$0")"

python -m venv .venv_cpu
source .venv_cpu/bin/activate

# Install required packages
pip install roboflow sahi

python init_dataset.py