// A string is a heap object too, so it passes the object-tag guard; the guard on the object's
// kind byte is what sends it back to the VM.
fn get(a, i) {
  return a[i]; // expect runtime error: can only index arrays
}
print get(["x", "y"], 0); // expect: x
print get("xy", 0);
