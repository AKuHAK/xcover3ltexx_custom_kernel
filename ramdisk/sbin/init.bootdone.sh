#!/sbin/sh

while pgrep bootanimation; do sleep 1; done
while pgrep dex2oat; do sleep 1; done
until pgrep systemui; do sleep 1; done

#
# CPUSET
#
echo "1" > /sys/devices/system/cpu/cpu1/online
echo "1" > /sys/devices/system/cpu/cpu2/online
echo "1" > /sys/devices/system/cpu/cpu3/online
echo "0-3" > /dev/cpuset/foreground/cpus
echo "0" > /dev/cpuset/background/cpus
echo "0-3" > /dev/cpuset/system-background/cpus
echo "0-3" > /dev/cpuset/invisible/cpus

#
# Set min/max cpufreq
#
# echo "1500000" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
# echo  "400000" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
# echo "1500000" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq
# echo  "400000" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_min_freq
# echo "1500000" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq
# echo  "400000" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq
# echo "1500000" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_max_freq
# echo  "400000" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_min_freq

