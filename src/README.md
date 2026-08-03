# RISC-V mmap Read/Write Benchmark

Minimal two-node benchmark for RISC-V Linux.

```text
node A: writer
node B: reader
model : A writes one chunk slot, then B reads that same slot
```

## Input File

The first line is the read/write granularity in bytes. All remaining bytes are the payload.

The whole input file is loaded into memory before timing starts. The timed path only uses the in-memory payload, chunk list, shared lock, and `memcpy`.

```text
4096
payload bytes...
```

## Config

Only these fields are supported:

```text
role = writer
device = /dev/riscv_shared_mem
physical_address = 0x80000000
map_size = 16M
input = /path/to/input.txt
```

Use `role = writer` on node A and `role = reader` on node B.

`physical_address` is used as the `mmap` offset and must be 4K aligned. The two nodes may use different physical addresses if the character device maps both addresses to the same shared memory.

The first 4096 bytes of the mapped region hold synchronization data. The remaining bytes are data slots.

Each chunk uses a different slot. Slot starts are 4K aligned. The next slot offset is calculated from the previous chunk's actual size:

```text
slot_0 = mmap_data_base
slot_i = mmap_data_base + sum(round_up(chunk_j_size, 4096), j = 0..i-1)
```

So `map_size` must cover all slots:

```text
map_size >= 4096 + sum(round_up(chunk_i_size, 4096), i = 0..chunk_count-2) + last_chunk_size
```

## Build

```sh
cd src
make CXX=riscv64-linux-gnu-g++
```

## Run

Start writer first, then reader:

```sh
./rw_mmap_bench writer.conf
./rw_mmap_bench reader.conf
```

The writer initializes the shared header and waits for `reader_ready` before writing chunks.

Timing excludes `reader_ready`, `seq`, and `ack_seq` waits. Each side measures only its per-chunk `memcpy` section and reports the accumulated value:

```text
chunk_memcpy_latency_sum_ns
chunk_memcpy_latency_sum_ms
```

The reader also prints `write_plus_read_memcpy_latency_sum_ns`, which is writer memcpy sum plus reader memcpy sum.

The minimal code assumes coherent visibility for the mapped memory. If the device mapping is non-coherent, cache flush/invalidate must be handled by the platform or driver.
