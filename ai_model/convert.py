#!/usr/bin/env python3
"""
Convert PyTorch (.pt) model to KModel (.kmodel) for K230

Usage:
    python convert.py --model model.pt --imgsz 320 --calib-dir ./images
"""

import argparse
import os
import sys
import numpy as np
import glob
import cv2


def export_to_onnx(model_path, imgsz):
    """Export PyTorch model to ONNX format"""
    print(f"[1/3] Exporting {model_path} to ONNX...")

    from ultralytics import YOLO
    import shutil

    model = YOLO(model_path)

    # Export to same directory as model first (Ultralytics requirement)
    temp_onnx_path = model_path.replace('.pt', '.onnx')

    model.export(
        format='onnx',
        imgsz=imgsz,
        simplify=True,
        opset=11
    )

    # Move to /tmp to avoid cluttering workspace
    onnx_filename = os.path.basename(temp_onnx_path)
    onnx_path = os.path.join('/tmp', onnx_filename)
    shutil.move(temp_onnx_path, onnx_path)

    print(f"  ✓ ONNX exported: {onnx_path}")
    return onnx_path


def simplify_onnx(onnx_path):
    """Simplify ONNX model (optional but recommended)"""
    print(f"[2/3] Simplifying ONNX model...")

    import onnx
    from onnxsim import simplify

    # Load model
    model = onnx.load(onnx_path)

    # Simplify
    model_simplified, check = simplify(model)

    if check:
        onnx.save(model_simplified, onnx_path)
        print(f"  ✓ ONNX simplified")
    else:
        print(f"  ⚠ Simplification failed, using original ONNX")

    return onnx_path


def load_calibration_images(calib_dir, imgsz, calib_samples):
    """Load real images for calibration"""

    # Validate calibration directory
    if not calib_dir:
        print(f"Error: --calib-dir is required!")
        sys.exit(1)

    if not os.path.exists(calib_dir):
        print(f"Error: Calibration directory '{calib_dir}' does not exist!")
        sys.exit(1)

    # Find image files
    image_extensions = ['*.jpg', '*.jpeg', '*.png', '*.bmp']
    image_files = []
    for ext in image_extensions:
        image_files.extend(glob.glob(os.path.join(calib_dir, ext)))
        image_files.extend(glob.glob(os.path.join(calib_dir, ext.upper())))

    if len(image_files) == 0:
        print(f"Error: No images found in '{calib_dir}'!")
        sys.exit(1)

    # If calib_samples is -1, use all available images
    if calib_samples == -1:
        calib_samples = len(image_files)
        print(f"  - Using all {calib_samples} images from '{calib_dir}'")
    else:
        # Validate requested sample count
        if calib_samples > len(image_files):
            print(f"Error: Requested {calib_samples} calibration samples, but only {len(image_files)} images found in '{calib_dir}'!")
            sys.exit(1)
        print(f"  - Found {len(image_files)} images in '{calib_dir}'")
        print(f"  - Loading {calib_samples} images for calibration...")

    calib_data = []
    for i, img_path in enumerate(image_files[:calib_samples]):
        try:
            # Read image
            img = cv2.imread(img_path)
            if img is None:
                continue

            # Convert BGR to RGB
            img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

            # Resize to target size
            img = cv2.resize(img, (imgsz, imgsz))

            # Keep as uint8 [0-255], convert to CHW format
            img = img.astype(np.uint8)
            img = np.transpose(img, (2, 0, 1))  # HWC -> CHW

            calib_data.append(img)

            if (i + 1) % 10 == 0:
                print(f"    Loaded {i + 1}/{min(calib_samples, len(image_files))} images")

        except Exception as e:
            print(f"    Warning: Failed to load {img_path}: {e}")
            continue

    if len(calib_data) == 0:
        print(f"  - Failed to load any images, using random data")
        return np.random.randint(0, 256, (calib_samples, 3, imgsz, imgsz), dtype=np.uint8)

    # Pad with random data if needed
    while len(calib_data) < calib_samples:
        calib_data.append(np.random.randint(0, 256, (3, imgsz, imgsz), dtype=np.uint8))

    print(f"  ✓ Loaded {len(calib_data)} calibration images")
    return np.array(calib_data, dtype=np.uint8)


