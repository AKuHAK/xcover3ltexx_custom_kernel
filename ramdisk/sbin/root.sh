#!/system/bin/sh

# Mount system as RW to move su.img to data
mount -o remount,rw /system

# Move su.img to data
mv /data/local/tmp/su.img /data/su.img

# Remount system as RO and data as RW
mount -o remount,ro /system
mount -o remount,rw /data
