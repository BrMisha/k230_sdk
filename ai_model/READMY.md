# Train model

## Using Jupyter notebook to train model then local CPU to convert to kmodel
### 1. Create Jupyter notebook and run all cells.

Follow instruction (check training result, get model file and calibration images).
Finally you need to have locally:
1. *.pt file with PyTorch model
2. Directory with images to calibrate

### 2. Convert model to kmodel

1. Edit `convert_model.sh` and set 3 variables:
   - `MODELS` - directory with your .pt file
   - `DATASET` - directory with calibration images
   - `MODEL_FILE_NAME` - name of your .pt file

2. Run conversion:
   ```bash
   ./convert_model.sh
   ```

Your `.kmodel` file will be created in `$MODELS` directory

## Using Runpod CPU+GPU+Volume
This method is faster but has more difficult setup.

All action will do here: https://console.runpod.io/
### 1. Create Network Storage
About 10GB per 1000 images of dataset + 20GB for temp files
### 2. Prepare dataset with CPU Runpod
On this step you need to select your `Network volume` and select `ubuntu-2404` as a template.
More CPU performance == faster processing.
When pod is created, you will see the section `SSH over exposed TCP`.
#### 2.1 Copy files to pod
Do the next command (replace HOST and PORT with yours):
./copy_to_pod.sh root@213.192.2.99 -P 40189
#### 2.2 Convert model
Open pod terminal (web or ssh) then:
```bash
cd /workspace/yolo_train/
./convert_model_step_1.sh
```
Finally, you will get a directory with yolo dataset for training.
`/workspace/yolo_train/yolo_dataset`
Now you can to remove the CPU pod to save your money.
### 3. Train model with GPU Runpod
This step similar to CPU, but you need to select `GPU`. Script is optimized for RTX 5090 x2.
#### 3.1. Train YOLO model
Open terminal and
```bash
cd /workspace/yolo_train/
./train_model_step_2.sh
```
Finally you will get a directory with yolo dataset (`/workspace/yolo_train/yolo_dataset/`) 
and model (`/workspace/yolo_train/runs/detect/tl_detector/`).
Download these 2 directories to your computer to convert mode.
#### 3.2. Convert model
Open `ai_model/convert_model.sh`, edit the `DATASET` and the `MODELS` then run this script
