
#ifndef __TRACER_H__
#define __TRACER_H__

#define TYPE_QBUF	1
#define TYPE_DQBUF	2
#define STAT_BUFS	1000

struct instance;

int tracer_init(struct instance *i);
void tracer_deinit(struct instance *i);
void tracer_buf_start(struct instance *i, unsigned int type);
void tracer_buf_finish(struct instance *i, unsigned int type);
void tracer_show(struct instance *inst);

#endif