def convert_to_kmodel(onnx_path, imgsz, calib_samples, calib_dir=None, w_quant_type="int16", quant_type="uint8", calibrate_method="NoClip", model_path=None):
    """Convert ONNX to KModel format"""
    print(f"[3/3] Converting ONNX to KModel...")
    print(f"  - Weight quantization: {w_quant_type}")
    print(f"  - Activation quantization: {quant_type}")
    print(f"  - Calibration method: {calibrate_method}")

    import nncase

    def read_model_file(path):
        with open(path, 'rb') as f:
            return f.read()

    # Compile options
    compile_options = nncase.CompileOptions()
    compile_options.target = "k230"
    compile_options.dump_dir = "/tmp"
    compile_options.dump_ir = False
    compile_options.dump_asm = False

    # Preprocessing options (bake preprocessing into kmodel)
    compile_options.preprocess = True
    compile_options.input_type = 'uint8'
    compile_options.input_range = [0, 1]
    compile_options.input_shape = [1, 3, imgsz, imgsz]
    compile_options.input_layout = "NCHW"
    compile_options.mean = [0, 0, 0]
    compile_options.std = [1, 1, 1]
    compile_options.swapRB = False

    # PTQ (Post-Training Quantization) options
    ptq_options = nncase.PTQTensorOptions()
    ptq_options.quant_type = quant_type           # Activation quantization
    ptq_options.w_quant_type = w_quant_type       # Weight quantization
    ptq_options.calibrate_method = calibrate_method
    ptq_options.finetune_weights_method = "NoFineTuneWeights"
    ptq_options.dump_quant_error = False
    ptq_options.dump_quant_error_symmetric_for_signed = False

    # Load calibration data
    if calib_samples == -1:
        print(f"  - Preparing all available calibration samples...")
    else:
        print(f"  - Preparing {calib_samples} calibration samples...")
    calib_data = load_calibration_images(calib_dir, imgsz, calib_samples)
    # Add batch dimension to each sample: (N, 3, H, W) -> (N, 1, 3, H, W)
    calib_data = calib_data[:, np.newaxis, :, :, :]  # Insert axis at position 1
    calib_data = [calib_data]  # Wrap in list for nncase
    # Use actual number of loaded images
    actual_samples = calib_data[0].shape[0]
    ptq_options.samples_count = actual_samples
    ptq_options.set_tensor_data(calib_data)

    # Compile
    print(f"  - Compiling (this may take a few minutes)...")
    compiler = nncase.Compiler(compile_options)
    model_content = read_model_file(onnx_path)
    import_options = nncase.ImportOptions()

    compiler.import_onnx(model_content, import_options)
    compiler.use_ptq(ptq_options)
    compiler.compile()

    # Save kmodel
    kmodel = compiler.gencode_tobytes()

    # Save kmodel in same directory as original .pt model
    if model_path:
        kmodel_path = model_path.replace('.pt', '.kmodel')
    else:
        # Fallback: use same directory as ONNX
        kmodel_path = onnx_path.replace('.onnx', '.kmodel')

    with open(kmodel_path, 'wb') as f:
        f.write(kmodel)

    file_size = os.path.getsize(kmodel_path) / 1024 / 1024
    print(f"  ✓ KModel saved: {kmodel_path}")
    print(f"  ✓ File size: {file_size:.2f} MB")

    return kmodel_path


def main():
    parser = argparse.ArgumentParser(
        description='Convert PyTorch model to KModel for K230',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python convert.py --model best.pt --imgsz 320
  python convert.py --model yolo.pt --imgsz 640 --calib-samples 20
        """
    )

    parser.add_argument('--model', required=True, help='Path to PyTorch model (.pt)')
    parser.add_argument('--imgsz', type=int, default=320, help='Input image size (default: 320)')
    parser.add_argument('--calib-samples', type=int, default=-1,
                        help='Number of calibration samples (default: -1, use all images)')
    parser.add_argument('--calib-dir', type=str, required=True,
                        help='Directory with calibration images (required)')
    parser.add_argument('--ptq-option', type=int, default=1,
                        choices=[0, 1, 2, 3, 4, 5],
                        help='PTQ option preset (default: 1). ' +
                             '0: uint8/uint8, ' +
                             '1: int16/uint8 (recommended), ' +
                             '2: uint8/int16, ' +
                             '3: uint8/uint8+Kld, ' +
                             '4: int16/uint8+Kld, ' +
                             '5: uint8/int16+Kld')
    parser.add_argument('--w-quant-type', type=str, default=None,
                        choices=['uint8', 'int8', 'int16'],
                        help='Weight quantization type (overrides --ptq-option)')
    parser.add_argument('--quant-type', type=str, default=None,
                        choices=['uint8', 'int8', 'int16'],
                        help='Activation quantization type (overrides --ptq-option)')
    parser.add_argument('--skip-simplify', action='store_true',
                        help='Skip ONNX simplification step')

    args = parser.parse_args()

    # Apply PTQ option presets if custom types not specified
    # Format: (w_quant, quant, calibrate_method)
    ptq_presets = {
        0: ('uint8', 'uint8', 'NoClip'),
        1: ('int16', 'uint8', 'NoClip'),   # Default - matches original script
        2: ('uint8', 'int16', 'NoClip'),
        3: ('uint8', 'uint8', 'Kld'),
        4: ('int16', 'uint8', 'Kld'),
        5: ('uint8', 'int16', 'Kld'),
    }

    preset_w_quant, preset_quant, preset_calibrate = ptq_presets[args.ptq_option]
    w_quant_type = args.w_quant_type if args.w_quant_type else preset_w_quant
    quant_type = args.quant_type if args.quant_type else preset_quant
    calibrate_method = preset_calibrate

    # Validate inputs
    if not os.path.exists(args.model):
        print(f"Error: Model file '{args.model}' not found!")
        sys.exit(1)

    if not args.model.endswith('.pt'):
        print(f"Error: Model file must be a .pt file!")
        sys.exit(1)

    # Print header
    print("=" * 70)
    print("K230 Model Conversion Tool")
    print("=" * 70)
    print(f"Input:  {args.model}")
    print(f"Size:   {args.imgsz}x{args.imgsz}")
    print(f"Calib:  {args.calib_samples} samples")
    print("=" * 70)

    try:
        # Step 1: Export to ONNX
        onnx_path = export_to_onnx(args.model, args.imgsz)

        # Step 2: Simplify ONNX (optional)
        if not args.skip_simplify:
            onnx_path = simplify_onnx(onnx_path)

        # Step 3: Convert to KModel
        kmodel_path = convert_to_kmodel(onnx_path, args.imgsz, args.calib_samples, args.calib_dir, w_quant_type, quant_type, calibrate_method, args.model)

        # Success
        print("=" * 70)
        print("✓ CONVERSION COMPLETE!")
        print("=" * 70)
        print(f"Output: {kmodel_path}")
        print("\nYou can now deploy this model to your K230 device!")

    except Exception as e:
        print("\n" + "=" * 70)
        print("✗ CONVERSION FAILED")
        print("=" * 70)
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()