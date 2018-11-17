#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/parseutils.h>
#include "json.h"

static int stop_signal = 0;
static unsigned int framerate = 1, quality = 70, enable_lenscorrection = 0;
static unsigned long size_limit = 0;
static double lenscorrection_k1 = -0.125;
static const char *src_filename = NULL, *dst_filename = NULL, *json_filename = NULL;
static AVFormatContext *src_fmt_ctx = NULL;
static AVFormatContext *dst_fmt_ctx = NULL;
static AVCodecContext *src_codec_ctx = NULL;
static AVCodecContext *dst_codec_ctx = NULL;
static AVStream *src_stream = NULL;
static AVStream *dst_stream = NULL;
static AVFilterGraph *filter_graph = NULL;
static AVFilterContext *buffersrc_ctx = NULL;
static AVFilterContext *filter_ctx = NULL;
static AVFilterContext *buffersink_ctx = NULL;
static int src_video_stream_idx = -1;
static int frame_filtered = 0;
static char dst_current_filename[1024];
static unsigned long dst_current_file = 0;
static unsigned long dst_current_frame_count = 0;
static unsigned long dst_current_bytes_written = 0;
static unsigned long dst_total_frame_count = 0;
static unsigned long dst_total_bytes_written = 0;

static int init_filter_graph() {
    char in_args[512];
    char lenscorrection_args[512];
    int ret;

    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *lenscorrection = avfilter_get_by_name("lenscorrection");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");

    filter_graph = avfilter_graph_alloc();

    AVFilterInOut *inputs = avfilter_inout_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();

    fprintf(stderr, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d\n",
           dst_codec_ctx->width, dst_codec_ctx->height, dst_codec_ctx->pix_fmt,
           dst_codec_ctx->time_base.num, dst_codec_ctx->time_base.den,
           dst_codec_ctx->sample_aspect_ratio.num, dst_codec_ctx->sample_aspect_ratio.den);
    fflush(stderr);
    snprintf(in_args, sizeof(in_args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             dst_codec_ctx->width, dst_codec_ctx->height, dst_codec_ctx->pix_fmt,
             dst_codec_ctx->time_base.num, dst_codec_ctx->time_base.den,
             dst_codec_ctx->sample_aspect_ratio.num, dst_codec_ctx->sample_aspect_ratio.den);
    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", in_args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source: %s\n", av_err2str(ret));
        return ret;
    }

    snprintf(lenscorrection_args, sizeof(lenscorrection_args), "cx=0.5:cy=0.5:k1=%f:k2=-0.012", lenscorrection_k1);
    ret = avfilter_graph_create_filter(&filter_ctx, lenscorrection, NULL, lenscorrection_args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create lenscorrection filter\n");
        return ret;
    }

    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);

    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    if (ret >= 0) ret = avfilter_link(buffersrc_ctx, 0, filter_ctx, 0);
    if (ret >= 0) ret = avfilter_link(filter_ctx, 0, buffersink_ctx, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error connecting filters\n");
        return ret;
    }

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        return ret;
    return 0;

}

static int open_src(AVFormatContext *fmt_ctx, enum AVMediaType type) {
    int ret;
    AVCodec *dec = NULL;
    AVDictionary *opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, &dec, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        return ret;
    } else {
        AVStream *st;
        int stream_idx = ret;
        st = fmt_ctx->streams[stream_idx];

        src_codec_ctx = avcodec_alloc_context3(dec);
        if (!src_codec_ctx) {
            fprintf(stderr, "Failed to allocate codec\n");
            return AVERROR(EINVAL);
        }

        ret = avcodec_parameters_to_context(src_codec_ctx, st->codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters to codec context\n");
            return ret;
        }

        if ((ret = avcodec_open2(src_codec_ctx, dec, &opts)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }

        src_video_stream_idx = stream_idx;
        src_stream = fmt_ctx->streams[src_video_stream_idx];
    }

    return 0;
}

