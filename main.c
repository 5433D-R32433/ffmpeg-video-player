#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#define SDL_AUDIO_BUFFER_SIZE (1024)
#define MAX_AUDIO_FRAME_SIZE 192000
#define MAX_AUDIO_QUEUE_SIZE (5 * 16 * 1024)
#define MAX_VIDEO_QUEUE_SIZE (5 * 256 * 1024)
#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 1.0
#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)
#define VIDEO_PICTURE_QUEUE_SIZE 1


#define false 0
#define true  1

typedef unsigned char      U8;
typedef unsigned short     U16;
typedef unsigned int       U32;
typedef unsigned long long U64;

typedef signed char        S8;
typedef signed short       S16;
typedef signed int         S32;
typedef signed long long   S64;

typedef float  F32;
typedef double F64;

typedef U32 bool32;



typedef struct packet_queue_t
{
    AVPacketList *first_packet;
    AVPacketList *last_packet;
    S32           num_packets;
    S32           size;
	SDL_mutex    *mutex;
    SDL_cond     *condition;
	
} packet_queue_t;


typedef struct video_picture_t
{
    AVFrame    *frame;
	S32         width;
	S32         height;
	bool32      allocated;
	F64         pts;
	
} video_picture_t;


typedef struct media_state_t
{
    AVFormatContext    *fmt_ctx;
	
	S32                 audio_stream_index;
    AVStream           *audio_stream;
    AVCodecContext     *audio_codec_ctx;
	packet_queue_t      audio_queue;
	U8                  audio_buffer [ ( MAX_AUDIO_FRAME_SIZE * 3 ) / 2 ];
	U32                 audio_buffer_size;
    U32                 audio_buffer_index;
    AVFrame             audio_frame;
    AVPacket            audio_packet;
	U8                 *audio_packet_data;
    S32                 audio_packet_size;
	
	F64                 audio_clock;
	S32                 audio_hardware_buffer_size;
	
	S32                 video_stream_index;
    AVStream           *video_stream;
    AVCodecContext     *video_codec_ctx;
    SDL_Texture        *texture;
    SDL_Renderer       *renderer;
    packet_queue_t      video_queue;
    struct SwsContext  *sws_ctx;
    
	F64                 frame_timer;
    F64                 frame_last_pts;
    F64                 frame_last_delay;
    F64                 video_clock;
	
	
	video_picture_t     picture_queue [ VIDEO_PICTURE_QUEUE_SIZE ];
	S32                 picture_queue_size;
	S32                 picture_queue_read_index;
	S32                 picture_queue_write_index;
    SDL_mutex          *picture_queue_mutex;
    SDL_cond           *picture_queue_condition;
	
    SDL_Thread *    decode_thread_id;
    SDL_Thread *    video_thread_id;
	
	S8 filename [ 1024 ];
	
    bool32 quit;
	
} media_state_t;



typedef struct audio_resampling_state_t
{
    SwrContext *swr_ctx;
	S64         in_channel_layout;
	U64         out_channel_layout;
	S32         out_num_channels;
	S32         out_linesize;
    S32         in_num_samples;
	S64         out_num_samples;
	S64         max_out_num_samples;
	U8        **resampled_data;
    S32         resampled_data_size;
	
} audio_resampling_state_t;


SDL_Window    *screen             = 0;
SDL_mutex     *screen_mutex       = 0;
media_state_t *global_media_state = 0;



static S64 guess_correct_pts ( AVCodecContext *ctx, 
							  S64 reordered_pts, 
							  S64 dts )
{
	S64 pts = AV_NOPTS_VALUE;
	
    if ( dts != AV_NOPTS_VALUE )
    {
        ctx->pts_correction_num_faulty_dts += dts <= ctx->pts_correction_last_dts;
        ctx->pts_correction_last_dts = dts;
    }
    else if ( reordered_pts != AV_NOPTS_VALUE )
    {
        ctx->pts_correction_last_dts = reordered_pts;
    }
	
    if ( reordered_pts != AV_NOPTS_VALUE )
    {
        ctx->pts_correction_num_faulty_pts += reordered_pts <= ctx->pts_correction_last_pts;
        ctx->pts_correction_last_pts = reordered_pts;
    }
    else if ( dts != AV_NOPTS_VALUE )
    {
        ctx->pts_correction_last_pts = dts;
    }
	
    if ( ( ctx->pts_correction_num_faulty_pts <= ctx->pts_correction_num_faulty_dts || dts == AV_NOPTS_VALUE ) && reordered_pts != AV_NOPTS_VALUE )
    {
        pts = reordered_pts;
    }
    else
    {
        pts = dts;
    }
	
    return pts;
}


F64 synchronize_video( media_state_t* media_state,
					  AVFrame * src_frame,
					  F64 pts )
{
	F64 frame_delay;
	
    if ( pts != 0 )
    {
        media_state->video_clock = pts;
    }
    else
    {
        pts = media_state->video_clock;
    }
	
    frame_delay  = av_q2d ( media_state->video_codec_ctx->time_base );
    frame_delay += src_frame->repeat_pict * ( frame_delay * 0.5 );
    media_state->video_clock += frame_delay;
	
    return pts;
}


