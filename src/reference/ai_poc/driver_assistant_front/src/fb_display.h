#ifndef FB_DISPLAY_H
#define FB_DISPLAY_H

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include "../../driver_assistant_detector/common.h"

class FbDisplay {
public:
    static constexpr int WIDTH = 320;
    static constexpr int HEIGHT = 170;
    static constexpr int BPP = 16;  // RGB565

    FbDisplay() : fd_(-1), fb_(nullptr), fb_size_(0) {}

    ~FbDisplay() { close(); }

    bool open(const char* device = "/dev/fb1") {
        fd_ = ::open(device, O_RDWR);
        if (fd_ < 0) return false;

        fb_size_ = WIDTH * HEIGHT * (BPP / 8);
        fb_ = static_cast<uint16_t*>(mmap(nullptr, fb_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (fb_ == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        clear();
        return true;
    }

    void close() {
        if (fb_ && fb_ != MAP_FAILED) {
            munmap(fb_, fb_size_);
            fb_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void clear(uint16_t color = 0x0000) {
        if (!fb_) return;
        for (int i = 0; i < WIDTH * HEIGHT; i++)
            fb_[i] = color;
    }

    void drawTestText() {
        if (!fb_) return;
        clear(0x0000);  // Dark background

        // Draw "AI" in large white letters
        const uint16_t white = 0xFFFF;
        int cx = WIDTH / 2;
        int cy = HEIGHT / 2;

        // Letter 'A' (left side)
        int ax = cx - 50;
        for (int y = -40; y <= 40; y++) {
            int half = 3 + (y + 40) * 17 / 80;  // Wider at bottom
            // Left leg
            for (int t = -3; t <= 3; t++)
                setPixel(ax - half + t, cy + y, white);
            // Right leg
            for (int t = -3; t <= 3; t++)
                setPixel(ax + half + t, cy + y, white);
            // Horizontal bar in middle
            if (y >= 5 && y <= 12) {
                for (int x = -half; x <= half; x++)
                    setPixel(ax + x, cy + y, white);
            }
        }

        // Letter 'I' (right side)
        int ix = cx + 50;
        // Vertical bar
        fillRect(ix - 5, cy - 40, 10, 80, white);
        // Top horizontal
        fillRect(ix - 20, cy - 40, 40, 8, white);
        // Bottom horizontal
        fillRect(ix - 20, cy + 32, 40, 8, white);
    }

    void drawSituation(const driver_assistant_detector::DetectedSituation& sit) {
        if (!fb_) return;
        clear();

        // Right half: traffic light color circle
        uint16_t color = 0x0000;  // black for NONE
        switch (sit.color) {
            case driver_assistant_detector::GREEN:  color = 0x07E0; break;  // Green
            case driver_assistant_detector::RED:    color = 0xF800; break;  // Red
            case driver_assistant_detector::YELLOW: color = 0xFFE0; break;  // Yellow
            default: break;
        }
        if (sit.color != driver_assistant_detector::NONE) {
            fillRect(170, 10, 150, 150, color);  // 150x150 rectangle on right side
        }

        // Left half: arrows (green on dark)
        const uint16_t arrow_color = 0x07E0;
        const int arrow_y = HEIGHT / 2;

        if (sit.arrow_left)    drawArrowLeft(30, arrow_y, arrow_color);
        if (sit.arrow_forward) drawArrowUp(80, arrow_y, arrow_color);
        if (sit.arrow_right)   drawArrowRight(130, arrow_y, arrow_color);
    }

private:
    int fd_;
    uint16_t* fb_;
    size_t fb_size_;

    void setPixel(int x, int y, uint16_t color) {
        if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
            fb_[y * WIDTH + x] = color;
    }

    void fillRect(int x, int y, int w, int h, uint16_t color) {
        for (int j = y; j < y + h; j++)
            for (int i = x; i < x + w; i++)
                setPixel(i, j, color);
    }

    void fillCircle(int cx, int cy, int r, uint16_t color) {
        for (int y = -r; y <= r; y++)
            for (int x = -r; x <= r; x++)
                if (x*x + y*y <= r*r)
                    setPixel(cx + x, cy + y, color);
    }

    // Draw left arrow at center (cx, cy)
    void drawArrowLeft(int cx, int cy, uint16_t color) {
        // Triangle pointing left (tip at cx-20)
        for (int i = 0; i < 25; i++) {
            int half = i;
            for (int j = -half; j <= half; j++)
                setPixel(cx - 20 + i, cy + j, color);
        }
        // Shaft connects to triangle
        fillRect(cx + 5, cy - 8, 20, 16, color);
    }

    // Draw right arrow at center (cx, cy)
    void drawArrowRight(int cx, int cy, uint16_t color) {
        // Triangle pointing right (tip at cx+20)
        for (int i = 0; i < 25; i++) {
            int half = i;
            for (int j = -half; j <= half; j++)
                setPixel(cx + 20 - i, cy + j, color);
        }
        // Shaft connects to triangle
        fillRect(cx - 25, cy - 8, 20, 16, color);
    }

    // Draw up arrow at center (cx, cy)
    void drawArrowUp(int cx, int cy, uint16_t color) {
        // Triangle pointing up (tip at cy-25)
        for (int i = 0; i < 25; i++) {
            int half = i;
            for (int j = -half; j <= half; j++)
                setPixel(cx + j, cy - 25 + i, color);
        }
        // Shaft connects to triangle
        fillRect(cx - 8, cy, 16, 25, color);
    }
};

#endif // FB_DISPLAY_H