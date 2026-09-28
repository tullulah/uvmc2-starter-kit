#!/bin/sh
# uvm2_sd_test.sh — uvm2_sd.c against real FAT16, FAT32 and exFAT images, MBR and GPT.
#
# macOS only: the images are made by hdiutil/newfs, filled from the Mac side, then read and
# written through uvm2_sd.c (tools/uvm2_sd_test.c), and finally checked twice — `fsck -n` must
# come out clean, and the Mac must read back exactly what the harness wrote.
#
#     sdk/uvm2-sdk/tools/uvm2_sd_test.sh
#
# Exit status 0 = every case passed. Nothing here touches a real card.
set -eu
cd "$(dirname "$0")/.."
FATFS=../../third_party/fatfs
W=$(mktemp -d)
MNT="$W/mnt"
DEV=""
trap '[ -n "$DEV" ] && hdiutil detach -quiet "$DEV" 2>/dev/null; rm -rf "$W"' EXIT

cc -O2 -Wall -DUVM2_SD_HOST -I. -I$FATFS -o "$W/t" \
   tools/uvm2_sd_test.c uvm2_sd.c $FATFS/ff.c $FATFS/ffunicode.c

fails=0
check() {   # check <label> <expected ok=/err= fragment> <output>
    case "$3" in *"$2"*) echo "  pass  $1" ;;
                 *)     echo "  FAIL  $1: wanted '$2', got '$3'"; fails=$((fails + 1)) ;; esac
}
same() {    # same <label> <a> <b>
    if cmp -s "$2" "$3"; then echo "  pass  $1"
    else echo "  FAIL  $1: contents differ"; fails=$((fails + 1)); fi
}

head -c 30000   /dev/urandom > "$W/rom.zip"
head -c 300000  /dev/urandom > "$W/big.bin"

# fs  size  layout   hdiutil -fs   fsck      expected fs_type (FatFs FS_*)
for spec in "FAT16 64m MBRSPUD MS-DOS+FAT16 fsck_msdos 2" \
            "FAT32 256m MBRSPUD MS-DOS+FAT32 fsck_msdos 3" \
            "exFAT 256m MBRSPUD ExFAT fsck_exfat 4" \
            "exFAT 256m GPTSPUD ExFAT fsck_exfat 4"; do
    set -- $spec
    name=$1; size=$2; layout=$3; fs=$(echo "$4" | tr + ' '); fsck=$5; want_fs=$6
    echo "$name ($layout)"
    img="$W/$name-$layout.dmg"
    rm -f "$img"
    hdiutil create -quiet -size "$size" -fs "$fs" -volname UVMC2 -layout "$layout" "$img"
    DEV=$(hdiutil attach -nobrowse -mountpoint "$MNT" "$img" | awk 'NR==1{print $1}')
    mkdir -p "$MNT/roms" "$MNT/config"
    cp "$W/rom.zip" "$MNT/roms/tacscan.zip"
    cp "$W/rom.zip" "$MNT/roms/A Long Romset Name.zip"
    printf 'old\n' > "$MNT/config/short.cfg"                 # shorter than 512 bytes
    head -c 1024 /dev/zero | tr '\0' x > "$MNT/config/uvm2.cfg"
    hdiutil detach -quiet "$DEV"; DEV=""

    T="$W/t $img"
    check "mount sees $name"    "fs=$want_fs"  "$($T read roms/tacscan.zip "$W/out")"
    same  "romset reads back"   "$W/rom.zip" "$W/out"
    check "case-insensitive"    "ok=1"         "$($T read ROMS/TACSCAN.ZIP "$W/out")"
    check "long file name"      "ok=1"         "$($T read "roms/a long romset name.zip" "$W/out")"
    same  "long name contents"  "$W/rom.zip" "$W/out"
    check "missing -> MISSING"  "ok=0 err=4"   "$($T read roms/nothere.zip "$W/out")"
    check "chunk"               "ok=1 err=0 n=1000" "$($T readfrom roms/tacscan.zip 20000 1000 "$W/out")"
    dd if="$W/rom.zip" of="$W/ref" bs=1 skip=20000 count=1000 2>/dev/null
    same  "chunk contents"      "$W/ref" "$W/out"
    check "chunk past the end"  "ok=1 err=0 n=0" "$($T readfrom roms/tacscan.zip 40000 10 "$W/out")"
    check "write, nested dirs"  "ok=1 err=0"   "$($T write traces/run1/big.bin "$W/big.bin")"
    check "write replaces"      "ok=1 err=0"   "$($T write traces/run1/big.bin "$W/rom.zip")"
    check "create"              "ok=1 err=0"   "$($T create newcfg/game.cfg "a=1")"
    check "overwrite existing"  "ok=1 err=0"   "$($T overwrite config/uvm2.cfg "b=2")"
    check "overwrite missing"   "ok=0 err=4"   "$($T overwrite config/none.cfg "c=3")"
    check "overwrite too short" "ok=0 err=5"   "$($T overwrite config/short.cfg "d=4")"

    # Attached without mounting: the first line is the disk, the last one the volume.
    hdiutil attach -nobrowse -nomount "$img" > "$W/att"
    DEV=$(awk 'NR==1{print $1}' "$W/att"); part=$(awk 'END{print $1}' "$W/att")
    if $fsck -n "$part" > "$W/fsck.log" 2>&1; then echo "  pass  $fsck -n clean"
    else echo "  FAIL  $fsck -n:"; sed 's/^/        /' "$W/fsck.log"; fails=$((fails + 1)); fi
    hdiutil detach -quiet "$DEV"
    DEV=$(hdiutil attach -nobrowse -mountpoint "$MNT" "$img" | awk 'NR==1{print $1}')
    same  "Mac reads the write" "$W/rom.zip" "$MNT/traces/run1/big.bin"
    printf 'a=1' > "$W/ref"; head -c 3 "$MNT/newcfg/game.cfg" > "$W/out"
    same  "Mac reads create"    "$W/ref" "$W/out"
    check "create is 512 bytes" "512"          "$(wc -c < "$MNT/newcfg/game.cfg" | tr -d ' ')"
    printf 'b=2' > "$W/ref"; head -c 3 "$MNT/config/uvm2.cfg" > "$W/out"
    same  "Mac reads overwrite" "$W/ref" "$W/out"
    check "overwrite keeps size" "1024"        "$(wc -c < "$MNT/config/uvm2.cfg" | tr -d ' ')"
    hdiutil detach -quiet "$DEV"; DEV=""
done

echo
if [ "$fails" -eq 0 ]; then echo "all passed"; else echo "$fails FAILED"; exit 1; fi
