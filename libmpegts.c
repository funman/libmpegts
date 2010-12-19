/*****************************************************************************
 * libmpegts.c :
 *****************************************************************************
 * Copyright (C) 2010 Kieran Kunhya
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/

#include "common.h"
#include "codecs.h"
#include "atsc/atsc.h"
#include "cablelabs/cablelabs.h"
#include "dvb/dvb.h"
#include "hdmv/hdmv.h"
#include "isdb/isdb.h"
#include "crc/crc.h"
#include <math.h>

static int steam_type_table[23][2] =
{
    { LIBMPEGTS_VIDEO_MPEG2, VIDEO_MPEG2 },
    { LIBMPEGTS_VIDEO_H264,  VIDEO_H264 },
    { LIBMPEGTS_AUDIO_MPEG1, AUDIO_MPEG1 },
    { LIBMPEGTS_AUDIO_MPEG2, AUDIO_MPEG2 },
    { LIBMPEGTS_AUDIO_ADTS,  AUDIO_ADTS },
    { LIBMPEGTS_AUDIO_LATM,  AUDIO_LATM },
    { LIBMPEGTS_AUDIO_AC3,   AUDIO_AC3 },    /* ATSC/Generic */
    { LIBMPEGTS_AUDIO_AC3,   PRIVATE_DATA }, /* DVB */
    { LIBMPEGTS_AUDIO_EAC3,  AUDIO_EAC3 },   /* ATSC/Generic */
    { LIBMPEGTS_AUDIO_EAC3,  PRIVATE_DATA }, /* DVB */
    { LIBMPEGTS_AUDIO_LPCM,  AUDIO_LPCM },
    { LIBMPEGTS_AUDIO_DTS,   AUDIO_DTS },
    { LIBMPEGTS_AUDIO_DOLBY_LOSSLESS,      AUDIO_DOLBY_LOSSLESS },
    { LIBMPEGTS_AUDIO_DTS_HD,              AUDIO_DTS_HD },
    { LIBMPEGTS_AUDIO_DTS_HD_XLL,          AUDIO_DTS_HD_XLL },
    { LIBMPEGTS_AUDIO_EAC3_SECONDARY,      AUDIO_EAC3_SECONDARY },
    { LIBMPEGTS_AUDIO_DTS_HD_SECONDARY,    AUDIO_DTS_HD_SECONDARY },
    { LIBMPEGTS_SUB_PRESENTATION_GRAPHICS, SUB_PRESENTATION_GRAPHICS },
    { LIBMPEGTS_SUB_INTERACTIVE_GRAPHICS,  SUB_INTERACTIVE_GRAPHICS },
    { LIBMPEGTS_SUB_TEXT,    SUB_TEXT },
    { LIBMPEGTS_AUDIO_302M,  PRIVATE_DATA },
    { LIBMPEGTS_SUB_DVB,     PRIVATE_DATA },
    { 0 },
};

/* Descriptors */
static void write_smoothing_buffer_descriptor( bs_t *s, ts_int_program_t *program );
static void write_video_stream_descriptor( bs_t *s, ts_int_stream_t *stream );
static void write_avc_descriptor( bs_t *s, ts_int_stream_t *stream );
static void write_data_stream_alignment_descriptor( bs_t *s );
static void write_timestamp( bs_t *s, uint64_t timestamp );
static void write_null_packet( ts_writer_t *w );

