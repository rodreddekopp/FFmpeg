/*
 * Copyright (c) 2000-2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * multimedia converter based on the FFmpeg libraries
 */

#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if HAVE_IO_H
#include <io.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#if HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#elif HAVE_GETPROCESSTIMES
#include <windows.h>
#endif
#if HAVE_GETPROCESSMEMORYINFO
#include <windows.h>
#include <psapi.h>
#endif
#if HAVE_SETCONSOLECTRLHANDLER
#include <windows.h>
#endif

#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#if HAVE_TERMIOS_H
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#elif HAVE_KBHIT
#include <conio.h>
#endif

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/dict.h"
#include "libavutil/file.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

#include "libavformat/avformat.h"

#include "libavdevice/avdevice.h"

#include "cmdutils.h"
#include "ffmpeg.h"
#include "ffmpeg_mux.h"
#include "ffmpeg_sched.h"
#include "ffmpeg_utils.h"
#include "graph/graphprint.h"

const char program_name[] = "ffmpeg";
const int program_birth_year = 2000;

FILE *vstats_file;

typedef struct BenchmarkTimeStamps {
    int64_t real_usec;
    int64_t user_usec;
    int64_t sys_usec;
} BenchmarkTimeStamps;

static BenchmarkTimeStamps get_benchmark_time_stamps(void);
static int64_t getmaxrss(void);

atomic_uint nb_output_dumped = 0;

static BenchmarkTimeStamps current_time;
AVIOContext *progress_avio = NULL;

InputFile   **input_files   = NULL;
int        nb_input_files   = 0;

OutputFile   **output_files   = NULL;
int         nb_output_files   = 0;

FilterGraph **filtergraphs;
int        nb_filtergraphs;

Decoder     **decoders;
int        nb_decoders;

typedef struct RecoveryState {
    int     active;
    int     resume_attempted;
    int64_t interval_us;
    int64_t last_update_wallclock_us;
    int64_t last_progress_us;
    int     preserve_checkpoints;
    int     have_resume_elapsed;
    int64_t resume_elapsed_us;
    int     have_checkpoint_frame;
    uint64_t last_checkpoint_frame;
} RecoveryState;

static const RecoveryState recovery_state_default = {
    .last_progress_us = AV_NOPTS_VALUE,
};

static RecoveryState recovery_state = {
    .last_progress_us = AV_NOPTS_VALUE,
};

#if HAVE_TERMIOS_H

/* init terminal so that we can grab keys */
static struct termios oldtty;
static int restore_tty;
#endif

static void term_exit_sigsafe(void)
{
#if HAVE_TERMIOS_H
    if(restore_tty)
        tcsetattr (0, TCSANOW, &oldtty);
#endif
}

void term_exit(void)
{
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    term_exit_sigsafe();
}

static volatile int received_sigterm = 0;
static volatile int received_nb_signals = 0;
static atomic_int transcode_init_done = 0;
static atomic_uintptr_t transcoding_scheduler = ATOMIC_VAR_INIT(0);
static atomic_int transcode_paused = 0;
#ifdef SIGUSR2
static atomic_int pause_toggle_pending = ATOMIC_VAR_INIT(0);
static void sigusr2_handler(int sig);
#endif
static volatile int ffmpeg_exited = 0;
static int64_t copy_ts_first_pts = AV_NOPTS_VALUE;

static int64_t pause_time_accum_us      = 0;
static int64_t pause_time_last_start_us = 0;
static int64_t encoding_time_offset_us  = 0;

static void pause_state_reset(void)
{
    pause_time_accum_us      = 0;
    pause_time_last_start_us = 0;
}

static void pause_state_begin(int64_t now)
{
    if (!pause_time_last_start_us)
        pause_time_last_start_us = now;
}

static void pause_state_end(int64_t now)
{
    if (pause_time_last_start_us) {
        if (now > pause_time_last_start_us)
            pause_time_accum_us += now - pause_time_last_start_us;
        pause_time_last_start_us = 0;
    }
}

static int64_t encoding_active_time_us(int64_t timer_start, int64_t cur_time)
{
    int64_t total = pause_time_accum_us;

    if (pause_time_last_start_us && cur_time > pause_time_last_start_us)
        total += cur_time - pause_time_last_start_us;

    if (cur_time <= timer_start)
        return encoding_time_offset_us;

    if (total > cur_time - timer_start)
        total = cur_time - timer_start;

    return FFMAX(cur_time - timer_start - total, 0) + encoding_time_offset_us;
}

static void
sigterm_handler(int sig)
{
    int ret;
    received_sigterm = sig;
    received_nb_signals++;
    term_exit_sigsafe();
    if(received_nb_signals > 3) {
        ret = write(2/*STDERR_FILENO*/, "Received > 3 system signals, hard exiting\n",
                    strlen("Received > 3 system signals, hard exiting\n"));
        if (ret < 0) { /* Do nothing */ };
        exit(123);
    }
}

#if HAVE_SETCONSOLECTRLHANDLER
static BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    av_log(NULL, AV_LOG_DEBUG, "\nReceived windows signal %ld\n", fdwCtrlType);

    switch (fdwCtrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        sigterm_handler(SIGINT);
        return TRUE;

    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        sigterm_handler(SIGTERM);
        /* Basically, with these 3 events, when we return from this method the
           process is hard terminated, so stall as long as we need to
           to try and let the main thread(s) clean up and gracefully terminate
           (we have at most 5 seconds, but should be done far before that). */
        while (!ffmpeg_exited) {
            Sleep(0);
        }
        return TRUE;

    default:
        av_log(NULL, AV_LOG_ERROR, "Received unknown windows signal %ld\n", fdwCtrlType);
        return FALSE;
    }
}
#endif

#ifdef __linux__
#define SIGNAL(sig, func)               \
    do {                                \
        action.sa_handler = func;       \
        sigaction(sig, &action, NULL);  \
    } while (0)
#else
#define SIGNAL(sig, func) \
    signal(sig, func)
#endif

