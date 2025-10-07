#!/bin/sh
set -eu

mode=$1
ffmpeg_arg=$2
target_path=$3
base=$4

if [ -z "$ffmpeg_arg" ]; then
    if [ -x "$target_path/ffmpeg_g" ]; then
        ffmpeg_arg=ffmpeg_g
    else
        ffmpeg_arg=ffmpeg
    fi
fi

case "$ffmpeg_arg" in
    /*) ffmpeg_exec="$ffmpeg_arg" ;;
    *) ffmpeg_exec="$target_path/$ffmpeg_arg" ;;
esac

ffmpeg_dir_tmp=$(dirname "$ffmpeg_exec")
ffmpeg_base=$(basename "$ffmpeg_exec")
ffmpeg_dir=$(cd "$ffmpeg_dir_tmp" && pwd)
ffmpeg_exec="$ffmpeg_dir/$ffmpeg_base"
ffprobe_exec="$ffmpeg_dir/$(printf '%s' "$ffmpeg_base" | sed 's/ffmpeg/ffprobe/')"

if [ ! -x "$ffprobe_exec" ]; then
    ffprobe_exec="$ffmpeg_dir/ffprobe"
fi

outdir="tests/data/fate"
mkdir -p "$outdir"

get_size() {
    file=$1
    if [ -f "$file" ]; then
        wc -c <"$file" | tr -d ' \t\n'
    else
        echo 0
    fi
}

cleanup_child() {
    pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

case "$mode" in
    pause)
        output="$outdir/ffmpeg-pause.raw"
        source_file="$outdir/ffmpeg-pause-source.yuv"
        progress_file="$outdir/ffmpeg-pause.progress"
        rm -f "$output"
        rm -f "$progress_file"

        if [ ! -f "$source_file" ]; then
            dd if=/dev/zero of="$source_file" bs=256 count=40 status=none
        fi

        "$ffmpeg_exec" -hide_banner -loglevel info -nostdin -stats_period 0.1 \
            -progress "$progress_file" \
            -re -stream_loop -1 \
            -f rawvideo -pix_fmt gray -s 16x16 -r 2 -i "$source_file" \
            -pix_fmt gray -c:v rawvideo -f rawvideo -y "$output" >/dev/null 2>&1 &
        pid=$!
        trap 'cleanup_child "$pid"' INT TERM EXIT

        # Wait for the encoder to start producing output
        tries=0
        while [ $tries -lt 60 ] && kill -0 "$pid" 2>/dev/null && [ ! -s "$output" ]; do
            sleep 0.1
            tries=$((tries + 1))
        done

        before_pause=$(get_size "$output")
        sleep 0.2

        read_progress() {
            if [ -f "$progress_file" ]; then
                awk -F= '/^frame=/{frame=$2} END{if (frame=="") print 0; else print frame+0}' "$progress_file"
            else
                echo 0
            fi
        }

        frame_before=0
        wait_iters=0
        while [ $wait_iters -lt 50 ]; do
            frame_before=$(read_progress)
            if [ "$frame_before" -gt 0 ]; then
                break
            fi
            sleep 0.1
            wait_iters=$((wait_iters + 1))
        done
        if [ "$frame_before" -le 0 ]; then
            echo "pause=no"
            exit 1
        fi

        kill -USR2 "$pid"
        sleep 0.2

        frame_during=$frame_before
        stable=0
        prev=$frame_before
        for _ in 1 2 3 4 5 6 7 8; do
            current=$(read_progress)
            if [ "$current" -gt "$prev" ]; then
                prev=$current
                stable=0
            else
                stable=$((stable + 1))
                if [ $stable -ge 3 ]; then
                    frame_during=$current
                    break
                fi
            fi
            sleep 0.2
        done

        if [ $stable -lt 3 ]; then
            echo "pause=no"
            exit 1
        fi

        kill -USR2 "$pid"
        sleep 1
        frame_after=$(read_progress)
        if [ "$frame_after" -le "$frame_during" ]; then
            echo "resume=no"
            exit 1
        fi

        kill -INT "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        trap - INT TERM EXIT

        echo "pause=yes"
        echo "resume=yes"
        ;;
    recovery)
        output="$outdir/ffmpeg-recovery.nut"
        sidecar="${output}.ffrecovery"
        rm -f "$output" "$sidecar"

        "$ffmpeg_exec" -hide_banner -loglevel info -nostdin -stats_period 0.1 \
            -auto_recovery -recovery_interval 0 \
            -f lavfi -i testsrc=size=640x360:rate=30 -frames:v 900 \
            -c:v mpeg2video -g 15 -f nut -y "$output" \
            >"$outdir/ffmpeg-recovery-initial.log" 2>&1 &
        pid=$!
        trap 'cleanup_child "$pid"' INT TERM EXIT

        sleep 0.2
        kill -INT "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        trap - INT TERM EXIT

        if [ ! -f "$sidecar" ]; then
            echo "checkpoint=missing"
            exit 1
        fi
        initial_size=$(get_size "$output")

        "$ffmpeg_exec" -hide_banner -loglevel info -nostdin -stats_period 0.1 \
            -auto_recovery -recovery_interval 0 \
            -f lavfi -i testsrc=size=640x360:rate=30 -frames:v 900 \
            -c:v mpeg2video -g 15 -f nut -y "$output" \
            >"$outdir/ffmpeg-recovery-resume.log" 2>&1

        if ! grep -q 'Loaded recovery checkpoint' "$outdir/ffmpeg-recovery-resume.log"; then
            echo "resume=no-checkpoint"
            exit 1
        fi

        final_size=$(get_size "$output")
        if [ "$final_size" -le "$initial_size" ]; then
            echo "resume=no-growth"
            exit 1
        fi

        if [ -f "$sidecar" ]; then
            echo "checkpoint=stale"
            exit 1
        fi


        echo "checkpoint=kept"
        echo "resume=ok"
        ;;
    *)
        echo "unknown mode" >&2
        exit 1
        ;;
esac