int decode_thread ( void *arg )
{
	media_state_t *media_state = ( media_state_t* ) arg;
	AVFormatContext *fmt_ctx   = 0;
	AVPacket packet            = { 0 };
	S32 ret                    = -1;
	
	if ( avformat_open_input ( &fmt_ctx, media_state->filename, 0, 0 ) != 0 )
	{
		fprintf ( stderr, "Couldn't open file: %s\n", media_state->filename );
		return -1;
	}
	
	
	if ( avformat_find_stream_info ( fmt_ctx, 0 ) < 0 )
	{
		fprintf ( stderr, "Couldn't find stream information %s\n", media_state->filename );
		return -1; 
	}
	
	av_dump_format ( fmt_ctx, 0, media_state->filename, 0 );
	
	media_state->video_stream_index = -1;
	media_state->audio_stream_index = -1;
	
	S32 video_stream_index = -1;
	S32 audio_stream_index = -1;
	
	
	global_media_state   = media_state;
	media_state->fmt_ctx = fmt_ctx;
	
	
	for ( int i = 0; i < fmt_ctx->nb_streams; i++ )
	{
		if ( fmt_ctx->streams [ i ]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index < 0 ) 
		{
			video_stream_index = i;
		}
		
		if ( fmt_ctx->streams [ i ]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index < 0 ) 
		{
			audio_stream_index = i;
		}
	}
	
	if ( video_stream_index == -1 )
	{
		fprintf ( stderr, "Could not find a video stream\n" );
		goto failure;
	}
	else
	{
        if ( stream_component_open ( media_state, video_stream_index ) < 0 )
        {
            printf("Could not open video codec\n");
            goto failure;
        }
	}
	
	
	if ( audio_stream_index == -1 )
	{
		fprintf ( stderr, "Couldn't find a audio stream\n" );
		return -1;
	}
	else
	{
        if ( stream_component_open ( media_state, audio_stream_index ) < 0 )
        {
            printf ( "Could not open audio codec\n" );
            goto failure;
        }
	}
	
    if ( media_state->video_stream_index < 0 || media_state->audio_stream_index < 0)
    {
        printf ( "Could not open codecs: %s\n", media_state->filename );
        goto failure;
    }
	
	
    for ( ; ; )
    {
        if ( media_state->quit )
        {
            break;
        }
		
        if ( media_state->audio_queue.size > MAX_AUDIO_QUEUE_SIZE || media_state->video_queue.size > MAX_VIDEO_QUEUE_SIZE )
        {
            SDL_Delay ( 10 );
            continue;
        }
		
        ret =  av_read_frame ( media_state->fmt_ctx, &packet );
		if ( ret < 0 )
        {
			if ( ret == AVERROR_EOF )
            {
                media_state->quit = true;
                break;
            }
			
            if ( media_state->fmt_ctx->pb->error == 0 )
            {
                SDL_Delay ( 10 );
                continue;
            }
            else
            {
                break;
            }
        }
		
        if ( packet.stream_index == media_state->video_stream_index )
        {
            packet_queue_put ( &media_state->video_queue, &packet );
        }
        else if ( packet.stream_index == media_state->audio_stream_index )
        {
            packet_queue_put ( &media_state->audio_queue, &packet );
        }
        else
        {
            av_packet_unref ( &packet );
        }
    }
	
    while ( !media_state->quit )
    {
        SDL_Delay ( 100 );
    }
	
	avformat_close_input ( &fmt_ctx );
	
	
	failure:
    {
        if ( 1 )
        {
            SDL_Event event;
            event.type = FF_QUIT_EVENT;
            event.user.data1 = media_state;
            SDL_PushEvent ( &event );
            return -1;
        }
    }
	
    return 0;
}


int video_thread ( void *arg )
{
    media_state_t *media_state = ( media_state_t* ) arg;
    bool32 frame_finished = false;
	S32 ret               = -1;
	F64 pts               =  0;
	
	AVPacket *packet = av_packet_alloc ( );
    if ( !packet )
    {
        fprintf ( stderr, "Could not alloc packet\n" );
        return -1;
    }
	
	static AVFrame *frame = 0;
    frame = av_frame_alloc ( );
    if ( !frame )
    {
        fprintf ( stderr, "Could not allocate AVFrame\n" );
        return -1;
    }
	
    for ( ; ; )
    {
        if ( packet_queue_get ( &media_state->video_queue, packet, 1 ) < 0 )
        {
            break;
        }
		
		pts = 0;
		
        int ret = avcodec_send_packet ( media_state->video_codec_ctx, packet );
        if ( ret < 0 )
		{
            fprintf ( stderr, "Error sending packet for decoding\n" );
            return -1;
        }
		
        while ( ret >= 0 )
        {
            ret = avcodec_receive_frame ( media_state->video_codec_ctx, frame );
			
            if ( ret == AVERROR ( EAGAIN ) || ret == AVERROR_EOF )
            {
                break;
            }
            else if ( ret < 0 )
            {
                fprintf ( stderr, "Error while decoding\n" );
                return -1;
            }
            else
            {
                frame_finished = true;
            }
			
            pts = guess_correct_pts ( media_state->video_codec_ctx, frame->pts, frame->pkt_dts);
			
            if ( pts == AV_NOPTS_VALUE )
            {
                pts = 0;
            }
			
            pts *= av_q2d ( media_state->video_stream->time_base );
			
            if ( frame_finished )
            {
				pts = synchronize_video ( media_state, frame, pts );
                if ( queue_picture ( media_state, frame, pts ) < 0 )
                {
                    break;
                }
            }
        }
		
        av_packet_unref ( packet );
    }
	
    av_frame_free ( &frame );
    av_free       (  frame );
	
    return 0;
}



