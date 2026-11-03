/* A header with an include guard and a static of its own, for
   open_unity_header_static.c. */
#ifndef OPEN_UNITY_HEADER_STATIC_H
#define OPEN_UNITY_HEADER_STATIC_H
static int next_id(void) {
  static int id = 0;
  return ++id;
}
#endif
