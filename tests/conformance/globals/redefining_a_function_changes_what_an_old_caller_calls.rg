// `caller` looks `target` up by name at every call, so redefining `target` redirects it.
fn target() { return "old"; }
fn caller() { return target(); }
print caller(); // expect: old
fn target() { return "new"; }
print caller(); // expect: new
