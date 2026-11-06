/* For pimpl.c: struct impl is complete only in Inputs/pimpl_b.c. */
struct impl;
struct widget { int id; struct impl *impl; };
int widget_value(struct widget *w);
void widget_init(struct widget *w);
