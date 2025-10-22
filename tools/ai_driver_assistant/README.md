# AI Driver Assistant Tools

Tools for working with AI Driver Assistant recordings and detections.

## detection_draw.py

Draw bounding boxes on video frames based on detection logs.

### Requirements

```bash
pip install opencv-python
```

Also requires `ffmpeg` to be installed and available in PATH.

### Usage

```bash
./detection_draw.py <mp4_file> <pts>
```

**Arguments:**
- `mp4_file`: Path to MP4 video file (e.g., `/tmp/1/3.mp4`)
- `pts`: Presentation timestamp in microseconds (uint64)

**Example:**
```bash
./detection_draw.py /tmp/1/3.mp4 658745
```

This will:
1. Validate that `/tmp/1/3.mp4` exists
2. Validate that `/tmp/1/3.txt` exists (detection log)
3. Extract the frame at PTS 658745 microseconds
4. Parse detections from the log file at that timestamp
5. Draw bounding boxes with labels on the frame
6. Display the annotated frame

**Detection Log Format:**

The `.txt` files contain detection data in the following format:
```
<pts_ms>;<class> <conf> <x> <y> <w> <h>;<class> <conf> <x> <y> <w> <h>;...
```

Where:
- `pts_ms`: Timestamp in milliseconds
- `class`: Object class name (e.g., "traffic_light", "car")
- `conf`: Confidence score (0.0 to 1.0)
- `x`, `y`: Normalized top-left corner coordinates (-1.0 to 1.0, where -1 is left/top edge, 1 is right/bottom edge)
- `w`, `h`: Normalized width and height (0.0 to 2.0)

**Notes:**
- The script requires an exact PTS match in the detection log (no approximate matching)
- Bounding boxes are drawn in green
- Labels show class name and confidence score
- Press any key to close the displayed image
- Coordinates use center-origin system: (0,0) is image center, (-1,-1) is top-left, (1,1) is bottom-right