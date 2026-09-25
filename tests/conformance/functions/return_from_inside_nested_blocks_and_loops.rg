fn find(limit) {
  for (let i = 0; i < limit; i = i + 1) {
    {
      if (i * i > 50) return i;
    }
  }
  return nil;
}
print find(100); // expect: 8
print find(5); // expect: nil
