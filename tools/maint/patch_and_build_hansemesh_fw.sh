#!/bin/bash  # Note: switched to bash for process substitution support

export PATH="$HOME/.platformio/penv/bin:$PATH"

LOGFILE="$PWD/meshcore-evo-fw.log"
FIRMWARE_VERSION="v1.11.0-evo_0.1.5"
FIRMWARE_BUILD_DATE=$(date '+%d-%b-%Y')

collect_bin_files(){
    DEST_DIR="./firmwares"
    mkdir -p "$DEST_DIR"
    BUILD_DIR=".pio/build"

    if [ ! -d "$BUILD_DIR" ]; then
        echo "Error: $BUILD_DIR not found. Did you run the build process?"
        exit 1
    fi

    echo "Copying firmware files to $DEST_DIR..."

    for target_path in "$BUILD_DIR"/*/; do
        echo $target_path
        target_name=$(basename "$target_path")
    #    if ls "$target_path"*.bin >/dev/null 2>&1; then
            for bin_file in "$target_path"*firmware*.{uf2,bin,zip}; do
                filename=$(basename "$bin_file")
                new_filename="${target_name}_${FIRMWARE_VERSION}_${FIRMWARE_BUILD_DATE}_${filename}"
                cp "$bin_file" "$DEST_DIR/$new_filename"
                echo "Done: $new_filename"
            done
    #    fi
    done
}

# Everything after this line goes to BOTH console and logfile
exec > >(tee -a "$LOGFILE") 2>&1

echo "-------------------- Build start ----------------"
date
echo "-------------------------------------------------"

# apply patches
# ./tools/maint/apply_patches.sh 1199 1338 1297

# build all repeater firmwares, the will be in .out
FIRMWARE_VERSION=$FIRMWARE_VERSION ./build.sh build-repeater-firmwares

# build single firmwares
#FIRMWARE_VERSION=$FIRMWARE_VERSION FIRMWARE_BUILD_DATE=$FIRMWARE_BUILD_DATE ./build.sh build-firmware ProMicro_repeater
#FIRMWARE_VERSION=$FIRMWARE_VERSION FIRMWARE_BUILD_DATE=$FIRMWARE_BUILD_DATE ./build.sh build-firmware RAK_4631_repeater
#FIRMWARE_VERSION=$FIRMWARE_VERSION FIRMWARE_BUILD_DATE=$FIRMWARE_BUILD_DATE ./build.sh build-firmware heltec_v4_repeater
#FIRMWARE_VERSION=$FIRMWARE_VERSION FIRMWARE_BUILD_DATE=$FIRMWARE_BUILD_DATE ./build.sh build-firmware Heltec_v3_repeater
#FIRMWARE_VERSION=$FIRMWARE_VERSION FIRMWARE_BUILD_DATE=$FIRMWARE_BUILD_DATE ./build.sh build-firmware Xiao_nrf52_repeater
#FIRMWARE_VERSION=$FIRMWARE_VERSION FIRMWARE_BUILD_DATE=$FIRMWARE_BUILD_DATE ./build.sh build-firmware LilyGo_T3S3_sx1262_repeater
#FIRMWARE_VERSION=$FIRMWARE_VERSION FIRMWARE_BUILD_DATE=$FIRMWARE_BUILD_DATE ./build.sh build-firmware Heltec_t114_without_display_repeater
#FIRMWARE_VERSION=$FIRMWARE_VERSION FIRMWARE_BUILD_DATE=$FIRMWARE_BUILD_DATE ./build.sh build-firmware Heltec_t114_repeater
#collect_bin_files


echo "-------------------- Build end ------------------"
date
echo "-------------------------------------------------"

#grep -E " SUCCESS | FAILED " hansemesh_fw.log