void term_init(void)
{
#if defined __linux__
    struct sigaction action = {0};
    action.sa_handler = sigterm_handler;

    /* block other interrupts while processing this one */
    sigfillset(&action.sa_mask);

    /* restart interruptible functions (i.e. don't fail with EINTR)  */
    action.sa_flags = SA_RESTART;
#endif

#if HAVE_TERMIOS_H
    if (stdin_interaction) {
        struct termios tty;
        if (tcgetattr (0, &tty) == 0) {
            oldtty = tty;
            restore_tty = 1;

            tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                             |INLCR|IGNCR|ICRNL|IXON);
            tty.c_oflag |= OPOST;
            tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
            tty.c_cflag &= ~(CSIZE|PARENB);
            tty.c_cflag |= CS8;
            tty.c_cc[VMIN] = 1;
            tty.c_cc[VTIME] = 0;

            tcsetattr (0, TCSANOW, &tty);
        }
        SIGNAL(SIGQUIT, sigterm_handler); /* Quit (POSIX).  */
    }
#endif

    SIGNAL(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    SIGNAL(SIGTERM, sigterm_handler); /* Termination (ANSI).  */
#ifdef SIGUSR2
    SIGNAL(SIGUSR2, sigusr2_handler); /* User-defined signal toggles pause. */
#endif
#ifdef SIGXCPU
    SIGNAL(SIGXCPU, sigterm_handler);
#endif
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN); /* Broken pipe (POSIX). */
#endif
#if HAVE_SETCONSOLECTRLHANDLER
    SetConsoleCtrlHandler((PHANDLER_ROUTINE) CtrlHandler, TRUE);
#endif
}

/* read a key without blocking */
static int read_key(void)
{
    unsigned char ch;
#if HAVE_TERMIOS_H
    int n = 1;
    struct timeval tv;
    fd_set rfds;

    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    n = select(1, &rfds, NULL, NULL, &tv);
    if (n > 0) {
        n = read(0, &ch, 1);
        if (n == 1)
            return ch;

        return n;
    }
#elif HAVE_KBHIT
#    if HAVE_PEEKNAMEDPIPE && HAVE_GETSTDHANDLE
    static int is_pipe;
    static HANDLE input_handle;
    DWORD dw, nchars;
    if(!input_handle){
        input_handle = GetStdHandle(STD_INPUT_HANDLE);
        is_pipe = !GetConsoleMode(input_handle, &dw);
    }

    if (is_pipe) {
        /* When running under a GUI, you will end here. */
        if (!PeekNamedPipe(input_handle, NULL, 0, NULL, &nchars, NULL)) {
            // input pipe may have been closed by the program that ran ffmpeg
            return -1;
        }
        //Read it
        if(nchars != 0) {
            if (read(0, &ch, 1) == 1)
                return ch;
            return 0;
        }else{
            return -1;
        }
    }
#    endif
    if(kbhit())
        return(getch());
#endif
    return -1;
}

static int decode_interrupt_cb(void *ctx)
{
    return received_nb_signals > atomic_load(&transcode_init_done);
}

const AVIOInterruptCB int_cb = { decode_interrupt_cb, NULL };

static int recovery_path_supported(const char *url)
{
    const char *ignored;

    if (!url || !*url)
        return 0;

    if (av_strstart(url, "pipe:", &ignored) ||
        av_strstart(url, "fd:", &ignored))
        return 0;

    if (strstr(url, "://"))
        return 0;

    return 1;
}

static char *recovery_build_path(const char *url)
{
    static const char suffix[] = ".ffrecovery";
    size_t len;
    char *path;

    if (!url)
        return NULL;

    len = strlen(url);
    path = av_malloc(len + sizeof(suffix));
    if (!path)
        return NULL;

    memcpy(path, url, len);
    memcpy(path + len, suffix, sizeof(suffix));

    return path;
}

static int recovery_load_file(const char *path, char **data_out, size_t *size_out)
{
    uint8_t *mapped = NULL;
    size_t mapped_size = 0;
    int ret;

    ret = av_file_map(path, &mapped, &mapped_size, 0, NULL);
    if (ret < 0)
        return ret;

    if (mapped_size > (1 << 20)) {
        av_file_unmap(mapped, mapped_size);
        return AVERROR(EINVAL);
    }

    *data_out = av_malloc(mapped_size + 1);
    if (!*data_out) {
        av_file_unmap(mapped, mapped_size);
        return AVERROR(ENOMEM);
    }

    memcpy(*data_out, mapped, mapped_size);
    (*data_out)[mapped_size] = '\0';
    *size_out = mapped_size;

    av_file_unmap(mapped, mapped_size);

    return 0;
}

static void recovery_apply_seeks(void)
{
    for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist)) {
        InputFile *f;
        int64_t target_ts;
        int ret;

        if (ist->recovery_target_pts_us == AV_NOPTS_VALUE)
            continue;

        f = ist->file;
        if (!f || !f->ctx)
            continue;

        target_ts = av_rescale_q(ist->recovery_target_pts_us,
                                  AV_TIME_BASE_Q, ist->st->time_base);

        ret = avformat_seek_file(f->ctx, ist->index, INT64_MIN,
                                 target_ts, target_ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            av_log(ist, AV_LOG_WARNING,
                   "Failed to seek for recovery on input #%d:%d: %s\n",
                   f->index, ist->index, av_err2str(ret));
        } else {
            av_log(ist, AV_LOG_INFO,
                   "Recovered input #%d:%d to %s\n",
                   f->index, ist->index,
                   av_ts2timestr(target_ts, &ist->st->time_base));
            avformat_flush(f->ctx);
        }

        ist->recovery_target_pts_us = AV_NOPTS_VALUE;
    }
}

