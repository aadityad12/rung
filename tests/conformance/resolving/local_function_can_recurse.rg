{
  fn countdown(n) {
    if (n == 0) return "done";
    return countdown(n - 1);
  }
  print countdown(5); // expect: done
}
