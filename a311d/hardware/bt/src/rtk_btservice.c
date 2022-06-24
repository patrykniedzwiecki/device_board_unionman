/******************************************************************************
 *
 *  Copyright (C) 2009-2018 Realtek Corporation.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  Filename:      rtk_btservice.c
 *
 *  Description:   start unix socketc
 *
 ******************************************************************************/

#define LOG_TAG "bt_service"
#define RTKBT_RELEASE_NAME "20190717_BT_ANDROID_9.0"

#include <utils/Log.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <termios.h>
#include <sys/syscall.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <semaphore.h>
#include <endian.h>
#include <byteswap.h>
#include <sys/un.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "bt_hci_bdroid.h"
#include "bt_vendor_rtk.h"
#include "userial.h"
#include "userial_vendor.h"
#include "upio.h"
#include "rtk_parse.h"
#include "rtk_btservice.h"

#include "bt_vendor_lib.h"

// HCI VENDOR Command opcode
#define HCI_VSC_READ_REGISTER 0xFFFF

#define RTKBTSERVICE_SOCKETPATH "@/data/misc/bluedroid/rtkbt_service.sock"
#define MAX_CONNECTION_NUMBER 10

#define RTK_HCICMD 0x01
#define RTK_CLOSESOCRET 0x02
#define RTK_INNER 0x03
#define OTHER 0xff

#define Rtk_Service_Data_SIZE 259
#define Rtk_Service_Send_Data_SIZE 259

#define HCICMD_REPLY_TIMEOUT_VALUE 8000 // ms

typedef void (*tTIMER_HANDLE_CBACK)(union sigval sigval_value);

typedef struct Rtk_Btservice_Info {
    int socketfd;
    int sig_fd[2];
    pthread_t cmdreadythd;
    pthread_t epollthd;
    int current_client_sock;
    int epoll_fd;
    int autopair_fd;
    sem_t cmdqueue_sem;
    sem_t cmdsend_sem;
    timer_t timer_hcicmd_reply;
    RT_LIST_HEAD cmdqueue_list;
    pthread_mutex_t cmdqueue_mutex;
    volatile uint8_t cmdqueue_thread_running;
    volatile uint8_t epoll_thread_running;
    void (*current_complete_cback)(HC_BT_HDR *);
} Rtk_Btservice_Info;

typedef struct Rtk_Queue_Data {
    RT_LIST_ENTRY list;
    int client_sock;
    uint16_t opcode;
    uint8_t parameter_len;
    uint8_t *parameter;
    void (*complete_cback)(void *);
} Rtkqueuedata;
typedef void (*tINT_CMD_CBACK)(void *p_mem);
static Rtk_Btservice_Info *rtk_btservice = NULL;
static void Rtk_Service_Send_Hwerror_Event(void);
static timer_t OsAllocateTimer(tTIMER_HANDLE_CBACK timer_callback)
{
    struct sigevent sigev;
    timer_t timerid;

    (void)memset_s(&sigev, sizeof(struct sigevent), 0, sizeof(struct sigevent));
    // Create the POSIX timer to generate signo
    sigev.sigev_notify = SIGEV_THREAD;
    sigev.sigev_notify_function = timer_callback;
    sigev.sigev_value.sival_ptr = &timerid;

    HILOGD("OsAllocateTimer bt_service sigev.sigev_notify_thread_id = syscall(__NR_gettid)!");
    // Create the Timer using timer_create signal

    if (timer_create(CLOCK_REALTIME, &sigev, &timerid) == 0) {
        return timerid;
    } else {
        HILOGE("timer_create error!");
        return (timer_t)-1;
    }
}

static int OsFreeTimer(timer_t timerid)
{
    int ret = 0;
    ret = timer_delete(timerid);
    if (ret != 0) {
        HILOGE("timer_delete fail with errno(%d)", errno);
    }

    return ret;
}