static int recovery_parse_checkpoint(OutputFile *of, char *content,
                                     int64_t *file_size_out,
                                     int64_t *progress_us_out)
{
    char *saveptr = NULL;
    char *line;
    int got_magic = 0;
    int64_t file_size = -1;
    int64_t progress_us = AV_NOPTS_VALUE;
    int64_t elapsed_us = -1;
    uint64_t checkpoint_frame = 0;

    while ((line = av_strtok(got_magic ? NULL : content, "\r\n", &saveptr))) {
        const char *arg;

        if (!got_magic) {
            if (!strcmp(line, "FFRECOV1")) {
                got_magic = 1;
                continue;
            }
            return AVERROR_INVALIDDATA;
        }

        if (av_strstart(line, "progress_us=", &arg)) {
            progress_us = strtoll(arg, NULL, 10);
        } else if (av_strstart(line, "file_size=", &arg)) {
            file_size = strtoll(arg, NULL, 10);
        } else if (av_strstart(line, "elapsed_us=", &arg)) {
            elapsed_us = strtoll(arg, NULL, 10);
        } else if (av_strstart(line, "IST ", &arg)) {
            int file_idx = 0;
            int stream_idx = 0;
            int64_t pts_us = AV_NOPTS_VALUE;
            uint64_t frames = 0;

            if (sscanf(line, "IST %d %d %" SCNd64 " %" SCNu64,
                       &file_idx, &stream_idx, &pts_us, &frames) == 4) {
                if (file_idx >= 0 && file_idx < nb_input_files) {
                    InputFile *f = input_files[file_idx];
                    if (f && stream_idx >= 0 && stream_idx < f->nb_streams) {
                        InputStream *ist = f->streams[stream_idx];
                        if (ist)
                            ist->recovery_target_pts_us = pts_us;
                    }
                }
            }
        } else if (av_strstart(line, "OST ", &arg)) {
            int file_idx = 0;
            int stream_idx = 0;
            uint64_t packets = 0;

            if (sscanf(line, "OST %d %d %" SCNu64,
                       &file_idx, &stream_idx, &packets) == 3) {
                if (file_idx >= 0 && file_idx < nb_output_files) {
                    OutputFile *of_it = output_files[file_idx];
                    if (of_it && stream_idx >= 0 && stream_idx < of_it->nb_streams) {
                        OutputStream *ost = of_it->streams[stream_idx];
                        if (ost) {
                            atomic_store(&ost->packets_written, packets);
                            if (ost->type == AVMEDIA_TYPE_VIDEO)
                                checkpoint_frame = FFMAX(checkpoint_frame, packets);
                        }
                    }
                }
            }
        }
    }

    if (!got_magic)
        return AVERROR_INVALIDDATA;
    if (file_size < 0)
        return AVERROR_INVALIDDATA;

    *file_size_out   = file_size;
    *progress_us_out = progress_us;

    if (elapsed_us < 0 && progress_us != AV_NOPTS_VALUE)
        elapsed_us = progress_us;

    of->recovery.elapsed_us = elapsed_us >= 0 ? elapsed_us : 0;
    of->recovery.last_frame = checkpoint_frame;

    if (elapsed_us >= 0) {
        recovery_state.resume_elapsed_us = FFMAX(recovery_state.resume_elapsed_us, elapsed_us);
        recovery_state.have_resume_elapsed = 1;
    }

    if (checkpoint_frame > 0) {
        recovery_state.last_checkpoint_frame = FFMAX(recovery_state.last_checkpoint_frame, checkpoint_frame);
        recovery_state.have_checkpoint_frame = 1;
    }

    recovery_apply_seeks();

    av_log(of, AV_LOG_INFO,
           "Loaded recovery checkpoint: size=%"PRId64" progress=%"PRId64"us\n",
           file_size, progress_us);

    return 0;
}

static int recovery_try_resume(OutputFile *of, int *open_flags)
{
    char *content = NULL;
    size_t content_size = 0;
    int64_t file_size = -1;
    int64_t progress_us = AV_NOPTS_VALUE;
    int ret;

    if (!of->recovery_path)
        return 0;

    if (access(of->recovery_path, F_OK) < 0)
        return 0;

    ret = recovery_load_file(of->recovery_path, &content, &content_size);
    if (ret < 0)
        return ret;

    ret = recovery_parse_checkpoint(of, content, &file_size, &progress_us);
    av_free(content);
    if (ret < 0)
        return ret;

    of->recovery.append      = 1;
    of->recovery.file_size   = file_size;
    of->recovery.progress_us = progress_us;

    if (progress_us != AV_NOPTS_VALUE)
        recovery_state.last_progress_us = progress_us;

    recovery_state.active = recovery_state.active || recovery_enabled;
    recovery_state.resume_attempted = 1;
    if (!recovery_state.interval_us)
        recovery_state.interval_us = recovery_interval;

    *open_flags |= AVIO_FLAG_READ;

    return 0;
}

static int recovery_write_snapshot(OutputFile *of, int64_t progress_us,
                                   int64_t elapsed_us)
{
    AVBPrint bp;
    char *data = NULL;
    AVIOContext *pb = NULL;
    int ret;
    int64_t file_size;
    uint64_t checkpoint_frame = 0;

    if (!of->recovery_path)
        return 0;

    ffmpeg_mux_checkpoint_flush(of);

    file_size = of_filesize(of);
    of->recovery.file_size   = file_size;
    of->recovery.progress_us = progress_us;
    of->recovery.elapsed_us  = elapsed_us >= 0 ? elapsed_us : 0;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&bp, "FFRECOV1\n");
    av_bprintf(&bp, "progress_us=%"PRId64"\n", progress_us);
    av_bprintf(&bp, "file_size=%"PRId64"\n", file_size);
    av_bprintf(&bp, "elapsed_us=%"PRId64"\n", of->recovery.elapsed_us);

    for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist)) {
        uint64_t frames = 0;
        int64_t last_pts = AV_NOPTS_VALUE;

        if (ist->decoder) {
            frames   = atomic_load(&ist->decoder->frames_decoded);
            last_pts = atomic_load(&ist->decoder->last_pts_us);
        }

        av_bprintf(&bp, "IST %d %d %"PRId64" %"PRIu64"\n",
                   ist->file->index, ist->index, last_pts, frames);
    }

    for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
        uint64_t packets = atomic_load(&ost->packets_written);

        av_bprintf(&bp, "OST %d %d %"PRIu64"\n",
                   ost->file->index, ost->index, packets);

        if (ost->type == AVMEDIA_TYPE_VIDEO)
            checkpoint_frame = FFMAX(checkpoint_frame, packets);
    }

    of->recovery.last_frame = checkpoint_frame;

    ret = av_bprint_finalize(&bp, &data);
    if (ret < 0)
        return ret;

    ret = avio_open2(&pb, of->recovery_path, AVIO_FLAG_WRITE, &int_cb, NULL);
    if (ret >= 0) {
        avio_write(pb, (const unsigned char *)data, strlen(data));
        avio_flush(pb);
        avio_closep(&pb);
    }

    if (ret < 0)
        av_log(of, AV_LOG_WARNING,
               "Failed to write recovery checkpoint to %s: %s\n",
               of->recovery_path, av_err2str(ret));

    av_free(data);

    return ret;
}

