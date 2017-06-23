/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Argument parser
 *
 * Copyright 2012 Samsung Electronics Co., Ltd.
 * Copyright (c) 2015 Linaro Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include "common.h"

int debug_level;

void print_usage(char *name)
{
	fprintf(stderr, "usage: %s [OPTS] <URL>\n", name);
	fprintf(stderr, "Where OPTS is a combination of:\n"
		"  -d <device>     video device (default /dev/video0)\n"
		"  -o <directory>  save frames to directory\n"
		"  -k use KMS\n"
		"  -w use wayland/weston\n"
		"\n");
}

int parse_args(struct instance *i, int argc, char **argv)
{
	int c;

	memset(i, 0, sizeof (*i));

	i->video.name = "/dev/video0";

	debug_level = 2;

	while ((c = getopt(argc, argv, "fhd:o:qvkw")) != -1) {
		switch (c) {
		case 'd':
			i->video.name = optarg;
			break;
		case 'o':
			i->save_frames = 1;
			i->save_path = optarg;
			break;
		case 'f':
			i->fullscreen = 1;
			break;
		case 'q':
			debug_level = 0;
			break;
		case 'v':
			debug_level++;
			break;
		case 'k':
			i->use_kms = 1;
			break;
		case 'w':
			i->use_wayland = 1;
			break;
		default:
			err("bad argument\n");
		case 'h':
			return -1;
		}
	}

	if (optind >= argc) {
		err("missing url to play\n");
		return -1;
	}

	i->url = argv[optind];

	return 0;
}

