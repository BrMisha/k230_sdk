# K230 SDK Build Manual

## Build Instructions

### 1. Prepare Source Code
```bash
cd k230_sdk/
source tools/get_download_url.sh && make prepare_sourcecode
```

### 2. Build Docker Image
```bash
docker build -f tools/docker/Dockerfile -t k230_docker tools/docker
```

### 3. Run Docker Container
```bash
docker run --rm -u root -it -v $(pwd):$(pwd) -v $(pwd)/toolchain:/opt/toolchain -w $(pwd) k230_docker /bin/bash
```

### 4. Build SDK (Inside Docker)
```bash
CMAKE_POLICY_VERSION_MINIMUM=3.5 make CONF=k230_canmv_01studio_defconfig
```