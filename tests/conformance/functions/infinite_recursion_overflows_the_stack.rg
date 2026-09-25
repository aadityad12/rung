fn forever() {
  return forever(); // expect runtime error: stack overflow
}
forever();