static int open_dst(const char *codec) {
    int ret = 0;
    AVCodec *encoder = avcodec_find_encoder_by_name(codec);
    if (!encoder) {
        av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
        return AVERROR_INVALIDDATA;
    }
    sprintf(dst_current_filename, dst_filename, dst_current_file);
    avformat_alloc_output_context2(&dst_fmt_ctx, NULL, NULL, dst_current_filename);
    if (!dst_fmt_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }
    dst_stream = avformat_new_stream(dst_fmt_ctx, encoder);
    if (!dst_stream) {
        av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
        return AVERROR_UNKNOWN;
    }
    dst_codec_ctx = avcodec_alloc_context3(encoder);
    dst_codec_ctx->qmax = 129 - (int) round(quality * 1.28);
    dst_codec_ctx->qmin = dst_codec_ctx->qmax;
    dst_codec_ctx->height = src_codec_ctx->height;
    dst_codec_ctx->width = src_codec_ctx->width;
    dst_codec_ctx->sample_aspect_ratio = src_codec_ctx->sample_aspect_ratio;
    if (encoder->pix_fmts)
        dst_codec_ctx->pix_fmt = encoder->pix_fmts[0];
    else
        dst_codec_ctx->pix_fmt = src_codec_ctx->pix_fmt;
    dst_codec_ctx->time_base = (AVRational) {1, framerate};
    dst_codec_ctx->framerate = (AVRational) {framerate, 1};
    avcodec_parameters_from_context(dst_stream->codecpar, dst_codec_ctx);
    dst_stream->time_base = dst_codec_ctx->time_base;
    dst_stream->avg_frame_rate = (AVRational) {1, 1};
    dst_stream->sample_aspect_ratio = dst_codec_ctx->sample_aspect_ratio;

    if ((ret = avcodec_open2(dst_codec_ctx, encoder, NULL)) != 0) {
        fprintf(stderr, "Failed to open output codec: %s\n", av_err2str(ret));
        return ret;
    }
    if ((ret = avio_open(&dst_fmt_ctx->pb, dst_current_filename, AVIO_FLAG_WRITE)) != 0) {
        fprintf(stderr, "Failed to open the output file: %s\n", av_err2str(ret));
        return ret;
    }

    if ((ret = avformat_write_header(dst_fmt_ctx, NULL)) != 0) {
        fprintf(stderr, "Failed to write output header: %s\n", av_err2str(ret));
        return ret;
    }
    return ret;
}

static int close_dst() {
    int ret = 0;
    if (dst_fmt_ctx == NULL) {
        return ret;
    }
    if ((ret = av_write_trailer(dst_fmt_ctx)) != 0) {
        fprintf(stderr, "Failed to write output trailer: %s\n", av_err2str(ret));
        return ret;
    }
    if ((ret = avio_close(dst_fmt_ctx->pb)) != 0) {
        fprintf(stderr, "Failed to close the output file: %s\n", av_err2str(ret));
        return ret;
    };
    avformat_free_context(dst_fmt_ctx);
    dst_fmt_ctx = NULL;
}

