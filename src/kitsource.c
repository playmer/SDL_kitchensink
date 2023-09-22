#include <stdlib.h>
#include <assert.h>

#include <libavformat/avformat.h>
#include <libavutil/opt.h>

#include "kitchensink/kitsource.h"
#include "kitchensink/kiterror.h"
#include "kitchensink/internal/utils/kitlog.h"

#define AVIO_BUF_SIZE 32768

static int _ScanSource(AVFormatContext *format_ctx) {
    av_opt_set_int(format_ctx, "probesize", INT_MAX, 0);
    av_opt_set_int(format_ctx, "analyzeduration", INT_MAX, 0);
    if(avformat_find_stream_info(format_ctx, NULL) < 0) {
        Kit_SetError("Unable to fetch source information");
        return 1;
    }
    return 0;
}

Kit_Source* Kit_CreateSourceFromUrl(const char *url) {
    assert(url != NULL);

    Kit_Source *src = calloc(1, sizeof(Kit_Source));
    if(src == NULL) {
        Kit_SetError("Unable to allocate source");
        return NULL;
    }

    // Attempt to open source
    if(avformat_open_input((AVFormatContext **)&src->format_ctx, url, NULL, NULL) < 0) {
        Kit_SetError("Unable to open source Url");
        goto EXIT_0;
    }

    // Scan source information (may seek forwards)
    if(_ScanSource(src->format_ctx)) {
        goto EXIT_1;
    }

    return src;

EXIT_1:
    avformat_close_input((AVFormatContext **)&src->format_ctx);
EXIT_0:
    free(src);
    return NULL;
}

Kit_Source* Kit_CreateSourceFromCustom(Kit_ReadCallback read_cb, Kit_SeekCallback seek_cb, void *userdata) {
    assert(read_cb != NULL);

    Kit_Source *src = calloc(1, sizeof(Kit_Source));
    if(src == NULL) {
        Kit_SetError("Unable to allocate source");
        return NULL;
    }

    uint8_t *avio_buf = av_malloc(AVIO_BUF_SIZE);
    if(avio_buf == NULL) {
        Kit_SetError("Unable to allocate avio buffer");
        goto EXIT_0;
    }

    AVFormatContext *format_ctx = avformat_alloc_context();
    if(format_ctx == NULL) {
        Kit_SetError("Unable to allocate format context");
        goto EXIT_1;
    }

    AVIOContext *avio_ctx = avio_alloc_context(
        avio_buf, AVIO_BUF_SIZE, 0, userdata, read_cb, 0, seek_cb);
    if(avio_ctx == NULL) {
        Kit_SetError("Unable to allocate avio context");
        goto EXIT_2;
    }

    // Set the format as AVIO format
    format_ctx->pb = avio_ctx;

    // Attempt to open source
    if(avformat_open_input(&format_ctx, "", NULL, NULL) < 0) {
        Kit_SetError("Unable to open custom source");
        goto EXIT_3;
    }

    // Scan source information (may seek forwards)
    if(_ScanSource(format_ctx)) {
        goto EXIT_4;
    }

    // Set internals
    src->format_ctx = format_ctx;
    src->avio_ctx = avio_ctx;
    return src;

EXIT_4:
    avformat_close_input(&format_ctx);
EXIT_3:
    av_freep(&avio_ctx);
EXIT_2:
    avformat_free_context(format_ctx);
EXIT_1:
    av_freep(&avio_buf);
EXIT_0:
    free(src);
    return NULL;
}

static int _RWReadCallback(void *userdata, uint8_t *buf, int size) {
    int bytes_read = SDL_RWread((SDL_RWops*)userdata, buf, 1, size);
    return bytes_read == 0 ? AVERROR_EOF : bytes_read;
}

static int64_t _RWGetSize(SDL_RWops *rw_ops) {
    int64_t current_pos;
    int64_t max_pos;
    
    // First, see if tell works at all, and fail with -1 if it doesn't.
    current_pos = SDL_RWtell(rw_ops);
    if(current_pos < 0) {
        return -1;
    }

    // Seek to end, get pos (this is the size), then return.
    if(SDL_RWseek(rw_ops, 0, RW_SEEK_END) < 0) {
        return -1; // Seek failed, never mind then
    }
    max_pos = SDL_RWtell(rw_ops);
    SDL_RWseek(rw_ops, current_pos, RW_SEEK_SET);
    return max_pos;
}