static int OsStartTimer(timer_t timerid, int msec, int mode)
{
    struct itimerspec itval;

#define TIMER_MEC_1000 1000
    itval.it_value.tv_sec = msec / TIMER_MEC_1000;
    itval.it_value.tv_nsec = (long)(msec % TIMER_MEC_1000) * (1000000L);

    if (mode == 1) {
        itval.it_interval.tv_sec = itval.it_value.tv_sec;
        itval.it_interval.tv_nsec = itval.it_value.tv_nsec;
    } else {
        itval.it_interval.tv_sec = 0;
        itval.it_interval.tv_nsec = 0;
    }

    // Set the Timer when to expire through timer_settime

    if (timer_settime(timerid, 0, &itval, NULL) != 0) {
        HILOGE("time_settime error!");
        return -1;
    }

    return 0;
}

static int OsStopTimer(timer_t timerid)
{
    return OsStartTimer(timerid, 0, 0);
}

static void init_cmdqueue_hash(Rtk_Btservice_Info *rtk_info)
{
    RT_LIST_HEAD *head = &rtk_info->cmdqueue_list;
    ListInitializeHeader(head);
}

static void delete_cmdqueue_from_hash(Rtkqueuedata *desc)
{
    Rtkqueuedata *parameter = desc;
    if (parameter) {
        ListDeleteNode(&parameter->list);
        free(parameter);
        parameter = NULL;
    }
}

static void flush_cmdqueue_hash(Rtk_Btservice_Info *rtk_info)
{
    RT_LIST_HEAD *head = &rtk_info->cmdqueue_list;
    RT_LIST_ENTRY *iter = NULL, *temp = NULL;
    Rtkqueuedata *desc = NULL;

    pthread_mutex_lock(&rtk_info->cmdqueue_mutex);
    LIST_FOR_EACH_SAFELY(iter, temp, head)
    {
        desc = LIST_ENTRY(iter, Rtkqueuedata, list);
        delete_cmdqueue_from_hash(desc);
    }
    pthread_mutex_unlock(&rtk_info->cmdqueue_mutex);
}

static void hcicmd_reply_timeout_handler(void) // (union sigval sigev_value)
{
    Rtk_Service_Send_Hwerror_Event();
}

static int hcicmd_alloc_reply_timer(void)
{
    // Create and set the timer when to expire
    rtk_btservice->timer_hcicmd_reply = OsAllocateTimer(hcicmd_reply_timeout_handler);

    return 0;
}

static int hcicmd_free_reply_timer(void)
{
    return OsFreeTimer(rtk_btservice->timer_hcicmd_reply);
}

static int hcicmd_start_reply_timer(void)
{
    return OsStartTimer(rtk_btservice->timer_hcicmd_reply, HCICMD_REPLY_TIMEOUT_VALUE, 1);
}

static int hcicmd_stop_reply_timer(void)
{
    return OsStopTimer(rtk_btservice->timer_hcicmd_reply);
}

static void Rtk_Client_Cmd_Cback(HC_BT_HDR *p_mem)
{
    HC_BT_HDR *p_evt_buf = (HC_BT_HDR *)p_mem;
    unsigned char *sendbuf = NULL;
    int len;

#define LENTH_8 8
    if (p_evt_buf != NULL) {
        len = LENTH_8 + p_evt_buf->len;
        sendbuf = p_mem;
        if (rtk_btservice->current_client_sock != -1) {
            write(rtk_btservice->current_client_sock, sendbuf, len);
        } else {
            HILOGE("%s current_client_sock is not exist!", __func__);
        }
    }
}

