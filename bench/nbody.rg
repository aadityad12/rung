// Float arithmetic: the classic five-body solar system, each body an array of floats
// [x, y, z, vx, vy, vz, mass]. Returns the total energy after a fixed number of steps.
// Rung has no sqrt, so `root` is Newton's method; it is part of the work being measured.
fn root(x) {
  let g = x;
  if (x > 1.0) g = x / 2.0;
  let k = 0;
  while (k < 12) {
    g = (g + x / g) / 2.0;
    k = k + 1;
  }
  return g;
}

fn make_bodies() {
  let pi = 3.141592653589793;
  let solar = 4.0 * pi * pi;
  let days = 365.24;
  return [
    [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, solar],
    [4.8414314424647209, -1.16032004402742839, -0.103622044471123109,
     0.00166007664274403694 * days, 0.00769901118419740425 * days,
     -0.0000690460016972063023 * days, 0.000954791938424326609 * solar],
    [8.34336671824457987, 4.12479856412430479, -0.403523417114321381,
     -0.00276742510726862411 * days, 0.00499852801234917238 * days,
     0.0000230417297573763929 * days, 0.000285885980666130812 * solar],
    [12.894369562139131, -15.1111514016986312, -0.223307578892655734,
     0.00296460137564761618 * days, 0.0023784717395948095 * days,
     -0.0000296589568540237556 * days, 0.0000436624404335156298 * solar],
    [15.3796971148509165, -25.9193146099879641, 0.179258772950371181,
     0.00268067772490389322 * days, 0.00162824170038242295 * days,
     -0.000095159225451971587 * days, 0.0000515138902046611451 * solar]
  ];
}

fn advance(bodies, dt) {
  let n = len(bodies);
  let i = 0;
  while (i < n) {
    let a = bodies[i];
    let j = i + 1;
    while (j < n) {
      let b = bodies[j];
      let dx = a[0] - b[0];
      let dy = a[1] - b[1];
      let dz = a[2] - b[2];
      let d2 = dx * dx + dy * dy + dz * dz;
      let mag = dt / (d2 * root(d2));
      a[3] = a[3] - dx * b[6] * mag;
      a[4] = a[4] - dy * b[6] * mag;
      a[5] = a[5] - dz * b[6] * mag;
      b[3] = b[3] + dx * a[6] * mag;
      b[4] = b[4] + dy * a[6] * mag;
      b[5] = b[5] + dz * a[6] * mag;
      j = j + 1;
    }
    i = i + 1;
  }
  i = 0;
  while (i < n) {
    let a = bodies[i];
    a[0] = a[0] + dt * a[3];
    a[1] = a[1] + dt * a[4];
    a[2] = a[2] + dt * a[5];
    i = i + 1;
  }
}

fn energy(bodies) {
  let e = 0.0;
  let n = len(bodies);
  let i = 0;
  while (i < n) {
    let a = bodies[i];
    e = e + 0.5 * a[6] * (a[3] * a[3] + a[4] * a[4] + a[5] * a[5]);
    let j = i + 1;
    while (j < n) {
      let b = bodies[j];
      let dx = a[0] - b[0];
      let dy = a[1] - b[1];
      let dz = a[2] - b[2];
      e = e - a[6] * b[6] / root(dx * dx + dy * dy + dz * dz);
      j = j + 1;
    }
    i = i + 1;
  }
  return e;
}

fn run() {
  let bodies = make_bodies();
  let step = 0;
  while (step < 5000) {
    advance(bodies, 0.01);
    step = step + 1;
  }
  return energy(bodies);
}
