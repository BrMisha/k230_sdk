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

} // namespace utils

#endif // UTILS_H