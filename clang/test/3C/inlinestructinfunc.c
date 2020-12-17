// RUN: 3c -alltypes -addcr %s -- | FileCheck -match-full-lines -check-prefixes="CHECK_ALL","CHECK" %s
// RUN: 3c -addcr %s -- | FileCheck -match-full-lines -check-prefixes="CHECK_NOALL","CHECK" %s
// RUN: 3c -addcr %s -- | %clang -c -fcheckedc-extension -x c -o /dev/null -
// RUN: 3c -output-postfix=checked -alltypes %s
// RUN: 3c -alltypes %S/inlinestructinfunc.checked.c -- | count 0
// RUN: rm %S/inlinestructinfunc.checked.c

void foo(int *x) {
	//CHECK: void foo(_Ptr<int> x) {
  struct bar { int *x; } *y = 0;
	//CHECK: struct bar { _Ptr<int> x; } *y = 0;
} 

void baz(int *x) {
	//CHECK: void baz(_Ptr<int> x) {
  struct bar { char *x; } *w = 0;
	//CHECK: struct bar { _Ptr<char> x; } *w = 0;
} 

