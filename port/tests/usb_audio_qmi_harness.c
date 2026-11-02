/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <strings.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define SNDRV_CARDS 8
#define SNDRV_PCM_STREAM_CAPTURE 1
#define USB_SPEED_HIGH 3
#define BUS_INTERVAL_HIGHSPEED_AND_ABOVE 125
#define BUS_INTERVAL_FULL_SPEED 1000
#define MAX_BINTERVAL_ISOC_EP 16
#define SND_PCM_CARD_NUM_MASK 0xffff0000
#define SND_PCM_DEV_NUM_MASK 0xff00
#define SND_PCM_STREAM_DIRECTION 0xff
#define USB_QMI_PCM_FORMAT_U32_BE 17
#define uaudio_info(...) ((void)0)
#define uaudio_err(...) ((void)0)
#define pr_debug(...) ((void)0)
#define pr_err(...) ((void)0)
#define atomic_read(p) (*(p))
struct usb_device { unsigned int speed; };
struct audioformat { int iface; };
struct snd_usb_stream;
struct snd_usb_substream { struct usb_device *dev; struct audioformat *cur_audiofmt; struct snd_usb_stream *stream; };
struct stream_list { struct snd_usb_stream *first; };
struct snd_usb_audio { int shutdown; unsigned int pcm_devs; struct stream_list pcm_list; };
struct snd_usb_stream { struct snd_usb_audio *chip; void *pcm; unsigned int pcm_index; struct snd_usb_substream substream[2]; struct snd_usb_stream *next; };
struct intf_info { bool in_use; int intf_num; };
struct uaudio_dev { struct snd_usb_audio *chip; struct intf_info *info; int num_intf; };
static struct uaudio_dev uadev[SNDRV_CARDS];
#define list_for_each_entry(pos,head,member) for(pos=(head)->first;pos;pos=pos->next)
/* ACTUAL_HEADERS */
/* ACTUAL_FUNCTIONS */
int main(void) {
 struct usb_device usb={.speed=USB_SPEED_HIGH}; struct audioformat format={.iface=3};
 struct snd_usb_audio chip={.pcm_devs=1}; struct snd_usb_stream stream={.chip=&chip,.pcm=&chip,.pcm_index=0};
 stream.substream[0]=(struct snd_usb_substream){.dev=&usb,.cur_audiofmt=&format,.stream=&stream};
 chip.pcm_list.first=&stream;uadev[0].chip=&chip;
 assert(!find_substream(SNDRV_CARDS,0,0));assert(!find_substream(65535,0,0));
 assert(!find_substream(0,0,2));assert(!find_substream(0,1,0));
 assert(find_substream(0,0,0)==&stream.substream[0]);
 struct qmi_uaudio_stream_req_msg_v01 req={.enable=1,.usb_token=0,.audio_format=0,.bit_rate=48000,.number_of_ch=2};int index;
 assert(check_valid_request(&req,&index)==0 && index==0);
 req.usb_token=UINT32_C(65535)<<16;assert(check_valid_request(&req,&index)==-EINVAL);req.usb_token=0;
 req.audio_format=18;assert(check_valid_request(&req,&index)==-EINVAL);req.audio_format=0;
 struct intf_info intf={.in_use=true,.intf_num=3};uadev[0].info=&intf;uadev[0].num_intf=1;
 assert(check_valid_request(&req,&index)==-EBUSY);
 req.enable=0;assert(check_valid_request(&req,&index)==0 && index==0);
 format.iface=7;assert(check_valid_request(&req,&index)==-EINVAL);
 req.enable=1;stream.substream[0].cur_audiofmt=NULL;assert(check_valid_request(&req,&index)==-EBUSY);
 chip.shutdown=1;assert(!find_substream(0,0,0));chip.shutdown=0;
 assert(get_data_interval_from_si(&stream.substream[0],125)==0);
 assert(get_data_interval_from_si(&stream.substream[0],250)==1);
 assert(get_data_interval_from_si(&stream.substream[0],375)==-EINVAL);
 assert(get_data_interval_from_si(&stream.substream[0],0)==-EINVAL);
 usb.speed=2;assert(get_data_interval_from_si(&stream.substream[0],1000)==0);
 assert(get_data_interval_from_si(&stream.substream[0],2000)==1);
 assert(get_data_interval_from_si(&stream.substream[0],125)==-EINVAL);
 puts("{\"cases\":20,\"request_bounds_checked\":true,\"service_intervals_checked\":true}");
}