static int64_t _RWSeekCallback(void *userdata, int64_t offset, int whence) {
    int rw_whence = 0;
    if(whence & AVSEEK_SIZE)
        return _RWGetSize(userdata);

    if((whence & ~AVSEEK_FORCE) == SEEK_CUR)
        rw_whence = RW_SEEK_CUR;
    else if((whence & ~AVSEEK_FORCE) == SEEK_SET)
        rw_whence = RW_SEEK_SET;
    else if((whence & ~AVSEEK_FORCE) == SEEK_END)
        rw_whence = RW_SEEK_END;

    return SDL_RWseek((SDL_RWops*)userdata, offset, rw_whence);
}


Kit_Source* Kit_CreateSourceFromRW(SDL_RWops *rw_ops) {
    return Kit_CreateSourceFromCustom(_RWReadCallback, _RWSeekCallback, rw_ops);
}

void Kit_CloseSource(Kit_Source *src) {
    assert(src != NULL);
    AVFormatContext *format_ctx = src->format_ctx;
    AVIOContext *avio_ctx = src->avio_ctx;
    avformat_close_input(&format_ctx);
    if(avio_ctx) {
        av_freep(&avio_ctx->buffer);
        av_freep(&avio_ctx);
    }
    free(src);
}

int Kit_GetSourceStreamInfo(const Kit_Source *src, Kit_SourceStreamInfo *info, int index) {
    assert(src != NULL);
    assert(info != NULL);

    const AVFormatContext *format_ctx = (AVFormatContext *)src->format_ctx;
    if(index < 0 || index >= format_ctx->nb_streams) {
        Kit_SetError("Invalid stream index");
        return 1;
    }

    const AVStream *stream = format_ctx->streams[index];
    enum AVMediaType codec_type = stream->codecpar->codec_type;
    switch(codec_type) {
        case AVMEDIA_TYPE_UNKNOWN: info->type = KIT_STREAMTYPE_UNKNOWN; break;
        case AVMEDIA_TYPE_DATA: info->type = KIT_STREAMTYPE_DATA; break;
        case AVMEDIA_TYPE_VIDEO: info->type = KIT_STREAMTYPE_VIDEO; break;
        case AVMEDIA_TYPE_AUDIO: info->type = KIT_STREAMTYPE_AUDIO; break;
        case AVMEDIA_TYPE_SUBTITLE: info->type = KIT_STREAMTYPE_SUBTITLE; break;
        case AVMEDIA_TYPE_ATTACHMENT: info->type = KIT_STREAMTYPE_ATTACHMENT; break;
        default:
            Kit_SetError("Unknown native stream type");
            return 1;
    }

    info->index = index;
    return 0;
}

int Kit_GetBestSourceStream(const Kit_Source *src, const Kit_StreamType type) {
    assert(src != NULL);
    int avmedia_type = 0;
    switch(type) {
        case KIT_STREAMTYPE_VIDEO: avmedia_type = AVMEDIA_TYPE_VIDEO; break;
        case KIT_STREAMTYPE_AUDIO: avmedia_type = AVMEDIA_TYPE_AUDIO; break;
        case KIT_STREAMTYPE_SUBTITLE: avmedia_type = AVMEDIA_TYPE_SUBTITLE; break;
        default: return -1;
    }
    int ret = av_find_best_stream((AVFormatContext *)src->format_ctx, avmedia_type, -1, -1, NULL, 0);
    if(ret == AVERROR_STREAM_NOT_FOUND) {
        return -1;
    }
    if(ret == AVERROR_DECODER_NOT_FOUND) {
        Kit_SetError("Unable to find a decoder for the stream");
        return 1;
    }
    return ret;
}

int Kit_GetSourceStreamCount(const Kit_Source *src) {
    assert(src != NULL);
    return ((AVFormatContext *)src->format_ctx)->nb_streams;
}
