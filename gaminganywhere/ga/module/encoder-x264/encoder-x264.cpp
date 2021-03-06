/*
 * Copyright (c) 2013-2014 Chun-Ying Huang
 *
 * This file is part of GamingAnywhere (GA).
 *
 * GA is free software; you can redistribute it and/or modify it
 * under the terms of the 3-clause BSD License as published by the
 * Free Software Foundation: http://directory.fsf.org/wiki/License:BSD_3Clause
 *
 * GA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the 3-clause BSD License along with GA;
 * if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>

#include "vsource.h"
#include "rtspconf.h"
#include "encoder-common.h"

#include "ga-common.h"
#include "ga-avcodec.h"
#include "ga-conf.h"
#include "ga-module.h"
#include "util.h"
#include "dpipe.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <x264.h>
#ifdef __cplusplus
}
#endif
#define	POOLSIZE		2
static struct RTSPConf *rtspconf = NULL;

static int vencoder_initialized = 0;
static int vencoder_started = 0;
static pthread_t vencoder_tid[VIDEO_SOURCE_CHANNEL_MAX];
static pthread_mutex_t vencoder_reconf_mutex[VIDEO_SOURCE_CHANNEL_MAX];
static ga_ioctl_reconfigure_t vencoder_reconf[VIDEO_SOURCE_CHANNEL_MAX];
//// encoders for encoding
static x264_t* vencoder[VIDEO_SOURCE_CHANNEL_MAX];

// specific data for h.264
static char *_sps[VIDEO_SOURCE_CHANNEL_MAX];
static int _spslen[VIDEO_SOURCE_CHANNEL_MAX];
static char *_pps[VIDEO_SOURCE_CHANNEL_MAX];
static int _ppslen[VIDEO_SOURCE_CHANNEL_MAX];
static char* _vps[VIDEO_SOURCE_CHANNEL_MAX];
static int _vpslen[VIDEO_SOURCE_CHANNEL_MAX];
static bool rc = 0;
static bool recording = 0;
static int frames = 0;
static int fps = 0;
static double bitrate = 0;
static FILE* encoded;
static double sum_enc = 0;

//#define	SAVEENC	"save.264"
//#ifdef SAVEENC
//static FILE *fsaveenc = NULL;
//#endif

static int
vencoder_deinit(void *arg) {
	int iid;
	frames = 0;
	static char* enc_param[VIDEO_SOURCE_CHANNEL_MAX][4];
#define	MAXPARAMLEN	64
	char** pipefmt = (char**)arg;
	static char pipename[VIDEO_SOURCE_CHANNEL_MAX][4][MAXPARAMLEN];
	for (iid = 0; iid < video_source_channels(); iid++) {
		snprintf(pipename[iid][1], MAXPARAMLEN, pipefmt[1], iid);
		snprintf(pipename[iid][2], MAXPARAMLEN, pipefmt[2], iid);
		dpipe_t* frame_rc = dpipe_lookup(pipename[iid][1]);
		if (frame_rc != NULL)
			dpipe_destroy(frame_rc);
		dpipe_t* bits_rc = dpipe_lookup(pipename[iid][2]);
		if (bits_rc != NULL)
			dpipe_destroy(bits_rc);
	}
	ga_error("video encoder: destroyed pipes \n");
#ifdef SAVEENC
	if(fsaveenc != NULL) {
		fclose(fsaveenc);
		fsaveenc = NULL;
	}
#endif
	for(iid = 0; iid < video_source_channels(); iid++) {
		if(_sps[iid] != NULL)
			free(_sps[iid]);
		if(_pps[iid] != NULL)
			free(_pps[iid]);
		if(vencoder[iid] != NULL)
			x264_encoder_close(vencoder[iid]);
		pthread_mutex_destroy(&vencoder_reconf_mutex[iid]);
		vencoder[iid] = NULL;
	}
	bzero(_sps, sizeof(_sps));
	bzero(_pps, sizeof(_pps));
	bzero(_spslen, sizeof(_spslen));
	bzero(_ppslen, sizeof(_ppslen));
	vencoder_initialized = 0;
	ga_error("video encoder: deinitialized.\n");
	return 0;
}

static int /* XXX: we need this because many GA config values are in bits, not Kbits */
ga_x264_param_parse_bit(x264_param_t *params, const char *name, const char *bitvalue) {
	int v = strtol(bitvalue, NULL, 0);
	char kbit[64];
	snprintf(kbit, sizeof(kbit), "%d", v / 1000);
	return x264_param_parse(params, name, kbit);
}

