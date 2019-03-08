/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2018
 *					All rights reserved
 *
 *  This file is part of GPAC / video flipping filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/filters.h>
#include <gpac/list.h>
#include <gpac/constants.h>
#include <gpac/network.h>

typedef struct
{
	//options
	u32 mode;

	//internal data
	Bool initialized;

	GF_FilterPid *ipid, *opid;
	u32 w, h, stride, s_pfmt;
	GF_Fraction ar;
	Bool passthrough;
	u32 dst_width, dst_height;

	u32 dst_stride[5];
	u32 src_stride[5];
	u32 nb_planes, nb_src_planes, out_size, out_src_size, src_uv_height, dst_uv_height;

	Bool use_reference;
	Bool packed_422;

	char *line_buffer;
} GF_VFlipCtx;

enum
{
	VFLIP_OFF = 0,
	VFLIP_VERT,
	VFLIP_HORIZ,
	VFLIP_BOTH,
};

static Bool fill_plane_dst_from_src(GF_VFlipCtx *ctx, u8 *src_plane, u8 *dst_plane, u32 height, u32 plane_idx, u32 wiB){
	u32 hy, i;

	hy = height/2;
	for (i=0; i<hy; i++) {
		u8 *src_first = src_plane+ i*ctx->src_stride[plane_idx];
		u8 *src_last  = src_plane+ (height  - 1 - i) * ctx->src_stride[plane_idx];

		u8 *dst_first = dst_plane+ i*ctx->dst_stride[plane_idx];
		u8 *dst_last  = dst_plane+ (height  - 1 - i) * ctx->dst_stride[plane_idx];

		memcpy(ctx->line_buffer, src_last, wiB);
		memcpy(dst_last, src_first, wiB);
		memcpy(dst_first, ctx->line_buffer, wiB);
	}
	return GF_OK;
}

