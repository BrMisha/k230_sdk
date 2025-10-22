#!/usr/bin/env python3
"""
Detection Draw - Draw bounding boxes on video frames based on detection logs

Usage:
    detection_draw.py <mp4_file> <pts>

Arguments:
    mp4_file: Path to MP4 video file (e.g., /tmp/1/3.mp4)
    pts: Presentation timestamp in milliseconds (uint64)

Example:
    detection_draw.py /tmp/1/3.mp4 658745
"""

import sys
import os
import argparse
import cv2
import subprocess
import tempfile
from pathlib import Path


def validate_files(mp4_path):
    """Validate MP4 and TXT files exist"""
    mp4_file = Path(mp4_path)

    # Check MP4 exists
    if not mp4_file.exists():
        print(f"Error: MP4 file not found: {mp4_path}", file=sys.stderr)
        sys.exit(1)

    # Derive TXT file path
    txt_file = mp4_file.with_suffix('.txt')

    # Check TXT exists
    if not txt_file.exists():
        print(f"Error: Detection log file not found: {txt_file}", file=sys.stderr)
        sys.exit(1)

    return mp4_file, txt_file


def extract_frame_at_pts(mp4_file, pts):
    """Extract frame from MP4 at specific PTS using ffmpeg"""
    with tempfile.NamedTemporaryFile(suffix='.png', delete=False) as tmp:
        tmp_path = tmp.name

    try:
        # Use ffmpeg to extract frame at specific PTS
        cmd = [
            'ffmpeg',
            '-i', str(mp4_file),
            '-vf', f'select=eq(pts\\,{pts})',
            '-frames:v', '1',
            '-y',
            tmp_path
        ]

        result = subprocess.run(cmd, capture_output=True, text=True)

        if result.returncode != 0:
            print(f"Error: Failed to extract frame at PTS {pts}", file=sys.stderr)
            print(f"ffmpeg error: {result.stderr}", file=sys.stderr)
            os.unlink(tmp_path)
            sys.exit(1)

        # Read the extracted frame
        frame = cv2.imread(tmp_path)
        os.unlink(tmp_path)

        if frame is None:
            print(f"Error: Failed to read extracted frame", file=sys.stderr)
            sys.exit(1)

        return frame

    except Exception as e:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        print(f"Error extracting frame: {e}", file=sys.stderr)
        sys.exit(1)


def parse_detections(txt_file, target_pts_ms):
    """
    Parse detection log file and find detections at target PTS

    Format: <pts_ms>;<class> <conf> <x> <y> <w> <h>
    """

    detections = []
    found_line = None

    with open(txt_file, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            # Parse timestamp
            parts = line.split(';')
            if len(parts) < 2:
                continue

            try:
                pts_ms = int(parts[0])

                # Only exact match, no closest approximation
                if pts_ms == target_pts_ms:
                    found_line = line
                    break
            except ValueError:
                continue

    if found_line is None:
        print(f"Warning: No detections found at exact PTS {target_pts_ms}ms", file=sys.stderr)
        return []

    # Parse detections from found line
    # Format: <pts_ms>;<detection1>;<detection2>;...
    # Where each detection is: <class> <conf> <x> <y> <w> <h>
    parts = found_line.split(';')
    pts_ms = int(parts[0])

    print(f"Found detections at PTS {pts_ms}ms")
    print(f"Detection line: {found_line}")

    if len(parts) < 2:
        print(f"Warning: No detection data in line", file=sys.stderr)
        return []

    # Iterate through all detection parts (skip first which is timestamp)
    for i in range(1, len(parts)):
        det_str = parts[i].strip()
        if not det_str:
            continue

        try:
            # Format: <class> <conf> <x> <y> <w> <h>
            tokens = det_str.split()
            if len(tokens) >= 6:
                detection = {
                    'class': tokens[0],
                    'confidence': float(tokens[1]),
                    'x': float(tokens[2]),
                    'y': float(tokens[3]),
                    'w': float(tokens[4]),
                    'h': float(tokens[5])
                }
                detections.append(detection)
            else:
                print(f"Warning: Insufficient tokens in detection: {det_str}", file=sys.stderr)
        except (ValueError, IndexError) as e:
            print(f"Warning: Failed to parse detection: {det_str} - {e}", file=sys.stderr)

    if len(detections) == 0:
        print(f"No detections at PTS {pts_ms}ms (detection result exists but empty)")

    return detections


def draw_detections(frame, detections):
    """Draw bounding boxes and labels on frame"""
    height, width = frame.shape[:2]

    for det in detections:
        # Convert normalized coordinates [-1, 1] to pixel coordinates
        # x, y are TOP-LEFT corner coordinates (not center!)
        # x=-1 is left edge, x=1 is right edge
        # y=-1 is top edge, y=1 is bottom edge
        x1 = int((det['x'] + 1) * width / 2)
        y1 = int((det['y'] + 1) * height / 2)
        box_width = int(det['w'] * width / 2)
        box_height = int(det['h'] * height / 2)

        # Calculate bottom-right corner
        x2 = x1 + box_width
        y2 = y1 + box_height

        # Draw bounding box
        color = (0, 255, 0)  # Green
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)

        # Draw label with confidence
        label = f"{det['class']} {det['confidence']:.2f}"
        label_size, baseline = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.5, 1)

        # Draw label background
        cv2.rectangle(frame,
                     (x1, y1 - label_size[1] - baseline),
                     (x1 + label_size[0], y1),
                     color, -1)

        # Draw label text
        cv2.putText(frame, label, (x1, y1 - baseline),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 0), 1)

    return frame


def main():
    parser = argparse.ArgumentParser(
        description='Draw bounding boxes on video frames based on detection logs',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument('mp4_file', help='Path to MP4 video file')
    parser.add_argument('pts', type=int, help='Presentation timestamp in milliseconds')

    args = parser.parse_args()

    # Validate files
    mp4_file, txt_file = validate_files(args.mp4_file)

    print(f"MP4 file: {mp4_file}")
    print(f"TXT file: {txt_file}")
    print(f"PTS: {args.pts} ms")

    # Extract frame at PTS
    print(f"\nExtracting frame at PTS {args.pts}...")
    frame = extract_frame_at_pts(mp4_file, args.pts)
    print(f"Frame extracted: {frame.shape[1]}x{frame.shape[0]}")

    # Parse detections
    print(f"\nParsing detections...")
    detections = parse_detections(txt_file, args.pts)
    print(f"Found {len(detections)} detections")

    # Print detections with both normalized and absolute coordinates
    height, width = frame.shape[:2]
    for i, det in enumerate(detections):
        # Calculate absolute pixel coordinates (top-left corner)
        x1_px = int((det['x'] + 1) * width / 2)
        y1_px = int((det['y'] + 1) * height / 2)
        w_px = int(det['w'] * width / 2)
        h_px = int(det['h'] * height / 2)

        print(f"  [{i}] {det['class']} conf={det['confidence']:.2f}")
        print(f"      Normalized: x={det['x']:.4f} y={det['y']:.4f} w={det['w']:.4f} h={det['h']:.4f}")
        print(f"      Absolute:   x1={x1_px}px y1={y1_px}px w={w_px}px h={h_px}px (top-left corner)")

    # Draw detections
    if detections:
        frame = draw_detections(frame, detections)
        print("\nDetections drawn on frame")

    # Display frame
    window_name = f"{mp4_file.name} - PTS {args.pts}"
    cv2.imshow(window_name, frame)
    print(f"\nDisplaying frame. Press any key to close...")
    cv2.waitKey(0)
    cv2.destroyAllWindows()


if __name__ == '__main__':
    main()