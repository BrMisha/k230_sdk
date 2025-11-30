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
#include <cstdio>

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

int get_linux_cpu_load() {
    static unsigned long long prev_idle = 0, prev_total = 0;
    static bool first_call = true;

    FILE* f = fopen("/proc/stat", "r");
    if (!f) return 0;

    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;

    // Read at least 4 fields (user, nice, system, idle), rest are optional
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    fclose(f);

    if (n < 4) return 0;

    unsigned long long idle_time = idle + iowait;
    unsigned long long total_time = user + nice + system + idle + iowait + irq + softirq + steal;

    unsigned long long idle_delta = idle_time - prev_idle;
    unsigned long long total_delta = total_time - prev_total;

    prev_idle = idle_time;
    prev_total = total_time;

    // Skip first call (delta from 0 gives average since boot)
    if (first_call) {
        first_call = false;
        return 0;
    }

    if (total_delta == 0) return 0;

    return static_cast<int>(100 * (total_delta - idle_delta) / total_delta);
}

} // namespace utils