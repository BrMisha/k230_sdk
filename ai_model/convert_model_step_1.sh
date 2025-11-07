#!/bin/bash
#cd "$(dirname "$0")"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python -m venv .venv_cpu
source .venv_cpu/bin/activate

# Install required packages
pip install roboflow sahi
python "$SCRIPT_DIR/init_dataset.py"