F64 get_audio_clock ( media_state_t* media_state )
{
	F64 pts                  = media_state->audio_clock;
    F32 hardware_buffer_size = media_state->audio_buffer_size - media_state->audio_buffer_index;
    F32 bytes_per_second     = 0;
    S32 n                    = 2 * media_state->audio_codec_ctx->channels;
	
    if ( media_state->audio_stream )
    {
        bytes_per_second = media_state->audio_codec_ctx->sample_rate * n;
    }
	
    if ( bytes_per_second )
    {
        pts -= ( F64 ) hardware_buffer_size / bytes_per_second;
    }
	
    return pts;
}


audio_resampling_state_t* get_audio_resampling ( U64 channel_layout )
{
	audio_resampling_state_t* ars =
		av_mallocz ( sizeof ( audio_resampling_state_t ) );
	
    ars->swr_ctx             = swr_alloc();
    ars->in_channel_layout   = channel_layout;
    ars->out_channel_layout  = AV_CH_LAYOUT_STEREO;
    ars->out_num_channels    = 0;
    ars->out_linesize        = 0;
    ars->in_num_samples      = 0;
    ars->out_num_samples     = 0;
    ars->max_out_num_samples = 0;
    ars->resampled_data      = 0;
    ars->resampled_data_size = 0;
	
    return ars;
}

void audio_callback ( void * userdata, U8 *stream, S32 length )
{
	media_state_t *media_state = ( media_state_t* ) userdata;
	
    S32 len        =-1;
    U32 audio_size =-1;
	F64 pts        = 0;
	
    while ( length > 0 )
    {
        if ( media_state->quit )
        {
            return;
        }
		
        if ( media_state->audio_buffer_index >= media_state->audio_buffer_size )
        {
            audio_size = audio_decode_frame ( media_state, 
											 media_state->audio_buffer, sizeof ( media_state->audio_buffer ), &pts );
			
            if ( audio_size < 0 )
            {
                media_state->audio_buffer_size = ( 1024 );
                memset ( media_state->audio_buffer, 0, media_state->audio_buffer_size );
                printf ( "audio_decode_frame() failed!\n"   );
            }
            else
            {
                media_state->audio_buffer_size = audio_size;
            }
			
            media_state->audio_buffer_index = 0;
        }
		
        len = media_state->audio_buffer_size - media_state->audio_buffer_index;
		
        if ( len > length )
        {
            len = length ;
        }
		
        memcpy ( stream, ( U8* ) media_state->audio_buffer + media_state->audio_buffer_index, len );
		
		length -= len;
		stream += len;
		media_state->audio_buffer_index += len;
	}
}



void packet_queue_init ( packet_queue_t *queue )
{
	memset ( queue, 0, sizeof ( packet_queue_t ) );
	queue->mutex = SDL_CreateMutex ( );
	if ( !queue->mutex )
	{
		fprintf ( stderr, "SDL_CreateMutex Error: %s\n", SDL_GetError ( ) );
		return;
	}
	
	queue->condition = SDL_CreateCond ( );
	if ( !queue->condition )
	{
		fprintf ( stderr, "SDL_CreateCond Error: %s\n", SDL_GetError ( ) );
		return;
	}
}

int packet_queue_put ( packet_queue_t *queue, AVPacket *packet )
{
	
	AVPacketList *av_packet_list = av_malloc ( sizeof ( AVPacketList ) );
	assert ( av_packet_list );
	
    av_packet_list->pkt  = *packet;
	
    av_packet_list->next = 0;
	
    SDL_LockMutex ( queue->mutex );
	
    if ( !queue->last_packet )
    {
        queue->first_packet = av_packet_list;
    }
    else
    {
        queue->last_packet->next = av_packet_list;
    }
	
    queue->last_packet = av_packet_list;
	
    queue->num_packets++;
	
    queue->size += av_packet_list->pkt.size;
	
    SDL_CondSignal ( queue->condition );
	
    SDL_UnlockMutex ( queue->mutex );
	
    return 0;
}

static int packet_queue_get ( packet_queue_t *queue, AVPacket *packet, int block )
{
	S32 ret                      = 0;
    AVPacketList *av_packet_list = 0;
    SDL_LockMutex ( queue->mutex );
	
    for ( ;; )
    {
        if ( global_media_state->quit )
        {
            ret = -1;
            break;
        }
		
		av_packet_list = queue->first_packet;
		
        if ( av_packet_list )
        {
            queue->first_packet = av_packet_list->next;
			
            if ( !queue->first_packet )
            {
                queue->last_packet = 0;
            }
			
            queue->num_packets--;
			
            queue->size -= av_packet_list->pkt.size;
			
            *packet = av_packet_list->pkt;
			
            av_free ( av_packet_list );
			
            ret = 1;
            
			break;
        }
        else if ( !block )
        {
            ret = 0;
            break;
        }
        else
        {
            SDL_CondWait ( queue->condition, queue->mutex );
        }
    }
    
	SDL_UnlockMutex ( queue->mutex );
	
    return ret;
}