static void recovery_cleanup(int success)
{
    if (!recovery_state.active && !recovery_state.resume_attempted)
        return;

    for (int i = 0; i < nb_output_files; i++) {
        OutputFile *of = output_files[i];

        if (!of || !of->recovery_path)
            continue;

        if (success && !recovery_state.preserve_checkpoints)
            unlink(of->recovery_path);
    }

    recovery_state = recovery_state_default;
    encoding_time_offset_us = 0;
}

int recovery_prepare_output(OutputFile *of, const char *filename, int *open_flags)
{
    int ret;

    if (!of || !filename || (!recovery_enabled && !recovery_resume_enabled))
        return 0;

    if (!recovery_path_supported(filename))
        return 0;

    if (!of->recovery_path) {
        of->recovery_path = recovery_build_path(filename);
        if (!of->recovery_path)
            return AVERROR(ENOMEM);
    }

    if (recovery_resume_enabled) {
        ret = recovery_try_resume(of, open_flags);
        if (ret < 0) {
            av_log(of, AV_LOG_WARNING,
                   "Ignoring recovery data for output %s: %s\n",
                   filename, av_err2str(ret));
            // ignore errors, continue without resuming
        }
    }

    if (recovery_enabled) {
        of->recovery.enabled = 1;
        recovery_state.active = 1;
        recovery_state.interval_us = recovery_interval;
        recovery_state.last_update_wallclock_us = 0;
    }

    return 0;
}

void recovery_checkpoint_tick(int is_last_report, int64_t wallclock_us,
                              int64_t progress_us, int64_t elapsed_us)
{
    int should_flush;

    if (!recovery_state.active)
        return;

    if (elapsed_us >= 0) {
        recovery_state.resume_elapsed_us = FFMAX(recovery_state.resume_elapsed_us, elapsed_us);
        recovery_state.have_resume_elapsed = 1;
    }

    should_flush = !is_last_report;

    if (is_last_report && recovery_state.preserve_checkpoints)
        should_flush = 1;

    if (!should_flush)
        return;

    if (progress_us == AV_NOPTS_VALUE)
        progress_us = recovery_state.last_progress_us;

    if (!is_last_report) {
        if (progress_us == AV_NOPTS_VALUE)
            return;

        if (recovery_state.interval_us > 0 &&
            recovery_state.last_update_wallclock_us &&
            wallclock_us - recovery_state.last_update_wallclock_us < recovery_state.interval_us)
            return;
    }

    if (progress_us == AV_NOPTS_VALUE)
        return;

    for (int i = 0; i < nb_output_files; i++) {
        OutputFile *of = output_files[i];
        int ret;

        if (!of || !of->recovery.enabled)
            continue;

        ret = recovery_write_snapshot(of, progress_us, elapsed_us);
        if (ret >= 0 && of->recovery.last_frame > 0) {
            recovery_state.last_checkpoint_frame = FFMAX(recovery_state.last_checkpoint_frame,
                                                        of->recovery.last_frame);
            recovery_state.have_checkpoint_frame = 1;
        }
    }

    recovery_state.last_progress_us = progress_us;
    recovery_state.last_update_wallclock_us = wallclock_us;
}

static void ffmpeg_cleanup(int ret)
{
    recovery_cleanup(ret >= 0);

    if ((print_graphs || print_graphs_file) && nb_output_files > 0)
        print_filtergraphs(filtergraphs, nb_filtergraphs, input_files, nb_input_files, output_files, nb_output_files);

    if (do_benchmark) {
        int64_t maxrss = getmaxrss() / 1024;
        av_log(NULL, AV_LOG_INFO, "bench: maxrss=%"PRId64"KiB\n", maxrss);
    }

    for (int i = 0; i < nb_filtergraphs; i++)
        fg_free(&filtergraphs[i]);
    av_freep(&filtergraphs);

    for (int i = 0; i < nb_output_files; i++)
        of_free(&output_files[i]);

    for (int i = 0; i < nb_input_files; i++)
        ifile_close(&input_files[i]);

    for (int i = 0; i < nb_decoders; i++)
        dec_free(&decoders[i]);
    av_freep(&decoders);

    if (vstats_file) {
        if (fclose(vstats_file))
            av_log(NULL, AV_LOG_ERROR,
                   "Error closing vstats file, loss of information possible: %s\n",
                   av_err2str(AVERROR(errno)));
    }
    av_freep(&vstats_filename);
    of_enc_stats_close();

    hw_device_free_all();

    av_freep(&filter_nbthreads);

    av_freep(&print_graphs_file);
    av_freep(&print_graphs_format);

    av_freep(&input_files);
    av_freep(&output_files);

    uninit_opts();

    avformat_network_deinit();

    if (received_sigterm) {
        av_log(NULL, AV_LOG_INFO, "Exiting normally, received signal %d.\n",
               (int) received_sigterm);
    } else if (ret && atomic_load(&transcode_init_done)) {
        av_log(NULL, AV_LOG_INFO, "Conversion failed!\n");
    }
    term_exit();
    ffmpeg_exited = 1;
}

OutputStream *ost_iter(OutputStream *prev)
{
    int of_idx  = prev ? prev->file->index : 0;
    int ost_idx = prev ? prev->index + 1  : 0;

    for (; of_idx < nb_output_files; of_idx++) {
        OutputFile *of = output_files[of_idx];
        if (ost_idx < of->nb_streams)
            return of->streams[ost_idx];

        ost_idx = 0;
    }

    return NULL;
}

InputStream *ist_iter(InputStream *prev)
{
    int if_idx  = prev ? prev->file->index : 0;
    int ist_idx = prev ? prev->index + 1  : 0;

    for (; if_idx < nb_input_files; if_idx++) {
        InputFile *f = input_files[if_idx];
        if (ist_idx < f->nb_streams)
            return f->streams[ist_idx];

        ist_idx = 0;
    }

    return NULL;
}

static void frame_data_free(void *opaque, uint8_t *data)
{
    FrameData *fd = (FrameData *)data;

    avcodec_parameters_free(&fd->par_enc);

    av_free(data);
}