static int
vencoder_init(void *arg) {
	ga_error("Inside X264 !!\n");
	int iid;
	char **pipefmt = (char**) arg;
	struct RTSPConf *rtspconf = rtspconf_global();
	char profile[16], preset[16], tune[16];
	char x264params[1024];
	char tmpbuf[64];
	dpipe_t* dstpipe[VIDEO_SOURCE_CHANNEL_MAX];
	dpipe_t* dstpipe_bitrates[VIDEO_SOURCE_CHANNEL_MAX];
	bzero(dstpipe, sizeof(dstpipe));
	bzero(dstpipe_bitrates, sizeof(dstpipe_bitrates));
	rc = ga_conf_readbool("content-aware", 0);
	//
	if(rtspconf == NULL) {
		ga_error("video encoder: no configuration found\n");
		return -1;
	}
	if(vencoder_initialized != 0)
		return 0;
	//
	frames = 0;
	sum_enc = 0;
	for(iid = 0; iid < video_source_channels(); iid++) {
		char pipename[64], dstpipename[64], dstpipename_bitrates[64];
		int outputW, outputH;
		dpipe_t *pipe;
		x264_param_t params;
		dpipe_buffer_t* data = NULL;
		//
		_sps[iid] = _pps[iid] = NULL;
		_spslen[iid] = _ppslen[iid] = 0;
		pthread_mutex_init(&vencoder_reconf_mutex[iid], NULL);
		vencoder_reconf[iid].id = -1;
		//
		snprintf(pipename, sizeof(pipename), pipefmt[0], iid);
		snprintf(dstpipename, sizeof(dstpipename), pipefmt[1], iid);
		snprintf(dstpipename_bitrates, sizeof(dstpipename_bitrates), pipefmt[2], iid);

		outputW = video_source_out_width(iid);
		outputH = video_source_out_height(iid);
		if(outputW % 4 != 0 || outputH % 4 != 0) {
			ga_error("video encoder: unsupported resolutin %dx%d\n", outputW, outputH);
			goto init_failed;
		}
		if((pipe = dpipe_lookup(pipename)) == NULL) {
			ga_error("video encoder: pipe %s is not found\n", pipename);
			goto init_failed;
		}
		ga_error("video encoder: video source #%d from '%s' (%dx%d).\n",
			iid, pipe->name, outputW, outputH, iid);
		//
		//************************************Start Initializing Buffer for Raw frames going to the RC Module************************
		if (rc) {
			if (dpipe_lookup(dstpipename) == NULL) {
				dstpipe[iid] = dpipe_create(iid, dstpipename, POOLSIZE,
					sizeof(vsource_frame_t) + video_source_mem_size(iid));
				if (dstpipe[iid] == NULL) {
					ga_error("Encoder x264 raw frame: create dst-pipeline failed (%s).\n", dstpipename);
					goto init_failed;
				}
				for (data = dstpipe[iid]->in; data != NULL; data = data->next) {
					if (vsource_frame_init(iid, (vsource_frame_t*)data->pointer) == NULL) {
						ga_error("Encoder x264 raw frame: init frame failed for %s.\n", dstpipename);
						goto init_failed;
					}
				}
				video_source_add_pipename(iid, dstpipename);
			}
			if (dpipe_lookup(dstpipename_bitrates) == NULL) {
				dstpipe_bitrates[iid] = dpipe_create(iid, dstpipename_bitrates, ga_conf_readint("video-fps"),
					sizeof(vsource_frame_t) + video_source_mem_size(iid));
				if (dstpipe_bitrates[iid] == NULL) {
					ga_error("Encoder x264 rates frame: create dst-pipeline failed (%s).\n", dstpipename_bitrates);
					goto init_failed;
				}
				for (data = dstpipe_bitrates[iid]->in; data != NULL; data = data->next) {
					if (vsource_frame_init(iid, (vsource_frame_t*)data->pointer) == NULL) {
						ga_error("Encoder x264 rates frame: init frame failed for %s.\n", dstpipename_bitrates);
						goto init_failed;
					}
				}
				video_source_add_pipename(iid, dstpipename_bitrates);
			}
		}

		//************************************End Initializing Buffer for Raw frames going to the RC Module************************



		bzero(&params, sizeof(params));
		x264_param_default(&params);
		// fill params
		preset[0] = tune[0] = '\0';
		ga_conf_mapreadv("video-specific", "preset", preset, sizeof(preset));
		ga_conf_mapreadv("video-specific", "tune", tune, sizeof(tune));
		if(preset[0] != '\0' || tune[0] != '\0') {
			if(x264_param_default_preset(&params, preset, tune) < 0) {
				ga_error("video encoder: bad x264 preset=%s; tune=%s\n", preset, tune);
				goto init_failed;
			} else {
				ga_error("video encoder: x264 preset=%s; tune=%s\n", preset, tune); 
			}
		}
		//
		//if(ga_conf_readbool("content-aware", 0) != 0){
		//	x264_param_parse(&params, "aq-mode", "1");			
		//	x264_param_parse(&params, "qp", "22");			
		//}
		//if (rc) {
			bitrate = ga_conf_mapreadint("video-specific", "b");
			fps = ga_conf_readint("video-fps");
			if (x264_param_parse(&params, "qp", "22") < 0)
				ga_error("video encoder: warning - bad x264 param [%s=%s]\n", "qp", "22");;
			params.rc.f_ip_factor = pow(2.0, 1.0 / 12.0);//to make sure that I frames have same QP as P frames			
			if (x264_param_parse(&params, "aq-mode", "1") < 0)
				ga_error("video encoder: warning - bad x264 param [%s=%s]\n", "aq-mode", "1");;
		//}
		//else if(ga_conf_mapreadv("video-specific", "b", tmpbuf, sizeof(tmpbuf)) != NULL)
		//else {
			if (ga_conf_mapreadv("video-specific", "b", tmpbuf, sizeof(tmpbuf)) != NULL)
				ga_x264_param_parse_bit(&params, "bitrate", tmpbuf);
			if (ga_conf_mapreadv("video-specific", "crf", tmpbuf, sizeof(tmpbuf)) != NULL)
				x264_param_parse(&params, "crf", tmpbuf);
			if (ga_conf_mapreadv("video-specific", "vbv-init", tmpbuf, sizeof(tmpbuf)) != NULL)
				x264_param_parse(&params, "vbv-init", tmpbuf);
			if (ga_conf_mapreadv("video-specific", "maxrate", tmpbuf, sizeof(tmpbuf)) != NULL)
				ga_x264_param_parse_bit(&params, "vbv-maxrate", tmpbuf);
			if (ga_conf_mapreadv("video-specific", "bufsize", tmpbuf, sizeof(tmpbuf)) != NULL)
				ga_x264_param_parse_bit(&params, "vbv-bufsize", tmpbuf);
		//}
		if(ga_conf_mapreadv("video-specific", "refs", tmpbuf, sizeof(tmpbuf)) != NULL)
			x264_param_parse(&params, "ref", tmpbuf);
		if(ga_conf_mapreadv("video-specific", "me_method", tmpbuf, sizeof(tmpbuf)) != NULL)
			x264_param_parse(&params, "me", tmpbuf);
		if(ga_conf_mapreadv("video-specific", "me_range", tmpbuf, sizeof(tmpbuf)) != NULL)
			x264_param_parse(&params, "merange", tmpbuf);
		if(ga_conf_mapreadv("video-specific", "g", tmpbuf, sizeof(tmpbuf)) != NULL)
			x264_param_parse(&params, "keyint", tmpbuf);
		if(ga_conf_mapreadv("video-specific", "intra-refresh", tmpbuf, sizeof(tmpbuf)) != NULL)
			x264_param_parse(&params, "intra-refresh", tmpbuf);
		//
		x264_param_parse(&params, "bframes", "0");
		//x264_param_apply_fastfirstpass(&params);
		if(ga_conf_mapreadv("video-specific", "profile", profile, sizeof(profile)) != NULL) {
			if(x264_param_apply_profile(&params, profile) < 0) {
				ga_error("video encoder: x264 - bad profile %s\n", profile);
				goto init_failed;
			}
		}
		//
		if(ga_conf_readv("video-fps", tmpbuf, sizeof(tmpbuf)) != NULL)
			x264_param_parse(&params, "fps", tmpbuf);
		if(ga_conf_mapreadv("video-specific", "threads", tmpbuf, sizeof(tmpbuf)) != NULL)
			x264_param_parse(&params, "threads", tmpbuf);
		if(ga_conf_mapreadv("video-specific", "slices", tmpbuf, sizeof(tmpbuf)) != NULL)
			x264_param_parse(&params, "slices", tmpbuf);
		//
		params.i_log_level = X264_LOG_INFO;
		params.i_csp = X264_CSP_I420;
		params.i_width  = outputW;
		params.i_height = outputH;
		//params.vui.b_fullrange = 1;
		params.b_repeat_headers = 1;
		params.b_annexb = 1;
		// handle x264-params
		if(ga_conf_mapreadv("video-specific", "x264-params", x264params, sizeof(x264params)) != NULL) {
			char *saveptr, *value;
			char *name = strtok_r(x264params, ":", &saveptr);
			while(name != NULL) {
				if((value = strchr(name, '=')) != NULL) {
					*value++ = '\0';
				}
				if(x264_param_parse(&params, name, value) < 0) {
					ga_error("video encoder: warning - bad x264 param [%s=%s]\n", name, value);
				}
				name = strtok_r(NULL, ":", &saveptr);
			}
		}
		//
		vencoder[iid] = x264_encoder_open(&params);
		if(vencoder[iid] == NULL)
			goto init_failed;
		ga_error("video encoder: opened! bitrate=%dKbps; me_method=%d; me_range=%d; refs=%d; g=%d; intra-refresh=%d; width=%d; height=%d; crop=%d,%d,%d,%d; threads=%d; slices=%d; repeat-hdr=%d; annexb=%d\n",
			params.rc.i_bitrate,
			params.analyse.i_me_method, params.analyse.i_me_range,
			params.i_frame_reference,
			params.i_keyint_max,
			params.b_intra_refresh,
			params.i_width, params.i_height,
			params.crop_rect.i_left, params.crop_rect.i_top,
			params.crop_rect.i_right, params.crop_rect.i_bottom,
			params.i_threads, params.i_slice_count,
			params.b_repeat_headers, params.b_annexb);
	}
#ifdef SAVEENC
	fsaveenc = fopen(SAVEENC, "wb");
#endif
	vencoder_initialized = 1;
	ga_error("video encoder: initialized.\n");
	return 0;
init_failed:
	if (rc) {
		for (iid = 0; iid < video_source_channels(); iid++) {
			if (dstpipe[iid] != NULL)
				dpipe_destroy(dstpipe[iid]);
			dstpipe[iid] = NULL;
		}
		for (iid = 0; iid < video_source_channels(); iid++) {
			if (dstpipe_bitrates[iid] != NULL)
				dpipe_destroy(dstpipe_bitrates[iid]);
			dstpipe_bitrates[iid] = NULL;
		}
	}
	vencoder_deinit(NULL);
	return -1;
}

