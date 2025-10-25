# Train model
Create Jupyter notebook and run all cells.

Follow instruction (check training result, get model file and calibration images).
Finally you need to have locally:
1. *.pt file with PyTorch model
2. Directory with images to calibrate

# Convert model to kmodel

1. Edit `convert_model.sh` and set 3 variables:
   - `MODELS` - directory with your .pt file
   - `DATASET` - directory with calibration images
   - `MODEL_FILE_NAME` - name of your .pt file

2. Run conversion:
   ```bash
   ./convert_model.sh
   ```

Your `.kmodel` file will be created in `$MODELS` directory