ts_writer_t *ts_create_writer( void )
{
    ts_writer_t *w = calloc( 1, sizeof(*w) );

    if( !w )
    {
        fprintf( stderr, "Malloc failed\n" );
        return NULL;
    }

    return w;
}
int ts_setup_mpegvideo_stream( ts_writer_t *w, int pid, int level, int profile, int vbv_maxrate, int vbv_bufsize, int frame_rate )
{
    int bs_mux, bs_oh;
    int level_idx = -1;

    ts_int_stream_t *stream = find_stream( w, pid );

    if( !stream )
    {
        fprintf( stderr, "Invalid PID\n" );
        return -1;
    }

    if( !( stream->stream_format == LIBMPEGTS_VIDEO_MPEG2 && stream->stream_format == LIBMPEGTS_VIDEO_H264 ) )
    {
        fprintf( stderr, "PID is not mpegvideo stream\n" );
        return -1;
    }

    if( stream->stream_format == LIBMPEGTS_VIDEO_MPEG2 )
    {
        if( level < MPEG2_LEVEL_LOW || level > MPEG2_LEVEL_HIGHP )
        {
            fprintf( stderr, "Invalid MPEG-2 Level\n" );
            return -1;
        }
        if( profile < MPEG2_PROFILE_SIMPLE || profile > MPEG2_PROFILE_422 )
        {
            fprintf( stderr, "Invalid MPEG-2 Profile\n" );
            return -1;
        }

        for( int i = 0; mpeg2_levels[i].level != 0; i++ )
        {
            if( level == mpeg2_levels[i].level && profile == mpeg2_levels[i].profile )
            {
                level_idx = i;
                break;
            }
        }
        if( level_idx == -1 )
        {
            fprintf( stderr, "Invalid MPEG-2 Level/Profile combination.\n" );
            return -1;            
        }
    }
    else if( stream->stream_format == LIBMPEGTS_VIDEO_H264 )
    {
        for( int i = 0; h264_levels[i].level_idc != 0; i++ )
        {
            if( level == h264_levels[i].level_idc )
            {
                level_idx = i;
                break;
            }
        }
        if( level_idx == -1 )
        {
            fprintf( stderr, "Invalid AVC Level\n" );
            return -1;            
        }
        if( profile < H264_BASELINE || profile > H264_CAVLC_444_INTRA )
        {
            fprintf( stderr, "Invalid AVC Profile\n" );
            return -1;
        }
    }

    if( !stream->mpegvideo_ctx )
    {
        stream->mpegvideo_ctx = calloc( 1, sizeof(stream->mpegvideo_ctx) );
        if( !stream->mpegvideo_ctx )
        {
            fprintf( stderr, "Malloc failed\n" );
            return -1;
        }
    }

    stream->mpegvideo_ctx->level = level;
    stream->mpegvideo_ctx->profile = profile;
    stream->mpegvideo_ctx->buffer_size = (double)vbv_bufsize / vbv_maxrate;

    stream->tb.buf_size = TB_SIZE;

    if( stream->stream_format == LIBMPEGTS_VIDEO_MPEG2 )
    {
        bs_mux = 0.004 * mpeg2_levels[level_idx].bitrate;
        bs_oh = 1.0 * mpeg2_levels[level_idx].bitrate/750.0;

        stream->rx = 1.2 * mpeg2_levels[level_idx].bitrate;
        stream->eb.buf_size = vbv_bufsize;

        if( level == MPEG2_LEVEL_LOW || level == MPEG2_LEVEL_MAIN )
        {
            stream->mb.buf_size = bs_mux + bs_oh + mpeg2_levels[level_idx].vbv - vbv_bufsize;
            stream->rbx = mpeg2_levels[level_idx].bitrate;
        }
        else
        {
            stream->mb.buf_size = bs_mux + bs_oh;
            stream->rbx = MIN( 1.05 * vbv_maxrate, mpeg2_levels[level_idx].bitrate );
        }
    }
    else if( stream->stream_format == LIBMPEGTS_VIDEO_H264 )
    {
        bs_mux = 0.004 * MAX( 1200 * h264_levels[level_idx].bitrate, 2000000 );
        bs_oh = 1.0 * MAX( 1200 * h264_levels[level_idx].bitrate, 2000000 )/750.0;

        stream->mb.buf_size = bs_mux + bs_oh;
        stream->eb.buf_size = 1200 * h264_levels[level_idx].cpb;

        stream->rx = 1200 * h264_levels[level_idx].bitrate;
        stream->rbx = 1200 * h264_levels[level_idx].bitrate;
    }

    return 0;
}

int ts_setup_302m_stream( ts_writer_t *w, int pid, int bit_depth, int num_channels )
{
    if( w->ts_type == TS_TYPE_BLU_RAY )
    {
        fprintf( stderr, "SMPTE 302M not allowed in Blu-Ray\n" );
        return -1;
    }
    else if( !(bit_depth == 16 || bit_depth == 20 || bit_depth == 24) )
    {
        fprintf( stderr, "Invalid Bit Depth for SMPTE 302M\n" );
        return -1;
    }
    else if( (num_channels & 1) || num_channels <= 0 || num_channels > 8 )
    {
        fprintf( stderr, "Invalid number of channels for SMPTE 302M\n" );
        return -1;
    }

    ts_int_stream_t *stream = find_stream( w, pid );

    if( !stream )
    {
        fprintf( stderr, "Invalid PID\n" );
        return -1;
    }

    stream->lpcm_ctx->bits_per_sample = bit_depth;
    stream->lpcm_ctx->num_channels = num_channels;

    stream->main_b.buf_size = SMPTE_302M_AUDIO_BS;

    /* 302M frame size is bit_depth / 4 + 1 */
    stream->rx = 1.2 * ((bit_depth >> 2) + 1) * SMPTE_302M_AUDIO_SR * 8;

    return 0;
}
int write_padding( bs_t *s, uint64_t start )
{
    bs_flush( s );
    uint8_t *p_start = s->p_start;
    int padding_bytes = TS_PACKET_SIZE - (bs_pos( s ) - start) / 8;

    memset( s->p, 0xff, padding_bytes );
    s->p += padding_bytes;

    bs_init( s, s->p, s->p_end - s->p );
    s->p_start = p_start;

    return padding_bytes;
}