void Rtk_Service_Vendorcmd_Hook(Rtk_Service_Data *RtkData, int client_sock)
{
    Rtkqueuedata *rtkqueue_data = NULL;
    if (!rtk_btservice) {
        HILOGE("rtkbt service is null");
        return;
    }

    pthread_mutex_lock(&rtk_btservice->cmdqueue_mutex);
    rtkqueue_data = (Rtkqueuedata *)malloc(sizeof(Rtkqueuedata));
    if (rtkqueue_data == NULL) {
        HILOGE("rtkqueue_data: allocate error");
        if (RtkData->parameter_len > 0) {
            free(RtkData->parameter);
        }
        return;
    }

    rtkqueue_data->opcode = RtkData->opcode;
    rtkqueue_data->parameter = RtkData->parameter;
    rtkqueue_data->parameter_len = RtkData->parameter_len;
    rtkqueue_data->client_sock = client_sock;
    rtkqueue_data->complete_cback = RtkData->complete_cback;

    ListAddToTail(&(rtkqueue_data->list), &(rtk_btservice->cmdqueue_list));
    sem_post(&rtk_btservice->cmdqueue_sem);
    pthread_mutex_unlock(&rtk_btservice->cmdqueue_mutex);
}

static void Rtk_Service_Cmd_Event_Cback(HC_BT_HDR *p_mem)
{
    if (p_mem != NULL) {
        if (rtk_btservice->current_complete_cback != NULL) {
            (*rtk_btservice->current_complete_cback)(p_mem);
        } else {
            HILOGE("%s current_complete_cback is not exist!", __func__);
        }
        rtk_btservice->current_complete_cback = NULL;
        hcicmd_stop_reply_timer();
        sem_post(&rtk_btservice->cmdsend_sem);
    }
}

static void Rtk_Service_Send_Hwerror_Event(void)
{
#define EVENT_P_BUF_2 2
#define EVENT_P_BUF_3 3
    unsigned char p_buf[4];
    int length = 4;
    p_buf[0] = 0x04;             // event
    p_buf[1] = 0x10;             // hardware error
    p_buf[EVENT_P_BUF_2] = 0x01; // len
    p_buf[EVENT_P_BUF_3] = 0xfd; // rtkbtservice error code
    userial_recv_rawdata_hook(p_buf, length);
}

static void *cmdready_thread(void)
{
    while (rtk_btservice->cmdqueue_thread_running) {
        sem_wait(&rtk_btservice->cmdqueue_sem);
        sem_wait(&rtk_btservice->cmdsend_sem);

        if (rtk_btservice->cmdqueue_thread_running != 0) {
            pthread_mutex_lock(&rtk_btservice->cmdqueue_mutex);
            RT_LIST_ENTRY *iter = ListGetTop(&(rtk_btservice->cmdqueue_list));
            Rtkqueuedata *desc = NULL;
            if (iter) {
                desc = LIST_ENTRY(iter, Rtkqueuedata, list);
                if (desc) {
                    ListDeleteNode(&desc->list);
                }
            }

            pthread_mutex_unlock(&rtk_btservice->cmdqueue_mutex);

            if (desc) {
                if (desc->opcode == 0xfc77) {
                    rtk_btservice->autopair_fd = desc->client_sock;
                }

                if (desc->opcode != 0xfc94) {
                    HILOGD("%s, transmit_command Opcode:%x", __func__, desc->opcode);
                }
                rtk_btservice->current_client_sock = desc->client_sock;
                rtk_btservice->current_complete_cback = desc->complete_cback;
                rtk_vendor_cmd_to_fw(desc->opcode, desc->parameter_len, desc->parameter, Rtk_Service_Cmd_Event_Cback);
                hcicmd_start_reply_timer();
                if (desc->parameter_len > 0) {
                    free(desc->parameter);
                }
            }
            free(desc);
        }
    }
    pthread_exit(0);
}

