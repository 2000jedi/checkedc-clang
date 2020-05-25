// RUN: cconv-standalone -alltypes %s -- | FileCheck -match-full-lines %s

int *sus(int *x, int*y) {
  int *z = malloc(sizeof(int)*2);
  *z = 1;
  x++;
  *x = 2;
  return z;
}
//CHECK: _Array_ptr<int> sus(int *x : itype(_Array_ptr<int>), _Ptr<int> y) {
//CHECK-NEXT:  _Array_ptr<int> z: count((sizeof(int) * 2)) =  malloc(sizeof(int)*2);

int* foo() {
  int sx = 3, sy = 4, *x = &sx, *y = &sy;
  int *z = sus(x, y);
  *z = *z + 1;
  return z;
}
//CHECK: _Ptr<int> foo(void) {
//CHECK: _Ptr<int> y = &sy;
//CHECK: _Ptr<int> z =  sus(x, y);


int* bar() {
  int sx = 3, sy = 4, *x = &sx, *y = &sy;
  int *z = sus(x, y) + 2;
  *z = -17;
  return z;
}
//CHECK: _Ptr<int> bar(void) {
//CHECK: _Ptr<int> y = &sy;
//CHECK: _Ptr<int> z =  sus(x, y) + 2;

