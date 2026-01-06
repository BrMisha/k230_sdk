#!/bin/sh
PID=$(pidof driver_assistant_front)

while true; do
  VSZ=$(awk '/VmSize/ {print $2}' /proc/$PID/status)
  RSS=$(awk '/VmRSS/ {print $2}' /proc/$PID/status)
  THREADS=$(awk '/Threads/ {print $2}' /proc/$PID/status)
  FREE=$(awk '/MemAvailable/ {printf "%.0f", $2/1024}' /proc/meminfo)
  
  # Fix: get CPU for exact PID, take first match only
  CPU=$(top -b -n1 | awk -v pid="$PID" '$1 == pid {print $7; exit}')
  
  echo "$(date +%H:%M:%S) | VSZ: ${VSZ}kB | RSS: ${RSS}kB | CPU: ${CPU} | Threads: $THREADS | Free: ${FREE}MB"
  
  usleep 500000
done
