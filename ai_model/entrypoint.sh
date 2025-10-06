#!/bin/bash

yolo export model=models/${MODEL_FILE_NAME} format=onnx imgsz=320

ONNX_PATH=$PWD"/models/${MODEL_FILE_NAME%.pt}.onnx"
echo "Exported ONNX model: $ONNX_PATH"

cd test_yolov8/detect/
python to_kmodel.py --target k230 --model $ONNX_PATH --dataset=../../dataset/ --input_width 320 --input_height 320 --ptq_option 1

exit