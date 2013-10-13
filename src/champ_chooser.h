#ifndef __CHAMP_CHOOSER_H
#define __CHAMP_CHOOSER_H

struct candidate
{
	char *path;
	int *score;
};

extern int champ_chooser_init(const char *sparse, struct config *conf);

extern int deduplicate(struct blk *blks, struct dpth *dpth, struct config *conf, uint64_t *wrap_up);
extern int deduplicate_maybe(struct blist *blist, struct blk *blk, struct dpth *dpth, struct config *conf, uint64_t *wrap_up);

#endif