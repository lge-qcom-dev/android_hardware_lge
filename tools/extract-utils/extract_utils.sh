#!/bin/bash
#
# SPDX-FileCopyrightText: 2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

#
# extract_kdz:
#
# Positional parameters:
# $1: path to manufacturer KDZ package
# $2: Directory to dump to
#
function extract_kdz() {
    KDZTOOLS_DIR="$ANDROID_ROOT/hardware/lge/tools/extract-utils/kdztools/"
    KDZ_EXTRACT_DIR="$EXTRACT_TMP_DIR/kdz_extracted"
    if [ ! -d "$KDZ_EXTRACT_DIR" ]; then
        python "$KDZTOOLS_DIR"/unkdz.py -f "${1}" -d "${KDZ_EXTRACT_DIR}" -x
    fi
    find "${KDZ_EXTRACT_DIR}" -name "*.dz" -exec python "$KDZTOOLS_DIR"/undz.py -f {} -s -d "${2}" \;

    # Clean up
    rm -rf "${2}"/*_b.img
    for i in "${2}"/*_a.img; do mv -- "${i}" "${i/_a.img/.img}"; done
}

#
# prepare_images:
#
# Positional parameters:
# $1: path to manufacturer OTA/firmware package
#
function prepare_images() {
    # Consume positional parameters
    local SRC="$1"
    shift
    KEEP_DUMP_DIR="$SRC"

    if [ -d "$SRC"/output ]; then
        EXTRACT_SRC="$SRC"/output
        EXTRACT_STATE=1
        return 0
    fi

    # Try KDZ first
    if [ -f "$SRC" ] && [ "${SRC##*.}" == "kdz" ]; then
        local BASENAME=$(basename "$SRC")
        local DIRNAME=$(dirname "$SRC")
        DUMPDIR="$EXTRACT_TMP_DIR"/system_dump
        KEEP_DUMP_DIR="$DIRNAME"/"${BASENAME%.kdz}"
        if [ "$KEEP_DUMP" == "true" ] || [ "$KEEP_DUMP" == "1" ]; then
            rm -rf "$KEEP_DUMP_DIR"
            mkdir "$KEEP_DUMP_DIR"
        fi

        # Check if we're working with the same kdz that was passed last time.
        # If so, let's just use what's already extracted.
        MD5=$(md5sum "$SRC" | awk '{print $1}')
        OLDMD5=""
        if [ -f "$DUMPDIR/zipmd5.txt" ]; then
            OLDMD5=$(cat "$DUMPDIR/zipmd5.txt")
        fi

        if [ "$MD5" != "$OLDMD5" ]; then
            rm -rf "$DUMPDIR"
            mkdir "$DUMPDIR"
            extract_kdz "$SRC" "$DUMPDIR"
            echo "$MD5" >"$DUMPDIR"/zipmd5.txt

            for PARTITION in "system" "odm" "product" "system_ext" "vendor"; do
                if [ -a "$DUMPDIR"/"$PARTITION".img ]; then
                    extract_img_data "$DUMPDIR"/"$PARTITION".img "$DUMPDIR"/"$PARTITION"/
                fi
            done
        fi

        SRC="$DUMPDIR"

    # Try an Android-based OTA package
    elif [ -f "$SRC" ] && [ "${SRC##*.}" == "zip" ]; then
        local BASENAME=$(basename "$SRC")
        local DIRNAME=$(dirname "$SRC")
        DUMPDIR="$EXTRACT_TMP_DIR"/system_dump
        KEEP_DUMP_DIR="$DIRNAME"/"${BASENAME%.zip}"
        if [ "$KEEP_DUMP" == "true" ] || [ "$KEEP_DUMP" == "1" ]; then
            rm -rf "$KEEP_DUMP_DIR"
            mkdir "$KEEP_DUMP_DIR"
        fi

        # Check if we're working with the same zip that was passed last time.
        # If so, let's just use what's already extracted.
        MD5=$(md5sum "$SRC" | awk '{print $1}')
        OLDMD5=""
        if [ -f "$DUMPDIR/zipmd5.txt" ]; then
            OLDMD5=$(cat "$DUMPDIR/zipmd5.txt")
        fi

        if [ "$MD5" != "$OLDMD5" ]; then
            rm -rf "$DUMPDIR"
            mkdir "$DUMPDIR"
            unzip "$SRC" -d "$DUMPDIR"
            echo "$MD5" >"$DUMPDIR"/zipmd5.txt

            # Extract A/B OTA
            if [ -a "$DUMPDIR"/payload.bin ]; then
                for PARTITION in "system" "odm" "product" "system_ext" "vendor"; do
                    "$OTA_EXTRACTOR" --payload "$DUMPDIR"/payload.bin --output_dir "$DUMPDIR" --partitions "$PARTITION" &
                    2>&1
                done
                wait
            fi

            for PARTITION in "system" "odm" "product" "system_ext" "vendor"; do
                # If OTA is block based, extract it.
                if [ -a "$DUMPDIR"/"$PARTITION".new.dat.br ]; then
                    echo "Converting $PARTITION.new.dat.br to $PARTITION.new.dat"
                    brotli -d "$DUMPDIR"/"$PARTITION".new.dat.br
                    rm "$DUMPDIR"/"$PARTITION".new.dat.br
                fi
                if [ -a "$DUMPDIR"/"$PARTITION".new.dat ]; then
                    echo "Converting $PARTITION.new.dat to $PARTITION.img"
                    python "$ANDROID_ROOT"/tools/extract-utils/sdat2img.py "$DUMPDIR"/"$PARTITION".transfer.list "$DUMPDIR"/"$PARTITION".new.dat "$DUMPDIR"/"$PARTITION".img 2>&1
                    rm -rf "$DUMPDIR"/"$PARTITION".new.dat "$DUMPDIR"/"$PARTITION"
                    mkdir "$DUMPDIR"/"$PARTITION" "$DUMPDIR"/tmp
                    extract_img_data "$DUMPDIR"/"$PARTITION".img "$DUMPDIR"/"$PARTITION"/
                    rm "$DUMPDIR"/"$PARTITION".img
                fi
                if [ -a "$DUMPDIR"/"$PARTITION".img ]; then
                    extract_img_data "$DUMPDIR"/"$PARTITION".img "$DUMPDIR"/"$PARTITION"/
                fi
            done
        fi

        SRC="$DUMPDIR"
    fi

    local SUPERIMGS=()
    if [ -d "$SRC" ] && [ -f "$SRC"/super.img ]; then
        SUPERIMGS=("$SRC"/super.img)
    elif [ -d "$SRC" ] && [ -f "$SRC"/super.img_sparsechunk.0 ]; then
        readarray -t SUPERIMGS < <(find "$SRC" -name 'super.img_sparsechunk.*' | sort -V)
    fi

    if [ "${#SUPERIMGS[@]}" -ne 0 ]; then
        DUMPDIR="$EXTRACT_TMP_DIR"/super_dump
        mkdir -p "$DUMPDIR"

        echo "Unpacking super.img"
        "$SIMG2IMG" "${SUPERIMGS[@]}" "$DUMPDIR"/super.raw
        # If simg2img failed, then the given super partition is likely already unsparsed
        if [ $? -ne 0 ]; then
            cp "${SUPERIMGS[@]}" "$DUMPDIR"/super.raw
        fi

        for PARTITION in "system" "odm" "product" "system_ext" "vendor"; do
            echo "Preparing $PARTITION"
            if "$LPUNPACK" -p "$PARTITION"_a "$DUMPDIR"/super.raw "$DUMPDIR"; then
                mv "$DUMPDIR"/"$PARTITION"_a.img "$DUMPDIR"/"$PARTITION".img
            else
                "$LPUNPACK" -p "$PARTITION" "$DUMPDIR"/super.raw "$DUMPDIR"
            fi
        done
        rm "$DUMPDIR"/super.raw

        if [ "$KEEP_DUMP" == "true" ] || [ "$KEEP_DUMP" == "1" ]; then
            rm -rf "$KEEP_DUMP_DIR"/super_dump
            cp -a "$DUMPDIR" "$KEEP_DUMP_DIR"/super_dump
        fi

        SRC="$DUMPDIR"
    fi

    if [ -d "$SRC" ] && [ -f "$SRC"/system.img ]; then
        DUMPDIR="$EXTRACT_TMP_DIR"/system_dump
        mkdir -p "$DUMPDIR"

        for PARTITION in "system" "odm" "product" "system_ext" "vendor"; do
            echo "Extracting $PARTITION"
            local IMAGE="$SRC"/"$PARTITION".img
            if [ -f "$IMAGE" ]; then
                if [[ $(file -b "$IMAGE") == EROFS* ]]; then
                    fsck.erofs --extract="$DUMPDIR"/"$PARTITION" "$IMAGE"
                elif [[ $(file -b "$IMAGE") == Linux* ]]; then
                    extract_img_data "$IMAGE" "$DUMPDIR"/"$PARTITION"
                elif [[ $(file -b "$IMAGE") == Android* ]]; then
                    "$SIMG2IMG" "$IMAGE" "$DUMPDIR"/"$PARTITION".raw
                    extract_img_data "$DUMPDIR"/"$PARTITION".raw "$DUMPDIR"/"$PARTITION"/
                else
                    echo "Unsupported $IMAGE"
                fi
            fi
        done

        if [ "$KEEP_DUMP" == "true" ] || [ "$KEEP_DUMP" == "1" ]; then
            rm -rf "$KEEP_DUMP_DIR"/output
            cp -a "$DUMPDIR" "$KEEP_DUMP_DIR"/output
        fi

        SRC="$DUMPDIR"
    fi

    EXTRACT_SRC="$SRC"
    EXTRACT_STATE=1
}
