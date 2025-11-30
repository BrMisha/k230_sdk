/**
 * @file utils.h
 * @brief Utility functions for driver assistant front application
 */

#ifndef UTILS_H
#define UTILS_H

#include <string>

namespace utils {

/**
 * Get the first non-loopback IPv4 address
 * @return IP address string, or "No IP" if no interface found
 */
std::string get_ip_address();

/**
 * Get current time formatted as HH:MM:SS
 * @return Time string in HH:MM:SS format
 */
std::string get_current_time();

/**
 * Get Linux CPU load percentage (0-100)
 * Calculates CPU usage since last call by reading /proc/stat
 * @return CPU usage percentage as integer (0-100)
 */
int get_linux_cpu_load();

} // namespace utils

#endif // UTILS_H