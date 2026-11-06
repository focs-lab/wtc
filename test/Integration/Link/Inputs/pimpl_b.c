/* For pimpl.c: the implementation. */
#include <stdlib.h>
#include "pimpl.h"
struct impl { int v; };
void widget_init(struct widget *w) { w->id = 1; w->impl = malloc(sizeof *w->impl); w->impl->v = 41; }
int widget_value(struct widget *w) { return w->id + w->impl->v; }
