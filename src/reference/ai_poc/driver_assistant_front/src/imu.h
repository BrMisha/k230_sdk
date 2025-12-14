#pragma once

#include <string>
#include <optional>

struct ImuData {
    float accel_x, accel_y, accel_z;  // m/s^2
};

struct RpyAngles {
    float roll;   // degrees
    float pitch;  // degrees
    float yaw;    // degrees
};

// Calibration values for accelerometer
struct ImuCalibration {
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

    // Initialize IMU - find accelerometer IIO device
    bool init();

    // Read accelerometer data from IIO sysfs (with calibration applied if set)
    std::optional<ImuData> read();

    // Calculate roll/pitch from accelerometer data (gravity-based)
    RpyAngles update(const ImuData& data);

    // Interactive calibration: tilt to find accel max for each axis
    ImuCalibration calibrate();

    // Set calibration offsets (can load from saved values)
    void set_calibration(const ImuCalibration& cal);

    // Get current calibration
    const ImuCalibration& get_calibration() const { return calibration_; }

    // Save calibration to file
    bool save_calibration(const std::string& path);

    // Load calibration from file
    bool load_calibration(const std::string& path);

    // Default calibration file path
    static constexpr const char* DEFAULT_CALIBRATION_FILE = "/sharefs/driver_assistant_detector/imu_calibration.txt";

    bool is_initialized() const { return initialized_; }
    bool is_calibrated() const { return calibrated_; }

private:
    bool initialized_ = false;
    bool calibrated_ = false;

    // IIO sysfs path
    std::string accel_path_;

    // Scale factor (from IIO)
    float accel_scale_ = 1.0f;

    // Calibration
    ImuCalibration calibration_;

    // Software lowpass filters
    static constexpr float FILTER_CUTOFF_HZ = 10.0f;
    static constexpr int SAMPLE_RATE_HZ = 52;
    BiquadFilter accel_filter_x_, accel_filter_y_, accel_filter_z_;
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