static int
vencoder_reconfigure(int iid) {
	int ret = 0;
	x264_param_t params;
	x264_t *encoder = vencoder[iid];
	ga_ioctl_reconfigure_t *reconf = &vencoder_reconf[iid];
	//
	pthread_mutex_lock(&vencoder_reconf_mutex[iid]);
	if(vencoder_reconf[iid].id >= 0) {
		int doit = 0;
		x264_encoder_parameters(encoder, &params);
		//
		if(reconf->crf > 0){// && !rc) {
			params.rc.f_rf_constant = 1.0 * reconf->crf;
			doit++;
		}
		if(reconf->framerate_n > 0){// && !rc) {
			params.i_fps_num = reconf->framerate_n;
			params.i_fps_den = reconf->framerate_d > 0 ? reconf->framerate_d : 1;
			doit++;
		}
		if(reconf->bitrateKbps > 0){// && !rc) {
			// XXX: do not use x264_param_parse("bitrate"), it switches mode to ABR
			// - although mode switching may be not allowed
			params.rc.i_bitrate = reconf->bitrateKbps;
			params.rc.i_vbv_max_bitrate = reconf->bitrateKbps;
			doit++;
		}
		if(reconf->bufsize > 0){// && !rc) {
			params.rc.i_vbv_buffer_size = reconf->bufsize;
			doit++;
		}
		//if (rc) {
			//ga_error("force flush:%d\n", params.forceFlush);
			//params.forceFlush = 1;
		//	doit++;
		//}
		//
		if(doit > 0) {
			if(x264_encoder_reconfig(encoder, &params) < 0) {
				ga_error("video encoder: reconfigure failed. crf=%d; framerate=%d/%d; bitrate=%d; bufsize=%d.\n",
						reconf->crf,
						reconf->framerate_n, reconf->framerate_d,
						reconf->bitrateKbps,
						reconf->bufsize);
				ret = -1;
			} else {
				ga_error("video encoder: reconfigured. crf=%.2f; framerate=%d/%d; bitrate=%d/%dKbps; bufsize=%dKbit.\n",
						params.rc.f_rf_constant,
						params.i_fps_num, params.i_fps_den,
						params.rc.i_bitrate, params.rc.i_vbv_max_bitrate,
						params.rc.i_vbv_buffer_size);
			}
		}
		reconf->id = -1;
	}
	pthread_mutex_unlock(&vencoder_reconf_mutex[iid]);
	return ret;
}