void write_bytes( bs_t *s, uint8_t *bytes, int length )
{
    bs_flush( s );
    uint8_t *p_start = s->p_start;

    memcpy( s->p, bytes, length );
    s->p += length;

    bs_init( s, s->p, s->p_end - s->p );
    s->p_start = p_start;
}

/**** Descriptors ****/
/* Registration Descriptor */
void write_registration_descriptor( bs_t *s, int descriptor_tag, int descriptor_length, char *format_id )
{
    bs_write( s, 8, descriptor_tag );    // descriptor_tag
    bs_write( s, 8, descriptor_length ); // descriptor_length
    while( *format_id != '\0' )
        bs_write( s, 8, *format_id++ );  // format_identifier
}

/* First loop of PMT Descriptors */
static void write_smoothing_buffer_descriptor( bs_t *s, ts_int_program_t *program )
{
    bs_write( s, 8, SMOOTHING_BUFFER_DESCRIPTOR_TAG ); // descriptor_tag
    bs_write( s, 8, 0x4 );                    // descriptor_length

    bs_write( s, 2, 0x3 );                    // reserved
    bs_write( s, 22, program->sb_leak_rate ); // sb_leak_rate
    bs_write( s, 2, 0x3 );                    // reserved
    bs_write( s, 22, program->sb_size );      // sb_size
}

/* Second loop of PMT Descriptors */
static void write_video_stream_descriptor( bs_t *s, ts_int_stream_t *stream )
{
    bs_write( s, 8, VIDEO_STREAM_DESCRIPTOR_TAG ); // descriptor_tag
    bs_write( s, 8, 0x04 );                        // descriptor_length

    bs_write1( s, 0 );   // multiple_frame_rate_flag
    bs_write( s, 4, 0 ); // frame_rate_code FIXME
    bs_write1( s, 0 );   // MPEG_1_only_flag
    bs_write1( s, 0 );   // constrained_parameter_flag
    bs_write1( s, 0 );   // still_picture_flag
    bs_write( s, 8, 0 ); // profile_and_level_indication FIXME
    bs_write( s, 2, 0 ); // chroma_format FIXME
    bs_write1( s, 0 );   // frame_rate_extension_flag FIXME
    bs_write( s, 5, 0x1f ); // reserved
}

