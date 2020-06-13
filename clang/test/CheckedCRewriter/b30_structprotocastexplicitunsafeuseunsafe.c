// RUN: cconv-standalone %s -- | FileCheck -match-full-lines %s

#define NULL ((void*)0)
extern _Itype_for_any(T) void *calloc(size_t nmemb, size_t size) : itype(_Array_ptr<T>) byte_count(nmemb * size);
extern _Itype_for_any(T) void free(void *pointer : itype(_Array_ptr<T>) byte_count(0));
extern _Itype_for_any(T) void *malloc(size_t size) : itype(_Array_ptr<T>) byte_count(size);
extern _Itype_for_any(T) void *realloc(void *pointer : itype(_Array_ptr<T>) byte_count(1), size_t size) : itype(_Array_ptr<T>) byte_count(size);
extern int printf(const char * restrict format : itype(restrict _Nt_array_ptr<const char>), ...);
extern _Unchecked char *strcpy(char * restrict dest, const char * restrict src : itype(restrict _Nt_array_ptr<const char>));

struct np {
  int x;
  int y;
};

struct p {
  int *x;
  char *y;
};

struct r {
  int data;
  struct r *next;
};

struct r *sus(struct r *, struct r *);
//CHECK: struct r *sus(struct r *x : itype(_Ptr<struct r>), struct r *y : itype(_Ptr<struct r>)) : itype(_Ptr<struct r>);

struct r *foo() {
  struct r *x, *y;
  x->data = 2;
  y->data = 1;
  x->next = &y;
  y->next = &x;
  struct r *z = (struct r *) sus(x, y);
  return z;
}
//CHECK: _Ptr<struct r> foo(void) {

struct np *bar() {
  struct r *x, *y;
  x->data = 2;
  y->data = 1;
  x->next = &y;
  y->next = &x;
  struct np *z = (struct np *) sus(x, y);
  z += 2;
  return z;
}
//CHECK: struct np *bar() {
//CHECK: struct np *z = (struct np *) sus(x, y);

struct r *sus(struct r *x, struct r *y) {
  x->next += 1;
  struct r *z = malloc(sizeof(struct r));
  z->data = 1;
  z->next = 0;
  return z;
}
//CHECK: struct r *sus(struct r *x : itype(_Ptr<struct r>), struct r *y : itype(_Ptr<struct r>)) : itype(_Ptr<struct r>) {
