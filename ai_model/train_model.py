from ultralytics import YOLO
import torch
import time
import functools
import requests

TOKEN = "8212098701:AAHKTZpdO8FRoxQF9bJBHte_EupMDWWD3Ls"
CHAT_ID = "390672240"

def notify(text):
    requests.get(f"https://api.telegram.org/bot{TOKEN}/sendMessage", params={
        "chat_id": CHAT_ID,
        "text": text
    })

yolo_dataset="yolo_dataset"

# Auto-detect available GPUs
gpu_count = torch.cuda.device_count()
device_list = list(range(gpu_count)) if gpu_count > 0 else 'cpu'
workers = len(device_list) * 4
print(f"Detected {gpu_count} GPU(s): {device_list}")
print(f"Using {workers} workers for data loading")

try:
    model = YOLO('yolo11n.pt')  # Note: no 'v' in yolo11
    results = model.train(
        data=f"{yolo_dataset}/data.yml",
        name='tl_detector_11n',
        imgsz=640,

        epochs=150,
        rect=False,
        multi_scale=False,
        #batch=len(device_list)*512*1.5,        # 512 per GPU - safe for 32GB VRAM
        batch=len(device_list)*512,        # 512 per GPU - safe for 32GB VRAM
        workers=workers,  # Auto-calculated: GPU_count * 3
        device=device_list,  # Auto-detect and use all available GPUs
        #cache='ram',
        cache=True, # Try it
        patience=40,    # finish when 40 epoches without improvement

        augment=True,  # включаем ручной контроль над аугментацией
        hsv_h=0.0,
        hsv_s=0.0,
        hsv_v=0.0,
        fliplr=0.0,
        flipud=0.0,
        mosaic=0.0,
        mixup=0.0,
        cutmix=0.0,
        copy_paste=0.0,
        auto_augment='none',
        erasing=0.0
    )

    # Export trained model to ONNX
    print("\n" + "="*70)
    print("Exporting model to ONNX...")
    print("="*70)
    model.export(format='onnx', imgsz=640, simplify=True, opset=11)

    save_dir = model.trainer.save_dir
    print(f"\nTraining completed! Results saved to: {save_dir}")
    notify(f'✅ Training completed successfully!\nResults: {save_dir}')

except Exception as e:
    error_msg = f'❌ Training failed with error:\n{type(e).__name__}: {str(e)}'
    print(error_msg)
    notify(error_msg)
    raise  # Re-raise to see full traceback