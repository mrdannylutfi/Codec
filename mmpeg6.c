#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

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

    // 初始化所有指针为 NULL，方便后续统一释放
    AVFormatContext* fmt_ctx = NULL;
    AVBSFContext* bsf_ctx = NULL;
    AVPacket* pkt = NULL;
    FILE* out_file = NULL;

    // 1. 打开输入文件并读取容器头部 Metadata
    if (avformat_open_input(&fmt_ctx, input_file, NULL, NULL) < 0) {
        fprintf(stderr, "[-] Could not open input file: %s\n", input_file);
        return -1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "[-] Failed to retrieve stream information.\n");
        goto end;
    }

    // 2. 查找最佳的视频流
    int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream_idx < 0) {
        fprintf(stderr, "[-] No video stream found in the container.\n");
        goto end;
    }

    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    AVCodecParameters* codec_par = video_stream->codecpar;

    // 识别编码器类型并选择对应的比特流过滤器
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
        fprintf(stderr, "[-] Unsupported codec: %d\n", codec_par->codec_id);
        goto end;
    }

    // 生成输出文件名并打开文件 (例如: output.mp4 -> output.mp4.h264)
    char out_filename[1024];
    snprintf(out_filename, sizeof(out_filename), "%s%s", input_file, ext);
    out_file = fopen(out_filename, "wb");
    if (!out_file) {
        fprintf(stderr, "[-] Could not open output file for writing: %s\n", out_filename);
        goto end;
    }

    // 3. 初始化比特流过滤器 (MP4的AVCC/HVCC格式转为可播放的AnnexB格式)
    const AVBitStreamFilter* bsf = av_bsf_get_by_name(filter_name);
    if (!bsf) {
        fprintf(stderr, "[-] Bitstream filter not found: %s\n", filter_name);
        goto end;
    }

    if (av_bsf_alloc(bsf, &bsf_ctx) < 0) {
        fprintf(stderr, "[-] Failed to allocate bitstream filter context.\n");
        goto end;
    }

    // 复制流参数到过滤器
    if (avcodec_parameters_copy(bsf_ctx->par_in, codec_par) < 0) {
        fprintf(stderr, "[-] Failed to copy codec parameters to BSF.\n");
        goto end;
    }

    if (av_bsf_init(bsf_ctx) < 0) {
        fprintf(stderr, "[-] Failed to initialize bitstream filter.\n");
        goto end;
    }

    // 4. 分配 AVPacket
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "[-] Failed to allocate AVPacket.\n");
        goto end;
    }

    printf("[+] Demuxing started. Saving to %s ...\n", out_filename);

    // 5. 读取数据包并进行过滤和写入
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_stream_idx) {
            // 如果开启了 only_keyframes，过滤掉非关键帧
            if (only_keyframes && !(pkt->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(pkt);
                continue;
            }

            // 将数据包装入过滤器
            if (av_bsf_send_packet(bsf_ctx, pkt) < 0) {
                fprintf(stderr, "[-] Error submitting packet to BSF.\n");
                av_packet_unref(pkt);
                goto end;
            }

            // 循环从过滤器中获取处理后的 AnnexB 数据
            while (av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
                fwrite(pkt->data, 1, pkt->size, out_file);
                av_packet_unref(pkt);
            }
        } else {
            // 非视频流的数据包直接释放
            av_packet_unref(pkt);
        }
    }

    printf("[+] Demuxing completed successfully.\n");

end:
    // 统一清理和释放资源 (对应 C++ 的智能指针析构和文件关闭)
    if (pkt) av_packet_free(&pkt);
    if (bsf_ctx) av_bsf_free(&bsf_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    if (out_file) fclose(out_file);

    return 0;
}
