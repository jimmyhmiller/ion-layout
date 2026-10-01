int classify(int n) {
  int acc = 0;
  while (n > 0) {
    if (n % 3 == 0) acc += n * 2;
    else if (n % 2 == 0) acc -= n;
    else acc += 1;
    n -= 1;
  }
  return acc;
}
int nested(const int *a, int count) {
  int total = 0;
  for (int i = 0; i < count; i++)
    for (int j = 0; j < i; j++)
      total += a[j] > a[i] ? a[j] : -a[i];
  return total;
}