static void *
vencoder_threadproc(void *arg) {
	// arg is pointer to source pipename
	int iid, outputW, outputH;
	vsource_frame_t *frame = NULL;
	//char *pipename = (char*) arg;
	char** pipename = (char**)arg;
	//dpipe_t *pipe = dpipe_lookup(pipename);
	//ga_error("start x264 thread\n");
	//ga_error("source name from x265 thread:%s\n",pipename[0]);
	dpipe_t* pipe = dpipe_lookup(pipename[0]);

	//ga_error("after source pipe lookup\n");
	//For the content-aware rate control module 
	dpipe_t* dstpipe = dpipe_lookup(pipename[1]);
	dpipe_buffer_t* dstdata = NULL;
	vsource_frame_t* dstframe = NULL;
	//ga_error("after dest pipe lookup\n");
	dpipe_t* dstpipe_bitrates = dpipe_lookup(pipename[2]);
	dpipe_buffer_t* dstdata_bitrates = NULL;
	vsource_frame_t* dstframe_bitrates = NULL;
	//ga_error("after bitrates dest pipe lookup\n");
	dpipe_t* pipe_qp = dpipe_lookup(pipename[3]);
	dpipe_buffer_t* pipedata_qp = NULL;
	vsource_frame_t* pipeframe_qp = NULL;

	dpipe_buffer_t *data = NULL;
	x264_t *encoder = NULL;
	//
	long long basePts = -1LL, newpts = 0LL, pts = -1LL, ptsSync = 0LL;
	pthread_mutex_t condMutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	//
	unsigned char *pktbuf = NULL;
	int pktbufsize = 0, pktbufmax = 0;
	int video_written = 0;
	int64_t x264_pts = 0;
	//
	if(pipe == NULL) {
		ga_error("video encoder: invalid pipeline specified (%s).\n", pipename);
		goto video_quit;
	}
	if (rc && dstpipe == NULL) {
		ga_error("video encoder: invalid RC pipeline specified (%s).\n", pipename[1]);
		goto video_quit;
	}

	if (rc && dstpipe_bitrates == NULL) {
		ga_error("video encoder: invalid RC pipeline specified (%s).\n", pipename[2]);
		goto video_quit;
	}

	if (rc && pipe_qp == NULL) {
		ga_error("video encoder: invalid qp pipeline specified (%s).\n", pipename[3]);
		goto video_quit;
	}
	//
	//ga_error("before rtsp conf global\n");
	//
	rtspconf = rtspconf_global();
	// init variables
	iid = pipe->channel_id;
	encoder = vencoder[iid];
	x264_param_t params;
	//ga_error("before getting params\n");
	x264_encoder_parameters(encoder, &params);
	//ga_error("after getting params\n");
	//
	//
	outputW = video_source_out_width(iid);
	outputH = video_source_out_height(iid);
	pktbufmax = outputW * outputH * 2;
	if((pktbuf = (unsigned char*) malloc(pktbufmax)) == NULL) {
		ga_error("video encoder: allocate memory failed.\n");
		goto video_quit;
	}
	// start encoding
	ga_error("video encoding started: tid=%ld %dx%d@%dfps.\n",
		ga_gettid(),
		outputW, outputH, rtspconf->video_fps);
	//
	while(vencoder_started != 0 && encoder_running() > 0) {
		x264_picture_t pic_in, pic_out = {0};
		x264_nal_t *nal;
		int i, size, nnal;
		struct timeval tv;
		struct timespec to;
		gettimeofday(&tv, NULL);
		// need reconfigure?
		vencoder_reconfigure(iid);
		// wait for notification
		to.tv_sec = tv.tv_sec+1;
		to.tv_nsec = tv.tv_usec * 1000;
		data = dpipe_load(pipe, &to);
		if(data == NULL) {
			ga_error("viedo encoder: image source timed out.\n");
			continue;
		}
		frame = (vsource_frame_t*) data->pointer;
		// handle pts
		if(basePts == -1LL) {
			basePts = frame->imgpts;
			ptsSync = encoder_pts_sync(rtspconf->video_fps);
			newpts = ptsSync;
		} else {
			newpts = ptsSync + frame->imgpts - basePts;
		}
		//
		x264_picture_init(&pic_in);
		//x264_picture_init(&params, &pic_in);
		//
		pic_in.img.i_csp = X264_CSP_I420;
		//pic_in.img.i_plane = 3;
		pic_in.img.i_stride[0] = frame->linesize[0];
		pic_in.img.i_stride[1] = frame->linesize[1];
		pic_in.img.i_stride[2] = frame->linesize[2];
		pic_in.img.plane[0] = frame->imgbuf;
		pic_in.img.plane[1] = pic_in.img.plane[0] + outputW*outputH;
		pic_in.img.plane[2] = pic_in.img.plane[1] + ((outputW * outputH) >> 2);
		if (rc) {

			//ga_error("duplicating raw frame\n");
			dstdata = dpipe_get(dstpipe);
			dstframe = (vsource_frame_t*)dstdata->pointer;
			vsource_dup_frame(frame, dstframe);
			dpipe_store(dstpipe, dstdata);
			//this is where the rc module should execute and return an array of QP offsets
			//ga_error("getting QP offsets\n");
			pipedata_qp = dpipe_load(pipe_qp, NULL);
			//ga_error("reading qps\n");
			pipeframe_qp = (vsource_frame_t*)pipedata_qp->pointer;
			//ga_error("UPDATING ROIS \n");
			pic_in.prop.quant_offsets = (float*)pipeframe_qp->imgbuf;
			//pic_in.quantOffsets = (float*)pipeframe_qp->imgbuf;
			/*unsigned int block_ind=0;
			for(int x=0;x< heightDelta;x++){
				for(int y=0;y< widthDelta;y++){
					ga_error("QP of %d,%d is %f\n",x,y,pic_in.prop.quant_offsets[block_ind++]);
				}
			}*/
			//ga_error("read qps\n");						
		}
		// pts must be monotonically increasing
		if(newpts > pts) {
			pts = newpts;
		} else {
			pts++;
		}
		//pic_in.i_pts = pts;
		pic_in.i_pts = x264_pts++;
		// encode
		//ga_error("before encoding\n");	
		clock_t begin_enc = clock();
		if((size = x264_encoder_encode(encoder, &nal, &nnal, &pic_in, &pic_out)) < 0) {
			ga_error("video encoder: encode failed, err = %d\n", size);
			dpipe_put(pipe, data);
			if (rc)
				dpipe_put(pipe_qp, pipedata_qp);
			break;
		}
		clock_t end_enc = clock();
		frames++;
		if (frames % TIME_REPORT == 0) {
			ga_error("average run encoder %d is %.25f ms sum is %.15f\n", frames / TIME_REPORT, sum_enc / TIME_REPORT, sum_enc);
			sum_enc = 0;
		}
		sum_enc = sum_enc + diffclock(end_enc, begin_enc);

		//ga_error("after encoding\n");
		dpipe_put(pipe, data);
		// encode
		if (rc && pipedata_qp != NULL) {
			dpipe_put(pipe_qp, pipedata_qp);
		}

		if(size > 0) {
			AVPacket pkt;
#if 1
			av_init_packet(&pkt);
			pkt.pts = pic_in.i_pts;
			pkt.stream_index = 0;
			// concatenate nals
			pktbufsize = 0;
			for(i = 0; i < nnal; i++) {
				if(pktbufsize + nal[i].i_payload > pktbufmax) {
					ga_error("video encoder: nal dropped (%d < %d).\n", i+1, nnal);
					break;
				}
				bcopy(nal[i].p_payload, pktbuf + pktbufsize, nal[i].i_payload);
				pktbufsize += nal[i].i_payload;
			}
			if (rc) {
				//ga_error("storing frame %d size %d \n",frames,pktbufsize);
				dstdata_bitrates = dpipe_get(dstpipe_bitrates);
				dstframe_bitrates = (vsource_frame_t*)dstdata_bitrates->pointer;
				dstframe_bitrates->pixelformat = AV_PIX_FMT_RGBA;	//yuv420p;
				dstframe_bitrates->realwidth = 1;
				dstframe_bitrates->realheight = 1;
				dstframe_bitrates->realstride = 1;
				dstframe_bitrates->realsize = 1;
				*((float*)dstframe_bitrates->imgbuf) = (float)pktbufsize;
				dpipe_store(dstpipe_bitrates, dstdata_bitrates);
				//ga_error("frame size stored\n");
			}
			pkt.size = pktbufsize;
			pkt.data = pktbuf;
#if 0			// XXX: dump naltype
			do {
				int codelen;
				unsigned char *ptr;
				fprintf(stderr, "[XXX-naldump]");
				for(	ptr = ga_find_startcode(pkt.data, pkt.data+pkt.size, &codelen);
					ptr != NULL;
					ptr = ga_find_startcode(ptr+codelen, pkt.data+pkt.size, &codelen)) {
					//
					fprintf(stderr, " (+%d|%d)-%02x", ptr-pkt.data, codelen, ptr[codelen] & 0x1f);
				}
				fprintf(stderr, "\n");
			} while(0);
#endif
			// send the packet
			if(encoder_send_packet("video-encoder",
					iid/*rtspconf->video_id*/, &pkt,
					pkt.pts, NULL) < 0) {
				goto video_quit;
			}
#ifdef SAVEENC
			if(fsaveenc != NULL)
				fwrite(pkt.data, sizeof(char), pkt.size, fsaveenc);
#endif
#else
			// handling special nals (type > 5)
			for(i = 0; i < nnal; i++) {
				unsigned char *ptr;
				int offset;
				if((ptr = ga_find_startcode(nal[i].p_payload, nal[i].p_payload + nal[i].i_payload, &offset))
				!= nal[i].p_payload) {
					ga_error("video encoder: no startcode found for nals\n");
					goto video_quit;
				}
				if((*(ptr+offset) & 0x1f) <= 5)
					break;
				av_init_packet(&pkt);
				pkt.pts = pic_in.i_pts;
				pkt.stream_index = 0;
				pkt.size = nal[i].i_payload;
				pkt.data = ptr;
				if(encoder_send_packet("video-encoder",
					iid/*rtspconf->video_id*/, &pkt, pkt.pts, NULL) < 0) {
					goto video_quit;
				}
#ifdef SAVEENC
				if(fsaveenc != NULL)
					fwrite(pkt.data, sizeof(char), pkt.size, fsaveenc);
#endif
			}
			// handling video frame data
			pktbufsize = 0;
			for(; i < nnal; i++) {
				if(pktbufsize + nal[i].i_payload > pktbufmax) {
					ga_error("video encoder: nal dropped (%d < %d).\n", i+1, nnal);
					break;
				}
				bcopy(nal[i].p_payload, pktbuf + pktbufsize, nal[i].i_payload);
				pktbufsize += nal[i].i_payload;
			}
			if(pktbufsize > 0) {
				av_init_packet(&pkt);
				pkt.pts = pic_in.i_pts;
				pkt.stream_index = 0;
				pkt.size = pktbufsize;
				pkt.data = pktbuf;
				if(encoder_send_packet("video-encoder",
					iid/*rtspconf->video_id*/, &pkt, pkt.pts, NULL) < 0) {
					goto video_quit;
				}
#ifdef SAVEENC
				if(fsaveenc != NULL)
					fwrite(pkt.data, sizeof(char), pkt.size, fsaveenc);
#endif
			}
#endif
			// free unused side-data
			if(pkt.side_data_elems > 0) {
				int i;
				for (i = 0; i < pkt.side_data_elems; i++)
					av_free(pkt.side_data[i].data);
				av_freep(&pkt.side_data);
				pkt.side_data_elems = 0;
			}
			//
			if(video_written == 0) {
				video_written = 1;
				ga_error("first video frame written (pts=%lld)\n", pic_in.i_pts);
			}
		}
	}
	//
video_quit:
	if(pipe) {
		pipe = NULL;
	}
	if(pktbuf != NULL) {
		free(pktbuf);
	}
	pktbuf = NULL;
	//
	ga_error("video encoder: thread terminated (tid=%ld).\n", ga_gettid());
	//
	return NULL;
}

