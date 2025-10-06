# Train model
Create Jupyter notebook and run all cells.

Follow instruction (check training result, get model file and calibration images).
Finally you need to have locally:
1. *.pt file with PyTorch model
2. Directory with images to calibrate

# Convert model to kmodel
Open the start_docker.sh and edit 3 variables
Do
`./convert_model.sh`

Now you can use *.kmodel from $MODELS