static GF_Err vflip_process(GF_Filter *filter)
{
	const char *data;
	char *output;
	u32 size;
	u32 bps;
	u32 i;
	u32 wiB, height;
	u8 *src_planes[5];
	u8 *dst_planes[5];
	GF_FilterPacket *dst_pck;
	GF_FilterFrameInterface *frame_ifce;
	GF_VFlipCtx *ctx = gf_filter_get_udta(filter);
	GF_FilterPacket *pck = gf_filter_pid_get_packet(ctx->ipid);

	if (!pck) {
		if (gf_filter_pid_is_eos(ctx->ipid)) {
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}

	if (ctx->passthrough) {
		gf_filter_pck_forward(pck, ctx->opid);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OK;
	}
	data = gf_filter_pck_get_data(pck, &size);
	frame_ifce = gf_filter_pck_get_frame_interface(pck);

	memset(src_planes, 0, sizeof(src_planes));
	memset(dst_planes, 0, sizeof(dst_planes));

	if (data) {
		src_planes[0] = (u8 *) data;

		if (ctx->nb_src_planes==1) {
		} else if (ctx->nb_src_planes==2) {
			src_planes[1] = src_planes[0] + ctx->src_stride[0]*ctx->h;
		} else if (ctx->nb_src_planes==3) {
			src_planes[1] = src_planes[0] + ctx->src_stride[0] * ctx->h;
			src_planes[2] = src_planes[1] + ctx->src_stride[1] * ctx->src_uv_height;
		} else if (ctx->nb_src_planes==4) {
			src_planes[1] = src_planes[0] + ctx->src_stride[0] * ctx->h;
			src_planes[2] = src_planes[1] + ctx->src_stride[1] * ctx->src_uv_height;
			src_planes[3] = src_planes[2] + ctx->src_stride[2] * ctx->src_uv_height;
		}
	} else if (frame_ifce && frame_ifce->get_plane) {
		u32 i=0;
		for (i=0; i<4; i++) {
		if (frame_ifce->get_plane(frame_ifce, i, (const u8 **) &src_planes[i], &ctx->src_stride[i])!=GF_OK)
			break;
		}
	} else {
		GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[VFlip] No data associated with packet, not supported\n"));
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NOT_SUPPORTED;
	}

	bps = gf_pixel_get_bytes_per_pixel(ctx->s_pfmt);


	if (frame_ifce){
		dst_pck = gf_filter_pck_new_alloc(ctx->opid, ctx->out_size, &output);
		gf_filter_pck_merge_properties(pck, dst_pck);
	} else {
		dst_pck = gf_filter_pck_new_clone(ctx->opid, pck, &output);
	}

	if (!dst_pck) {
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}


	dst_planes[0] = output;
	if (ctx->nb_planes==1) {
	} else if (ctx->nb_planes==2) {
		dst_planes[1] = output + ctx->dst_stride[0] * ctx->dst_height;
	} else if (ctx->nb_planes==3) {
		dst_planes[1] = output + ctx->dst_stride[0] * ctx->dst_height;
		dst_planes[2] = dst_planes[1] + ctx->dst_stride[1]*ctx->dst_uv_height;
	} else if (ctx->nb_planes==4) {
		dst_planes[1] = output + ctx->dst_stride[0] * ctx->dst_height;
		dst_planes[2] = dst_planes[1] + ctx->dst_stride[1]*ctx->dst_uv_height;
		dst_planes[3] = dst_planes[2] + ctx->dst_stride[2]*ctx->dst_uv_height;
	}


	//YUYV variations need *2 on horizontal dimension
	for (i=0; i<ctx->nb_planes; i++) {
		if (i==0) {
			if (ctx->packed_422) {
				wiB = bps * ctx->dst_width *2;
			} else {
				wiB = bps * ctx->dst_width;
			}
			height = ctx->h;
		}else {
			//nv12/21
			if (i==1 && ctx->nb_planes==2) {
				//half vertical res (/2)
				//half horizontal res (/2) but two chroma packed per pixel (*2)
				wiB = bps * ctx->dst_width;
				height = ctx->h / 2;
			} else if (ctx->nb_planes>=3) {
				u32 div_x, div_y;
				//alpha/depth/other plane, treat as luma plane
				if (i==3 && ctx->nb_planes==4) {
					wiB = bps * ctx->dst_width;
					height = ctx->h;
				}else if (i==1 || i==2) {
					div_x = (ctx->src_stride[1]==ctx->src_stride[0]) ? 1 : 2;
					div_y = (ctx->src_uv_height==ctx->h) ? 1 : 2;
					 height = ctx->dst_height;
					height /= div_y;
					wiB = bps * ctx->dst_width;
					wiB /= div_x;
				}
			}
		}
		fill_plane_dst_from_src(ctx, src_planes[i], dst_planes[i], height, i, wiB);
	}

	gf_filter_pck_send(dst_pck);
	gf_filter_pid_drop_packet(ctx->ipid);
	return GF_OK;
}


static GF_Err vflip_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *p;
	u32 w, h, stride, pfmt;
	GF_Fraction sar;
	GF_VFlipCtx *ctx = gf_filter_get_udta(filter);

	if (is_remove) {
		if (ctx->opid) {
			gf_filter_pid_remove(ctx->opid);
		}
		return GF_OK;
	}
	if (! gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	if (!ctx->opid) {
		ctx->opid = gf_filter_pid_new(filter);
	}
	//copy properties at init or reconfig
	gf_filter_pid_copy_properties(ctx->opid, pid);

	if (!ctx->ipid) {
		ctx->ipid = pid;
	}
	w = h = pfmt = stride = 0;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_WIDTH);
	if (p) w = p->value.uint;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_HEIGHT);
	if (p) h = p->value.uint;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_STRIDE);
	if (p) stride = p->value.uint;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_PIXFMT);
	if (p) pfmt = p->value.uint;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_SAR);
	if (p) sar = p->value.frac;
	else sar.den = sar.num = 1;

	if (!w || !h || !pfmt) {
		ctx->passthrough = GF_TRUE;
		return GF_OK;
	}
	ctx->passthrough = GF_FALSE;

	if ((ctx->w == w) && (ctx->h == h) && (ctx->s_pfmt == pfmt) && (ctx->stride == stride)) {
		//nothing to reconfigure
		ctx->passthrough = GF_TRUE;
	} else if (ctx->mode==VFLIP_OFF) {
		ctx->passthrough = GF_TRUE;
	} else {
		Bool res;

		ctx->w = w;
		ctx->h = h;
		ctx->s_pfmt = pfmt;
		ctx->stride = stride;
		ctx->dst_width  = w;
		ctx->dst_height = h;
		ctx->passthrough = GF_FALSE;

		//get layout info for source
		memset(ctx->src_stride, 0, sizeof(ctx->src_stride));
		if (ctx->stride) ctx->src_stride[0] = ctx->stride;

		res = gf_pixel_get_size_info(pfmt, w, h, &ctx->out_src_size, &ctx->src_stride[0], &ctx->src_stride[1], &ctx->nb_src_planes, &ctx->src_uv_height);
		if (!res) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[VFlip] Failed to query source pixel format characteristics\n"));
			return GF_NOT_SUPPORTED;
		}
		if (ctx->nb_src_planes==3) ctx->src_stride[2] = ctx->src_stride[1];
		if (ctx->nb_src_planes==4) ctx->src_stride[3] = ctx->src_stride[0];


		//get layout info for dest
		memset(ctx->dst_stride, 0, sizeof(ctx->dst_stride));
		res = gf_pixel_get_size_info(pfmt, ctx->dst_width, ctx->dst_height, &ctx->out_size, &ctx->dst_stride[0], &ctx->dst_stride[1], &ctx->nb_planes, &ctx->dst_uv_height);
		if (!res) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[VFlip] Failed to query output pixel format characteristics\n"));
			return GF_NOT_SUPPORTED;
		}
		if (ctx->nb_planes==3) ctx->dst_stride[2] = ctx->dst_stride[1];
		if (ctx->nb_planes==4) ctx->dst_stride[3] = ctx->dst_stride[0];


		ctx->w = w;
		ctx->h = h;
		ctx->s_pfmt = pfmt;

		GF_LOG(GF_LOG_INFO, GF_LOG_MEDIA, ("[VFlip] Configured output full frame size %dx%d\n", ctx->w, ctx->h));

		ctx->line_buffer = gf_realloc(ctx->line_buffer, sizeof(char)*ctx->dst_stride[0] );
		ctx->packed_422 = GF_FALSE;
		switch (pfmt) {
		//for YUV 422, adjust to multiple of 2 on horizontal dim
		case GF_PIXEL_YUYV:
		case GF_PIXEL_YVYU:
		case GF_PIXEL_UYVY:
		case GF_PIXEL_VYUY:
			ctx->packed_422 = GF_TRUE;
			break;
		}
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->dst_width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->dst_height));

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(ctx->dst_stride[0] ));
	if (ctx->nb_planes>1)
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE_UV, &PROP_UINT(ctx->dst_stride[1]));

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAR, &PROP_FRAC(sar) );

	return GF_OK;
}

