/**
 * @file utils.cpp
 * @brief Utility functions implementation
 */

#include "utils.h"
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>

namespace utils {

std::string get_ip_address() {
    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) == -1) {
        return "No IP";
    }

    // Iterate through linked list of interfaces
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr)
            continue;

        // Check for IPv4 address
        if (ifa->ifa_addr->sa_family == AF_INET) {
            // Skip loopback interface
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;

            // Get IP address string
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                               host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);
            if (s == 0) {
                freeifaddrs(ifaddr);
                return std::string(host);
            }
        }
    }

    freeifaddrs(ifaddr);
    return "No IP";
}

std::string get_current_time() {
    time_t now = time(nullptr);
    struct tm *local_time = localtime(&now);

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << local_time->tm_hour << ":"
        << std::setfill('0') << std::setw(2) << local_time->tm_min << ":"
        << std::setfill('0') << std::setw(2) << local_time->tm_sec;

    return oss.str();
}

} // namespace utils