#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* 显式包含 FFmpeg 7.x 对应的头文件 */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <input_mp4_file> [only_keyframes: 0 or 1]\n", argv[0]);
        return -1;
    }

    const char* input_file = argv[1];
    bool only_keyframes = (argc >= 3 && strcmp(argv[2], "1") == 0);

    /* 统一初始化指针为 NULL，以确保 goto 清理逻辑安全 */
    AVFormatContext* fmt_ctx = NULL;
    AVBSFContext* bsf_ctx = NULL;
    AVPacket* pkt = NULL;
    FILE* out_file = NULL;

    // 1. 打开输入文件并读取媒体头部 Metadata
    if (avformat_open_input(&fmt_ctx, input_file, NULL, NULL) < 0) {
        fprintf(stderr, "[-] Could not open input file: %s\n", input_file);
        return -1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "[-] Failed to retrieve stream information.\n");
        goto end;
    }

    // 2. 查找最佳视频流 (FFmpeg 7.x 规范，最后的解码器指针要求为 const AVCodec**)
    int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream_idx < 0) {
        fprintf(stderr, "[-] No video stream found in the container.\n");
        goto end;
    }

    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    AVCodecParameters* codec_par = video_stream->codecpar;

    // 识别视频编码类型并匹配正确的比特流过滤器 (BSF)
    const char* ext = ".bin";
    const char* filter_name = NULL;
    if (codec_par->codec_id == AV_CODEC_ID_H264) {
        ext = ".h264";
        filter_name = "h264_mp4toannexb";
        printf("[+] Detected H.264 (AVC) Video Stream.\n");
    } else if (codec_par->codec_id == AV_CODEC_ID_HEVC) {
        ext = ".h265";
        filter_name = "hevc_mp4toannexb";
        printf("[+] Detected H.265 (HEVC) Video Stream.\n");
    } else {
        fprintf(stderr, "[-] Unsupported video codec ID: %d\n", codec_par->codec_id);
        goto end;
    }

    // 分配输出文件名缓冲区 (通过动态计算或安全数组避免溢出)
    char out_filename[512];
    snprintf(out_filename, sizeof(out_filename), "%s%s", input_file, ext);
    out_file = fopen(out_filename, "wb");
    if (!out_file) {
        fprintf(stderr, "[-] Could not open output file for writing: %s\n", out_filename);
        goto end;
    }

    // 3. 初始化 FFmpeg 7.x 标准比特流过滤器
    const AVBitStreamFilter* bsf = av_bsf_get_by_name(filter_name);
    if (!bsf) {
        fprintf(stderr, "[-] Bitstream filter not found: %s\n", filter_name);
        goto end;
    }

    if (av_bsf_alloc(bsf, &bsf_ctx) < 0) {
        fprintf(stderr, "[-] Failed to allocate bitstream filter context.\n");
        goto end;
    }

    // 将视频流的参数安全复制到过滤器的输入端
    if (avcodec_parameters_copy(bsf_ctx->par_in, codec_par) < 0) {
        fprintf(stderr, "[-] Failed to copy codec parameters to BSF.\n");
        goto end;
    }

    // FFmpeg 7.x 初始化过滤上下文
    if (av_bsf_init(bsf_ctx) < 0) {
        fprintf(stderr, "[-] Failed to initialize bitstream filter.\n");
        goto end;
    }

    // 4. 分配专用的 AVPacket 容器
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "[-] Failed to allocate AVPacket.\n");
        goto end;
    }

    printf("[+] Demuxing started. Saving AnnexB stream to: %s ...\n", out_filename);

    // 5. 循环读取帧数据
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_stream_idx) {
            
            // 过滤逻辑：如果要求只留关键帧
            if (only_keyframes && !(pkt->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(pkt);
                continue;
            }

            // 将 MP4（AVCC/HVCC）包发送给过滤器进行格式改写
            int ret = av_bsf_send_packet(bsf_ctx, pkt);
            if (ret < 0) {
                fprintf(stderr, "[-] Error submitting packet to BSF: %d\n", ret);
                av_packet_unref(pkt);
                goto end;
            }

            // 排空过滤器：有些包可能会被过滤器拆分或组合，必须通过循环接收
            while (ret >= 0) {
                ret = av_bsf_receive_packet(bsf_ctx, pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break; // 需要更多输入包或者已经到结尾
                } else if (ret < 0) {
                    fprintf(stderr, "[-] Error during inside BSF filter process: %d\n", ret);
                    goto end;
                }

                // 成功获取重新拼接了 SPS/PPS（AnnexB 头）的裸流数据，写入文件
                fwrite(pkt->data, 1, pkt->size, out_file);
                av_packet_unref(pkt); // 及时释放当前取出的包数据引用
            }
        } else {
            // 音频或其他流的数据，直接释放，防止内存泄漏
            av_packet_unref(pkt);
        }
    }

    // 刷新比特流过滤器中可能残留的最后一帧尾部数据（FFmpeg 7.x 推荐操作）
    av_bsf_send_packet(bsf_ctx, NULL);
    while (av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
        fwrite(pkt->data, 1, pkt->size, out_file);
        av_packet_unref(pkt);
    }

    printf("[+] Demuxing completed successfully.\n");

end:
    /* 使用 7.x 安全释放函数，传入 NULL 指针时内部会自动忽略 */
    if (pkt) av_packet_free(&pkt);
    if (bsf_ctx) av_bsf_free(&bsf_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    if (out_file) fclose(out_file);

    return 0;
}