int stream_component_open ( media_state_t *media_state, S32 stream_index )
{
	
    AVFormatContext *fmt_ctx = media_state->fmt_ctx;
	S32 ret = -1;
	
    if ( stream_index < 0 || stream_index >= fmt_ctx->nb_streams )
    {
        printf ( "Invalid stream index\n" );
        return -1;
    }
	
	
	const AVCodec *codec = avcodec_find_decoder( fmt_ctx->streams [ stream_index ]->codecpar->codec_id );
	if ( !codec )
    {
        printf ( "Unsupported codec\n" );
        return -1;
    }
	
    AVCodecContext *codec_ctx = avcodec_alloc_context3 ( codec );
    if ( avcodec_parameters_to_context ( codec_ctx, fmt_ctx->streams [ stream_index ]->codecpar ) < 0 )
    {
        printf ( "Could not copy codec context\n" );
        return -1;
    }
	
    if ( codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO )
    {
        SDL_AudioSpec wanted_specs = { 0 };
        SDL_AudioSpec specs        = { 0 };
		
        wanted_specs.freq     = codec_ctx->sample_rate;
        wanted_specs.format   = AUDIO_S16SYS;
        wanted_specs.channels = codec_ctx->channels;
        wanted_specs.silence  = 0;
        wanted_specs.samples  = SDL_AUDIO_BUFFER_SIZE;
        wanted_specs.callback = audio_callback;
        wanted_specs.userdata = media_state;
		
        
		if ( SDL_OpenAudio ( &wanted_specs, &specs ) < 0 )
        {
            fprintf ( stderr, "SDL_OpenAudio: %s\n", SDL_GetError ( ) );
            return -1;
        }
    }
	
    if ( avcodec_open2 ( codec_ctx, codec, 0 ) < 0 )
    {
		printf ( "Unsupported codec!\n" );
		return -1;
	}
	
	switch ( codec_ctx->codec_type )
	{
		case AVMEDIA_TYPE_AUDIO:
		{
			media_state->audio_stream_index = stream_index;
			media_state->audio_stream       = fmt_ctx->streams [ stream_index ];
			media_state->audio_codec_ctx    = codec_ctx;
			media_state->audio_buffer_size  = 0;
			media_state->audio_buffer_index = 0;
			
			memset ( &media_state->audio_packet, 0, sizeof ( media_state->audio_packet ) );
			packet_queue_init ( &media_state->audio_queue );
			SDL_PauseAudio ( 0 );
			
		} break;
		
		case AVMEDIA_TYPE_VIDEO:
		{
			media_state->video_stream_index  = stream_index;
			media_state->video_stream        = fmt_ctx->streams [ stream_index ];
			media_state->video_codec_ctx     = codec_ctx;
			
			
            media_state->frame_timer      = ( F64 ) av_gettime ( ) / 1000000.0;
			media_state->frame_last_delay = 40e-3;
			
			packet_queue_init ( &media_state->video_queue );
			
			media_state->video_thread_id = SDL_CreateThread ( video_thread, "Video Thread", media_state );
			
			media_state->sws_ctx = sws_getContext ( media_state->video_codec_ctx->width,
												   media_state->video_codec_ctx->height,
												   media_state->video_codec_ctx->pix_fmt,
												   media_state->video_codec_ctx->width,
												   media_state->video_codec_ctx->height,
												   AV_PIX_FMT_YUV420P,
												   SWS_BILINEAR,
												   0,
												   0,
												   0 );
            screen_mutex = SDL_CreateMutex ( );
			
		} break;
		
        default:
        {
        } break;
    }
	
    return 0;
}


static uint32_t sdl_refresh_timer_callback ( uint32_t interval, void *param )
{
    SDL_Event event  = { 0 };
    event.type       = FF_REFRESH_EVENT;
    event.user.data1 = param;
    SDL_PushEvent ( &event );
	
    return 0;
}


static void schedule_refresh ( media_state_t *media_state, int delay )
{
    S32 ret = SDL_AddTimer ( delay, sdl_refresh_timer_callback, media_state );
	
    if ( ret == 0 )
    {
        printf ( "Could not schedule refresh callback: %s\n", SDL_GetError ( ) );
    }
}

void alloc_picture ( void *userdata )
{
    media_state_t   *media_state   = ( media_state_t* ) userdata;
    video_picture_t *video_picture = &media_state->picture_queue [ media_state->picture_queue_write_index ];
	
	if ( video_picture->frame )
	{
		av_frame_free ( &video_picture->frame );
		av_free       (  video_picture->frame );
	}
	SDL_LockMutex ( screen_mutex );
	
	int num_bytes = av_image_get_buffer_size ( AV_PIX_FMT_YUV420P,
											  media_state->video_codec_ctx->width,
											  media_state->video_codec_ctx->height,
											  32 );
	
	U8 *buffer = ( U8* ) av_malloc ( num_bytes * sizeof ( U8 ) );
	
	video_picture->frame = av_frame_alloc ( );
	if ( !video_picture->frame )
	{
		fprintf ( stderr, "Could not allocate frame\n" );
		return;
	}
	
	av_image_fill_arrays ( video_picture->frame->data,
						  video_picture->frame->linesize,
						  buffer,
						  AV_PIX_FMT_YUV420P,
						  media_state->video_codec_ctx->width,
						  media_state->video_codec_ctx->height,
						  32 );
	
    SDL_UnlockMutex ( screen_mutex );
	
    video_picture->width     = media_state->video_codec_ctx->width;
    video_picture->height    = media_state->video_codec_ctx->height;
    video_picture->allocated = true;
}

