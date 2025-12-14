#pragma once

#include <string>
#include <optional>

// Fusion AHRS library
extern "C" {
#include "Fusion.h"
}

struct ImuData {
    float accel_x, accel_y, accel_z;  // m/s^2
    float gyro_x, gyro_y, gyro_z;     // deg/s
};

struct RpyAngles {
    float roll;   // degrees
    float pitch;  // degrees
    float yaw;    // degrees
};

// Calibration values for accelerometer and gyroscope
struct ImuCalibration {
    // Gyro offsets (subtracted from raw readings)
    float gyro_offset_x = 0.0f;
    float gyro_offset_y = 0.0f;
    float gyro_offset_z = 0.0f;
    // Accel scale factors (multiply raw readings to normalize to 1g)
    float accel_scale_x = 1.0f;
    float accel_scale_y = 1.0f;
    float accel_scale_z = 1.0f;
};

// VESC-style biquad lowpass filter
class BiquadFilter {
public:
    BiquadFilter() = default;

    // Configure as lowpass filter
    // cutoff_freq: cutoff frequency in Hz
    // sample_rate: sampling rate in Hz
    void configure_lowpass(float cutoff_freq, float sample_rate);

    // Process a single sample
    float process(float input);

    // Reset filter state
    void reset();

private:
    // Filter coefficients
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f;
    float a1_ = 0.0f, a2_ = 0.0f;

    // Filter state (delay elements)
    float x1_ = 0.0f, x2_ = 0.0f;  // Input history
    float y1_ = 0.0f, y2_ = 0.0f;  // Output history
};

class Imu {
public:
    Imu();
    ~Imu() = default;

    // Initialize IMU - find IIO devices and set up Fusion
    bool init();

    // Read raw IMU data from IIO sysfs (with calibration applied if set)
    std::optional<ImuData> read();

    // Update AHRS with new IMU data and get RPY angles
    RpyAngles update(const ImuData& data, float delta_time_sec);

    // Get current RPY angles without new data
    RpyAngles get_rpy() const;

    // VESC-style interactive calibration:
    // 1. Gyro: show live values, user presses Enter when stable
    // 2. Accel X/Y/Z: tilt to find max, user presses Enter when max found
    ImuCalibration calibrate();

    // Set calibration offsets (can load from saved values)
    void set_calibration(const ImuCalibration& cal);

    // Get current calibration
    const ImuCalibration& get_calibration() const { return calibration_; }

    bool is_initialized() const { return initialized_; }
    bool is_calibrated() const { return calibrated_; }

private:
    bool initialized_ = false;
    bool calibrated_ = false;

    // IIO sysfs paths
    std::string accel_path_;
    std::string gyro_path_;

    // Scale factors (from IIO)
    float accel_scale_ = 1.0f;
    float gyro_scale_ = 1.0f;

    // Calibration offsets
    ImuCalibration calibration_;

    // Fusion AHRS
    FusionOffset offset_;
    FusionAhrs ahrs_;

    // Software lowpass filters (VESC-style)
    // Lower cutoff = more smoothing but more lag
    static constexpr float FILTER_CUTOFF_HZ = 10.0f;  // Moderate filtering
    static constexpr int SAMPLE_RATE_HZ = 52;         // 52 Hz - higher rates cause noise on this sensor
    BiquadFilter accel_filter_x_, accel_filter_y_, accel_filter_z_;
    BiquadFilter gyro_filter_x_, gyro_filter_y_, gyro_filter_z_;
    bool filters_enabled_ = true;

    // Find IIO device by name
    std::string find_iio_device(const std::string& name);

    // Read a single value from sysfs
    int read_sysfs_int(const std::string& path);
    float read_sysfs_float(const std::string& path);

    // Write a string value to sysfs
    bool write_sysfs_string(const std::string& path, const std::string& value);

    // Read raw data without calibration applied
    std::optional<ImuData> read_raw();
};