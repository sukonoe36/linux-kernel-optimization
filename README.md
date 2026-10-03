# linux-kernel-optimization

Tooling for measuring and tuning Linux kernel behaviour: CO-RE eBPF tracers for
TCP retransmissions and block I/O latency, commented sysctl tuning profiles with
a safe apply/revert script, and a repeatable benchmarking methodology.

> **No performance numbers live in this repository.** Any claim about a tuning
> change must be produced on your own hardware with the scripts in `bench/` and
> reported using the rules in [`docs/METHODOLOGY.md`](docs/METHODOLOGY.md).

## Layout

```
ebpf/
  tcp_retrans/     Trace TCP retransmits, aggregated per destination address
  biolatency/      Block I/O latency histogram (log2 microseconds)
tuning/
  network-latency.conf   sysctl profile for latency-sensitive workloads
  high-throughput.conf   sysctl profile for bulk/throughput workloads
  apply-profile.sh       Apply a profile, backing up prior values; --revert
bench/
  common.sh              Shared helpers (run dirs, metadata, logging)
  run-iperf3.sh          Capture iperf3 network measurements
  run-fio.sh             Capture fio block I/O measurements
  run-perf-stat.sh       Capture perf stat counters for a command
systemd/
  tuning.service         Apply the selected profile at boot
docs/
  METHODOLOGY.md         Warmup, repeats, and p99 reporting rules
.github/workflows/ci.yml shellcheck + eBPF compile check
```

## Requirements

- Linux with BTF (`/sys/kernel/btf/vmlinux`) for CO-RE.
- Kernel 5.8+ recommended.
- `clang`, `llvm`, `bpftool`, `libbpf`, `libelf`, `zlib` for building the eBPF
  programs and their userspace loaders.
- `root` (or `CAP_BPF` + `CAP_PERFMON`) to load and attach BPF programs.
- For benchmarks: `iperf3`, `fio`, `perf` (linux-tools), `sysstat` (optional).

## Quick start

Build a tracer:

```bash
cd ebpf/tcp_retrans
make            # generates vmlinux.h, compiles the .bpf.o and the loader
sudo ./tcp_retrans -i 2 -n 10
```

Block I/O latency histogram, optionally filtered to one device:

```bash
cd ebpf/biolatency
make
sudo ./biolatency --device 259:0
```

Apply a sysctl profile (records the previous values first):

```bash
sudo tuning/apply-profile.sh --profile tuning/network-latency.conf
sudo tuning/apply-profile.sh --revert
```

Run a before/after comparison:

```bash
bench/run-iperf3.sh --server 10.0.0.5 --phase before
sudo tuning/apply-profile.sh --profile tuning/network-latency.conf
bench/run-iperf3.sh --server 10.0.0.5 --phase after
```

Results are written under `bench/results/<UTC-timestamp>-<tool>-<phase>/`
together with a `metadata.txt` describing the host and settings used.

## Apply at boot

```bash
sudo install -m 0644 systemd/tuning.service /etc/systemd/system/tuning.service
sudo systemctl daemon-reload
sudo systemctl enable --now tuning.service
```

Edit the unit (or use `systemctl edit` for a drop-in) to select the profile and
the checkout path.

## Continuous integration

CI runs [shellcheck](https://www.shellcheck.net/) over every `.sh` file and
compiles the BPF objects with `make check`. It does **not** load or attach the
programs: that needs privileges and a matching kernel, so it is left to the
operator.

## License

Files carry an SPDX identifier in their header (`GPL-2.0 OR BSD-3-Clause` for
eBPF sources, GPL-2.0 for kernel-loadable objects, and the project default for
scripts and configuration).

