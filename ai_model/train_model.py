from ultralytics import YOLO

yolo_dataset="yolo_dataset"

model = YOLO('yolov8n.pt')
results = model.train(
    data=f"{yolo_dataset}/data.yml",
    epochs=100,
    imgsz=320,
    rect=False,
    multi_scale=False,
    batch=512,        # Increased - you have plenty of VRAM (only using 11/32 GB)
    workers=8,        # Increased workers for dual GPU setup
    device=[0, 1],    # Use BOTH GPUs
    cache='ram',
    name='tl_detector',
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

#%%
#print(f"Result: {results.save_dir}/confusion_matrix_normalized.png")
#print(f"Model: {results.save_dir}/weights/best.pt")
#print(f"Calibration images: sliced/")


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

notify('Finished')