static int frame_data_ensure(AVBufferRef **dst, int writable)
{
    AVBufferRef *src = *dst;

    if (!src || (writable && !av_buffer_is_writable(src))) {
        FrameData *fd;

        fd = av_mallocz(sizeof(*fd));
        if (!fd)
            return AVERROR(ENOMEM);

        *dst = av_buffer_create((uint8_t *)fd, sizeof(*fd),
                                frame_data_free, NULL, 0);
        if (!*dst) {
            av_buffer_unref(&src);
            av_freep(&fd);
            return AVERROR(ENOMEM);
        }

        if (src) {
            const FrameData *fd_src = (const FrameData *)src->data;

            memcpy(fd, fd_src, sizeof(*fd));
            fd->par_enc = NULL;

            if (fd_src->par_enc) {
                int ret = 0;

                fd->par_enc = avcodec_parameters_alloc();
                ret = fd->par_enc ?
                      avcodec_parameters_copy(fd->par_enc, fd_src->par_enc) :
                      AVERROR(ENOMEM);
                if (ret < 0) {
                    av_buffer_unref(dst);
                    av_buffer_unref(&src);
                    return ret;
                }
            }

            av_buffer_unref(&src);
        } else {
            fd->dec.frame_num = UINT64_MAX;
            fd->dec.pts       = AV_NOPTS_VALUE;

            for (unsigned i = 0; i < FF_ARRAY_ELEMS(fd->wallclock); i++)
                fd->wallclock[i] = INT64_MIN;
        }
    }

    return 0;
}

FrameData *frame_data(AVFrame *frame)
{
    int ret = frame_data_ensure(&frame->opaque_ref, 1);
    return ret < 0 ? NULL : (FrameData*)frame->opaque_ref->data;
}

const FrameData *frame_data_c(AVFrame *frame)
{
    int ret = frame_data_ensure(&frame->opaque_ref, 0);
    return ret < 0 ? NULL : (const FrameData*)frame->opaque_ref->data;
}

FrameData *packet_data(AVPacket *pkt)
{
    int ret = frame_data_ensure(&pkt->opaque_ref, 1);
    return ret < 0 ? NULL : (FrameData*)pkt->opaque_ref->data;
}

const FrameData *packet_data_c(AVPacket *pkt)
{
    int ret = frame_data_ensure(&pkt->opaque_ref, 0);
    return ret < 0 ? NULL : (const FrameData*)pkt->opaque_ref->data;
}

int check_avoptions_used(const AVDictionary *opts, const AVDictionary *opts_used,
                         void *logctx, int decode)
{
    const AVClass  *class = avcodec_get_class();
    const AVClass *fclass = avformat_get_class();

    const int flag = decode ? AV_OPT_FLAG_DECODING_PARAM :
                              AV_OPT_FLAG_ENCODING_PARAM;
    const AVDictionaryEntry *e = NULL;

    while ((e = av_dict_iterate(opts, e))) {
        const AVOption *option, *foption;
        char *optname, *p;

        if (av_dict_get(opts_used, e->key, NULL, 0))
            continue;

        optname = av_strdup(e->key);
        if (!optname)
            return AVERROR(ENOMEM);

        p = strchr(optname, ':');
        if (p)
            *p = 0;

        option = av_opt_find(&class, optname, NULL, 0,
                             AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ);
        foption = av_opt_find(&fclass, optname, NULL, 0,
                              AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ);
        av_freep(&optname);
        if (!option || foption)
            continue;

        if (!(option->flags & flag)) {
            av_log(logctx, AV_LOG_ERROR, "Codec AVOption %s (%s) is not a %s "
                   "option.\n", e->key, option->help ? option->help : "",
                   decode ? "decoding" : "encoding");
            return AVERROR(EINVAL);
        }

        av_log(logctx, AV_LOG_WARNING, "Codec AVOption %s (%s) has not been used "
               "for any stream. The most likely reason is either wrong type "
               "(e.g. a video option with no video streams) or that it is a "
               "private option of some decoder which was not actually used "
               "for any stream.\n", e->key, option->help ? option->help : "");
    }

    return 0;
}

void update_benchmark(const char *fmt, ...)
{
    if (do_benchmark_all) {
        BenchmarkTimeStamps t = get_benchmark_time_stamps();
        va_list va;
        char buf[1024];

        if (fmt) {
            va_start(va, fmt);
            vsnprintf(buf, sizeof(buf), fmt, va);
            va_end(va);
            av_log(NULL, AV_LOG_INFO,
                   "bench: %8" PRIu64 " user %8" PRIu64 " sys %8" PRIu64 " real %s \n",
                   t.user_usec - current_time.user_usec,
                   t.sys_usec - current_time.sys_usec,
                   t.real_usec - current_time.real_usec, buf);
        }
        current_time = t;
    }
}