int queue_picture ( media_state_t *media_state, AVFrame *frame, F64 pts )
{
    SDL_LockMutex ( media_state->picture_queue_mutex );
	
    while ( media_state->picture_queue_size >= VIDEO_PICTURE_QUEUE_SIZE && !media_state->quit )
    {
        SDL_CondWait ( media_state->picture_queue_condition, media_state->picture_queue_mutex );
    }
	
    SDL_UnlockMutex ( media_state->picture_queue_mutex );
	
    if ( media_state->quit )
    {
        return -1;
    }
	
    video_picture_t *video_picture = &media_state->picture_queue [ media_state->picture_queue_write_index ];
	
    if ( !video_picture->frame || video_picture->width != media_state->video_codec_ctx->width || video_picture->height != media_state->video_codec_ctx->height )
    {
        video_picture->allocated = false;
        alloc_picture ( media_state );
        if ( media_state->quit )
        {
            return -1;
        }
    }
	
    if ( video_picture->frame )
    {
		video_picture->pts                           = pts;
        video_picture->frame->pict_type              = frame->pict_type;
        video_picture->frame->pts                    = frame->pts;
        video_picture->frame->pkt_dts                = frame->pkt_dts;
        video_picture->frame->key_frame              = frame->key_frame;
        video_picture->frame->coded_picture_number   = frame->coded_picture_number;
        video_picture->frame->display_picture_number = frame->display_picture_number;
        video_picture->frame->width                  = frame->width;
        video_picture->frame->height                 = frame->height;
		
        sws_scale ( media_state->sws_ctx,
				   ( U8 const* const* ) frame->data,
				   frame->linesize,
				   0,
				   media_state->video_codec_ctx->height,
				   video_picture->frame->data,
				   video_picture->frame->linesize );
		
        ++media_state->picture_queue_write_index;
		
        if ( media_state->picture_queue_write_index == VIDEO_PICTURE_QUEUE_SIZE )
        {
            media_state->picture_queue_write_index = 0;
        }
		
        SDL_LockMutex   ( media_state->picture_queue_mutex );
        media_state->picture_queue_size++;
        SDL_UnlockMutex ( media_state->picture_queue_mutex );
    }
	
    return 0;
}

void video_display ( media_state_t *media_state )
{
	if ( !screen )
	{
		if ( ( media_state->video_codec_ctx->width <= 1280 ) && ( media_state->video_codec_ctx->height <= 720 ) )
		{
			
			screen = SDL_CreateWindow ( "5433D R32433 <saeed@rezaee.net>",
									   SDL_WINDOWPOS_UNDEFINED,
									   SDL_WINDOWPOS_UNDEFINED,
									   media_state->video_codec_ctx->width,
									   media_state->video_codec_ctx->height,
									   SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI );
		}
		else 
		{
			screen = SDL_CreateWindow ( "5433D R32433 <saeed@rezaee.net>",
									   SDL_WINDOWPOS_UNDEFINED,
									   SDL_WINDOWPOS_UNDEFINED,
									   media_state->video_codec_ctx->width / 2,
									   media_state->video_codec_ctx->height / 2,
									   SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI );
			
		}
		
		
		
		SDL_GL_SetSwapInterval ( 1 );
	}
	if ( !screen )
	{
		fprintf ( stderr, "SDL: could not create window - exiting\n" );
		return;
	}
	
	if ( !media_state->renderer )
	{
		media_state->renderer = SDL_CreateRenderer ( screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE );
	}
	
	if ( !media_state->texture )
	{
		media_state->texture = SDL_CreateTexture( media_state->renderer,
												 SDL_PIXELFORMAT_YV12,
												 SDL_TEXTUREACCESS_STREAMING,
												 media_state->video_codec_ctx->width,
												 media_state->video_codec_ctx->height );
	}
	
	video_picture_t *video_picture = &media_state->picture_queue [ media_state->picture_queue_read_index ];
	
	F32 aspect_ratio;
	S32 w, h, x, y;
	
	if ( video_picture->frame )
	{
		if ( media_state->video_codec_ctx->sample_aspect_ratio.num == 0 )
		{
			aspect_ratio = 0;
		}
		else
		{
			aspect_ratio = av_q2d ( media_state->video_codec_ctx->sample_aspect_ratio ) * media_state->video_codec_ctx->width / media_state->video_codec_ctx->height;
		}
		
		if ( aspect_ratio <= 0.0 )
		{
			aspect_ratio = ( F32 ) media_state->video_codec_ctx->width / ( F32 ) media_state->video_codec_ctx->height;
		}
		
		S32 screen_width;
		S32 screen_height;
		SDL_GetWindowSize ( screen, &screen_width, &screen_height );
		
		h = screen_height;
		
		w = ( ( S32 ) rint ( h * aspect_ratio ) ) & -3;
		
		if ( w > screen_width )
		{
			w = screen_width;
			h = ( ( S32 ) rint ( w / aspect_ratio ) ) & -3;
		}
		
		x = ( screen_width  - w );
		y = ( screen_height - h );
		
		printf ( "Frame %c (%d) pts %lld dts %lld key_frame %d [coded_picture_number %d, display_picture_number %d, %dx%d]\n",
				av_get_picture_type_char ( video_picture->frame->pict_type ),
				media_state->video_codec_ctx->frame_number,
				video_picture->frame->pts,
				video_picture->frame->pkt_dts,
				video_picture->frame->key_frame,
				video_picture->frame->coded_picture_number,
				video_picture->frame->display_picture_number,
				video_picture->frame->width,
				video_picture->frame->height );
		
		SDL_Rect rect = { 0 };
		rect.x        = x;
		rect.y        = y;
		rect.w        = 2 * w;
		rect.h        = 2 * h;
		
		SDL_LockMutex ( screen_mutex );
		
		SDL_UpdateYUVTexture ( media_state->texture,
							  &rect,
							  video_picture->frame->data     [ 0 ],
							  video_picture->frame->linesize [ 0 ],
							  video_picture->frame->data     [ 1 ],
							  video_picture->frame->linesize [ 1 ],
							  video_picture->frame->data     [ 2 ],
							  video_picture->frame->linesize [ 2 ] );
		
		SDL_RenderClear ( media_state->renderer );
		
		SDL_RenderCopy ( media_state->renderer, media_state->texture, 0, 0 );
		
		SDL_RenderPresent ( media_state->renderer );
		
		SDL_UnlockMutex ( screen_mutex );
	}
	else
	{
		SDL_Event event;
		event.type       = FF_QUIT_EVENT;
		event.user.data1 = media_state;
		SDL_PushEvent ( &event );
	}
}



