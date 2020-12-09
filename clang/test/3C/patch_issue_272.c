// RUN: 3c -alltypes %s | FileCheck -match-full-lines %s
// RUN: 3c -alltypes %s | %clang -c -f3c-tool -fcheckedc-extension -x c -o /dev/null -

/********************************************************************/
/* Tests to keep pointer level from                                 */
/* https://github.com/correctcomputation/checkedc-clang/issues/272  */
/********************************************************************/

int *a[];
void b() { a[0][0]; }

// CHECK: _Array_ptr<int> a _Checked[1] = {((void *)0)};
