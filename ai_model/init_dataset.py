from roboflow import Roboflow
from sahi.slicing import slice_coco
from sahi.utils.file import save_json
from sahi.utils.coco import Coco
import os
import shutil
from sahi.utils.coco import Coco, export_coco_as_yolo


rf = Roboflow(api_key="mz3cNkxiO8av9JAjZbS3")
project = rf.workspace("my-ws-lwkgs").project("tl_detector-coivv")
version = project.version(35)
dataset = version.download("coco")



#out_dir = f"{dataset.location}/sliced"
sliced_dir = "sliced"
try:
    os.rmdir(sliced_dir)
except:
    pass

coco_dict, coco_path = slice_coco(
    coco_annotation_file_path=f"{dataset.location}/train/_annotations.coco.json",
    image_dir=f"{dataset.location}/train",
    output_coco_annotation_file_name="annotations",
    output_dir=f"{sliced_dir}",
    slice_height=512,
    slice_width=512,
    overlap_height_ratio=0.3,
    overlap_width_ratio=0.3,
    min_area_ratio=0.7,
    ignore_negative_samples=False,
)

# Remove the original dataset directory after slicing
'''if os.path.exists(dataset.location):
    shutil.rmtree(dataset.location)
    print(f"Removed original dataset: {dataset.location}")
'''
# create a dictionary for yolo dataset
coco = Coco.from_coco_dict_or_path(coco_dict, image_dir=f"{sliced_dir}")
result = coco.split_coco_as_train_val(train_split_rate=0.85)

# Remove old yolo dataset if exist
yolo_dataset=f"yolo_dataset"
try:
    os.rmdir(yolo_dataset)
except:
    pass
#
data_yml_path = export_coco_as_yolo(
    output_dir=yolo_dataset,
    train_coco=result["train_coco"],
    val_coco=result["val_coco"]
)

# NOTE: Do NOT delete sliced_dir - YOLO dataset contains symlinks to these images!
print(f"Dataset ready at: {yolo_dataset}")
print(f"Sliced images kept at: {sliced_dir}")