#include "imu.h"

#include <dirent.h>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <poll.h>

#include "../../driver_assistant_detector/scoped_timing.hpp"

// IIO base path
static constexpr const char* IIO_BASE_PATH = "/sys/bus/iio/devices/";

// Gravity constant
static constexpr float G = 9.80665f;

// ============== BiquadFilter Implementation ==============

void BiquadFilter::configure_lowpass(float cutoff_freq, float sample_rate) {
    // Compute normalized frequency (0 to 0.5)
    float fc = cutoff_freq / sample_rate;
    if (fc >= 0.5f) fc = 0.499f;  // Limit to below Nyquist
    if (fc <= 0.0f) fc = 0.001f;

    // Compute coefficients using bilinear transform
    // 2nd order Butterworth lowpass
    const float pi = 3.14159265358979f;
    float omega = 2.0f * pi * fc;
    float sn = std::sin(omega);
    float cs = std::cos(omega);
    float Q = 0.7071067811865476f;  // Butterworth Q = 1/sqrt(2)
    float alpha = sn / (2.0f * Q);

    float a0 = 1.0f + alpha;
    b0_ = ((1.0f - cs) / 2.0f) / a0;
    b1_ = (1.0f - cs) / a0;
    b2_ = ((1.0f - cs) / 2.0f) / a0;
    a1_ = (-2.0f * cs) / a0;
    a2_ = (1.0f - alpha) / a0;

    reset();
}

float BiquadFilter::process(float input) {
    // Direct Form II Transposed
    float output = b0_ * input + x1_;
    x1_ = b1_ * input - a1_ * output + x2_;
    x2_ = b2_ * input - a2_ * output;
    return output;
}

void BiquadFilter::reset() {
    x1_ = x2_ = y1_ = y2_ = 0.0f;
}

Imu::Imu() {
    // Nothing to initialize - accel-only mode
}

bool Imu::init() {
    // Find accelerometer device
    accel_path_ = find_iio_device("lsm6ds3_accel");
    if (accel_path_.empty()) {
        std::cerr << "IMU: Could not find lsm6ds3_accel IIO device" << std::endl;
        return false;
    }
    std::cout << "IMU: Found accelerometer at " << accel_path_ << std::endl;

    // Read scale factor
    accel_scale_ = read_sysfs_float(accel_path_ + "/in_accel_scale");
    if (accel_scale_ == 0.0f) {
        accel_scale_ = 0.000598f;  // Default for LSM6DS3 at +/-2g
    }
    std::cout << "IMU: Accel scale = " << accel_scale_ << std::endl;

    // Set sampling frequency
    std::string sample_rate_str = std::to_string(SAMPLE_RATE_HZ);
    if (write_sysfs_string(accel_path_ + "/sampling_frequency", sample_rate_str)) {
        std::cout << "IMU: Accel sample rate set to " << SAMPLE_RATE_HZ << " Hz" << std::endl;
    }

    // Configure software lowpass filters
    accel_filter_x_.configure_lowpass(FILTER_CUTOFF_HZ, SAMPLE_RATE_HZ);
    accel_filter_y_.configure_lowpass(FILTER_CUTOFF_HZ, SAMPLE_RATE_HZ);
    accel_filter_z_.configure_lowpass(FILTER_CUTOFF_HZ, SAMPLE_RATE_HZ);
    std::cout << "IMU: Software lowpass filters configured at " << FILTER_CUTOFF_HZ << " Hz" << std::endl;

    initialized_ = true;
    return true;
}

std::optional<ImuData> Imu::read_raw() {
    if (!initialized_) {
        return std::nullopt;
    }

    ImuData data{};

    // Read accelerometer (raw values in IIO units)
    int accel_x_raw = read_sysfs_int(accel_path_ + "/in_accel_x_raw");
    int accel_y_raw = read_sysfs_int(accel_path_ + "/in_accel_y_raw");
    int accel_z_raw = read_sysfs_int(accel_path_ + "/in_accel_z_raw");

    // Convert to m/s^2 (IIO scale is already in m/s^2 units)
    float ax = accel_x_raw * accel_scale_;
    float ay = accel_y_raw * accel_scale_;
    float az = accel_z_raw * accel_scale_;

    // Apply software lowpass filters
    if (filters_enabled_) {
        data.accel_x = accel_filter_x_.process(ax);
        data.accel_y = accel_filter_y_.process(ay);
        data.accel_z = accel_filter_z_.process(az);
    } else {
        data.accel_x = ax;
        data.accel_y = ay;
        data.accel_z = az;
    }

    return data;
}

