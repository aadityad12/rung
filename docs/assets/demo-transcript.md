# Demo transcript

Real command output; compile durations and native code sizes omitted. Playback is paced for reading, not execution time.

Captured with rung 0.1.0 on Darwin arm64.

Commands use `rung` for readability; point them at your built or installed binary.

## Tree walker

One program. Two calls. The same answer.

```sh
rung --engine=tree examples/hot_loop.rg
```

stdout:

```text
994657
994657
```

## Stack VM

One program. Two calls. The same answer.

```sh
rung --engine=stack examples/hot_loop.rg
```

stdout:

```text
994657
994657
```

## Register VM

One program. Two calls. The same answer.

```sh
rung --engine=register examples/hot_loop.rg
```

stdout:

```text
994657
994657
```

## ARM64 JIT

One program. Two calls. The same answer.

```sh
rung --engine=jit examples/hot_loop.rg
```

stdout:

```text
994657
994657
```

## Background JIT

One program. Two calls. The same answer.

```sh
rung --engine=jit --jit-background examples/hot_loop.rg
```

stdout:

```text
994657
994657
```

## Stack bytecode

Function excerpt: return a + b;

```sh
rung --engine=stack --dump-bytecode examples/bytecode.rg
```

stdout:

```text
== add (arity 2, upvalues 0) ==
0000    3 GET_LOCAL      1
0002    3 GET_LOCAL      2
0004    3 ADD
0005    3 RETURN
0006    2 NIL
0007    2 RETURN
```

## Register bytecode

Function excerpt: return a + b;

```sh
rung --engine=register --dump-bytecode examples/bytecode.rg
```

stdout:

```text
== add (arity 2, upvalues 0, frame 3) ==
0000    3 ADD            r2 r0 r1
0001    3 RETURN         r2
0002    2 RETURN_NIL
```

## A hot function becomes machine code

Default threshold: first call heats up; second call enters compiled code.

```sh
rung --engine=jit --jit-log examples/hot_loop.rg
```

stderr:

```text
[jit] compiled sum: 10 bytecode instructions
```

stdout:

```text
994657
994657
```

## A float takes the safe path back

Threshold 1 for this example: add(20, 22), then add(20, 2.5).

```sh
rung --engine=jit --jit-threshold=1 --jit-log examples/jit_fallback.rg
```

stderr:

```text
[jit] compiled add: 3 bytecode instructions
[jit] bail-out in add at instruction 0 (ADD, line 4)
```

stdout:

```text
42
22.5
```

## Recursive calls enter native code

fib(20): both recursive calls use the function's own ARM64 entry.

```sh
rung --engine=jit --jit-threshold=1 --jit-log examples/recursion.rg
```

stderr:

```text
[jit] compiled fib: 12 bytecode instructions
```

stdout:

```text
6765
```

## A bailout four calls deep

A float fails a guard in the innermost call; the VM finishes all five frames.

```sh
rung --engine=jit --jit-threshold=1 --jit-log examples/recursive_fallback.rg
```

stderr:

```text
[jit] compiled tally: 13 bytecode instructions
[jit] bail-out in tally at instruction 4 (ADD, line 7)
```

stdout:

```text
7
6.5
```
