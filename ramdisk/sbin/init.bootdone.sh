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
echo "1" > /sys/devices/system/cpu/cpu4/online
echo "1" > /sys/devices/system/cpu/cpu5/online
echo "1" > /sys/devices/system/cpu/cpu6/online
echo "1" > /sys/devices/system/cpu/cpu7/online
echo "0-7" > /dev/cpuset/foreground/cpus
echo "4-7" > /dev/cpuset/foreground/boost/cpus
echo "0" > /dev/cpuset/background/cpus
echo "0-3" > /dev/cpuset/system-background/cpus
echo "0-3" > /dev/cpuset/invisible/cpus

#
# Set min/max cpufreq
#
echo "1500000" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
echo  "400000" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo "1500000" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq
echo  "400000" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_min_freq
echo "1500000" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq
echo  "400000" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq
echo "1500000" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_max_freq
echo  "400000" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_min_freq
echo "1500000" > /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq
echo  "400000" > /sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq
echo "1500000" > /sys/devices/system/cpu/cpu5/cpufreq/scaling_max_freq
echo  "400000" > /sys/devices/system/cpu/cpu5/cpufreq/scaling_min_freq
echo "1500000" > /sys/devices/system/cpu/cpu6/cpufreq/scaling_max_freq
echo  "400000" > /sys/devices/system/cpu/cpu6/cpufreq/scaling_min_freq
echo "1500000" > /sys/devices/system/cpu/cpu7/cpufreq/scaling_max_freq
echo  "400000" > /sys/devices/system/cpu/cpu7/cpufreq/scaling_min_freq

# Set Parameters for Lazyplug
echo "1" > /sys/module/lazyplug/parameters/touch_boost_active
# Set your own Lazyplug Profile (Default one is Balanced)
# 0=Balanced 1=Performance 2=Conservative 3=Eco-Performance 4=Eco-Conservative
echo "0" > /sys/module/lazyplug/parameters/nr_run_profile_sel
# Enable Lazyplug CPU hotplug
echo "1" > /sys/module/lazyplug/parameters/lazyplug_active
	