int Getpacket_RTK_HCICMD(int client_sock_temp, int recvlen_temp)
{
    unsigned char opcodeh = 0;
    unsigned char opcodel = 0;
    unsigned char parameter_length = 0;
    unsigned char *parameter = NULL;
    int client_sock = 0;
    int recvlen = 0;

    Rtk_Service_Data *p_buf;

    recvlen = read(client_sock, &opcodeh, 1);
    if (recvlen <= 0) {
        HILOGE("read opcode high char error");
        return -1;
    }
    recvlen = read(client_sock, &opcodel, 1);
    if (recvlen <= 0) {
        HILOGE("read opcode low char error");
        return -1;
    }
    recvlen = read(client_sock, &parameter_length, 1);
    if (recvlen <= 0) {
        HILOGE("read parameter_length char error");
        return -1;
    }

    if (parameter_length > 0) {
        parameter = (unsigned char *)malloc(sizeof(char) * parameter_length);
        if (!parameter) {
        HILOGE("%s parameter alloc fail!", __func__);
        return 1;
        }
        recvlen = read(client_sock, parameter, parameter_length);
        HILOGD("%s parameter_length=%d,recvlen=%d", __func__, parameter_length, recvlen);
        if (recvlen <= 0 || recvlen != parameter_length) {
            HILOGE("read parameter_length char error recvlen=%d,parameter_length=%d\n", recvlen,
                parameter_length);
            free(parameter);
            return -1;
        }
    }
    p_buf = (Rtk_Service_Data *)malloc(sizeof(Rtk_Service_Data));
    if (p_buf == NULL) {
        HILOGE("p_buf: allocate error");
        if (parameter) {
            free(parameter);
        }
        return 1;
    }

    p_buf->opcode = (((unsigned short)opcodeh) << 8L) | opcodel;
    p_buf->parameter = parameter;
    p_buf->parameter_len = parameter_length;
    p_buf->complete_cback = Rtk_Client_Cmd_Cback;
    Rtk_Service_Vendorcmd_Hook(p_buf, client_sock);
    free(p_buf);
    return -1;
}

static void Getpacket(int client_sock)
{
    unsigned char type = 0;
    int recvlen = 0;
    int ret = 0;

    recvlen = read(client_sock, &type, 1);
    HILOGD("%s recvlen=%d,type=%d", __func__, recvlen, type);
    if (recvlen <= 0) {
        close(client_sock);
        if (client_sock == rtk_btservice->autopair_fd) {
            rtk_btservice->autopair_fd = -1;
        }
        return;
    }

    switch (type) {
        case RTK_HCICMD: {
            ret = Getpacket_RTK_HCICMD(client_sock, recvlen);
            if (ret == 1) {
                return;
            }
            if (ret == -1) {
                break;
            }
        }
        case RTK_CLOSESOCRET: {
            close(client_sock);
            break;
        }
        default: {
            HILOGE("%s The RtkSockData type is not found!", __func__);
            break;
        }
    }
}