static int write_frame(AVFrame *frame) {
    int ret = 0;
    AVPacket packet = {0};
    if (dst_current_frame_count == 0) {
        if ((ret = open_dst("mjpeg")) < 0) {
            fprintf(stderr, "Could not open the destination file: %s\n", av_err2str(ret));
            return ret;
        }
    }
    if (filter_graph == NULL)
        init_filter_graph();
    frame->pts = packet.dts = dst_current_frame_count + 1;
    if (enable_lenscorrection && !frame_filtered) {
        frame_filtered = 1;
        if ((ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
            return ret;
        }
        if ((ret = av_buffersink_get_frame(buffersink_ctx, frame)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error while reading from the filtergraph\n");
            return ret;
        }
        while (ret >= 0) {
            ret = av_buffersink_get_frame(buffersink_ctx, NULL);
        }
    }
    if ((ret = avcodec_send_frame(dst_codec_ctx, frame)) != 0) {
        fprintf(stderr, "Failed to send a frame for encoding: %s\n", av_err2str(ret));
        return ret;
    }
    while ((ret = avcodec_receive_packet(dst_codec_ctx, &packet)) >= 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "Error during encoding: %s\n", av_err2str(ret));
            return ret;
        }

        int packet_size = packet.size;

        if (size_limit > 0 && dst_current_bytes_written + packet_size >= size_limit) {
            close_dst();
            av_packet_unref(&packet);
            if (dst_current_frame_count < 1) {
                fprintf(stderr, "Frame size grater than size limit\n");
                return -1;
            }
            dst_current_frame_count = 0;
            dst_current_bytes_written = 0;
            dst_current_file++;
            return write_frame(frame);
        }

        if (packet.pts != AV_NOPTS_VALUE)
            packet.pts = av_rescale_q(packet.pts, dst_codec_ctx->time_base, dst_stream->time_base);
        if (packet.dts != AV_NOPTS_VALUE)
            packet.dts = av_rescale_q(packet.dts, dst_codec_ctx->time_base, dst_stream->time_base);

        if ((ret = av_interleaved_write_frame(dst_fmt_ctx, &packet)) != 0) {
            fprintf(stderr, "Failed to write output frame: %s\n", av_err2str(ret));
            av_packet_unref(&packet);
            return ret;
        }

        frame_filtered = 0;
        dst_current_frame_count++;
        dst_total_frame_count++;
        dst_total_bytes_written += packet_size;
        dst_current_bytes_written += packet_size;

        av_packet_unref(&packet);
    }
    return 0;
}

static void print_usage(const char *self) {
    fprintf(stderr, "Usage: %s [OPTION]... <INPUT> <JSON> <OUTPUT>\n"
                    "\n"
                    "  -h              show help and exit\n"
                    "  -f 1..60        output framerate\n"
                    "  -l -1.0..1.0    quadratic lens correction coefficient\n"
                    "  -q 1..100       output quality\n"
                    "  -s BYTES        output file size limit\n"
                    "\n"
                    "If the size limit is set, the OUTPUT argument should contain a %%d format specifier. Example:\n"
                    "  %s -s 500000000 input.avi example.json output_%%d.avi\n",
            self, self);
}

static void stop(int sig) {
    stop_signal = sig;
}