/*
static int
vencoder_start(void *arg) {
	int iid;
	char **pipefmt = (char**) arg;
#define	MAXPARAMLEN	64
	static char pipename[VIDEO_SOURCE_CHANNEL_MAX][MAXPARAMLEN];
	if(vencoder_started != 0)
		return 0;
	vencoder_started = 1;
	for(iid = 0; iid < video_source_channels(); iid++) {
		snprintf(pipename[iid], MAXPARAMLEN, pipefmt[0], iid);
		if(pthread_create(&vencoder_tid[iid], NULL, vencoder_threadproc, pipename[iid]) != 0) {
			vencoder_started = 0;
			ga_error("video encoder: create thread failed.\n");
			return -1;
		}
	}
	ga_error("video encdoer: all started (%d)\n", iid);
	return 0;
}
*/
static int
vencoder_start(void* arg) {
	int iid;
	char** pipefmt = (char**)arg;
	static char* enc_param[VIDEO_SOURCE_CHANNEL_MAX][4];
#define	MAXPARAMLEN	64
	static char pipename[VIDEO_SOURCE_CHANNEL_MAX][4][MAXPARAMLEN];
	if (vencoder_started != 0)
		return 0;
	vencoder_started = 1;
	for (iid = 0; iid < video_source_channels(); iid++) {
		snprintf(pipename[iid][0], MAXPARAMLEN, pipefmt[0], iid);
		//ga_error("source:%s\n",pipename[iid][0]);
		snprintf(pipename[iid][1], MAXPARAMLEN, pipefmt[1], iid);
		//ga_error("dest:%s\n",pipename[iid][1]);
		snprintf(pipename[iid][2], MAXPARAMLEN, pipefmt[2], iid);
		//ga_error("dest_bitrates:%s\n",pipename[iid][2]);
		snprintf(pipename[iid][3], MAXPARAMLEN, pipefmt[3], iid);
		//ga_error("src_qps:%s\n",pipename[iid][3]);
		enc_param[iid][0] = pipename[iid][0];
		enc_param[iid][1] = pipename[iid][1];
		enc_param[iid][2] = pipename[iid][2];
		enc_param[iid][3] = pipename[iid][3];
		if (pthread_create(&vencoder_tid[iid], NULL, vencoder_threadproc, enc_param[iid]) != 0) {
			vencoder_started = 0;
			ga_error("video encoder: create thread failed.\n");
			return -1;
		}
	}
	ga_error("video encdoer: all started (%d)\n", iid);
	return 0;
}

