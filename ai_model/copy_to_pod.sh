#!/bin/bash
cd "$(dirname "$0")"

# Check if SSH destination argument is provided
if [ -z "$1" ]; then
    echo "Error: SSH destination required"
    echo "Usage: ./copy_to_pod.sh USER@HOST [-P PORT] [-i IDENTITY_FILE]"
    echo "Example: ./copy_to_pod.sh root@213.192.2.99 -P 40189 -i ~/.ssh/id_ed25519"
    exit 1
fi

SSH_DEST="$1"
shift  # Remove first argument

echo "Copying to ${SSH_DEST}:/workspace/yolo_train/"
scp "$@" convert_model_step_1.sh init_dataset.py train_model_step_2.sh train_model.py "${SSH_DEST}:/workspace/yolo_train/"