static void print_report(int is_last_report, int64_t timer_start, int64_t cur_time, int64_t pts)
{
    AVBPrint buf, buf_script;
    int64_t total_size = of_filesize(output_files[0]);
    int vid;
    double bitrate;
    double speed;
    static int64_t last_time = -1;
    static int first_report = 1;
    uint64_t nb_frames_dup = 0, nb_frames_drop = 0;
    int mins, secs, ms, us;
    int64_t hours;
    const char *hours_sign;
    int ret;
    double elapsed;
    int64_t active_time_us;

    active_time_us = encoding_active_time_us(timer_start, cur_time);
    recovery_checkpoint_tick(is_last_report, cur_time, pts, active_time_us);

    if (!print_stats && !is_last_report && !progress_avio)
        return;

    if (!is_last_report) {
        if (last_time == -1) {
            last_time = cur_time;
        }
        if (((cur_time - last_time) < stats_period && !first_report) ||
            (first_report && atomic_load(&nb_output_dumped) < nb_output_files))
            return;
        last_time = cur_time;
    }

    elapsed = active_time_us / 1000000.0;

    vid = 0;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprint_init(&buf_script, 0, AV_BPRINT_SIZE_AUTOMATIC);

    for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
        const float q = ost->enc ? atomic_load(&ost->quality) / (float) FF_QP2LAMBDA : -1;

        if (vid && ost->type == AVMEDIA_TYPE_VIDEO) {
            av_bprintf(&buf, "q=%2.1f ", q);
            av_bprintf(&buf_script, "stream_%d_%d_q=%.1f\n",
                       ost->file->index, ost->index, q);
        }
        if (!vid && ost->type == AVMEDIA_TYPE_VIDEO) {
            double fps;
            uint64_t frame_number = atomic_load(&ost->packets_written);

            fps = elapsed > 1.0 ? frame_number / elapsed : 0.0;
            av_bprintf(&buf, "frame=%5"PRId64" fps=%3.*f q=%3.1f ",
                     frame_number, fps < 9.95, fps, q);
            av_bprintf(&buf_script, "frame=%"PRId64"\n", frame_number);
            av_bprintf(&buf_script, "fps=%.2f\n", fps);
            av_bprintf(&buf_script, "stream_%d_%d_q=%.1f\n",
                       ost->file->index, ost->index, q);
            if (is_last_report)
                av_bprintf(&buf, "L");

            if (ost->filter) {
                nb_frames_dup  = atomic_load(&ost->filter->nb_frames_dup);
                nb_frames_drop = atomic_load(&ost->filter->nb_frames_drop);
            }

            vid = 1;
        }
    }

    if (copy_ts) {
        if (copy_ts_first_pts == AV_NOPTS_VALUE && pts > 1)
            copy_ts_first_pts = pts;
        if (copy_ts_first_pts != AV_NOPTS_VALUE)
            pts -= copy_ts_first_pts;
    }

    us    = FFABS64U(pts) % AV_TIME_BASE;
    secs  = FFABS64U(pts) / AV_TIME_BASE % 60;
    mins  = FFABS64U(pts) / AV_TIME_BASE / 60 % 60;
    hours = FFABS64U(pts) / AV_TIME_BASE / 3600;
    hours_sign = (pts < 0) ? "-" : "";

    bitrate = pts != AV_NOPTS_VALUE && pts && total_size >= 0 ? total_size * 8 / (pts / 1000.0) : -1;
    speed   = pts != AV_NOPTS_VALUE && elapsed > 0.0 ? (double)pts / AV_TIME_BASE / elapsed : -1;

    if (total_size < 0) av_bprintf(&buf, "size=N/A time=");
    else                av_bprintf(&buf, "size=%8.0fKiB time=", total_size / 1024.0);
    if (pts == AV_NOPTS_VALUE) {
        av_bprintf(&buf, "N/A ");
    } else {
        av_bprintf(&buf, "%s%02"PRId64":%02d:%02d.%02d ",
                   hours_sign, hours, mins, secs, (100 * us) / AV_TIME_BASE);
    }

    if (bitrate < 0) {
        av_bprintf(&buf, "bitrate=N/A");
        av_bprintf(&buf_script, "bitrate=N/A\n");
    }else{
        av_bprintf(&buf, "bitrate=%6.1fkbits/s", bitrate);
        av_bprintf(&buf_script, "bitrate=%6.1fkbits/s\n", bitrate);
    }

    if (total_size < 0) av_bprintf(&buf_script, "total_size=N/A\n");
    else                av_bprintf(&buf_script, "total_size=%"PRId64"\n", total_size);
    if (pts == AV_NOPTS_VALUE) {
        av_bprintf(&buf_script, "out_time_us=N/A\n");
        av_bprintf(&buf_script, "out_time_ms=N/A\n");
        av_bprintf(&buf_script, "out_time=N/A\n");
    } else {
        av_bprintf(&buf_script, "out_time_us=%"PRId64"\n", pts);
        av_bprintf(&buf_script, "out_time_ms=%"PRId64"\n", pts);
        av_bprintf(&buf_script, "out_time=%s%02"PRId64":%02d:%02d.%06d\n",
                   hours_sign, hours, mins, secs, us);
    }

    if (nb_frames_dup || nb_frames_drop)
        av_bprintf(&buf, " dup=%"PRId64" drop=%"PRId64, nb_frames_dup, nb_frames_drop);
    av_bprintf(&buf_script, "dup_frames=%"PRId64"\n", nb_frames_dup);
    av_bprintf(&buf_script, "drop_frames=%"PRId64"\n", nb_frames_drop);

    if (speed < 0) {
        av_bprintf(&buf, " speed=N/A");
        av_bprintf(&buf_script, "speed=N/A\n");
    } else {
        av_bprintf(&buf, " speed=%4.3gx", speed);
        av_bprintf(&buf_script, "speed=%4.3gx\n", speed);
    }

    if (recovery_state.active || recovery_state.resume_attempted) {
        if (recovery_state.have_checkpoint_frame) {
            av_bprintf(&buf, " chkpt=%"PRIu64, recovery_state.last_checkpoint_frame);
            av_bprintf(&buf_script, "checkpoint_frame=%"PRIu64"\n",
                       recovery_state.last_checkpoint_frame);
        } else {
            av_bprintf(&buf, " chkpt=N/A");
            av_bprintf(&buf_script, "checkpoint_frame=N/A\n");
        }
    }

    {
        int64_t total_secs;
        double fractional;

        if (elapsed < 0.0)
            elapsed = 0.0;

        total_secs = (int64_t)elapsed;
        fractional = elapsed - total_secs;
        if (fractional < 0.0)
            fractional = 0.0;

        ms = (int)(fractional * 1000.0);
        if (ms < 0)
            ms = 0;
        else if (ms > 999)
            ms = 999;

        secs = (int)(total_secs % 60);
        mins = (int)((total_secs / 60) % 60);
        hours = total_secs / 3600;
    }

    av_bprintf(&buf, " elapsed=%"PRId64":%02d:%02d.%02d", hours, mins, secs, ms / 10);

    if (print_stats || is_last_report) {
        const char end = is_last_report ? '\n' : '\r';
        if (print_stats==1 && AV_LOG_INFO > av_log_get_level()) {
            fprintf(stderr, "%s    %c", buf.str, end);
        } else
            av_log(NULL, AV_LOG_INFO, "%s    %c", buf.str, end);

        fflush(stderr);
    }
    av_bprint_finalize(&buf, NULL);

    if (progress_avio) {
        av_bprintf(&buf_script, "progress=%s\n",
                   is_last_report ? "end" : "continue");
        avio_write(progress_avio, buf_script.str,
                   FFMIN(buf_script.len, buf_script.size - 1));
        avio_flush(progress_avio);
        av_bprint_finalize(&buf_script, NULL);
        if (is_last_report) {
            if ((ret = avio_closep(&progress_avio)) < 0)
                av_log(NULL, AV_LOG_ERROR,
                       "Error closing progress log, loss of information possible: %s\n", av_err2str(ret));
        }
    }

    first_report = 0;
}