void video_refresh_timer ( void *userdata )
{
    media_state_t   *media_state   = ( media_state_t* ) userdata;
    video_picture_t *video_picture = 0;
	
	HANDLE hc = GetStdHandle ( STD_OUTPUT_HANDLE );
	
	
    F64 pts_delay         = 0;
    F64 audio_ref_clock   = 0;
    F64 sync_threshold    = 0;
    F64 real_delay        = 0;
    F64 audio_video_delay = 0;
	
	
    if ( media_state->video_stream )
    {
        if ( media_state->picture_queue_size == 0 )
        {
            schedule_refresh ( media_state, 1 );
        }
        else
        {
            video_picture = &media_state->picture_queue [ media_state->picture_queue_read_index ];
			
			SetConsoleTextAttribute  ( hc, 2 );
			
			printf ( "Current Frame PTS:      %f\n",   video_picture->pts        );
            printf ( "Last Frame    PTS:      %f\n", media_state->frame_last_pts );
			
            pts_delay = video_picture->pts - media_state->frame_last_pts;
            printf ( "PTS Delay:              %f\n", pts_delay );
			
            if ( pts_delay <= 0 || pts_delay >= 1.0 )
            {
                pts_delay = media_state->frame_last_delay;
            }
			
			printf ( "Corrected PTS Delay:    %f\n", pts_delay );
			
            media_state->frame_last_delay = pts_delay;
            media_state->frame_last_pts   = video_picture->pts;
			
            audio_ref_clock = get_audio_clock ( media_state );
			
            printf ( "Audio Ref Clock:        %f\n", audio_ref_clock );
			
            audio_video_delay = video_picture->pts - audio_ref_clock;
			
            printf ( "Audio Video Delay:      %f\n", audio_video_delay );
			
            sync_threshold = ( pts_delay > AV_SYNC_THRESHOLD) ? pts_delay : AV_SYNC_THRESHOLD;
			
            printf ( "Sync Threshold:         %f\n", sync_threshold );
			
            if ( fabs ( audio_video_delay ) < AV_NOSYNC_THRESHOLD )
            {
                if ( audio_video_delay <= -sync_threshold )
                {
                    pts_delay = 0;
                }
                else if ( audio_video_delay >= sync_threshold )
                {
                    pts_delay = 2 * pts_delay;
                }
            }
			
            printf ( "Corrected PTS delay:    %f\n", pts_delay );
			
            media_state->frame_timer += pts_delay;
			
            real_delay = media_state->frame_timer - ( av_gettime ( ) / 1000000.0 );
			
            printf ( "Real Delay:             %f\n", real_delay );
			
            if ( real_delay < 0.010 )
            {
                real_delay = 0.010;
            }
			
            printf ( "Corrected Real Delay:   %f\n", real_delay );
			
            schedule_refresh ( media_state, ( S32 ) ( real_delay * 1000 + 0.5 ) );
			
            printf("Next Scheduled Refresh: %f\n\n", ( F64 ) ( real_delay * 1000 + 0.5 ) );
			
            video_display ( media_state );
			
            if ( ++media_state->picture_queue_read_index == VIDEO_PICTURE_QUEUE_SIZE )
            {
                media_state->picture_queue_read_index = 0 ;
            }
			
			SetConsoleTextAttribute  ( hc, 7 );
			
            SDL_LockMutex ( media_state->picture_queue_mutex );
			
            media_state->picture_queue_size--;
            SDL_CondSignal ( media_state->picture_queue_condition );
            
			SDL_UnlockMutex ( media_state->picture_queue_mutex );
        }
    }
    else
    {
        schedule_refresh ( media_state, 100 );
    }
}




