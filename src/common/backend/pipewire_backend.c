//
// Created by adrian on 03.06.21.
//

#include "pipewire_backend.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include "common/logger.h"

struct pipewire_backend_t
{
    struct audio_backend_t  parent;
    size_t                  frame_size;
    struct pw_thread_loop   *loop;
    struct pw_stream        *stream;
    struct pw_buffer        *pw_buf;
    size_t                  read_length;
    size_t                  read_index;
};

static int pipewire_open(audio_backend_handle_t handle, char const* device_name, char const* description, enum audio_direction direction, size_t buffer_size, struct stream_config_t const* config);
static int pipewire_close(audio_backend_handle_t handle);
static int pipewire_write(audio_backend_handle_t handle, char const* data, size_t size);
static int pipewire_read(audio_backend_handle_t handle, char* data, size_t size);

static enum spa_audio_format vban_to_pipewire_format(enum VBanBitResolution bit_resolution)
{
    switch (bit_resolution)
    {
        case VBAN_BITFMT_8_INT:
            return SPA_AUDIO_FORMAT_U8;

        case VBAN_BITFMT_16_INT:
            return SPA_AUDIO_FORMAT_S16;

        case VBAN_BITFMT_24_INT:
            return SPA_AUDIO_FORMAT_S24;

        case VBAN_BITFMT_32_INT:
            return SPA_AUDIO_FORMAT_S32;

        case VBAN_BITFMT_32_FLOAT:
            return SPA_AUDIO_FORMAT_F32;

        case VBAN_BITFMT_64_FLOAT:
            return SPA_AUDIO_FORMAT_F64;

        default:
            return SPA_AUDIO_FORMAT_UNKNOWN;
    }
}

static void on_process(void *userdata)
{
    struct pipewire_backend_t* const pipewire_backend = userdata;
    pw_thread_loop_signal(pipewire_backend->loop, false);
}

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .process = on_process,
};

int pipewire_backend_init(audio_backend_handle_t* handle)
{
    struct pipewire_backend_t* pipewire_backend = 0;

    if (handle == 0)
    {
        logger_log(LOG_FATAL, "%s: null handle pointer", __func__);
        return -EINVAL;
    }

    pipewire_backend = calloc(1, sizeof(struct pipewire_backend_t));
    if (pipewire_backend == 0)
    {
        logger_log(LOG_FATAL, "%s: could not allocate memory", __func__);
        return -ENOMEM;
    }

    pipewire_backend->parent.open               = pipewire_open;
    pipewire_backend->parent.close              = pipewire_close;
    pipewire_backend->parent.write              = pipewire_write;
    pipewire_backend->parent.read               = pipewire_read;

    *handle = (audio_backend_handle_t)pipewire_backend;

    return 0;
}

int pipewire_open(audio_backend_handle_t handle, char const* device_name, char const* description, enum audio_direction direction, size_t buffer_size, struct stream_config_t const* config)
{
    int ret;
    long target_id = PW_ID_ANY;
    char *target_id_end;
    const struct spa_pod *params[2];
    int n_params = 1;
    uint8_t buffer[4096];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct pipewire_backend_t* const pipewire_backend = (struct pipewire_backend_t*)handle;

    if (handle == 0)
    {
        logger_log(LOG_FATAL, "%s: handle pointer is null", __func__);
        return -EINVAL;
    }

    pw_init(NULL, NULL);
    pipewire_backend->frame_size = VBanBitResolutionSize[config->bit_fmt] * config->nb_channels;
    pipewire_backend->pw_buf = NULL;
    pipewire_backend->read_length = 0;
    pipewire_backend->read_index = 0;
    pipewire_backend->loop = pw_thread_loop_new(NULL, NULL);
    pipewire_backend->stream = pw_stream_new_simple(pw_thread_loop_get_loop(pipewire_backend->loop),
                                                    (description[0] == '\0') ? "vban" : description,
                                                    pw_properties_new(
                                                            PW_KEY_MEDIA_TYPE, "Audio",
                                                            PW_KEY_MEDIA_CATEGORY, (direction == AUDIO_OUT) ? "Playback" : "Capture",
                                                            PW_KEY_MEDIA_ROLE, "Remote",
                                                            NULL), &stream_events, pipewire_backend);

    buffer_size -= buffer_size % pipewire_backend->frame_size;

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
                                           &SPA_AUDIO_INFO_RAW_INIT(
                                                   .format = vban_to_pipewire_format(config->bit_fmt),
                                                   .channels = config->nb_channels,
                                                   .rate = config->sample_rate));

    if (direction == AUDIO_OUT)
    {
        params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                                               SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 8, 64),
                                               SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                                               SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(buffer_size * 2, buffer_size * 2, buffer_size * 4),
                                               SPA_PARAM_BUFFERS_stride, SPA_POD_Int(pipewire_backend->frame_size),
                                               SPA_PARAM_BUFFERS_align, SPA_POD_Int(16));
        n_params++;
    }

    if (device_name[0] != '\0')
    {
        target_id = strtol(device_name, &target_id_end, 10);
        if (target_id_end == device_name || *target_id_end != '\0' || errno == ERANGE)
            target_id = PW_ID_ANY;
    }

    ret = pw_stream_connect(pipewire_backend->stream, (direction == AUDIO_OUT) ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
                      target_id, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                      params, n_params);

    if (ret < 0)
    {
        logger_log(LOG_FATAL, "pipewire_open: stream_connect error: %d", ret);
        pw_stream_destroy(pipewire_backend->stream);
        pw_thread_loop_destroy(pipewire_backend->loop);
        return ret;
    }

    pw_thread_loop_start(pipewire_backend->loop);
    return ret;
}

