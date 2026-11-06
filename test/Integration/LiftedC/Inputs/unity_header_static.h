/* A header with an include guard and a static of its own, for
   unity_header_static.c. */
#ifndef UNITY_HEADER_STATIC_H
#define UNITY_HEADER_STATIC_H
static int next_id(void) {
  static int id = 0;
  return ++id;
}
#endif