int main(int argc, char **argv) {
    int ret = 0, success = 0;
    int video_frame_count = 0;

    signal(SIGTERM, stop);
    signal(SIGINT, stop);

    int opt;
    while ((opt = getopt(argc, argv, "hf:l:q:s:")) != -1) {
        unsigned long ulong_value = 0;
        double double_value = 0.0;
        if (opt == 'l')
            double_value = optarg != NULL ? strtod(optarg, (char **) NULL) : 0;
        else
            ulong_value = optarg != NULL ? strtoul(optarg, (char **) NULL, 10) : 0;
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                exit(0);
            case 'f':
                if (ulong_value < 1 || ulong_value > 60) {
                    print_usage(argv[0]);
                    exit(1);
                }
                framerate = (unsigned int) ulong_value;
                break;
            case 'l':
                if (double_value < -1.0 || double_value > 1.0) {
                    print_usage(argv[0]);
                    exit(1);
                }
                enable_lenscorrection = 1;
                lenscorrection_k1 = double_value;
                break;
            case 'q':
                if (ulong_value < 1 || ulong_value > 100) {
                    print_usage(argv[0]);
                    exit(1);
                }
                quality = (unsigned int) ulong_value;
                break;
            case 's':
                if (ulong_value < 1) {
                    print_usage(argv[0]);
                    exit(1);
                }
                size_limit = ulong_value;
                break;
            default:
                break;
        }
    }
    if (argc - optind != 3) {
        print_usage(argv[0]);
        exit(1);
    }
    src_filename = argv[optind];
    json_filename = argv[optind + 1];
    dst_filename = argv[optind + 2];

    if (size_limit > 0 && strstr(dst_filename, "%d") == NULL) {
        print_usage(argv[0]);
        exit(1);
    }

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
#endif
    if (enable_lenscorrection)
        avfilter_register_all();

    if ((ret = json_parse(json_filename)) < 0) {
        fprintf(stderr, "Could not parse JSON: %s\n", json_err2str(ret));
        goto end;
    }

    if ((ret = avformat_open_input(&src_fmt_ctx, src_filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Could not open the source file %s: %s\n", src_filename, av_err2str(ret));
        goto end;
    }

    if ((ret = avformat_find_stream_info(src_fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find stream information: %s\n", av_err2str(ret));
        goto end;
    }

    if ((ret = open_src(src_fmt_ctx, AVMEDIA_TYPE_VIDEO)) < 0) {
        fprintf(stderr, "Could not open the source fie: %s\n", av_err2str(ret));
        goto end;
    }

    if (!src_stream) {
        fprintf(stderr, "Could not find video stream in the input, aborting\n");
        goto end;
    }

    for (int i = 0; i < json_token_count() && stop_signal == 0; i++) {
        if (json_token(i).type == JSMN_STRING &&
            strlen("time") == (unsigned int) (json_token(i).end - json_token(i).start) &&
            strncmp(json_buffer() + json_token(i).start, "time",
                    (size_t) (json_token(i).end - json_token(i).start)) == 0) {
            AVPacket pkt = {0};
            AVFrame *frame = NULL;
            AVFrame *prev_frame = NULL;
            int64_t req_ts = NULL;
            int done = 0;

            size_t size = (size_t) (json_token(i + 1).end - json_token(i + 1).start);
            char *time_str = (char *) malloc((size + 1) * sizeof(char));
            memcpy(time_str, json_buffer() + json_token(i + 1).start, size);
            time_str[size] = '\0';
            av_parse_time(&req_ts, time_str, 1);
            free(time_str);
            req_ts /= av_q2d(src_stream->time_base) * 1e+6;

            frame = av_frame_alloc();
            prev_frame = av_frame_alloc();
            if (!frame || !prev_frame) {
                fprintf(stderr, "Could not allocate frame\n");
                goto end;
            }

            av_seek_frame(src_fmt_ctx, src_video_stream_idx, req_ts, AVSEEK_FLAG_BACKWARD);
            while (!done && av_read_frame(src_fmt_ctx, &pkt) >= 0) {
                if (pkt.stream_index != src_video_stream_idx || pkt.dts == AV_NOPTS_VALUE) {
                    av_packet_unref(&pkt);
                    continue;
                }
                if ((ret = avcodec_send_packet(src_codec_ctx, &pkt)) < 0) {
                    av_packet_unref(&pkt);
                    fprintf(stderr, "Error while sending a packet to the decoder: %s\n", av_err2str(ret));
                    goto end;
                }
                while (prev_frame->format < 0) {
                    av_frame_unref(frame);
                    avcodec_receive_frame(src_codec_ctx, prev_frame);
                }
                while (!done) {
                    ret = avcodec_receive_frame(src_codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        fprintf(stderr, "Error while receiving a frame from the decoder: %s\n", av_err2str(ret));
                        break;
                    }
                    video_frame_count++;
                    if (llabs(req_ts - frame->best_effort_timestamp) >=
                        llabs(req_ts - prev_frame->best_effort_timestamp)) {
                        if (write_frame(prev_frame) != 0) {
                            av_frame_unref(frame);
                            av_frame_unref(prev_frame);
                            av_frame_free(&frame);
                            av_frame_free(&prev_frame);
                            goto end;
                        }
                        fprintf(stderr, "%ld %.3f %s\n", dst_total_frame_count,
                                prev_frame->best_effort_timestamp * av_q2d(src_stream->time_base),
                                dst_current_filename);
                        done = 1;
                    } else {
                        av_frame_unref(prev_frame);
                        av_frame_copy(prev_frame, frame);
                    }
                    av_frame_unref(frame);
                }
            }
            av_frame_unref(prev_frame);
            av_frame_free(&frame);
            av_frame_free(&prev_frame);
        }
    }

    if (close_dst() != 0)
        goto end;

    success = 1;

    end:
    avcodec_free_context(&src_codec_ctx);
    avformat_close_input(&src_fmt_ctx);
    json_free();
    return success > 0 ? 0 : 3;
}
