#!/system/bin/sh
# Deploy libmodloader.so to PFX VR via tmpfs overlay
set -e

LIB_DIR=$(find /data/app -path '*PFXVRQuest*/lib/arm64' -type d 2>/dev/null | head -1)
if [ -z "$LIB_DIR" ]; then
    echo "ERROR: PFX VR lib dir not found"
    exit 1
fi
echo "LIB_DIR=$LIB_DIR"

BACKUP=/data/local/tmp/pfxvr_backup
NEWSO=/data/local/tmp/libmodloader.so

# Check new .so exists
if [ ! -f "$NEWSO" ]; then
    echo "ERROR: $NEWSO not found — push it first"
    exit 1
fi

# Backup originals
mkdir -p "$BACKUP"
echo "Backing up original .so files..."
for f in "$LIB_DIR"/*.so; do
    fname=$(basename "$f")
    if [ ! -f "$BACKUP/$fname" ]; then
        cp "$f" "$BACKUP/$fname"
        echo "  backed up: $fname"
    else
        echo "  already backed up: $fname"
    fi
done
ls -la "$BACKUP/"

# Mount tmpfs over the lib directory
echo "Mounting tmpfs overlay over $LIB_DIR ..."
mount -t tmpfs tmpfs "$LIB_DIR"
echo "  tmpfs mounted"

# Copy all originals back + add libmodloader.so
echo "Restoring .so files + adding libmodloader.so ..."
for f in "$BACKUP"/*.so; do
    cp "$f" "$LIB_DIR/$(basename $f)"
done
cp "$NEWSO" "$LIB_DIR/libmodloader.so"
chmod 755 "$LIB_DIR"/*.so

echo "Final contents:"
ls -la "$LIB_DIR/"
echo "DONE — libmodloader.so deployed via tmpfs overlay"