static int audio_resample ( media_state_t *media_state, 
						   AVFrame *decoded_audio_frame,
						   enum AVSampleFormat out_sample_fmt,
						   U8*out_buffer )
{
	S32 ret = -1;
	
	audio_resampling_state_t *ars = get_audio_resampling ( media_state->audio_codec_ctx->channel_layout );
	
	if ( !ars->swr_ctx )
	{
		fprintf ( stderr, "swr_alloc error\n" );
		return -1;
	}
	
	ars->in_channel_layout = ( media_state->audio_codec_ctx->channels == av_get_channel_layout_nb_channels ( media_state->audio_codec_ctx->channel_layout ) ) ? 
		media_state->audio_codec_ctx->channel_layout :
	av_get_default_channel_layout ( media_state->audio_codec_ctx->channels );
	
	if ( ars->in_channel_layout <= 0 )
	{
		fprintf ( stderr, "in_channel_layout error\n" );
		return -1;
	}
	
    if ( media_state->audio_codec_ctx->channels == 1 )
    {
        ars->out_channel_layout = AV_CH_LAYOUT_MONO;
    }
    else if ( media_state->audio_codec_ctx->channels == 2 )
    {
        ars->out_channel_layout = AV_CH_LAYOUT_STEREO;
    }
    else
    {
        ars->out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }
	
    ars->in_num_samples = decoded_audio_frame->nb_samples;
    if ( ars->in_num_samples <= 0 )
    {
        printf ( "in_num_samples error\n");
        return -1;
    }
	
	av_opt_set_int ( ars->swr_ctx,
					"in_channel_layout",
					ars->in_channel_layout,
					0 );
	
	av_opt_set_int ( ars->swr_ctx,
					"in_sample_rate",
					media_state->audio_codec_ctx->sample_rate,
					0 );
	
	
    av_opt_set_sample_fmt(
						  ars->swr_ctx,
						  "in_sample_fmt",
						  media_state->audio_codec_ctx->sample_fmt,
						  0
						  );
	//=========================================================================
	
	av_opt_set_int ( ars->swr_ctx,
					"out_channel_layout",
					ars->out_channel_layout,
					0 );
	
	av_opt_set_int ( ars->swr_ctx,
					"out_sample_rate",
					media_state->audio_codec_ctx->sample_rate,
					0 );
	
	av_opt_set_sample_fmt ( ars->swr_ctx,
						   "out_sample_fmt",
						   out_sample_fmt,
						   0 );
	
	ret = swr_init ( ars->swr_ctx );
	if ( ret < 0 )
	{
		fprintf ( stderr, "Failed to initialize the resampling context\n" );
		return -1;
	}
	
	ars->max_out_num_samples = ars->out_num_samples = av_rescale_rnd ( ars->in_num_samples,
																	  media_state->audio_codec_ctx->sample_rate,
																	  media_state->audio_codec_ctx->sample_rate,
																	  AV_ROUND_UP );
	if ( ars->max_out_num_samples <= 0 )
	{
		fprintf ( stderr, "av_rescale_rnd error\n" );
		return -1;
	}
	
	ars->out_num_channels = av_get_channel_layout_nb_channels ( ars->out_channel_layout );
	
	ret = av_samples_alloc_array_and_samples ( &ars->resampled_data,
											  &ars->out_linesize,
											  ars->out_num_channels,
											  ars->out_num_samples,
											  out_sample_fmt,
											  0 );
	
	if ( ret < 0 )
	{
		fprintf ( stderr, "av_samples_alloc_array_and_samples() error: Could not allocate destination samples\n" );
		return -1;
	}
	
	ars->out_num_samples = av_rescale_rnd ( swr_get_delay ( ars->swr_ctx, media_state->audio_codec_ctx->sample_rate ) + ars->in_num_samples,
										   media_state->audio_codec_ctx->sample_rate,
										   media_state->audio_codec_ctx->sample_rate,
										   AV_ROUND_UP );
	
	if ( ars->out_num_samples <= 0 )
	{
		fprintf ( stderr, "av_rescale_rnd error\n" );
		return -1;
	}
	
	if ( ars->out_num_samples > ars->max_out_num_samples )
	{
		av_free ( ars->resampled_data [ 0 ] );
		
		ret = av_samples_alloc ( ars->resampled_data,
								&ars->out_linesize,
								ars->out_num_channels,
								ars->out_num_samples,
								out_sample_fmt,
								1 );
		
		if ( ret < 0 )
		{
			fprintf ( stderr, "av_samples_alloc failed!\n" );
			return -1;
		}
		
		ars->max_out_num_samples = ars->out_num_samples;
	}
	
	if ( ars->swr_ctx )
	{
		ret = swr_convert ( ars->swr_ctx,
						   ars->resampled_data,
						   ars->out_num_samples,
						   ( const U8** ) decoded_audio_frame->data,
						   decoded_audio_frame->nb_samples );
		
		if ( ret < 0 )
		{
			fprintf ( stderr, "swr_convert_error\n" );
			return -1;
		}
		
		ars->resampled_data_size = av_samples_get_buffer_size ( &ars->out_linesize,
															   ars->out_num_channels,
															   ret,
															   out_sample_fmt,
															   1 );
		
		if ( ars->resampled_data_size < 0 )
		{
			fprintf ( stderr, "av_samples_get_buffer_size error\n" );
			return -1;
		}
	}
	else
	{
		fprintf ( stderr, "swr_ctx null error\n" );
		return -1;
	}
	
	memcpy ( out_buffer, ars->resampled_data [ 0 ], ars->resampled_data_size );
	
	if ( ars->resampled_data )
	{
		av_freep ( &ars->resampled_data [ 0 ] );
	}
	
	av_freep ( &ars->resampled_data );
	ars->resampled_data = 0;
	
	if ( ars->swr_ctx )
	{
		swr_free ( &ars->swr_ctx );
	}
	
	return ars->resampled_data_size;
}


