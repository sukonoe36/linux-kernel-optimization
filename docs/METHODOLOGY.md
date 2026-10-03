# Benchmark Methodology

This document describes **how** to measure the effect of a tuning change. It
deliberately contains no results: numbers depend on your hardware, kernel,
workload and network, so every figure must be produced on the target host with
the scripts in `bench/`.

The goal is a reproducible, honest before/after comparison rather than a single
lucky run.

## 1. Establish a baseline ("before")

1. Confirm the machine is otherwise idle (no background jobs, no package
   updates, no other tenants on shared storage/network).
2. Record the starting kernel settings. `tuning/apply-profile.sh` writes a
   backup of every key it is about to change to `/var/backups/sysctl-profiles/`;
   keep that file as the reference for the "before" state.
3. Run each benchmark script with `--phase before`. Results land in
   `bench/results/<UTC-timestamp>-<tool>-before/`.

## 2. Apply the change

```bash
sudo tuning/apply-profile.sh --profile tuning/network-latency.conf
```

The script prints each key it applied. Any failures are reported and the script
exits non-zero, so a partial apply is visible.

## 3. Run the "after" measurement

Re-run the **same** scripts with `--phase after`, against the same target, with
the same flags, on the same host, at a comparable time of day. Results land in
`bench/results/<UTC-timestamp>-<tool>-after/`.

## 4. Warmup

The first run of any benchmark is not representative: page cache is cold, CPU
frequency scaling has not settled, TCP has not converged its congestion window,
and storage caches are empty. Always:

- Perform at least one **discard/warmup run** whose output you do not report.
- Discard warmup output for `fio` (page cache fill) and `iperf3` (slow-start
  ramp) in particular.

## 5. Repeat runs

- Run every configuration **at least 5 times** and report the distribution, not
  a single sample.
- Alternate the order (before, after, before, after, ...) when comparing two
  configurations on the same host so that drift over time affects both equally.
- Keep all raw files. Do not delete outliers silently; note them and explain.

## 6. Reporting p99 (and other percentiles)

Mean throughput hides tail behavior, which is usually what matters for
latency-sensitive services. For every metric:

- Report **p50, p90, p99** and the **max**, alongside the mean.
- For `iperf3`, parse the per-interval JSON (`--json`) and compute percentiles
  over the intervals, not just the summary line.
- For `fio`, use `--output-format=json` and read `clat`/`lat` percentiles from
  the JSON rather than the human-readable summary.
- For `biolatency`, report the histogram buckets and derive p99 from the bucket
  boundaries; state the bucket resolution so the reader knows the precision.
- For `tcp_retrans`, report retransmits per destination and the share of total;
  a lower count is only meaningful when total traffic is comparable.

Percentiles computed from a histogram are approximations. Say so.

## 7. Comparing configurations

- Compute the delta as `(after - before) / before` per metric and report it with
  the spread across repeats.
- Only claim an improvement when the distributions are separated by more than
  their run-to-run spread.
- State what did **not** improve. A profile that helps throughput often hurts
  latency; document the tradeoff rather than hiding it.

## 8. What not to do

- Do not copy numbers from another machine, a blog post, or a vendor datasheet.
- Do not report a single run as a result.
- Do not tune the benchmark harness (e.g. drop caches mid-run) without saying
  so.
- Do not claim a change is faster without the raw files to back it up.
