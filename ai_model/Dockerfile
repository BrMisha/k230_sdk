FROM python:3.10.14-slim-bookworm

WORKDIR /app

RUN apt-get update && apt-get install -y \
    build-essential \
    wget \
    curl \
    unzip \
    libgl1-mesa-glx \
    libsm6 \
    libxext6 \
    libxrender-dev \
    libglib2.0-0 \
    libglib2.0-dev \
    nano \
    mc \
    && rm -rf /var/lib/apt/lists/*

# Install .NET 7.0 SDK for nncase (as per official Kendryte instructions)
RUN curl -sSL https://packages.microsoft.com/keys/microsoft.asc | apt-key add - \
    && echo "deb [arch=amd64] https://packages.microsoft.com/repos/microsoft-debian-bookworm-prod bookworm main" > /etc/apt/sources.list.d/microsoft.list \
    && apt-get update \
    && apt-get install -y dotnet-sdk-7.0 \
    && rm -rf /var/lib/apt/lists/*

# Install packages following official Kendryte instructions
RUN pip install --upgrade pip

RUN pip install --no-cache-dir \
    nncase==2.10.0 \
    nncase-kpu==2.10.0 \
    onnx \
    onnxruntime \
    onnxsim \
    ultralytics \
    numpy \
    pillow

# Download and extract official Kendryte YOLO example
RUN wget https://kendryte-download.canaan-creative.com/developer/k230/yolo_files/test_yolov8.zip
RUN unzip test_yolov8.zip

# Copy model
RUN mkdir models
RUN mkdir dataset
#COPY ../models/ models/

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]