void vflip_finalize(GF_Filter *filter)
{
	GF_VFlipCtx *ctx = gf_filter_get_udta(filter);
	if (ctx->line_buffer) gf_free(ctx->line_buffer);
}


#define OFFS(_n)	#_n, offsetof(GF_VFlipCtx, _n)
static GF_FilterArgs VFlipArgs[] =
{
	{ OFFS(mode), "Sets flip mode", GF_PROP_UINT, "vert", "off|vert|horiz|both", GF_FS_ARG_UPDATE | GF_FS_ARG_HINT_ADVANCED},
	{0}
};

static const GF_FilterCapability VFlipCaps[] =
{
	CAP_UINT(GF_CAPS_INPUT_OUTPUT,GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_UINT(GF_CAPS_INPUT_OUTPUT,GF_PROP_PID_CODECID, GF_CODECID_RAW)
};

GF_FilterRegister VFlipRegister = {
	.name = "vflip",
	GF_FS_SET_DESCRIPTION("Video flip filter")
	GF_FS_SET_HELP("Samir first filter !!")
	.private_size = sizeof(GF_VFlipCtx),
	.flags = GF_FS_REG_EXPLICIT_ONLY,
	.args = VFlipArgs,
	.configure_pid = vflip_configure_pid,
	SETCAPS(VFlipCaps),
	.process = vflip_process,
	.finalize = vflip_finalize,
};



const GF_FilterRegister *vflip_register(GF_FilterSession *session)
{
	return &VFlipRegister;
}
