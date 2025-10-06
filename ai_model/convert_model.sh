#!/bin/bash

MODELS=/home/misha/projects/ai_driver_assistant/tmp
DATASET=/home/misha/projects/ai_driver_assistant/tmp/images
MODEL_FILE_NAME=best.pt

docker build -t nncase-image .
docker run  -v ${MODELS}:/app/models -v ${DATASET}:/app/dataset -e MODEL_FILE_NAME=${MODEL_FILE_NAME} --rm -it nncase-image