static void print_stream_maps(void)
{
    av_log(NULL, AV_LOG_INFO, "Stream mapping:\n");
    for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist)) {
        for (int j = 0; j < ist->nb_filters; j++) {
            if (!filtergraph_is_simple(ist->filters[j]->graph)) {
                av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d (%s) -> %s",
                       ist->file->index, ist->index, ist->dec ? ist->dec->name : "?",
                       ist->filters[j]->name);
                if (nb_filtergraphs > 1)
                    av_log(NULL, AV_LOG_INFO, " (graph %d)", ist->filters[j]->graph->index);
                av_log(NULL, AV_LOG_INFO, "\n");
            }
        }
    }

    for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
        if (ost->attachment_filename) {
            /* an attached file */
            av_log(NULL, AV_LOG_INFO, "  File %s -> Stream #%d:%d\n",
                   ost->attachment_filename, ost->file->index, ost->index);
            continue;
        }

        if (ost->filter && !filtergraph_is_simple(ost->filter->graph)) {
            /* output from a complex graph */
            av_log(NULL, AV_LOG_INFO, "  %s", ost->filter->name);
            if (nb_filtergraphs > 1)
                av_log(NULL, AV_LOG_INFO, " (graph %d)", ost->filter->graph->index);

            av_log(NULL, AV_LOG_INFO, " -> Stream #%d:%d (%s)\n", ost->file->index,
                   ost->index, ost->enc->enc_ctx->codec->name);
            continue;
        }

        av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d -> #%d:%d",
               ost->ist->file->index,
               ost->ist->index,
               ost->file->index,
               ost->index);
        if (ost->enc) {
            const AVCodec *in_codec    = ost->ist->dec;
            const AVCodec *out_codec   = ost->enc->enc_ctx->codec;
            const char *decoder_name   = "?";
            const char *in_codec_name  = "?";
            const char *encoder_name   = "?";
            const char *out_codec_name = "?";
            const AVCodecDescriptor *desc;

            if (in_codec) {
                decoder_name  = in_codec->name;
                desc = avcodec_descriptor_get(in_codec->id);
                if (desc)
                    in_codec_name = desc->name;
                if (!strcmp(decoder_name, in_codec_name))
                    decoder_name = "native";
            }

            if (out_codec) {
                encoder_name   = out_codec->name;
                desc = avcodec_descriptor_get(out_codec->id);
                if (desc)
                    out_codec_name = desc->name;
                if (!strcmp(encoder_name, out_codec_name))
                    encoder_name = "native";
            }

            av_log(NULL, AV_LOG_INFO, " (%s (%s) -> %s (%s))",
                   in_codec_name, decoder_name,
                   out_codec_name, encoder_name);
        } else
            av_log(NULL, AV_LOG_INFO, " (copy)");
        av_log(NULL, AV_LOG_INFO, "\n");
    }
}

static void set_tty_echo(int on)
{
#if HAVE_TERMIOS_H
    struct termios tty;
    if (tcgetattr(0, &tty) == 0) {
        if (on) tty.c_lflag |= ECHO;
        else    tty.c_lflag &= ~ECHO;
        tcsetattr(0, TCSANOW, &tty);
    }
#endif
}

static void toggle_transcoding_pause(const char *origin)
{
    Scheduler *sch = (Scheduler *)atomic_load(&transcoding_scheduler);

    if (!sch) {
        if (origin)
            av_log(NULL, AV_LOG_WARNING,
                   "\n\n%s pause request ignored: transcoder not ready yet.\n\n",
                   origin);
        return;
    }

    for (;;) {
        int paused = atomic_load(&transcode_paused);
        int ret = paused ? sch_resume(sch) : sch_pause(sch);

        if (ret < 0) {
            if (origin)
                av_log(NULL, AV_LOG_ERROR,
                       "\n\nUnable to %s transcoding: %s\n\n",
                       paused ? "resume" : "pause", av_err2str(ret));
            break;
        }

        if (atomic_compare_exchange_strong(&transcode_paused, &paused, !paused)) {
            int64_t now = av_gettime_relative();
            if (!paused)
                pause_state_begin(now);
            else
                pause_state_end(now);
            if (origin) {
                if (!paused)
                    av_log(NULL, AV_LOG_INFO,
                           "\n\nTranscoding paused. Use %s again to resume.\n\n",
                           origin);
                else
                    av_log(NULL, AV_LOG_INFO,
                           "\n\nTranscoding resumed.\n\n");
            }
            break;
        }
        /* Another thread raced us; try again with the updated state. */
    }
}

static int check_keyboard_interaction(int64_t cur_time)
{
    int i, key;
    static int64_t last_time;
    /* read_key() returns 0 on EOF */
    if (cur_time - last_time >= 100000) {
        key =  read_key();
        last_time = cur_time;
    }else
        key = -1;
    if (key == 'q') {
        av_log(NULL, AV_LOG_INFO, "\n\n[q] command received. Exiting.\n\n");
        return AVERROR_EXIT;
    }
    if (key == 'p')
        toggle_transcoding_pause("[p]");
    if (key == '+') av_log_set_level(av_log_get_level()+10);
    if (key == '-') av_log_set_level(av_log_get_level()-10);
    if (key == 'c' || key == 'C'){
        char buf[4096], target[64], command[256], arg[256] = {0};
        double time;
        int k, n = 0;
        fprintf(stderr, "\nEnter command: <target>|all <time>|-1 <command>[ <argument>]\n");
        i = 0;
        set_tty_echo(1);
        while ((k = read_key()) != '\n' && k != '\r' && i < sizeof(buf)-1)
            if (k > 0)
                buf[i++] = k;
        buf[i] = 0;
        set_tty_echo(0);
        fprintf(stderr, "\n");
        if (k > 0 &&
            (n = sscanf(buf, "%63[^ ] %lf %255[^ ] %255[^\n]", target, &time, command, arg)) >= 3) {
            av_log(NULL, AV_LOG_DEBUG, "Processing command target:%s time:%f command:%s arg:%s",
                   target, time, command, arg);
            for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
                if (ost->fg_simple)
                    fg_send_command(ost->fg_simple, time, target, command, arg,
                                    key == 'C');
            }
            for (i = 0; i < nb_filtergraphs; i++)
                fg_send_command(filtergraphs[i], time, target, command, arg,
                                key == 'C');
        } else {
            av_log(NULL, AV_LOG_ERROR,
                   "Parse error, at least 3 arguments were expected, "
                   "only %d given in string '%s'\n", n, buf);
        }
    }
    if (key == '?'){
        fprintf(stderr, "key    function\n"
                        "?      show this help\n"
                        "+      increase verbosity\n"
                        "-      decrease verbosity\n"
                        "p      toggle pause/resume\n"
                        "c      Send command to first matching filter supporting it\n"
                        "C      Send/Queue command to all matching filters\n"
                        "h      dump packets/hex press to cycle through the 3 states\n"
                        "q      quit\n"
                        "s      Show QP histogram\n"
        );
    }
    return 0;
}

