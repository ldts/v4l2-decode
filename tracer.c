
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "tracer.h"
#include "common.h"

int tracer_init(struct instance *i)
{
	i->stats = calloc(STAT_BUFS, sizeof(struct buf_stats));
	if (!i->stats)
		return -1;

	return 0;
}

void tracer_deinit(struct instance *i)
{
	if (i->stats)
		free(i->stats);
}

void tracer_buf_start(struct instance *i, unsigned int type)
{
	struct buf_stats *stats = i->stats;

	if (stats->dqbuf_counter >= STAT_BUFS ||
	    stats->qbuf_counter >= STAT_BUFS)
		return;

	if (stats->dqbuf_counter == 0)
		gettimeofday(&stats->start, NULL);

	if (type == TYPE_DQBUF)
		gettimeofday(&stats[stats->dqbuf_counter].dqbuf_start, NULL);
	else
		gettimeofday(&stats[stats->qbuf_counter].qbuf_start, NULL);
}

void tracer_buf_finish(struct instance *i, unsigned int type)
{
	struct buf_stats *stats = i->stats;

	if (stats->dqbuf_counter >= STAT_BUFS ||
	    stats->qbuf_counter >= STAT_BUFS)
		return;

	if (type == TYPE_DQBUF) {
		gettimeofday(&stats[stats->dqbuf_counter].dqbuf_end, NULL);
		stats->dqbuf_counter++;
	} else {
		gettimeofday(&stats[stats->qbuf_counter].qbuf_end, NULL);
		stats->qbuf_counter++;
	}

	gettimeofday(&stats->end, NULL);
}

void tracer_show(struct instance *inst)
{
	struct buf_stats *stats = inst->stats;
	unsigned long long delta, time, fps;
	unsigned int last = stats->dqbuf_counter - 1;

	delta = (stats[last].dqbuf_end.tv_sec * 1000000 +
		 stats[last].dqbuf_end.tv_usec);

	delta -=(stats[0].dqbuf_end.tv_sec * 1000000 +
		 stats[0].dqbuf_end.tv_usec);

	time = delta / last; /* time per frame in us */
	fps = 1000000 / time;

	fprintf(stdout, "%lld fps (%lld ms)\n", fps, time / 1000);

	time = stats->end.tv_sec * 1000000 + stats->end.tv_usec;
	time -= stats->start.tv_sec * 1000000 + stats->start.tv_usec;

	fprintf(stdout, "total time %lld ms (%lld)\n", time / 1000,
		last * 1000000 / time);
}
