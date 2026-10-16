#[path = "../src/xor_cipher.rs"]
mod xor_cipher;

use std::hint::black_box;
use std::time::Duration;

use criterion::{BenchmarkId, Criterion, Throughput, criterion_group, criterion_main};

fn bench_xor_cipher(c: &mut Criterion) {
    let mut group = c.benchmark_group("xor_cipher");
    for kib in [1usize, 2, 4, 8, 16, 32, 64, 65] {
        let len = kib * 1024;
        let mut data = vec![0x5a; len];
        let key = vec![0xa5; len];
        group.throughput(Throughput::Bytes(len as u64));
        group.bench_function(format!("{kib}KiB"), |b| {
            b.iter(|| xor_cipher::xor(black_box(&mut data), black_box(&key)));
        });
    }
    group.finish();
}

fn bench_xor_alignment(c: &mut Criterion) {
    const SIZE: usize = 65_535;

    let mut group = c.benchmark_group("xor_alignment");
    group
        .warm_up_time(Duration::from_millis(200))
        .measurement_time(Duration::from_millis(500))
        .sample_size(20)
        .nresamples(1000)
        .throughput(Throughput::Bytes(SIZE as u64));

    for src_align in 0..4 {
        for dst_align in 0..4 {
            let src = vec![0xABu8; SIZE + 4];
            let mut dst = vec![0xCDu8; SIZE + 4];

            let src = &src[src_align..src_align + SIZE];
            let dst = &mut dst[dst_align..dst_align + SIZE];

            group.bench_with_input(
                BenchmarkId::new("xor", format!("src{src_align}_dst{dst_align}")),
                &(src_align, dst_align),
                |b, _| {
                    b.iter(|| {
                        xor_cipher::xor(black_box(&mut *dst), black_box(src));
                    });
                },
            );
        }
    }

    group.finish();
}

criterion_group!(benches, bench_xor_cipher, bench_xor_alignment);
criterion_main!(benches);
