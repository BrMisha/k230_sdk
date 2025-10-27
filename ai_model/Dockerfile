# Dockerfile for K230 Model Conversion
# Converts PyTorch (.pt) models to KModel (.kmodel) format

FROM ghcr.io/kendryte/k230_sdk

# Switch to root user for installation
USER root

# Install system dependencies for OpenCV and SSH
RUN apt-get update && \
    apt-get install -y \
    libgl1-mesa-glx \
    libglib2.0-0 \
    libsm6 \
    libxext6 \
    libxrender-dev \
    openssh-server \
    && rm -rf /var/lib/apt/lists/*

# Install Python packages for model conversion
RUN pip install --upgrade pip && \
    pip install -v \
    ultralytics \
    nncase \
    nncase-kpu \
    "numpy<2.0" \
    onnx>=1.15.0 \
    onnxslim>=0.1.71 \
    onnxsim \
    opencv-python-headless \
    scikit-learn

# Create workspace and copy conversion script
WORKDIR /app/workspace
COPY convert.py /app/workspace/convert.py

# Create mount point directories
RUN mkdir -p /models /calib_images

# Default command
CMD ["/bin/bash"]