static int
vencoder_stop(void *arg) {
	int iid;
	void *ignored;
	if(vencoder_started == 0)
		return 0;
	vencoder_started = 0;
	for(iid = 0; iid < video_source_channels(); iid++) {
		pthread_join(vencoder_tid[iid], &ignored);
	}
	ga_error("video encdoer: all stopped (%d)\n", iid);
	return 0;
}

static void *
vencoder_raw(void *arg, int *size) {
#if defined __APPLE__
	int64_t in = (int64_t) arg;
	int iid = (int) (in & 0xffffffffLL);
#elif defined __x86_64__
	int iid = (long long) arg;
#else
	int iid = (int) arg;
#endif
	if(vencoder_initialized == 0)
		return NULL;
	if(size)
		*size = sizeof(vencoder[iid]);
	return vencoder[iid];
}

static int
x264_reconfigure(ga_ioctl_reconfigure_t *reconf) {
	if(vencoder_started == 0 || encoder_running() == 0) {
		ga_error("video encoder: reconfigure - not running.\n");
		return 0;
	}
	pthread_mutex_lock(&vencoder_reconf_mutex[reconf->id]);
	bcopy(reconf, &vencoder_reconf[reconf->id], sizeof(ga_ioctl_reconfigure_t));
	pthread_mutex_unlock(&vencoder_reconf_mutex[reconf->id]);
	return 0;
}

