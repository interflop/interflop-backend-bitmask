# interflop-backend-bitmask

## Arguments

The Bitmask backend implements a fast first order model of noise. It
relies on bitmask operations to achieve low overhead. Unlike MCA backends,
the introduced noise is biased, which means that the expected value of the noise
is not equal to 0 as explained in [Chatelain's thesis, section 2.3.2](https://tel.archives-ouvertes.fr/tel-02473301/document).

```
VFC_BACKENDS="libinterflop_bitmask.so --help" ./test
test: verificarlo loaded backend libinterflop_bitmask.so
Usage: libinterflop_bitmask.so [OPTION...]

  -m, --mode=MODE            select BITMASK mode among {ieee, full, ib, ob}
  -o, --operator=OPERATOR    select BITMASK operator among {zero, one, rand}
      --precision-binary32=PRECISION
                             select precision for binary32 (PRECISION > 0)
      --precision-binary64=PRECISION
                             select precision for binary64 (PRECISION > 0)
  -d, --daz                  denormals-are-zero: sets denormals inputs to zero
  -f, --ftz                  flush-to-zero: sets denormal output to zero
  -s, --seed=SEED            fix the random generator seed
  -?, --help                 Give this help list
      --usage                Give a short usage message
```

Three options control the behavior of the Bitmask backend.

The option `--mode=MODE` controls the arithmetic error mode. It
accepts the following case insensitive values:

* `ieee`: the program uses the standard IEEE arithmetic, no errors are introduced
* `ib`: InBound precision errors only
* `ob`: OutBound precision errors only (*default mode*)
* `full`: InBound and OutBound modes combined

The option `--operator=OPERATOR` controls the bitmask operator to
apply. It accepts the following case insensitive values:

* `zero`: sets the last `t` bits of the mantissa to 0
* `one`: sets the last `t` bits of the mantissa to 1
* `rand`: applies a XOR of random bits to the last `t` bits of the mantissa (default mode)

Modes `zero` and `one` are deterministic and require only one
execution.  The `rand` mode is random and must be used like `mca`
backends.

The option `--precision-binary64=PRECISION` controls the virtual
precision used for the floating point operations in double precision
(respectively for single precision with --precision-binary32) It
accepts an integer value that represents the virtual precision at
which MCA operations are performed. Its default value is 53 for
binary64 and 24 for binary32. For the Bitmask backend, the virtual
precision corresponds to the number of preserved bits in the mantissa.

The option `--seed` fixes the random generator seed. It should not
generally be used except to reproduce a particular Bitmask
trace.