int pipewire_close(audio_backend_handle_t handle)
{
    int ret = 0;
    struct pipewire_backend_t* const pipewire_backend = (struct pipewire_backend_t*)handle;

    if (handle == 0)
    {
        logger_log(LOG_FATAL, "%s: handle pointer is null", __func__);
        return -EINVAL;
    }

    if (pipewire_backend->loop == NULL)
    {
        /** nothing to do */
        return 0;
    }

    pw_stream_destroy(pipewire_backend->stream);
    pw_thread_loop_stop(pipewire_backend->loop);
    pw_thread_loop_destroy(pipewire_backend->loop);
    pipewire_backend->stream = NULL;
    pipewire_backend->loop = NULL;

    return ret;
}

int pipewire_write(audio_backend_handle_t handle, char const* data, size_t size)
{
    int ret = 0;
    size_t bytes_left = size;
    struct pipewire_backend_t* const pipewire_backend = (struct pipewire_backend_t*)handle;

    if ((handle == 0) || (data == 0))
    {
        logger_log(LOG_ERROR, "%s: handle or data pointer is null", __func__);
        return -EINVAL;
    }

    if (pipewire_backend->stream == NULL)
    {
        logger_log(LOG_ERROR, "%s: stream not open", __func__);
        return -ENODEV;
    }

    pw_thread_loop_lock(pipewire_backend->loop);

    while (bytes_left > 0)
    {
        struct spa_buffer *buf;
        void *dst;
        size_t l;

        while (!pipewire_backend->pw_buf)
        {
            if ((pipewire_backend->pw_buf = pw_stream_dequeue_buffer(pipewire_backend->stream)) == NULL)
            {
                pw_thread_loop_wait(pipewire_backend->loop);
            }
            else if (pipewire_backend->pw_buf->buffer->datas[0].maxsize <= 0)
            {
                pw_stream_queue_buffer(pipewire_backend->stream, pipewire_backend->pw_buf);
                pw_thread_loop_wait(pipewire_backend->loop);
            }
        }

        buf = pipewire_backend->pw_buf->buffer;
        if ((dst = buf->datas[0].data) == NULL)
        {
            logger_log(LOG_ERROR, "%s: no data inside stream", __func__);
            pw_stream_queue_buffer(pipewire_backend->stream, pipewire_backend->pw_buf);
            pipewire_backend->pw_buf = NULL;
            pw_thread_loop_unlock(pipewire_backend->loop);
            return -ENODEV;
        }
        l = buf->datas[0].maxsize;

        if (l > bytes_left)
            l = bytes_left;
        memcpy(dst, data, l);

        data += l;
        bytes_left -= l;
        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = pipewire_backend->frame_size;
        buf->datas[0].chunk->size = l;

        pw_stream_queue_buffer(pipewire_backend->stream, pipewire_backend->pw_buf);
        pipewire_backend->pw_buf = NULL;
    }

    pw_thread_loop_unlock(pipewire_backend->loop);
    return (ret < 0) ? ret : size;
}

int pipewire_read(audio_backend_handle_t handle, char* data, size_t size)
{
    int ret = 0;
    size_t bytes_left = size;
    struct pipewire_backend_t* const pipewire_backend = (struct pipewire_backend_t*)handle;

    if ((handle == 0) || (data == 0))
    {
        logger_log(LOG_ERROR, "%s: handle or data pointer is null", __func__);
        return -EINVAL;
    }

    if (pipewire_backend->stream == NULL)
    {
        logger_log(LOG_ERROR, "%s: stream not open", __func__);
        return -ENODEV;
    }

    pw_thread_loop_lock(pipewire_backend->loop);

    while (bytes_left > 0)
    {
        struct spa_buffer *buf;
        void *src;
        size_t l;

        while (!pipewire_backend->pw_buf)
        {
            if ((pipewire_backend->pw_buf = pw_stream_dequeue_buffer(pipewire_backend->stream)) == NULL)
            {
                pw_thread_loop_wait(pipewire_backend->loop);
            }
            else if (pipewire_backend->pw_buf->buffer->datas[0].chunk->size <= 0)
            {
                pw_stream_queue_buffer(pipewire_backend->stream, pipewire_backend->pw_buf);
                pipewire_backend->pw_buf = NULL;
                pw_thread_loop_wait(pipewire_backend->loop);
            }
            else
            {
                pipewire_backend->read_length = pipewire_backend->pw_buf->buffer->datas[0].chunk->size;
                pipewire_backend->read_index = 0;
            }
        }

        buf = pipewire_backend->pw_buf->buffer;
        if ((src = buf->datas[0].data) == NULL)
        {
            logger_log(LOG_ERROR, "%s: no data inside stream", __func__);
            pw_stream_queue_buffer(pipewire_backend->stream, pipewire_backend->pw_buf);
            pipewire_backend->pw_buf = NULL;
            pipewire_backend->read_length = 0;
            pipewire_backend->read_index = 0;
            pw_thread_loop_unlock(pipewire_backend->loop);
            return -ENODEV;
        }

        l = pipewire_backend->read_length < bytes_left ? pipewire_backend->read_length : bytes_left;
        memcpy(data, ((char *)src)+pipewire_backend->read_index, l);

        data += l;
        bytes_left -= l;

        pipewire_backend->read_index += l;
        pipewire_backend->read_length -= l;

        if (!pipewire_backend->read_length)
        {
            pw_stream_queue_buffer(pipewire_backend->stream, pipewire_backend->pw_buf);
            pipewire_backend->pw_buf = NULL;
            pipewire_backend->read_length = 0;
            pipewire_backend->read_index = 0;
        }
    }

    pw_thread_loop_unlock(pipewire_backend->loop);
    return (ret < 0) ? ret : size;
}