static int
x264_get_sps_pps(int iid) {
	x264_nal_t *p_nal;
	int ret = 0;
	int i, i_nal;
	// alread obtained?
	if(_sps[iid] != NULL)
		return 0;
	//
	if(vencoder_initialized == 0)
		return GA_IOCTL_ERR_NOTINITIALIZED;
	if(x264_encoder_headers(vencoder[iid], &p_nal, &i_nal) < 0)
		return GA_IOCTL_ERR_NOTFOUND;
	for(i = 0; i < i_nal; i++) {
		if(p_nal[i].i_type == NAL_SPS) {
			if((_sps[iid] = (char*) malloc(p_nal[i].i_payload)) == NULL) {
				ret = GA_IOCTL_ERR_NOMEM;
				break;
			}
			bcopy(p_nal[i].p_payload, _sps[iid], p_nal[i].i_payload);
			_spslen[iid] = p_nal[i].i_payload;
		} else if(p_nal[i].i_type == NAL_PPS) {
			if((_pps[iid] = (char*) malloc(p_nal[i].i_payload)) == NULL) {
				ret = GA_IOCTL_ERR_NOMEM;
				break;
			}
			bcopy(p_nal[i].p_payload, _pps[iid], p_nal[i].i_payload);
			_ppslen[iid] = p_nal[i].i_payload;
		}
	}
	//
	if(_sps[iid] == NULL || _pps[iid] == NULL) {
		if(_sps[iid])	free(_sps[iid]);
		if(_pps[iid])	free(_pps[iid]);
		_sps[iid] = _pps[iid] = NULL;
		_spslen[iid] = _ppslen[iid] = 0;
	} else {
		ga_error("video encoder: found sps (%d bytes); pps (%d bytes)\n",
			_spslen[iid], _ppslen[iid]);
	}
	return ret;
}