std::optional<ImuData> Imu::read() {
    auto raw = read_raw();
    if (!raw) {
        return std::nullopt;
    }

    // Apply calibration if calibrated
    if (calibrated_) {
        // Accel: multiply by scale factor to normalize to 1g
        raw->accel_x *= calibration_.accel_scale_x;
        raw->accel_y *= calibration_.accel_scale_y;
        raw->accel_z *= calibration_.accel_scale_z;
    }

    return raw;
}

// Helper: check if Enter was pressed (non-blocking)
static bool enter_pressed() {
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0) {
        char c;
        if (::read(STDIN_FILENO, &c, 1) > 0) {
            return (c == '\n');
        }
    }
    return false;
}

// Helper: flush any pending input
static void flush_stdin() {
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    while (poll(&pfd, 1, 0) > 0) {
        char c;
        ::read(STDIN_FILENO, &c, 1);
    }
}

ImuCalibration Imu::calibrate() {
    if (!initialized_) {
        std::cerr << "IMU: Cannot calibrate - not initialized" << std::endl;
        return {};
    }

    ImuCalibration cal;

    std::cout << "\n========== Accelerometer Calibration ==========" << std::endl;

    // Reset filters before calibration
    accel_filter_x_.reset();
    accel_filter_y_.reset();
    accel_filter_z_.reset();

    // Warm-up period: run filters for ~2 seconds to let them settle
    std::cout << "\nWarming up filters..." << std::endl;
    for (int i = 0; i < 100; i++) {  // ~100 samples at 20ms = 2 seconds
        read_raw();
        if (i % 25 == 0) {
            printf("\r  Settling [%d%%]   ", (i * 100) / 100);
            fflush(stdout);
        }
        usleep(20000);
    }
    std::cout << "\nFilter warm-up complete.\n" << std::endl;

    // === Step 1: Accel X calibration ===
    std::cout << "[1/3] ACCEL X CALIBRATION" << std::endl;
    std::cout << "Tilt device so X axis points UP. Press ENTER when max found.\n" << std::endl;

    flush_stdin();
    float max_x = 0;

    while (!enter_pressed()) {
        auto data = read_raw();
        if (data) {
            if (std::fabs(data->accel_x) > std::fabs(max_x)) {
                max_x = data->accel_x;
            }
            printf("\rLive: Ax=%7.2f   Max: %7.2f m/s² (target ~9.81)   ",
                   data->accel_x, max_x);
            fflush(stdout);
        }
        usleep(20000);  // 20ms - match sensor rate
    }

    cal.accel_scale_x = (std::fabs(max_x) > 0.1f) ? (G / std::fabs(max_x)) : 1.0f;
    std::cout << "\nX max: " << max_x << " m/s², scale: " << cal.accel_scale_x << std::endl;

    // === Step 2: Accel Y calibration ===
    std::cout << "\n[2/3] ACCEL Y CALIBRATION" << std::endl;
    std::cout << "Tilt device so Y axis points UP. Press ENTER when max found.\n" << std::endl;

    flush_stdin();
    float max_y = 0;

    while (!enter_pressed()) {
        auto data = read_raw();
        if (data) {
            if (std::fabs(data->accel_y) > std::fabs(max_y)) {
                max_y = data->accel_y;
            }
            printf("\rLive: Ay=%7.2f   Max: %7.2f m/s² (target ~9.81)   ",
                   data->accel_y, max_y);
            fflush(stdout);
        }
        usleep(20000);  // 20ms - match sensor rate
    }

    cal.accel_scale_y = (std::fabs(max_y) > 0.1f) ? (G / std::fabs(max_y)) : 1.0f;
    std::cout << "\nY max: " << max_y << " m/s², scale: " << cal.accel_scale_y << std::endl;

    // === Step 3: Accel Z calibration ===
    std::cout << "\n[3/3] ACCEL Z CALIBRATION" << std::endl;
    std::cout << "Place device FLAT (Z axis points UP). Press ENTER when max found.\n" << std::endl;

    flush_stdin();
    float max_z = 0;

    while (!enter_pressed()) {
        auto data = read_raw();
        if (data) {
            if (std::fabs(data->accel_z) > std::fabs(max_z)) {
                max_z = data->accel_z;
            }
            printf("\rLive: Az=%7.2f   Max: %7.2f m/s² (target ~9.81)   ",
                   data->accel_z, max_z);
            fflush(stdout);
        }
        usleep(20000);  // 20ms - match sensor rate
    }

    cal.accel_scale_z = (std::fabs(max_z) > 0.1f) ? (G / std::fabs(max_z)) : 1.0f;
    std::cout << "\nZ max: " << max_z << " m/s², scale: " << cal.accel_scale_z << std::endl;

    // === Summary ===
    std::cout << "\n========== Calibration Complete ==========" << std::endl;
    std::cout << "Accel scales: X=" << cal.accel_scale_x
              << ", Y=" << cal.accel_scale_y << ", Z=" << cal.accel_scale_z << std::endl;

    set_calibration(cal);

    // Auto-save calibration to file
    save_calibration(DEFAULT_CALIBRATION_FILE);

    return cal;
}