static void write_avc_descriptor( bs_t *s, ts_int_stream_t *stream )
{
    bs_write( s, 8, AVC_DESCRIPTOR_TAG ); // descriptor_tag
    bs_write( s, 8, 0x04 );               // descriptor_length

    bs_write( s, 8, stream->mpegvideo_ctx->profile & 0xff ); // profile_idc

    bs_write1( s, stream->mpegvideo_ctx->profile == H264_BASELINE ); // constraint_set0_flag
    bs_write1( s, stream->mpegvideo_ctx->profile <= H264_MAIN );     // constraint_set1_flag
    bs_write1( s, 0 );                                               // constraint_set2_flag
        if( stream->mpegvideo_ctx->level == 9 && stream->mpegvideo_ctx->profile <= H264_MAIN ) // level 1b
            bs_write1( s, 1 );                                           // constraint_set3_flag
        else if( stream->mpegvideo_ctx->profile == H264_HIGH_10_INTRA   ||
                 stream->mpegvideo_ctx->profile == H264_CAVLC_444_INTRA ||
                 stream->mpegvideo_ctx->profile == H264_HIGH_444_INTRA )
            bs_write1( s, 1 );                                           // constraint_set3_flag
        else
            bs_write1( s, 0 );                                           // constraint_set3_flag
    bs_write1( s, 0 );                                               // constraint_set4_flag
    bs_write1( s, 0 );                                               // constraint_set5_flag

    bs_write( s, 2, 0 );    // reserved
    bs_write( s, 8, stream->mpegvideo_ctx->level & 0xff ); // level_idc
    bs_write( s, 1, 0 );    // AVC_still_present
    bs_write( s, 1, 0 );    // AVC_24_hour_picture_flag
    bs_write( s, 6, 0x3f ); // reserved
}
static void write_iso_lang_descriptor( bs_t *s, ts_int_stream_t *stream )
{
    bs_write( s, 8, ISO_693_LANGUAGE_DESCRIPTOR_TAG ); // descriptor_tag
    bs_write( s, 8, 0x04 ); // descriptor_length
    for( int i = 0; i < 3; i++ )
        bs_write( s, 8, stream->lang_code[i] );

    bs_write(s, 8, 0 ); // audio_type
}
/**** PSI ****/
static void write_pat( ts_writer_t *w )
{
    int start;
    bs_t *s = &w->out.bs;

    write_packet_header( w, 1, PAT_PID, PAYLOAD_ONLY, &w->pat_cc );
    bs_write( s, 8, 0 ); // pointer field

    start = bs_pos( s );
    bs_write( s, 8, PAT_TID ); // table_id
    bs_write1( s, 1 );      // section_syntax_indicator
    bs_write1( s, 0 );      // '0'
    bs_write( s, 2, 0x03 ); // reserved`

    // FIXME when multiple programs are allowed do this properly
    int section_length = w->num_programs * 4 + w->network_pid * 4 + 9;
    bs_write( s, 12, section_length & 0x3ff );

    bs_write( s, 16, w->ts_id & 0xffff ); // transport_stream_id
    bs_write( s, 2, 0x03 ); // reserved
    bs_write( s, 5, 0 );    // version_number
    bs_write1( s, 1 );      // current_next_indicator
    bs_write( s, 8, 0 );    // section_number
    bs_write( s, 8, 0 );    // last_section_number

    if( w->network_pid )
    {
        bs_write( s, 16, 0 );   // program_number
        bs_write( s, 3, 0x07 ); // reserved
        bs_write( s, 13, w->network_pid & 0x1fff ); // network_PID
    }

    for( int i = 0; i < w->num_programs; i++ )
    {
        bs_write( s, 16, w->programs[i]->program_num & 0xffff ); // program_number
        bs_write( s, 3, 0x07 ); // reserved
        bs_write( s, 13, w->programs[i]->pmt.pid & 0x1fff ); // program_map_PID
    }

    bs_flush( s );
    write_crc( s, start );

    // -40 to include header and pointer field
    write_padding( s, start - 40 );
    increase_pcr( w, 1 );
}
static void write_timestamp( bs_t *s, uint64_t timestamp )
{
    bs_write( s, 3, (timestamp >> 30) & 0x07 ); // timestamp [32..30]
    bs_write1( s, 1 );                          // marker_bit
    bs_write( s, 8, (timestamp >> 22) & 0xff ); // timestamp [29..15]
    bs_write( s, 7, (timestamp >> 15) & 0x7f ); // timestamp [29..15]
    bs_write1( s, 1 );                          // marker_bit
    bs_write( s, 8, (timestamp >> 7) & 0xff );  // timestamp [14..0]
    bs_write( s, 7, timestamp & 0x7f );         // timestamp [14..0]
    bs_write1( s, 1 );                          // marker_bit
}

void write_crc( bs_t *s, uint64_t start )
{
    uint8_t *p_start = s->p_start;
    int pos = (bs_pos( s ) - start) / 8;
    uint32_t crc = crc_32( s->p - pos, pos );

    bs_init( s, s->p, s->p_end - s->p );
    s->p_start = p_start;

    bs_write32( s, crc );
}
static void write_null_packet( ts_writer_t *w )
{
    uint64_t start;
    int cc = 0;

    bs_t *s = &w->out.bs;
    start = bs_pos( s );

    write_packet_header( w, 0, NULL_PID, PAYLOAD_ONLY, &cc );
    write_padding( s, start );

    increase_pcr( w, 1 );
}

ts_int_stream_t *find_stream( ts_writer_t *w, int pid )
{
    for( int i = 0; i < w->programs[0]->num_streams; i++ )
    {
        if( pid == w->programs[0]->streams[i]->pid )
            return w->programs[0]->streams[i];
    }
    return NULL;
}