static int
vencoder_ioctl(int command, int argsize, void *arg) {
	int ret = 0;
	ga_ioctl_buffer_t *buf = (ga_ioctl_buffer_t*) arg;
	//
	if(vencoder_initialized == 0)
		return GA_IOCTL_ERR_NOTINITIALIZED;
	//
	switch(command) {
	case GA_IOCTL_RECONFIGURE:
		if(argsize != sizeof(ga_ioctl_reconfigure_t))
			return GA_IOCTL_ERR_INVALID_ARGUMENT;
		x264_reconfigure((ga_ioctl_reconfigure_t*) arg);
		break;
	case GA_IOCTL_GETSPS:
		if(argsize != sizeof(ga_ioctl_buffer_t))
			return GA_IOCTL_ERR_INVALID_ARGUMENT;
		if(x264_get_sps_pps(buf->id) < 0)
			return GA_IOCTL_ERR_NOTFOUND;
		if(buf->size < _spslen[buf->id])
			return GA_IOCTL_ERR_BUFFERSIZE;
		buf->size = _spslen[buf->id];
		bcopy(_sps[buf->id], buf->ptr, buf->size);
		break;
	case GA_IOCTL_GETPPS:
		if(argsize != sizeof(ga_ioctl_buffer_t))
			return GA_IOCTL_ERR_INVALID_ARGUMENT;
		if(x264_get_sps_pps(buf->id) < 0)
			return GA_IOCTL_ERR_NOTFOUND;
		if(buf->size < _ppslen[buf->id])
			return GA_IOCTL_ERR_BUFFERSIZE;
		buf->size = _ppslen[buf->id];
		bcopy(_pps[buf->id], buf->ptr, buf->size);
		break;
	default:
		ret = GA_IOCTL_ERR_NOTSUPPORTED;
		break;
	}
	return ret;
}

ga_module_t *
module_load() {
	static ga_module_t m;
	//
	bzero(&m, sizeof(m));
	m.type = GA_MODULE_TYPE_VENCODER;
	m.name = strdup("x264-video-encoder");
	m.mimetype = strdup("video/H264");
	m.init = vencoder_init;
	m.start = vencoder_start;
	//m.threadproc = vencoder_threadproc;
	m.stop = vencoder_stop;
	m.deinit = vencoder_deinit;
	//
	m.raw = vencoder_raw;
	m.ioctl = vencoder_ioctl;
	return &m;
}