void Imu::set_calibration(const ImuCalibration& cal) {
    calibration_ = cal;
    calibrated_ = true;
    std::cout << "IMU: Calibration applied" << std::endl;
}

bool Imu::save_calibration(const std::string& path) {
    std::ofstream file(path);
    if (!file) {
        std::cerr << "IMU: Failed to open calibration file for writing: " << path << std::endl;
        return false;
    }

    file << "# IMU Calibration File (Accel only)\n";
    file << "accel_scale_x=" << calibration_.accel_scale_x << "\n";
    file << "accel_scale_y=" << calibration_.accel_scale_y << "\n";
    file << "accel_scale_z=" << calibration_.accel_scale_z << "\n";

    if (file.good()) {
        std::cout << "IMU: Calibration saved to " << path << std::endl;
        return true;
    }
    std::cerr << "IMU: Failed to write calibration file" << std::endl;
    return false;
}

bool Imu::load_calibration(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cout << "IMU: No calibration file found at " << path << std::endl;
        return false;
    }

    ImuCalibration cal;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = line.substr(0, eq_pos);
        float value = std::stof(line.substr(eq_pos + 1));

        if (key == "accel_scale_x") cal.accel_scale_x = value;
        else if (key == "accel_scale_y") cal.accel_scale_y = value;
        else if (key == "accel_scale_z") cal.accel_scale_z = value;
    }

    set_calibration(cal);
    std::cout << "IMU: Calibration loaded from " << path << std::endl;
    std::cout << "  Accel scales: X=" << cal.accel_scale_x << ", Y=" << cal.accel_scale_y << ", Z=" << cal.accel_scale_z << std::endl;
    return true;
}

RpyAngles Imu::update(const ImuData& data) {
    // Calculate roll/pitch from accelerometer (gravity vector)
    // Convert to g units
    float ax = data.accel_x / G;
    float ay = data.accel_y / G;
    float az = data.accel_z / G;

    constexpr float RAD_TO_DEG = 57.2957795f;

    // Roll: rotation around X axis (positive = right side down)
    float roll = std::atan2(ay, az) * RAD_TO_DEG;

    // Pitch: rotation around Y axis (positive = nose up)
    float pitch = std::atan2(-ax, std::sqrt(ay * ay + az * az)) * RAD_TO_DEG;

    // Yaw cannot be determined from accelerometer alone
    float yaw = 0.0f;

    return RpyAngles{roll, pitch, yaw};
}

std::string Imu::find_iio_device(const std::string& name) {
    DIR* dir = opendir(IIO_BASE_PATH);
    if (!dir) {
        return "";
    }

    std::string result;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "iio:device", 10) == 0) {
            std::string device_path = std::string(IIO_BASE_PATH) + entry->d_name;
            std::string name_path = device_path + "/name";

            std::ifstream name_file(name_path);
            if (name_file) {
                std::string device_name;
                std::getline(name_file, device_name);
                if (device_name == name) {
                    result = device_path;
                    break;
                }
            }
        }
    }
    closedir(dir);
    return result;
}

int Imu::read_sysfs_int(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return 0;
    }
    int value = 0;
    file >> value;
    return value;
}

float Imu::read_sysfs_float(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return 0.0f;
    }
    float value = 0.0f;
    file >> value;
    return value;
}

bool Imu::write_sysfs_string(const std::string& path, const std::string& value) {
    std::ofstream file(path);
    if (!file) {
        std::cerr << "IMU: Failed to open " << path << " for writing" << std::endl;
        return false;
    }
    file << value;
    return file.good();
}