void rtk_btservice_internal_event_intercept(uint8_t *p_full_msg, uint8_t *p_msg)
{
    uint8_t *p = p_msg;
    uint8_t event_code = *p++;
    uint8_t subcode;
    HC_BT_HDR *p_evt_buf = (HC_BT_HDR *)p_full_msg;
    if (event_code == 0xff) {
        HILOGD("rtk_btservice_internal_event_intercept event_code=0x%x", event_code);
    }
    switch (event_code) {
        case HCI_VENDOR_SPECIFIC_EVT: {
            STREAM_TO_UINT8(subcode, p);
            switch (subcode) {
                case HCI_RTKBT_AUTOPAIR_EVT: {
                    HILOGD("p_evt_buf_len=%d", p_evt_buf->len);
                    if (rtk_btservice->autopair_fd != -1) {
#define P_EVT_BUF_LEN_8 8
#define P_BLUEDROID_3 3
                        write(rtk_btservice->autopair_fd, p_evt_buf, p_evt_buf->len + P_EVT_BUF_LEN_8);
                        uint8_t p_bluedroid_len = p_evt_buf->len + 1;
                        uint8_t p_bluedroid[p_bluedroid_len];
                        p_bluedroid[0] = DATA_TYPE_EVENT;
                        (void)memcpy_s((uint8_t *)(p_bluedroid + 1), p_evt_buf->len, p_msg, p_evt_buf->len);
                        p_bluedroid[1] = 0x3e;             // event_code
                        p_bluedroid[P_BLUEDROID_3] = 0x02; // subcode
                        userial_recv_rawdata_hook(p_bluedroid, p_bluedroid_len);
                    }
                }
                    break;
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }
}

static int socket_accept(socketfd)
{
    struct sockaddr_un un;
    socklen_t len;
    int client_sock = 0;
    len = sizeof(un);
    struct epoll_event event;

    client_sock = accept(socketfd, (struct sockaddr *)&un, &len);
    if (client_sock < 0) {
        printf("accept failed\n");
        return -1;
    }

    event.data.fd = client_sock;
    event.events = EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR;
    if (epoll_ctl(rtk_btservice->epoll_fd, EPOLL_CTL_ADD, client_sock, &event) == -1) {
        HILOGE("%s unable to register fd %d to epoll set: %s", __func__, client_sock, strerror(errno));
        close(client_sock);
        return -1;
    }
    return 0;
}

static void *epoll_thread(void)
{
    struct epoll_event events[64];
    int nfds = 0;
    int i = 0;

    while (rtk_btservice->epoll_thread_running) {
#define EPOLL_WAIT_32 32
#define EPOLL_WAIT_500 500
        nfds = epoll_wait(rtk_btservice->epoll_fd, events, EPOLL_WAIT_32, EPOLL_WAIT_500);
        if (rtk_btservice->epoll_thread_running != 0) {
            if (nfds > 0) {
                for (i = 0; i < nfds; i++) {
                    if (events[i].data.fd == rtk_btservice->sig_fd[1]) {
                        HILOGE("epoll_thread , receive exit signal");
                        continue;
                    }

                    if (events[i].data.fd == rtk_btservice->socketfd && (events[i].events & EPOLLIN)) {
                        if (socket_accept(events[i].data.fd) < 0) {
                            pthread_exit(0);
                        }
                    } else if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                        HILOGD("%s events[i].data.fd = %d ", __func__, events[i].data.fd);
                        Getpacket(events[i].data.fd);
                    }
                }
            }
        }
    }
    pthread_exit(0);
}

static int unix_socket_start(const char *servername)
{
    int len;
    struct sockaddr_un un;
    struct epoll_event event;

    if ((rtk_btservice->socketfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        HILOGE("%s create AF_UNIX socket fail!", __func__);
        return -1;
    }

    (void)memset_s(&un, sizeof(un), 0, sizeof(un));
    un.sun_family = AF_UNIX;
    (void)strcpy_s(un.sun_path, sizeof(un.sun_path), servername);
    un.sun_path[0] = 0;
    len = offsetof(struct sockaddr_un, sun_path) + strlen(servername);

    if (bind(rtk_btservice->socketfd, (struct sockaddr *)&un, len) < 0) {
        HILOGE("%s bind socket fail!", __func__);
        return -1;
    }

    if (listen(rtk_btservice->socketfd, MAX_CONNECTION_NUMBER) < 0) {
        HILOGE("%s listen socket fail!", __func__);
        return -1;
    }

    event.data.fd = rtk_btservice->socketfd;
    event.events = EPOLLIN;
    if (epoll_ctl(rtk_btservice->epoll_fd, EPOLL_CTL_ADD, rtk_btservice->socketfd, &event) == -1) {
        HILOGE("%s unable to register fd %d to epoll set: %s", __func__, rtk_btservice->socketfd, strerror(errno));
        return -1;
    }

    event.data.fd = rtk_btservice->sig_fd[1];
    event.events = EPOLLIN;
    if (epoll_ctl(rtk_btservice->epoll_fd, EPOLL_CTL_ADD, rtk_btservice->sig_fd[1], &event) == -1) {
        HILOGE("%s unable to register signal fd %d to epoll set: %s", __func__, rtk_btservice->sig_fd[1],
               strerror(errno));
        return -1;
    }
    return 0;
}

void RTK_btservice_send_close_signal(void)
{
    unsigned char close_signal = 1;
    ssize_t ret;
    RTK_NO_INTR(ret = write(rtk_btservice->sig_fd[0], &close_signal, 1));
}

int RTK_btservice_thread_start(void)
{
    rtk_btservice->epoll_thread_running = 1;
    if (pthread_create(&rtk_btservice->epollthd, NULL, epoll_thread, NULL) != 0) {
        HILOGE("pthread_create epoll_thread: %s", strerror(errno));
        return -1;
    }

    rtk_btservice->cmdqueue_thread_running = 1;
    if (pthread_create(&rtk_btservice->cmdreadythd, NULL, cmdready_thread, NULL) != 0) {
        HILOGE("pthread_create cmdready_thread: %s", strerror(errno));
        return -1;
    }

    return 0;
}

void RTK_btservice_thread_stop(void)
{
    rtk_btservice->epoll_thread_running = 0;
    rtk_btservice->cmdqueue_thread_running = 0;
    RTK_btservice_send_close_signal();
    sem_post(&rtk_btservice->cmdqueue_sem);
    sem_post(&rtk_btservice->cmdsend_sem);
    pthread_join(rtk_btservice->cmdreadythd, NULL);
    pthread_join(rtk_btservice->epollthd, NULL);
    close(rtk_btservice->epoll_fd);
    HILOGD("%s end!", __func__);
}

int RTK_btservice_init(void)
{
    int ret;
    rtk_btservice = (Rtk_Btservice_Info *)malloc(sizeof(Rtk_Btservice_Info));
    if (rtk_btservice) {
        (void)memset_s(rtk_btservice, sizeof(Rtk_Btservice_Info), 0, sizeof(Rtk_Btservice_Info));
    } else {
        HILOGE("%s, alloc fail", __func__);
        return -1;
    }

    rtk_btservice->current_client_sock = -1;
    rtk_btservice->current_complete_cback = NULL;
    rtk_btservice->autopair_fd = -1;
    hcicmd_alloc_reply_timer();

    sem_init(&rtk_btservice->cmdqueue_sem, 0, 0);
    sem_init(&rtk_btservice->cmdsend_sem, 0, 1);

    pthread_mutex_init(&rtk_btservice->cmdqueue_mutex, NULL);
    init_cmdqueue_hash(rtk_btservice);
    if (bt_vendor_cbacks == NULL) {
        HILOGE("%s bt_vendor_cbacks is NULL!", __func__);
        return -1;
    }

    if ((ret = socketpair(AF_UNIX, SOCK_STREAM, 0, rtk_btservice->sig_fd)) < 0) {
        HILOGE("%s, errno : %s", __func__, strerror(errno));
        return ret;
    }

#define EPOLL_CREATE_64 64
    rtk_btservice->epoll_fd = epoll_create(EPOLL_CREATE_64);
    if (rtk_btservice->epoll_fd == -1) {
        HILOGE("%s unable to create epoll instance: %s", __func__, strerror(errno));
        return -1;
    }

    if (unix_socket_start(RTKBTSERVICE_SOCKETPATH) < 0) {
        HILOGE("%s unix_socket_start fail!", __func__);
        return -1;
    }

    ret = RTK_btservice_thread_start();
    if (ret < 0) {
        HILOGE("%s RTK_btservice_thread_start fail!", __func__);
        return -1;
    }
    HILOGD("%s init done!", __func__);
    return 0;
}

void RTK_btservice_destroyed(void)
{
    if (!rtk_btservice) {
        return;
    }
    RTK_btservice_thread_stop();
    close(rtk_btservice->socketfd);
    rtk_btservice->socketfd = -1;
    close(rtk_btservice->sig_fd[0]);
    close(rtk_btservice->sig_fd[1]);
    sem_destroy(&rtk_btservice->cmdqueue_sem);
    sem_destroy(&rtk_btservice->cmdsend_sem);
    flush_cmdqueue_hash(rtk_btservice);
    hcicmd_free_reply_timer();
    pthread_mutex_destroy(&rtk_btservice->cmdqueue_mutex);
    rtk_btservice->autopair_fd = -1;
    rtk_btservice->current_client_sock = -1;
    free(rtk_btservice);
    rtk_btservice = NULL;
    HILOGD("%s destroyed done!", __func__);
}