/*
 * The following code is the main loop of the file converter
 */
static int transcode(Scheduler *sch)
{
    int ret = 0;
    int64_t timer_start, transcode_ts = 0;
    int stop_requested = 0;

    print_stream_maps();

    atomic_store(&transcode_init_done, 1);

    ret = sch_start(sch);
    if (ret < 0)
        return ret;

    atomic_store(&transcoding_scheduler, (uintptr_t)sch);
    atomic_store(&transcode_paused, 0);
    pause_state_reset();
    encoding_time_offset_us = recovery_state.have_resume_elapsed ?
                              recovery_state.resume_elapsed_us : 0;

    if (stdin_interaction) {
        av_log(NULL, AV_LOG_INFO, "Press [q] to stop, [p] to pause/resume, [?] for help\n");
    }

    timer_start = av_gettime_relative();

    while (!sch_wait(sch, stats_period, &transcode_ts)) {
        int64_t cur_time= av_gettime_relative();

        if (received_nb_signals)
            { stop_requested = 1; break; }

#ifdef SIGUSR2
        if (atomic_exchange(&pause_toggle_pending, 0))
            toggle_transcoding_pause("SIGUSR2");
#endif

        /* if 'q' pressed, exits */
        if (stdin_interaction)
            if (check_keyboard_interaction(cur_time) < 0) {
                stop_requested = 1;
                break;
            }

        /* dump report by using the output first video and audio streams */
        print_report(0, timer_start, cur_time, transcode_ts);
    }

    if (received_nb_signals)
        stop_requested = 1;

    if (stop_requested)
        recovery_state.preserve_checkpoints = 1;

    ret = sch_stop(sch, &transcode_ts);

    atomic_store(&transcoding_scheduler, (uintptr_t)NULL);
    atomic_store(&transcode_paused, 0);

    /* write the trailer if needed */
    for (int i = 0; i < nb_output_files; i++) {
        int err = of_write_trailer(output_files[i]);
        ret = err_merge(ret, err);
    }

    term_exit();

    /* dump report by using the first video and audio streams */
    print_report(1, timer_start, av_gettime_relative(), transcode_ts);

    return ret;
}

static BenchmarkTimeStamps get_benchmark_time_stamps(void)
{
    BenchmarkTimeStamps time_stamps = { av_gettime_relative() };
#if HAVE_GETRUSAGE
    struct rusage rusage;

    getrusage(RUSAGE_SELF, &rusage);
    time_stamps.user_usec =
        (rusage.ru_utime.tv_sec * 1000000LL) + rusage.ru_utime.tv_usec;
    time_stamps.sys_usec =
        (rusage.ru_stime.tv_sec * 1000000LL) + rusage.ru_stime.tv_usec;
#elif HAVE_GETPROCESSTIMES
    HANDLE proc;
    FILETIME c, e, k, u;
    proc = GetCurrentProcess();
    GetProcessTimes(proc, &c, &e, &k, &u);
    time_stamps.user_usec =
        ((int64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10;
    time_stamps.sys_usec =
        ((int64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) / 10;
#else
    time_stamps.user_usec = time_stamps.sys_usec = 0;
#endif
    return time_stamps;
}

static int64_t getmaxrss(void)
{
#if HAVE_GETRUSAGE && HAVE_STRUCT_RUSAGE_RU_MAXRSS
    struct rusage rusage;
    getrusage(RUSAGE_SELF, &rusage);
    return (int64_t)rusage.ru_maxrss * 1024;
#elif HAVE_GETPROCESSMEMORYINFO
    HANDLE proc;
    PROCESS_MEMORY_COUNTERS memcounters;
    proc = GetCurrentProcess();
    memcounters.cb = sizeof(memcounters);
    GetProcessMemoryInfo(proc, &memcounters, sizeof(memcounters));
    return memcounters.PeakPagefileUsage;
#else
    return 0;
#endif
}

int main(int argc, char **argv)
{
    Scheduler *sch = NULL;

    int ret;
    BenchmarkTimeStamps ti;

    init_dynload();

    setvbuf(stderr,NULL,_IONBF,0); /* win32 runtime needs this */

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    show_banner(argc, argv, options);

    sch = sch_alloc();
    if (!sch) {
        ret = AVERROR(ENOMEM);
        goto finish;
    }

    /* parse options and open all input/output files */
    ret = ffmpeg_parse_options(argc, argv, sch);
    if (ret < 0)
        goto finish;

    if (nb_output_files <= 0 && nb_input_files == 0) {
        show_usage();
        av_log(NULL, AV_LOG_WARNING, "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        ret = 1;
        goto finish;
    }

    if (nb_output_files <= 0) {
        av_log(NULL, AV_LOG_FATAL, "At least one output file must be specified\n");
        ret = 1;
        goto finish;
    }

    current_time = ti = get_benchmark_time_stamps();
    ret = transcode(sch);
    if (ret >= 0 && do_benchmark) {
        int64_t utime, stime, rtime;
        current_time = get_benchmark_time_stamps();
        utime = current_time.user_usec - ti.user_usec;
        stime = current_time.sys_usec  - ti.sys_usec;
        rtime = current_time.real_usec - ti.real_usec;
        av_log(NULL, AV_LOG_INFO,
               "bench: utime=%0.3fs stime=%0.3fs rtime=%0.3fs\n",
               utime / 1000000.0, stime / 1000000.0, rtime / 1000000.0);
    }

    ret = received_nb_signals                 ? 255 :
          (ret == FFMPEG_ERROR_RATE_EXCEEDED) ?  69 : ret;

finish:
    if (ret == AVERROR_EXIT)
        ret = 0;

    ffmpeg_cleanup(ret);

    sch_free(&sch);

    av_log(NULL, AV_LOG_VERBOSE, "\n");
    av_log(NULL, AV_LOG_VERBOSE, "Exiting with exit code %d\n", ret);

    return ret;
}
#ifdef SIGUSR2
static void sigusr2_handler(int sig)
{
    (void)sig;
    atomic_store(&pause_toggle_pending, 1);
}
#endif
