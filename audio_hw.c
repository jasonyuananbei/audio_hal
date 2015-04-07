/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "vf_audio_hw"
/*#define LOG_NDEBUG 0*/

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>

#include <errno.h>
#include <pthread.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>
#include <cutils/sched_policy.h>
#include <utils/ThreadDefs.h>
#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <tinyalsa/asoundlib.h>
#include <semaphore.h>

/*
    build notes:
    v20141230:增加了record_vld标记，用于标示遥控器
    异常断开后，再次连接上，因为没有启动遥控器端录音，  
    而导致in_read函数无法正常退出的问题
*/
#define VF_SOCKET_SERVER_NAME "@/data/misc/bluedroid/.vohog_in"
#define VF_SOCKET_SERVER_CTRL_NAME "@/data/misc/bluedroid/.vohog_ctrl"

#define PCM_LOG (1)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
enum
{
    eRECORD_START = 0,
    eRECORD_STOP,
    eRECORD_OP_MAX
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define FLY_VEN_DEC_IN_SIZE  (40)
#define FLY_VEN_DEC_OUT_SIZE (320)

#define FLY_VEN_NOTIF_LEN   (20)
#define FLY_VEN_FIFO_SIZE   (10)

static FILE * h_pcm_log = NULL;

struct audio_device
{
    struct audio_hw_device hw_device;
    pthread_mutex_t lock;
    bool standby;
};

struct vf_stream_in
{
    struct audio_stream_in stream;
};

typedef struct
{
    unsigned char data[FLY_VEN_DEC_OUT_SIZE * sizeof(short)];
}   t_voice_item;

typedef struct
{
    t_voice_item notifi_fifo[20];
    unsigned int head;
    unsigned int tail;
    unsigned int vld;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
}   t_voice_fifo_cb;

typedef struct
{
    unsigned char opcode;
    unsigned char param[8];
} t_vf_sock_param __attribute__ ((packed));

static t_voice_fifo_cb voice_fifo_blk;
static pthread_t vf_sock_client_service;
static pthread_t vf_sock_client_ctrl_service;

static unsigned char dec_in_data[FLY_VEN_DEC_IN_SIZE];
static short dec_out_data[FLY_VEN_DEC_OUT_SIZE];

static unsigned int v_item_left = 0;
static unsigned int v_item_cp = 0;
static t_voice_item * p_cur_item;

static int vf_socket_ctrl_fd;
static int vf_socket_ctrl_ready;

static unsigned int log_serial_num;

//  ctrl for stream open and close
static int stream_flag;
static sem_t stream_sem;

static int server_life_time;
static int is_connect;
static int record_vld;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void vf_voice_fifo_put(t_voice_fifo_cb * p_cb, unsigned char* p_v_data, int v_size);

///////////////////////////////////////////////////////////////////////////////////////////////////////////
/* set thread proid to ANDROID_PRIORITY_AUDIO */
void vf_set_prio(void)
{
    int rc = 0;
    int tid = gettid();

    return ;

    rc = set_sched_policy(tid, SP_FOREGROUND);
    if (rc)
    {
        ALOGD("failed to change sched policy, tid %d, err: %d", tid, errno);
    }

    if (setpriority(PRIO_PROCESS, tid, ANDROID_PRIORITY_AUDIO) < 0)
    {
        ALOGD("failed to change priority tid: %d to %d", tid, ANDROID_PRIORITY_AUDIO);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
    codec2 funcations
*/
extern int vf_codec2_init(void);

extern void vf_codec2_deinit(void);

extern void vf_codec2_dec(unsigned char * in, short * out);

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
    data chan socket
*/
void * vf_socket_client_looper(void * parg)
{
    int sockfd;
    socklen_t len;
    struct sockaddr_un address;
    int result;
    int nread;

con_init:
    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, VF_SOCKET_SERVER_NAME);
    address.sun_path[0]=0;
    len =  strlen(VF_SOCKET_SERVER_NAME)  + offsetof(struct sockaddr_un, sun_path);

con_retry:
    result = connect(sockfd, (struct sockaddr*)&address, len);
    if(result == -1)
    {
        ALOGW("data chan socket connect errno=[%d], err=[%s]", errno, strerror(errno));
        sleep(2);
        goto con_retry;
    }
    else
    {
        ALOGW("data chan socket connect ok");
    }

    server_life_time = 0;

    while(1)
    {
        nread = read(sockfd, dec_in_data, FLY_VEN_DEC_IN_SIZE);
        if(nread == FLY_VEN_DEC_IN_SIZE)
        {
            //ALOGE("test-dec");
            vf_codec2_dec(dec_in_data, dec_out_data);
            vf_voice_fifo_put(&voice_fifo_blk, (unsigned char*)dec_out_data, FLY_VEN_DEC_OUT_SIZE * sizeof(short));
        }
        else
        {
            if(server_life_time == 1)
            {
                ALOGW("read data chan err1:%d maybe closed exit thread", nread);
                break;
            }
            else
            {
                ALOGW("read data chan err2:%d maybe closed exit thread", nread);
                break;
            }
        }
    }

    close(sockfd);

    ALOGW("data chan socket closed");

    sleep(3);

    ALOGW("data chan socket re-connect");

    goto con_init;

    return NULL;
}

void vf_socket_client_init(void)
{
    //  to do
}

void vf_socket_client_deinit(void)
{
    //  to do
}

void vf_socket_client_start(void)
{
    pthread_create(&vf_sock_client_service, NULL, vf_socket_client_looper, NULL);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
    ctrl chan socket
*/
void * vf_socket_client_ctrl_looper(void * parg)
{
    int nread;
    socklen_t len;
    struct sockaddr_un address;
    int result;
    t_vf_sock_param server_msg;
    int try_cnt = 100;

con_init:
    vf_socket_ctrl_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, VF_SOCKET_SERVER_CTRL_NAME);
    address.sun_path[0]=0;
    len =  strlen(VF_SOCKET_SERVER_CTRL_NAME)  + offsetof(struct sockaddr_un, sun_path);

con_retry:
    result = connect(vf_socket_ctrl_fd, (struct sockaddr*)&address, len);
    if(result == -1)
    {
        ALOGW("ctrl chan socket connect errno=[%d], err=[%s]", errno, strerror(errno));
        sleep(2);
        goto con_retry;
    }
    else
    {
        ALOGW("ctrl chan socket connect ok");
    }

    vf_socket_ctrl_ready = 1;
    try_cnt = 100;

    while(1)
    {
        nread = read(vf_socket_ctrl_fd, &server_msg, sizeof(t_vf_sock_param));
        if(nread == sizeof(t_vf_sock_param))
        {
            if(server_msg.opcode == 0xff)
            {
                ALOGW("ctlr chan recv quit msg and quit from thread");
                server_life_time = 1;
                break;
            }
            else if(server_msg.opcode == 0xfe)
            {
                switch(server_msg.param[0])
                {
                    case 0:
                        ALOGW("ctrl chan recv rcu disconnected");
                        // 遥控器与主机断开连接时，语音会被重置
                        record_vld = eRECORD_STOP;
                        is_connect = 0;
                        break;
                    case 1:
                        ALOGW("ctrl chan recv rcu connected");
                        is_connect = 1;
                        break;
                    default:
                        break;
                }
            }
        }
        else
        {
            ALOGW("ctrl chan read err %d %d", nread, ((unsigned char *)&server_msg)[0]);
            try_cnt --;
            if(try_cnt == 0)
            {
                server_life_time = 1;
                break;
            }
        }
    }

    is_connect = 0;
    close(vf_socket_ctrl_fd);

    ALOGW("ctrl chan socket closed");

    sleep(3);

    ALOGW("ctrl chan socket re-connect");

    goto con_init;

    return NULL;
}

void vf_socket_client_ctrl_send(unsigned char * p_data, unsigned int len)
{
    if(vf_socket_ctrl_ready)
    {
        write(vf_socket_ctrl_fd, p_data, len);
    }
}

void vf_socket_client_ctrl_init(void)
{
    vf_socket_ctrl_ready = 0;
}

void vf_socket_client_ctrl_deinit(void)
{
    //  to do
}

void vf_socket_client_ctrl_start(void)
{
    pthread_create(&vf_sock_client_ctrl_service, NULL, vf_socket_client_ctrl_looper, NULL);
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/* voice queue */
static void vf_voice_fifo_init(t_voice_fifo_cb * p_cb)
{
    p_cb->head = 0;
    p_cb->tail = 0;
    p_cb->vld = 0;

    pthread_mutex_init(&p_cb->mutex, NULL);
    pthread_cond_init(&p_cb->cond, NULL);
}

static void vf_voice_fifo_deinit(t_voice_fifo_cb * p_cb)
{
    pthread_cond_destroy(&p_cb->cond);
    pthread_mutex_destroy(&p_cb->mutex);
}

static void vf_voice_fifo_reset(t_voice_fifo_cb * p_cb)
{
    pthread_mutex_lock(&p_cb->mutex);
    p_cb->head = 0;
    p_cb->tail = 0;
    p_cb->vld = 0;
    pthread_mutex_unlock(&p_cb->mutex);
}

static void vf_voice_fifo_put(t_voice_fifo_cb * p_cb, unsigned char* p_v_data, int v_size)
{
    t_voice_item * p_data;

    pthread_mutex_lock(&p_cb->mutex);
    if(p_cb->vld >= FLY_VEN_FIFO_SIZE)
    {
        pthread_mutex_unlock(&p_cb->mutex);
        // just for debug
        ALOGE("====================vfmodule:full");
        //  cp data
        p_data = p_cb->notifi_fifo + p_cb->tail;
        memcpy(p_data->data, p_v_data, FLY_VEN_DEC_OUT_SIZE * sizeof(short));
        pthread_mutex_lock(&p_cb->mutex);
        p_cb->tail ++;
        if(p_cb->tail >= FLY_VEN_FIFO_SIZE)
            p_cb->tail = 0;
        p_cb->head ++;
        if(p_cb->head >= FLY_VEN_FIFO_SIZE)
            p_cb->head = 0;
        pthread_mutex_unlock(&p_cb->mutex);
        pthread_cond_signal(&p_cb->cond);
    }
    else
    {
        pthread_mutex_unlock(&p_cb->mutex);
        //  cp data
        p_data = p_cb->notifi_fifo + p_cb->tail;
        memcpy(p_data->data, p_v_data, FLY_VEN_DEC_OUT_SIZE * sizeof(short));
        pthread_mutex_lock(&p_cb->mutex);
        p_cb->vld ++;
        p_cb->tail ++;
        if(p_cb->tail >= FLY_VEN_FIFO_SIZE)
            p_cb->tail = 0;
        pthread_mutex_unlock(&p_cb->mutex);
        pthread_cond_signal(&p_cb->cond);
    }
}

static void vf_voice_fifo_get(t_voice_fifo_cb * p_cb, t_voice_item ** p_p_data)
{
    pthread_mutex_lock(&p_cb->mutex);
    do
    {
        if(p_cb->vld == 0)
        {
#if 1
            struct timeval now;
            struct timespec timeout;
            int retcode;

            gettimeofday(&now, NULL);
            if(now.tv_usec + 20000 >= 1000000)
            {
                timeout.tv_sec = now.tv_sec + 1;
                timeout.tv_nsec = (now.tv_usec + 20000 - 1000000) * 1000;
            }
            else
            {
                timeout.tv_sec = now.tv_sec;
                timeout.tv_nsec = (now.tv_usec + 20000) * 1000;
            }
            retcode = pthread_cond_timedwait(&p_cb->cond, &p_cb->mutex, &timeout);
            if (retcode == ETIMEDOUT)
            {
                pthread_mutex_unlock(&p_cb->mutex);
                *p_p_data = NULL;
                break;
            }
#else
            pthread_cond_wait(&p_cb->cond, &p_cb->mutex);
#endif
        }
        else
        {
            pthread_mutex_unlock(&p_cb->mutex);
            *p_p_data = p_cb->notifi_fifo + p_cb->head;
            break;
        }
    }
    while(1);
}

static void vf_voice_fifo_end(t_voice_fifo_cb * p_cb)
{
    pthread_mutex_lock(&p_cb->mutex);
    p_cb->vld --;
    p_cb->head ++;
    if(p_cb->head >= FLY_VEN_FIFO_SIZE)
        p_cb->head = 0;
    pthread_mutex_unlock(&p_cb->mutex);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void stream_single_ctrl_init(void)
{
    sem_init(&stream_sem, 0, 1);
    stream_flag = 0;
}

static void stream_single_ctrl_deinit(void)
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/* API functions */

static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    return 16000;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    return 320;
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_IN_MONO;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return 0;
}

static int in_standby(struct audio_stream *stream)
{
    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    return 0;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes_p)
{
    unsigned char * p_buf = (unsigned char *)buffer;
    size_t bytes = bytes_p;
    if(is_connect == 0 || record_vld == eRECORD_STOP)
    {
        memset(buffer, 0x00, bytes_p);
        return bytes_p;
//        return -1;
    }

    //ALOGD("in_read:%d", bytes);

    while(bytes != 0)
    {
        if(v_item_left == 0)
        {
            //  get a new item from fifo
            vf_voice_fifo_get(&voice_fifo_blk, &p_cur_item);
            if(p_cur_item == NULL)
            {
                //ALOGD("occur");
#if 0
                memset(buffer, 0x00, bytes_p);
                return bytes_p;
#else
                return 0;
#endif
            }
            v_item_left = FLY_VEN_DEC_OUT_SIZE * sizeof(short);
            v_item_cp = 0;
        }
        else
        {
            if(bytes >= v_item_left)
            {
                memcpy(p_buf, p_cur_item->data + v_item_cp, v_item_left);
                p_buf += v_item_left;
                bytes -= v_item_left;
                v_item_cp += v_item_left;
                v_item_left = 0;
                vf_voice_fifo_end(&voice_fifo_blk);
            }
            else
            {
                memcpy(p_buf, p_cur_item->data + v_item_cp, bytes);
                p_buf += bytes;
                v_item_left -= bytes;
                v_item_cp += bytes;
                bytes = 0;
            }
        }
    }

#if (PCM_LOG == 1)
    if(h_pcm_log)
    {
        fwrite(buffer, sizeof(unsigned char), bytes_p, h_pcm_log);
    }
#endif

    //ALOGD("test-in_read:%d", bytes_p);

    return bytes_p;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    return 0;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    return 0;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    return -ENOSYS;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    return -ENOSYS;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
        const struct audio_config *config)
{
    return 0;
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    struct stub_audio_device *ladev = (struct stub_audio_device *)dev;
    struct vf_stream_in *in;
    int ret;
    t_vf_sock_param sock_param;

    sem_wait(&stream_sem);
    if(stream_flag == 1)
    {
        sem_post(&stream_sem);
        return 0;
    }
    else
    {
        stream_flag = 1;
    }
    sem_post(&stream_sem);

    ALOGD("adev_open_input_stream >>>");

    ALOGD("VENDOR_REALTEK v20141230");

#if (PCM_LOG == 1)
    char filename[20];

    sprintf(filename, "/data/vflog/%d.pcm", log_serial_num++);
    ALOGD("logfile name:%s", filename);
    if(h_pcm_log == NULL)
    {
        h_pcm_log = fopen(filename, "w+b");
        ALOGW("open logfile errno=[%d], err=[%s] handle=[%x]", errno, strerror(errno), h_pcm_log);
        if(h_pcm_log == NULL)
        {
            ALOGE("open vfmodule.pcm fail");
        }
        else
        {
            ALOGE("open vfmodule.pcm success");
        }
    }
#endif

    //vf_voice_fifo_reset(&voice_fifo_blk);

    sock_param.opcode = 0x01;
    vf_socket_client_ctrl_send(&sock_param, sizeof(t_vf_sock_param));

    record_vld = eRECORD_START;

    in = (struct vf_stream_in *)calloc(1, sizeof(struct vf_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    *stream_in = &in->stream;

    ALOGD("adev_open_input_stream <<<");

    return 0;

err_open:
    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                    struct audio_stream_in *stream)
{
    t_vf_sock_param sock_param;

    sem_wait(&stream_sem);
    if(stream_flag == 0)
    {
        sem_post(&stream_sem);
        return ;
    }
    else
    {
        stream_flag = 0;
    }
    sem_post(&stream_sem);

    ALOGD("adev_close_input_stream >>>");

#if (PCM_LOG == 1)
    if(h_pcm_log)
    {
        fclose(h_pcm_log);
        h_pcm_log = NULL;
    }
#endif

    sock_param.opcode = 0x02;
    vf_socket_client_ctrl_send(&sock_param, sizeof(t_vf_sock_param));

    record_vld = eRECORD_STOP;

    ALOGD("adev_close_input_stream <<<");
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct audio_device *adev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    *device = &adev->hw_device.common;

    ALOGW("vf_codec2_init:%d", vf_codec2_init());
    vf_voice_fifo_init(&voice_fifo_blk);
    vf_socket_client_init();
    vf_socket_client_start();
    vf_socket_client_ctrl_init();
    vf_socket_client_ctrl_start();
    stream_single_ctrl_init();
    log_serial_num = 0;
    server_life_time = 0;
    is_connect = 0;
    record_vld = eRECORD_STOP;

    return 0;
}

static struct hw_module_methods_t hal_module_methods =
{
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM =
{
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "vf audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