int audio_decode_frame ( media_state_t *media_state, 
						U8             *audio_buffer, 
						S32             buffer_size,
						F64            *pts_ptr )
{
	AVPacket *packet = av_packet_alloc ( );
	
	static U8 *audio_packet_data = 0;
	static S32 audio_packet_size = 0;
	
	static AVFrame *frame = 0;
	frame = av_frame_alloc ( );
	if  ( !frame )
	{
		fprintf ( stderr, "Could not allocate AVFrame\n" );
		return -1;
	}
	
	S32 len       = 0;
	S32 data_size = 0;
	
	F64 pts       = 0;
	S32 channels  = 0;
	
	for ( ; ; )
	{
		if ( media_state->quit )
		{
			return -1;
		}
		
		while ( audio_packet_size > 0 )
		{
			bool32 got_frame = false;
			
			int ret = avcodec_receive_frame ( media_state->audio_codec_ctx, frame );
			if ( ret == 0 )
			{
				got_frame = true;
			}
			if ( ret == AVERROR ( EAGAIN ) )
			{
				ret = 0;
			}
			if ( ret == 0 )
			{
				ret = avcodec_send_packet ( media_state->audio_codec_ctx, packet );
			}
			if ( ret == AVERROR ( EAGAIN ) )
			{
				ret = 0;
			}
			else if ( ret < 0 )
			{
				fprintf ( stderr, "avcodec_receive_frame error" );
				return -1;
			}
			else
			{
				len = packet->size;
			}
			
			if ( len < 0 )
			{
				audio_packet_size = 0;
				break;
			}
			
			audio_packet_data += len;
			audio_packet_size -= len;
			data_size          = 0;
			
			if ( got_frame )
			{
				data_size = audio_resample ( media_state,
											frame,
											AV_SAMPLE_FMT_S16,
											audio_buffer );
				
                assert ( data_size <= buffer_size );
            }
			
            if ( data_size <= 0 )
            {
                continue;
            }
			
			pts                      = media_state->audio_clock;
            *pts_ptr                 = pts;
			channels                 = 2 * media_state->audio_codec_ctx->channels;
            media_state->audio_clock += ( F64 ) data_size / ( F64 )( channels * media_state->audio_codec_ctx->sample_rate );
			
            return data_size;
        }
		
        if ( packet->data )
        {
            av_packet_unref ( packet );
        }
		
        int ret = packet_queue_get ( &media_state->audio_queue, packet, 1 );
		
        if ( ret < 0 )
        {
            return -1;
        }
		
        audio_packet_data = packet->data;
        audio_packet_size = packet->size;
		
        if ( packet->pts != AV_NOPTS_VALUE )
        {
            media_state->audio_clock = av_q2d ( media_state->audio_stream->time_base ) * packet->pts;
        }
    }
	
    return 0;
}


int main ( int argc, char **argv )
{
	SDL_SetMainReady();
	int ret = -1;
	
	
#ifdef WIN32
	HANDLE hc = GetStdHandle ( STD_OUTPUT_HANDLE );
	SetEnvironmentVariableA ( "SDL_AUDIODRIVER", "directsound" );
#endif
	
	if ( !( argc == 2 ) )
    {
		
#ifdef WIN32
		SetConsoleTextAttribute  ( hc, 2 );
        printf  ( "\n5433D R32433 <saeed@rezaee.net>\n" );
		SetConsoleTextAttribute  ( hc, 6 );
		fprintf ( stderr, "Usage: %s video_file_path\n", argv [ 0 ] );
		SetConsoleTextAttribute  ( hc, 7 );
#endif
		
		return -1;
    }
	
	if ( SDL_Init ( SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER ) ) 
	{
		fprintf ( stderr, "Could not initialize SDL - %s\n", SDL_GetError ( ) );
		exit ( EXIT_FAILURE );
	}
	
	media_state_t *media_state = av_mallocz ( sizeof ( media_state_t ) );
	assert ( media_state );
	
	av_strlcpy ( media_state->filename, argv [ 1 ], sizeof ( media_state->filename ) );
	
	
	media_state->picture_queue_mutex     = SDL_CreateMutex ( );
	media_state->picture_queue_condition = SDL_CreateCond  ( );
	
	
    schedule_refresh ( media_state, 100 );
	
    media_state->decode_thread_id = SDL_CreateThread ( decode_thread, "Decoding Thread", media_state );
    if ( !media_state->decode_thread_id )
    {
        fprintf ( stderr, "Could not start decoding SDL_Thread - exiting\n" );
        av_free ( media_state );
        return -1;
    }
	
	SDL_Event event;
	for ( ; ; )
	{
		ret = SDL_WaitEvent ( &event );
		if ( !ret )
		{
			fprintf ( stderr, "SDL_WaitEvent failed: %s\n", SDL_GetError ( ) );
		}
		
        switch(event.type)
        {
            case FF_QUIT_EVENT:
            case SDL_QUIT:
            {
                media_state->quit = true;
                SDL_Quit ( );
            } break;
			
            case FF_REFRESH_EVENT:
            {
                video_refresh_timer ( event.user.data1 );
            } break;
			
			case SDL_KEYDOWN:
			{
				switch( event.key.keysym.sym )
				{
					case SDLK_p:
					{
					} break;
					
					case SDLK_ESCAPE:
					{
						media_state->quit = true;
						SDL_Quit ( );
					} break;
					
					
					default:
					{
					} break;
				}
			} break;
			
            default:
            {
            } break;
        }
		
        if ( media_state->quit )
        {
            break;
        }
	}
	
	return 0;
}
