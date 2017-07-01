#!/system/bin/sh

# Mount system as RW to move su.img to data
mount -o remount,rw /system

# Move su.img & superuser.apk to data
mv /data/local/tmp/su.img /data/su.img
mv /data/local/tmp/superuser.apk /data/SuperSu.apk
chmod 0644 /data/SuperSu.apk
chcon u:object_r:system_file:s0 /data/SuperSu.apk

# Remount system as RO and data as RW
mount -o remount,ro /system
mount -o remount,rw /data
